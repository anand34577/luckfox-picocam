package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

const schemaAccess = `
CREATE TABLE IF NOT EXISTS api_keys (
	id           INTEGER PRIMARY KEY AUTOINCREMENT,
	name         TEXT NOT NULL,
	hash         TEXT NOT NULL UNIQUE,
	prefix       TEXT NOT NULL,
	role         TEXT NOT NULL,           -- read | admin
	created_ms   INTEGER NOT NULL,
	last_used_ms INTEGER NOT NULL DEFAULT 0,
	expires_ms   INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS settings (k TEXT PRIMARY KEY, v TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS audit (
	id      INTEGER PRIMARY KEY AUTOINCREMENT,
	ts_ms   INTEGER NOT NULL,
	actor   TEXT NOT NULL,
	ip      TEXT NOT NULL,
	method  TEXT NOT NULL,
	path    TEXT NOT NULL,
	status  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit(ts_ms);
`

const (
	roleRead   = "read"
	roleAdmin  = "admin"
	cookieName = "edgeai_session"
	sessionTTL = 12 * time.Hour
)

type principal struct {
	Role     string
	KeyID    int64 // 0 = the master key (-api-key)
	Name     string
	ByCookie bool
}

// ------------------------------------------------------------------ settings

func (s *Store) getSetting(k string) (string, bool) {
	var v string
	err := s.db.QueryRow(`SELECT v FROM settings WHERE k = ?`, k).Scan(&v)
	return v, err == nil
}

func (s *Store) setSetting(k, v string) error {
	_, err := s.db.Exec(`INSERT INTO settings(k, v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v = excluded.v`, k, v)
	return err
}

// secret returns the server's signing secret, creating it on first use. It signs sessions and image URLs.
func (s *Store) secret() []byte {
	if v, ok := s.getSetting("signing_secret"); ok {
		if b, err := hex.DecodeString(v); err == nil && len(b) >= 16 {
			return b
		}
	}
	b := make([]byte, 32)
	rand.Read(b)
	s.setSetting("signing_secret", hex.EncodeToString(b))
	return b
}

func (a *App) sign(parts ...string) string {
	m := hmac.New(sha256.New, a.secret)
	m.Write([]byte(strings.Join(parts, "\n")))
	return hex.EncodeToString(m.Sum(nil))
}

// ------------------------------------------------------------------ signed image links

// img builds the absolute URL of a stored image. With auth on, the link carries an expiring signature, so an
// <img> tag or a webhook consumer can load it without the API key.
func (a *App) img(base, p string) string {
	if p == "" {
		return ""
	}
	if a.apiKey == "" {
		return base + p
	}
	exp := strconv.FormatInt(time.Now().Add(a.imageTTL).Unix(), 10)
	return base + p + "?exp=" + exp + "&sig=" + a.sign("img", p, exp)
}

func (a *App) validImageSig(r *http.Request) bool {
	q := r.URL.Query()
	exp, err := strconv.ParseInt(q.Get("exp"), 10, 64)
	if err != nil || exp < time.Now().Unix() {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(q.Get("sig")), []byte(a.sign("img", r.URL.Path, q.Get("exp")))) == 1
}

// ------------------------------------------------------------------ keys

func hashKey(k string) string {
	h := sha256.Sum256([]byte(k))
	return hex.EncodeToString(h[:])
}

type KeyInfo struct {
	ID         int64  `json:"id"`
	Name       string `json:"name"`
	Prefix     string `json:"prefix"` // first characters, to recognise a key
	Role       string `json:"role"`
	CreatedMs  int64  `json:"created_ms"`
	LastUsedMs int64  `json:"last_used_ms"`
	ExpiresMs  int64  `json:"expires_ms"`    // 0 = never
	Key        string `json:"key,omitempty"` // only in the create response
}

func (a *App) lookupKey(k string) *principal {
	if k == "" {
		return nil
	}
	if a.apiKey != "" && subtle.ConstantTimeCompare([]byte(k), []byte(a.apiKey)) == 1 {
		return &principal{Role: roleAdmin, Name: "master"}
	}
	var id, exp int64
	var role, name string
	err := a.store.db.QueryRow(`SELECT id, role, name, expires_ms FROM api_keys WHERE hash = ?`, hashKey(k)).
		Scan(&id, &role, &name, &exp)
	if err != nil || (exp > 0 && exp < time.Now().UnixMilli()) {
		return nil
	}
	a.touchKey(id)
	return &principal{Role: role, KeyID: id, Name: name}
}

// touchKey records last use at most once a minute per key, so reads do not turn into writes.
func (a *App) touchKey(id int64) {
	a.touchMu.Lock()
	defer a.touchMu.Unlock()
	now := time.Now()
	if now.Sub(a.touched[id]) < time.Minute {
		return
	}
	a.touched[id] = now
	go a.store.db.Exec(`UPDATE api_keys SET last_used_ms = ? WHERE id = ?`, now.UnixMilli(), id)
}

func (a *App) listKeys(w http.ResponseWriter, r *http.Request) {
	rows, err := a.store.db.Query(`SELECT id, name, prefix, role, created_ms, last_used_ms, expires_ms FROM api_keys ORDER BY id`)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	defer rows.Close()
	out := []KeyInfo{}
	for rows.Next() {
		var k KeyInfo
		if rows.Scan(&k.ID, &k.Name, &k.Prefix, &k.Role, &k.CreatedMs, &k.LastUsedMs, &k.ExpiresMs) == nil {
			out = append(out, k)
		}
	}
	writeJSON(w, map[string]any{"data": out})
}

func (a *App) createKey(w http.ResponseWriter, r *http.Request) {
	var b struct {
		Name      string `json:"name"`
		Role      string `json:"role"`
		ExpiresIn int64  `json:"expires_in_days"`
	}
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<10)).Decode(&b) != nil || strings.TrimSpace(b.Name) == "" ||
		len(b.Name) > 60 {
		apiError(w, 400, "bad_request", "body must be JSON with a name (1-60 characters)")
		return
	}
	if b.Role == "" {
		b.Role = roleRead
	}
	if b.Role != roleRead && b.Role != roleAdmin || b.ExpiresIn < 0 || b.ExpiresIn > 3650 {
		apiError(w, 400, "bad_request", `role must be "read" or "admin"; expires_in_days 0-3650`)
		return
	}
	raw := make([]byte, 24)
	rand.Read(raw)
	key := "ek_" + hex.EncodeToString(raw)
	now := time.Now().UnixMilli()
	var exp int64
	if b.ExpiresIn > 0 {
		exp = now + b.ExpiresIn*86_400_000
	}
	res, err := a.store.db.Exec(`INSERT INTO api_keys(name, hash, prefix, role, created_ms, expires_ms) VALUES(?,?,?,?,?,?)`,
		strings.TrimSpace(b.Name), hashKey(key), key[:9], b.Role, now, exp)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	id, _ := res.LastInsertId()
	writeStatus(w, 201, KeyInfo{ID: id, Name: strings.TrimSpace(b.Name), Prefix: key[:9], Role: b.Role, CreatedMs: now,
		ExpiresMs: exp, Key: key})
}

func (a *App) deleteKey(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, 400, "bad_request", "id must be a positive integer")
		return
	}
	res, _ := a.store.db.Exec(`DELETE FROM api_keys WHERE id = ?`, id)
	if n, _ := res.RowsAffected(); n == 0 {
		apiError(w, 404, "not_found", "no such key")
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

// ------------------------------------------------------------------ sessions (dashboard login)

func (a *App) sessionToken(p *principal) string {
	payload := fmt.Sprintf("%s|%d|%s|%d", p.Role, p.KeyID, base64.RawURLEncoding.EncodeToString([]byte(p.Name)),
		time.Now().Add(sessionTTL).Unix())
	return base64.RawURLEncoding.EncodeToString([]byte(payload)) + "." + a.sign("session", payload)
}

func (a *App) readSession(r *http.Request) *principal {
	c, err := r.Cookie(cookieName)
	if err != nil {
		return nil
	}
	i := strings.IndexByte(c.Value, '.')
	if i < 0 {
		return nil
	}
	raw, err := base64.RawURLEncoding.DecodeString(c.Value[:i])
	if err != nil || subtle.ConstantTimeCompare([]byte(c.Value[i+1:]), []byte(a.sign("session", string(raw)))) != 1 {
		return nil
	}
	f := strings.Split(string(raw), "|")
	if len(f) != 4 {
		return nil
	}
	exp, _ := strconv.ParseInt(f[3], 10, 64)
	id, _ := strconv.ParseInt(f[1], 10, 64)
	name, _ := base64.RawURLEncoding.DecodeString(f[2])
	if exp < time.Now().Unix() {
		return nil
	}
	if id != 0 { // a deleted or expired key ends its sessions too
		var n int
		a.store.db.QueryRow(`SELECT COUNT(*) FROM api_keys WHERE id = ? AND (expires_ms = 0 OR expires_ms > ?)`, id,
			time.Now().UnixMilli()).Scan(&n)
		if n == 0 {
			return nil
		}
	}
	return &principal{Role: f[0], KeyID: id, Name: string(name), ByCookie: true}
}

// loginLimiter slows password guessing: 8 failures per address per 5 minutes.
type loginLimiter struct {
	mu   sync.Mutex
	fail map[string][]time.Time
}

func (l *loginLimiter) blocked(ip string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	cut := time.Now().Add(-5 * time.Minute)
	keep := l.fail[ip][:0]
	for _, t := range l.fail[ip] {
		if t.After(cut) {
			keep = append(keep, t)
		}
	}
	l.fail[ip] = keep
	if len(keep) == 0 {
		delete(l.fail, ip)
	}
	return len(keep) >= 8
}

func (l *loginLimiter) fails(ip string) {
	l.mu.Lock()
	l.fail[ip] = append(l.fail[ip], time.Now())
	l.mu.Unlock()
}

func clientIP(r *http.Request) string {
	h, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return h
}

func isHTTPS(r *http.Request) bool {
	return r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https"
}

func (a *App) login(w http.ResponseWriter, r *http.Request) {
	if a.apiKey == "" {
		writeJSON(w, map[string]any{"ok": true, "role": roleAdmin, "auth_required": false})
		return
	}
	ip := clientIP(r)
	if a.limiter.blocked(ip) {
		w.Header().Set("Retry-After", "300")
		apiError(w, http.StatusTooManyRequests, "rate_limited", "too many failed logins; try again in a few minutes")
		return
	}
	var b struct{ Key string }
	json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<10)).Decode(&b)
	p := a.lookupKey(strings.TrimSpace(b.Key))
	if p == nil {
		a.limiter.fails(ip)
		apiError(w, 401, "unauthorized", "invalid key")
		return
	}
	http.SetCookie(w, &http.Cookie{Name: cookieName, Value: a.sessionToken(p), Path: "/", HttpOnly: true,
		SameSite: http.SameSiteStrictMode, Secure: isHTTPS(r), MaxAge: int(sessionTTL.Seconds())})
	writeJSON(w, map[string]any{"ok": true, "role": p.Role, "name": p.Name, "auth_required": true})
}

func (a *App) logout(w http.ResponseWriter, r *http.Request) {
	http.SetCookie(w, &http.Cookie{Name: cookieName, Value: "", Path: "/", HttpOnly: true, MaxAge: -1,
		SameSite: http.SameSiteStrictMode, Secure: isHTTPS(r)})
	writeJSON(w, map[string]any{"ok": true})
}

// session tells the dashboard whether it must show a login form.
func (a *App) session(w http.ResponseWriter, r *http.Request) {
	p := a.authenticate(r)
	out := map[string]any{"auth_required": a.apiKey != "", "authenticated": a.apiKey == "" || p != nil}
	if a.apiKey == "" {
		out["role"] = roleAdmin
	} else if p != nil {
		out["role"], out["name"] = p.Role, p.Name
	}
	writeJSON(w, out)
}

// ------------------------------------------------------------------ middleware

func publicPath(r *http.Request) bool {
	switch {
	case r.Method == http.MethodPost && (r.URL.Path == "/api/v1/events" || r.URL.Path == "/api/v1/heartbeat"):
		return true
	case r.URL.Path == "/api/v1/health", r.URL.Path == "/api/v1/openapi.json", r.URL.Path == "/api/v1/docs",
		r.URL.Path == "/api/v1/session", r.URL.Path == "/api/v1/login", r.URL.Path == "/api/v1/logout":
		return true
	case r.Method == http.MethodOptions:
		return true
	case !strings.HasPrefix(r.URL.Path, "/api/") && !strings.HasPrefix(r.URL.Path, "/images/"):
		return true // the dashboard's static files
	}
	return false
}

// adminOnly lists what a read-only key may not touch even with GET.
func adminOnly(path string) bool {
	for _, p := range []string{"/api/v1/keys", "/api/v1/webhooks", "/api/v1/rules", "/api/v1/channels",
		"/api/v1/settings", "/api/v1/alerts", "/api/v1/audit", "/api/v1/backup", "/api/v1/maintenance"} {
		if path == p || strings.HasPrefix(path, p+"/") {
			return true
		}
	}
	return false
}

func requestKey(r *http.Request) string {
	if h := r.Header.Get("Authorization"); len(h) > 7 && strings.EqualFold(h[:7], "bearer ") {
		return strings.TrimSpace(h[7:])
	}
	if k := r.Header.Get("X-API-Key"); k != "" {
		return k
	}
	return r.URL.Query().Get("api_key")
}

func (a *App) authenticate(r *http.Request) *principal {
	if p := a.lookupKey(requestKey(r)); p != nil {
		return p
	}
	return a.readSession(r)
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (s *statusRecorder) WriteHeader(c int) { s.status = c; s.ResponseWriter.WriteHeader(c) }

// withAccess adds CORS (opt-in), authentication, roles and the audit log.
func (a *App) withAccess(h http.Handler, corsOrigin string) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if corsOrigin != "" {
			w.Header().Set("Access-Control-Allow-Origin", corsOrigin)
			w.Header().Set("Access-Control-Allow-Headers", "Authorization, X-API-Key, Content-Type")
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS")
			w.Header().Set("Access-Control-Expose-Headers", "Content-Disposition")
			w.Header().Set("Vary", "Origin")
			if r.Method == http.MethodOptions {
				w.WriteHeader(http.StatusNoContent)
				return
			}
		}
		actor := "open"
		if a.apiKey != "" && !publicPath(r) {
			p := a.authenticate(r)
			switch {
			case p == nil && strings.HasPrefix(r.URL.Path, "/images/") && a.validImageSig(r):
				p = &principal{Role: roleRead, Name: "signed-url"}
			case p == nil:
				w.Header().Set("WWW-Authenticate", `Bearer realm="edgeai"`)
				apiError(w, http.StatusUnauthorized, "unauthorized", "missing or invalid API key")
				return
			}
			safe := r.Method == http.MethodGet || r.Method == http.MethodHead
			if p.ByCookie && !safe {
				// cookies are sent by any site the browser visits; only accept same-origin writes
				if o := r.Header.Get("Origin"); o != "" {
					if u, err := url.Parse(o); err != nil || u.Host != r.Host {
						apiError(w, http.StatusForbidden, "forbidden", "cross-site request refused")
						return
					}
				}
			}
			if p.Role != roleAdmin && (!safe || adminOnly(r.URL.Path)) {
				apiError(w, http.StatusForbidden, "forbidden", "this key is read-only or may not use this endpoint")
				return
			}
			actor = p.Name
			if p.KeyID != 0 {
				actor += "#" + strconv.FormatInt(p.KeyID, 10)
			}
		}
		if r.Method != http.MethodGet && r.Method != http.MethodHead && strings.HasPrefix(r.URL.Path, "/api/") &&
			!(r.Method == http.MethodPost && (r.URL.Path == "/api/v1/events" || r.URL.Path == "/api/v1/heartbeat" ||
				r.URL.Path == "/api/v1/login" || r.URL.Path == "/api/v1/logout")) {
			rec := &statusRecorder{ResponseWriter: w, status: 200}
			h.ServeHTTP(rec, r)
			go a.store.db.Exec(`INSERT INTO audit(ts_ms, actor, ip, method, path, status) VALUES(?,?,?,?,?,?)`,
				time.Now().UnixMilli(), actor, clientIP(r), r.Method, r.URL.Path, rec.status)
			return
		}
		h.ServeHTTP(w, r)
	})
}

// ------------------------------------------------------------------ audit endpoint

func (a *App) listAudit(w http.ResponseWriter, r *http.Request) {
	limit := limitParam(r.URL.Query().Get("limit"), 100, 1000)
	args := []any{}
	where := ""
	if c, _ := strconv.ParseInt(r.URL.Query().Get("cursor"), 10, 64); c > 0 {
		where, args = " WHERE id < ?", append(args, c)
	}
	rows, err := a.store.db.Query(`SELECT id, ts_ms, actor, ip, method, path, status FROM audit`+where+
		` ORDER BY id DESC LIMIT ?`, append(args, limit)...)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	defer rows.Close()
	type row struct {
		ID     int64  `json:"id"`
		TsMs   int64  `json:"ts_ms"`
		Time   string `json:"time"`
		Actor  string `json:"actor"`
		IP     string `json:"ip"`
		Method string `json:"method"`
		Path   string `json:"path"`
		Status int    `json:"status"`
	}
	out := []row{}
	for rows.Next() {
		var x row
		if rows.Scan(&x.ID, &x.TsMs, &x.Actor, &x.IP, &x.Method, &x.Path, &x.Status) == nil {
			x.Time = isoTime(x.TsMs)
			out = append(out, x)
		}
	}
	page := Page[row]{Data: out}
	if len(out) == limit {
		next := out[len(out)-1].ID
		page.NextCursor = &next
	}
	a.store.db.QueryRow(`SELECT COUNT(*) FROM audit`).Scan(&page.Total)
	writeJSON(w, page)
}

var _ = sql.ErrNoRows
var _ = log.Printf
