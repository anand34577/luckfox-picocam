package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"net/http"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const schemaRules = `
CREATE TABLE IF NOT EXISTS channels (
	id      INTEGER PRIMARY KEY AUTOINCREMENT,
	name    TEXT NOT NULL,
	kind    TEXT NOT NULL,
	config  TEXT NOT NULL,
	enabled INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS rules (
	id         INTEGER PRIMARY KEY AUTOINCREMENT,
	def        TEXT NOT NULL,
	created_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS alerts (
	id           INTEGER PRIMARY KEY AUTOINCREMENT,
	ts_ms        INTEGER NOT NULL,
	rule_id      INTEGER NOT NULL,
	rule_name    TEXT NOT NULL,
	subject      TEXT NOT NULL,
	message      TEXT NOT NULL,
	detection_id INTEGER NOT NULL DEFAULT 0,
	delivered    INTEGER NOT NULL DEFAULT 0,
	failed       INTEGER NOT NULL DEFAULT 0,
	error        TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_alerts_ts ON alerts(ts_ms);
`

// ------------------------------------------------------------------ model

type RuleMatch struct {
	Type         string   `json:"type,omitempty"`          // detection rules: face | vehicle | "" (both)
	Device       string   `json:"device,omitempty"`        // only this camera
	Watchlist    string   `json:"watchlist,omitempty"`     // vehicles: allow | block
	Face         string   `json:"face,omitempty"`          // faces: unknown | named
	PlatePattern string   `json:"plate_pattern,omitempty"` // vehicles: glob on the plate, e.g. MH12*
	PlateRead    *bool    `json:"plate_read,omitempty"`    // vehicles: true = plate was read, false = unreadable
	MinQuality   float64  `json:"min_quality,omitempty"`   // faces
	Hours        string   `json:"hours,omitempty"`         // "22:00-06:00", may cross midnight
	Days         []int    `json:"days,omitempty"`          // 0 = Sunday ... 6 = Saturday; empty = every day
	Tz           string   `json:"tz,omitempty"`            // IANA name for hours/days; default server time zone
	Metric       string   `json:"metric,omitempty"`        // device_health: temp_c | cpu_pct | npu_load_pct | mem_avail_kb | cma_pct
	Op           string   `json:"op,omitempty"`            // device_health / server_disk: > or <
	Value        *float64 `json:"value,omitempty"`
}

type Rule struct {
	ID        int64     `json:"id"`
	Name      string    `json:"name"`
	Enabled   bool      `json:"enabled"`
	On        string    `json:"on"` // detection | device_offline | device_online | device_health | server_disk
	Match     RuleMatch `json:"match"`
	Channels  []int64   `json:"channels"`
	CooldownS int       `json:"cooldown_s"` // minimum seconds between alerts for the same subject
	CreatedMs int64     `json:"created_ms"`
}

type Channel struct {
	ID      int64             `json:"id"`
	Name    string            `json:"name"`
	Kind    string            `json:"kind"` // telegram | ntfy | email | http
	Config  map[string]string `json:"config"`
	Enabled bool              `json:"enabled"`
}

var channelFields = map[string][]string{
	"telegram": {"bot_token", "chat_id"},
	"ntfy":     {"url", "token"},
	"email":    {"host", "port", "security", "username", "password", "from", "to"},
	"http":     {"url", "secret"},
}
var channelRequired = map[string][]string{
	"telegram": {"bot_token", "chat_id"},
	"ntfy":     {"url"},
	"email":    {"host", "from", "to"},
	"http":     {"url"},
}
var secretFields = map[string]bool{"bot_token": true, "password": true, "token": true, "secret": true}

const masked = "********"

var hoursRe = regexp.MustCompile(`^([01]\d|2[0-3]):[0-5]\d-([01]\d|2[0-3]):[0-5]\d$`)

func (r *Rule) validate(chans map[int64]Channel) string {
	r.Name = strings.TrimSpace(r.Name)
	if r.Name == "" || len(r.Name) > 80 {
		return "name is required (max 80 characters)"
	}
	m := r.Match
	switch r.On {
	case "detection":
		if m.Type != "" && m.Type != "face" && m.Type != "vehicle" {
			return "match.type must be face or vehicle"
		}
		if m.Watchlist != "" && m.Watchlist != "allow" && m.Watchlist != "block" {
			return "match.watchlist must be allow or block"
		}
		if m.Face != "" && m.Face != "unknown" && m.Face != "named" {
			return "match.face must be unknown or named"
		}
		if (m.Watchlist != "" || m.PlatePattern != "" || m.PlateRead != nil) && m.Type == "face" {
			return "watchlist / plate_pattern / plate_read only apply to vehicle rules"
		}
		if (m.Face != "" || m.MinQuality != 0) && m.Type == "vehicle" {
			return "face / min_quality only apply to face rules"
		}
		if _, err := path.Match(strings.ToUpper(m.PlatePattern), ""); err != nil {
			return "match.plate_pattern is not a valid pattern (use * and ?)"
		}
		if m.MinQuality < 0 || m.MinQuality > 1 {
			return "match.min_quality must be between 0 and 1"
		}
	case "device_offline", "device_online":
	case "device_health":
		switch m.Metric {
		case "temp_c", "cpu_pct", "npu_load_pct", "mem_avail_kb", "cma_pct":
		default:
			return "match.metric must be temp_c, cpu_pct, npu_load_pct, mem_avail_kb or cma_pct"
		}
		if (m.Op != ">" && m.Op != "<") || m.Value == nil {
			return `match.op (">" or "<") and match.value are required`
		}
	case "server_disk":
		if (m.Op != ">" && m.Op != "<") || m.Value == nil || m.Metric != "" && m.Metric != "disk_free_pct" {
			return `server_disk needs match.op (">" or "<") and match.value (percent of free disk)`
		}
	default:
		return "on must be detection, device_offline, device_online, device_health or server_disk"
	}
	if m.Hours != "" && !hoursRe.MatchString(m.Hours) {
		return `match.hours must look like "22:00-06:00"`
	}
	for _, d := range m.Days {
		if d < 0 || d > 6 {
			return "match.days are 0 (Sunday) to 6 (Saturday)"
		}
	}
	if m.Tz != "" {
		if _, err := time.LoadLocation(m.Tz); err != nil {
			return "match.tz is not a known time zone"
		}
	}
	if len(r.Channels) == 0 {
		return "channels: pick at least one"
	}
	for _, id := range r.Channels {
		if _, ok := chans[id]; !ok {
			return fmt.Sprintf("channel %d does not exist", id)
		}
	}
	if r.CooldownS < 0 || r.CooldownS > 86400 {
		return "cooldown_s must be 0 to 86400"
	}
	return ""
}

// ------------------------------------------------------------------ engine

type Alerts struct {
	a        *App
	mu       sync.RWMutex
	rules    []Rule
	chans    map[int64]Channel
	loaded   bool
	cdMu     sync.Mutex
	cooldown map[string]time.Time
	sem      chan struct{} // at most 4 notifications in flight: bounded RAM and sockets
	devState map[string]*devHealth
}

type devHealth struct {
	offlineSince time.Time
	notified     bool
}

func newAlerts(a *App) *Alerts {
	return &Alerts{a: a, cooldown: map[string]time.Time{}, sem: make(chan struct{}, 4), devState: map[string]*devHealth{}}
}

func (al *Alerts) reload() {
	rules, chans := []Rule{}, map[int64]Channel{}
	if rows, err := al.a.store.db.Query(`SELECT id, def, created_ms FROM rules ORDER BY id`); err == nil {
		for rows.Next() {
			var id, c int64
			var def string
			var r Rule
			if rows.Scan(&id, &def, &c) == nil && json.Unmarshal([]byte(def), &r) == nil {
				r.ID, r.CreatedMs = id, c
				rules = append(rules, r)
			}
		}
		rows.Close()
	}
	if rows, err := al.a.store.db.Query(`SELECT id, name, kind, config, enabled FROM channels ORDER BY id`); err == nil {
		for rows.Next() {
			var c Channel
			var cfg string
			if rows.Scan(&c.ID, &c.Name, &c.Kind, &cfg, &c.Enabled) == nil {
				json.Unmarshal([]byte(cfg), &c.Config)
				chans[c.ID] = c
			}
		}
		rows.Close()
	}
	al.mu.Lock()
	al.rules, al.chans, al.loaded = rules, chans, true
	al.mu.Unlock()
}

func (al *Alerts) snapshot() ([]Rule, map[int64]Channel) {
	al.mu.RLock()
	loaded := al.loaded
	al.mu.RUnlock()
	if !loaded {
		al.reload()
	}
	al.mu.RLock()
	defer al.mu.RUnlock()
	return al.rules, al.chans
}

// timeOK checks the optional hours/days window in the rule's time zone.
func (m RuleMatch) timeOK(now time.Time) bool {
	if m.Hours == "" && len(m.Days) == 0 {
		return true
	}
	if m.Tz != "" {
		if loc, err := time.LoadLocation(m.Tz); err == nil {
			now = now.In(loc)
		}
	}
	if len(m.Days) > 0 {
		ok := false
		for _, d := range m.Days {
			if d == int(now.Weekday()) {
				ok = true
			}
		}
		if !ok {
			return false
		}
	}
	if m.Hours == "" {
		return true
	}
	mins := now.Hour()*60 + now.Minute()
	from := atoi(m.Hours[0:2])*60 + atoi(m.Hours[3:5])
	to := atoi(m.Hours[6:8])*60 + atoi(m.Hours[9:11])
	if from <= to {
		return mins >= from && mins < to
	}
	return mins >= from || mins < to // crosses midnight
}

func atoi(s string) int { n, _ := strconv.Atoi(s); return n }

// matchDetection reports whether the rule fires for this detection, and why not when it does not.
func (r Rule) matchDetection(d Detection, now time.Time) (bool, string) {
	m := r.Match
	if r.On != "detection" {
		return false, "not a detection rule"
	}
	if m.Type != "" && m.Type != d.Type {
		return false, "type differs"
	}
	if m.Device != "" && m.Device != d.DeviceID {
		return false, "other camera"
	}
	if !m.timeOK(now) {
		return false, "outside the hours/days window"
	}
	switch d.Type {
	case "face":
		if d.Person == nil {
			return false, "face has no person yet"
		}
		if m.Face == "unknown" && d.Person.Named || m.Face == "named" && !d.Person.Named {
			return false, "face name state differs"
		}
		if d.Quality < m.MinQuality {
			return false, "quality below minimum"
		}
	case "vehicle":
		v := d.Vehicle
		if v == nil {
			return false, "no vehicle data"
		}
		if m.Watchlist != "" && v.Watchlist != m.Watchlist {
			return false, "watchlist differs"
		}
		if m.PlateRead != nil && (v.Plate != nil) != *m.PlateRead {
			return false, "plate read state differs"
		}
		if m.PlatePattern != "" {
			if v.Plate == nil {
				return false, "no plate"
			}
			if ok, _ := path.Match(strings.ToUpper(m.PlatePattern), v.Plate.Text); !ok {
				return false, "plate pattern differs"
			}
		}
	}
	return true, ""
}

func subjectOf(d Detection) string {
	switch {
	case d.Person != nil:
		return "person:" + strconv.FormatInt(d.Person.ID, 10)
	case d.Vehicle != nil && d.Vehicle.Plate != nil:
		return "plate:" + d.Vehicle.Plate.Text
	}
	return "device:" + d.DeviceID
}

func alertText(d Detection) string {
	loc := ""
	if t := time.UnixMilli(d.TsMs); d.TsMs > 0 {
		loc = " at " + t.Format("15:04")
	}
	switch d.Type {
	case "face":
		who := "Unknown face"
		if d.Person != nil && d.Person.Named {
			who = d.Person.Name
		} else if d.Person != nil {
			who = fmt.Sprintf("Unknown person #%d", d.Person.ID)
		}
		return fmt.Sprintf("%s seen on %s%s", who, d.DeviceID, loc)
	default:
		v := d.Vehicle
		plate := "no readable plate"
		if v != nil && v.Plate != nil {
			plate = "plate " + v.Plate.Text
		}
		s := fmt.Sprintf("%s, %s on %s%s", capFirst(v.Type), plate, d.DeviceID, loc)
		switch v.Watchlist {
		case "block":
			s = "BLOCKED vehicle: " + s
		case "allow":
			s += " (allowed)"
		}
		if v.Owner != "" {
			s += ", owner " + v.Owner
		}
		return s
	}
}

// onDetection runs every detection rule. It is called for each stored event, so it only reads memory.
func (al *Alerts) onDetection(ev EventRow, d Detection) {
	rules, _ := al.snapshot()
	now := time.Now()
	for _, r := range rules {
		if !r.Enabled || r.On != "detection" {
			continue
		}
		if ok, _ := r.matchDetection(d, now); !ok {
			continue
		}
		if !al.cooledDown(r, subjectOf(d)) {
			continue
		}
		al.fire(r, alertText(d), subjectOf(d), d.ID, &d, al.a.imageFile(ev))
	}
}

func (al *Alerts) cooledDown(r Rule, subject string) bool {
	if r.CooldownS <= 0 {
		return true
	}
	key := strconv.FormatInt(r.ID, 10) + "|" + subject
	al.cdMu.Lock()
	defer al.cdMu.Unlock()
	now := time.Now()
	if len(al.cooldown) > 5000 { // forget expired entries so the map cannot grow without bound
		for k, t := range al.cooldown {
			if now.After(t) {
				delete(al.cooldown, k)
			}
		}
	}
	if t, ok := al.cooldown[key]; ok && now.Before(t) {
		return false
	}
	al.cooldown[key] = now.Add(time.Duration(r.CooldownS) * time.Second)
	return true
}

// imageFile picks the picture sent with an alert: the whole frame, else the crop.
func (a *App) imageFile(e EventRow) string {
	for _, u := range []string{e.FrameURL, e.CropURL} {
		if rel := strings.TrimPrefix(u, "/images/"); u != "" && rel != u {
			p, _ := filepath.Abs(filepath.Join(a.dataDir, "images", filepath.FromSlash(rel)))
			if strings.HasPrefix(p, a.imagesRoot()+string(filepath.Separator)) {
				return p
			}
		}
	}
	return ""
}

// fire delivers a message to every channel of the rule in the background and records the outcome.
func (al *Alerts) fire(r Rule, msg, subject string, detID int64, d *Detection, imgPath string) {
	_, chans := al.snapshot()
	go func() {
		al.sem <- struct{}{}
		defer func() { <-al.sem }()
		var ok, bad int
		var errs []string
		for _, id := range r.Channels {
			c, found := chans[id]
			if !found || !c.Enabled {
				continue
			}
			if err := al.a.send(c, r, msg, d, imgPath); err != nil {
				bad++
				errs = append(errs, c.Name+": "+err.Error())
			} else {
				ok++
			}
		}
		al.a.store.db.Exec(`INSERT INTO alerts(ts_ms, rule_id, rule_name, subject, message, detection_id, delivered, failed, error)
			VALUES(?,?,?,?,?,?,?,?,?)`, time.Now().UnixMilli(), r.ID, r.Name, subject, msg, detID, ok, bad,
			strings.Join(errs, "; "))
	}()
}

// ------------------------------------------------------------------ health monitor

func (al *Alerts) Start() {
	go func() {
		for range time.Tick(15 * time.Second) {
			al.checkHealth()
		}
	}()
}

var offlineGrace = 60 * time.Second

func (al *Alerts) checkHealth() {
	rules, _ := al.snapshot()
	hasHealth := false
	for _, r := range rules {
		if r.Enabled && r.On != "detection" {
			hasHealth = true
		}
	}
	if !hasHealth {
		return
	}
	now := time.Now()
	for _, dev := range al.a.devices.List() {
		st := al.devState[dev.ID]
		if st == nil {
			st = &devHealth{}
			al.devState[dev.ID] = st
		}
		if !dev.Online {
			if st.offlineSince.IsZero() {
				st.offlineSince = time.UnixMilli(dev.LastSeen)
			}
			if !st.notified && now.Sub(st.offlineSince) >= offlineGrace {
				st.notified = true
				al.deviceRules("device_offline", dev, fmt.Sprintf("Camera %s is offline (last seen %s)", dev.ID,
					st.offlineSince.Format("15:04:05")))
			}
			continue
		}
		if st.notified {
			al.deviceRules("device_online", dev, fmt.Sprintf("Camera %s is back online", dev.ID))
		}
		st.offlineSince, st.notified = time.Time{}, false
		var s map[string]float64
		raw := map[string]any{}
		json.Unmarshal(dev.Status, &raw)
		s = map[string]float64{}
		for k, v := range raw {
			if f, ok := v.(float64); ok {
				s[k] = f
			}
		}
		if tot := s["cma_total_kb"]; tot > 0 {
			s["cma_pct"] = s["cma_used_kb"] / tot * 100
		}
		for _, r := range rules {
			if !r.Enabled || r.On != "device_health" || (r.Match.Device != "" && r.Match.Device != dev.ID) {
				continue
			}
			v, have := s[r.Match.Metric]
			if !have || !r.Match.timeOK(now) {
				continue
			}
			if (r.Match.Op == ">" && v > *r.Match.Value) || (r.Match.Op == "<" && v < *r.Match.Value) {
				if al.cooledDown(r, "dev:"+dev.ID) {
					al.fire(r, fmt.Sprintf("Camera %s: %s is %.1f (alert when %s %.1f)", dev.ID, r.Match.Metric, v,
						r.Match.Op, *r.Match.Value), "dev:"+dev.ID, 0, nil, "")
				}
			}
		}
	}
	free, total := diskSpace(al.a.imagesRoot())
	if total > 0 {
		pct := float64(free) / float64(total) * 100
		for _, r := range rules {
			if !r.Enabled || r.On != "server_disk" || !r.Match.timeOK(now) {
				continue
			}
			if (r.Match.Op == "<" && pct < *r.Match.Value) || (r.Match.Op == ">" && pct > *r.Match.Value) {
				if al.cooledDown(r, "server_disk") {
					al.fire(r, fmt.Sprintf("Server disk: %.1f%% free (%.1f GB of %.1f GB)", pct, float64(free)/1e9,
						float64(total)/1e9), "server_disk", 0, nil, "")
				}
			}
		}
	}
}

func (al *Alerts) deviceRules(on string, dev Device, msg string) {
	rules, _ := al.snapshot()
	for _, r := range rules {
		if r.Enabled && r.On == on && (r.Match.Device == "" || r.Match.Device == dev.ID) && r.Match.timeOK(time.Now()) &&
			al.cooledDown(r, "dev:"+dev.ID) {
			al.fire(r, msg, "dev:"+dev.ID, 0, nil, "")
		}
	}
}

// ------------------------------------------------------------------ endpoints: channels

func maskChannel(c Channel) Channel {
	cfg := map[string]string{}
	for k, v := range c.Config {
		if secretFields[k] && v != "" {
			v = masked
		}
		cfg[k] = v
	}
	c.Config = cfg
	return c
}

func cleanChannel(c *Channel, old *Channel) string {
	c.Name = strings.TrimSpace(c.Name)
	allowed, ok := channelFields[c.Kind]
	if !ok {
		return "kind must be telegram, ntfy, email or http"
	}
	if c.Name == "" || len(c.Name) > 60 {
		return "name is required (max 60 characters)"
	}
	cfg := map[string]string{}
	for _, f := range allowed {
		v := strings.TrimSpace(c.Config[f])
		if v == masked && old != nil { // the client sent the masked value back: keep the stored secret
			v = old.Config[f]
		}
		cfg[f] = v
	}
	for _, f := range channelRequired[c.Kind] {
		if cfg[f] == "" {
			return "config." + f + " is required for " + c.Kind
		}
	}
	for k := range c.Config {
		if _, known := cfg[k]; !known {
			return "unknown config field " + k
		}
	}
	if c.Kind == "ntfy" || c.Kind == "http" {
		if !validWebhookURL(cfg["url"]) {
			return "config.url must be an http(s) URL"
		}
	}
	if c.Kind == "email" {
		if cfg["port"] != "" {
			if p, err := strconv.Atoi(cfg["port"]); err != nil || p < 1 || p > 65535 {
				return "config.port must be 1-65535"
			}
		}
		if s := cfg["security"]; s != "" && s != "tls" && s != "starttls" && s != "none" {
			return `config.security must be "tls", "starttls" or "none"`
		}
	}
	c.Config = cfg
	return ""
}

func (a *App) listChannels(w http.ResponseWriter, r *http.Request) {
	_, chans := a.alerts.snapshot()
	out := make([]Channel, 0, len(chans))
	for _, c := range chans {
		out = append(out, maskChannel(c))
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	writeJSON(w, map[string]any{"data": out})
}

func (a *App) createChannel(w http.ResponseWriter, r *http.Request) {
	var c Channel
	c.Enabled = true
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10)).Decode(&c) != nil {
		apiError(w, 400, "bad_request", "body must be JSON: {name, kind, config, enabled}")
		return
	}
	if msg := cleanChannel(&c, nil); msg != "" {
		apiError(w, 400, "bad_request", msg)
		return
	}
	cfg, _ := json.Marshal(c.Config)
	res, err := a.store.db.Exec(`INSERT INTO channels(name, kind, config, enabled) VALUES(?,?,?,?)`, c.Name, c.Kind, string(cfg), c.Enabled)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	c.ID, _ = res.LastInsertId()
	a.alerts.reload()
	writeStatus(w, 201, maskChannel(c))
}

func (a *App) updateChannel(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	_, chans := a.alerts.snapshot()
	old, exists := chans[id]
	if !ok || !exists {
		apiError(w, 404, "not_found", "no such channel")
		return
	}
	c := old // fields left out of the body keep their value
	c.Config = nil
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10)).Decode(&c) != nil || c.Kind != old.Kind {
		apiError(w, 400, "bad_request", "body must be JSON; the kind of a channel cannot change")
		return
	}
	if c.Config == nil {
		c.Config = old.Config
	}
	c.ID = id
	if msg := cleanChannel(&c, &old); msg != "" {
		apiError(w, 400, "bad_request", msg)
		return
	}
	cfg, _ := json.Marshal(c.Config)
	a.store.db.Exec(`UPDATE channels SET name = ?, config = ?, enabled = ? WHERE id = ?`, c.Name, string(cfg), c.Enabled, id)
	a.alerts.reload()
	writeJSON(w, maskChannel(c))
}

func (a *App) deleteChannel(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	rules, chans := a.alerts.snapshot()
	if _, exists := chans[id]; !ok || !exists {
		apiError(w, 404, "not_found", "no such channel")
		return
	}
	for _, ru := range rules {
		for _, c := range ru.Channels {
			if c == id {
				apiError(w, 400, "bad_request", fmt.Sprintf("rule %q still uses this channel", ru.Name))
				return
			}
		}
	}
	a.store.db.Exec(`DELETE FROM channels WHERE id = ?`, id)
	a.alerts.reload()
	writeJSON(w, map[string]any{"ok": true})
}

func (a *App) testChannel(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	_, chans := a.alerts.snapshot()
	c, exists := chans[id]
	if !ok || !exists {
		apiError(w, 404, "not_found", "no such channel")
		return
	}
	if err := a.send(c, Rule{Name: "Test"}, "Test message from EdgeAI", nil, ""); err != nil {
		writeJSON(w, map[string]any{"ok": false, "error": err.Error()})
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

// ------------------------------------------------------------------ endpoints: rules

func (a *App) listRules(w http.ResponseWriter, r *http.Request) {
	rules, _ := a.alerts.snapshot()
	writeJSON(w, map[string]any{"data": rules})
}

func (a *App) saveRule(w http.ResponseWriter, r *http.Request, id int64) {
	rule := Rule{Enabled: true, CooldownS: 60}
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&rule); err != nil {
		apiError(w, 400, "bad_request", "invalid rule JSON: "+err.Error())
		return
	}
	_, chans := a.alerts.snapshot()
	if msg := rule.validate(chans); msg != "" {
		apiError(w, 400, "bad_request", msg)
		return
	}
	rule.ID = id
	def, _ := json.Marshal(rule)
	if id == 0 {
		rule.CreatedMs = time.Now().UnixMilli()
		res, err := a.store.db.Exec(`INSERT INTO rules(def, created_ms) VALUES(?,?)`, string(def), rule.CreatedMs)
		if err != nil {
			apiError(w, 500, "internal", err.Error())
			return
		}
		rule.ID, _ = res.LastInsertId()
		a.alerts.reload()
		writeStatus(w, 201, rule)
		return
	}
	res, _ := a.store.db.Exec(`UPDATE rules SET def = ? WHERE id = ?`, string(def), id)
	if n, _ := res.RowsAffected(); n == 0 {
		apiError(w, 404, "not_found", "no such rule")
		return
	}
	a.alerts.reload()
	writeJSON(w, rule)
}

func (a *App) createRule(w http.ResponseWriter, r *http.Request) { a.saveRule(w, r, 0) }

func (a *App) replaceRule(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, 400, "bad_request", "id must be a positive integer")
		return
	}
	a.saveRule(w, r, id)
}

func (a *App) deleteRule(w http.ResponseWriter, r *http.Request) {
	id, _ := parseID(r.PathValue("id"))
	res, _ := a.store.db.Exec(`DELETE FROM rules WHERE id = ?`, id)
	if n, _ := res.RowsAffected(); n == 0 {
		apiError(w, 404, "not_found", "no such rule")
		return
	}
	a.alerts.reload()
	writeJSON(w, map[string]any{"ok": true})
}

// testRule answers "would this rule fire for that past detection?" without sending anything.
func (a *App) testRule(w http.ResponseWriter, r *http.Request) {
	id, _ := parseID(r.PathValue("id"))
	var body struct {
		DetectionID int64 `json:"detection_id"`
	}
	json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<10)).Decode(&body)
	rules, _ := a.alerts.snapshot()
	for _, ru := range rules {
		if ru.ID != id {
			continue
		}
		ev, err := a.store.GetEvent(body.DetectionID)
		if err == sql.ErrNoRows || body.DetectionID <= 0 {
			apiError(w, 404, "not_found", "no such detection (send {\"detection_id\": N})")
			return
		}
		d := a.detections(a.base(r), []EventRow{ev})[0]
		ok, why := ru.matchDetection(d, time.UnixMilli(d.TsMs))
		out := map[string]any{"matches": ok, "message": alertText(d)}
		if !ok {
			out["reason"] = why
		}
		writeJSON(w, out)
		return
	}
	apiError(w, 404, "not_found", "no such rule")
}

func (a *App) listAlerts(w http.ResponseWriter, r *http.Request) {
	limit := limitParam(r.URL.Query().Get("limit"), 50, 500)
	args := []any{}
	where := ""
	if c, _ := strconv.ParseInt(r.URL.Query().Get("cursor"), 10, 64); c > 0 {
		where, args = " WHERE id < ?", append(args, c)
	}
	rows, err := a.store.db.Query(`SELECT id, ts_ms, rule_id, rule_name, subject, message, detection_id, delivered, failed, error
		FROM alerts`+where+` ORDER BY id DESC LIMIT ?`, append(args, limit)...)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	defer rows.Close()
	type row struct {
		ID          int64  `json:"id"`
		TsMs        int64  `json:"ts_ms"`
		Time        string `json:"time"`
		RuleID      int64  `json:"rule_id"`
		RuleName    string `json:"rule_name"`
		Subject     string `json:"subject"`
		Message     string `json:"message"`
		DetectionID int64  `json:"detection_id,omitempty"`
		Delivered   int    `json:"delivered"`
		Failed      int    `json:"failed"`
		Error       string `json:"error,omitempty"`
	}
	out := []row{}
	for rows.Next() {
		var x row
		if rows.Scan(&x.ID, &x.TsMs, &x.RuleID, &x.RuleName, &x.Subject, &x.Message, &x.DetectionID, &x.Delivered, &x.Failed, &x.Error) == nil {
			x.Time = isoTime(x.TsMs)
			out = append(out, x)
		}
	}
	page := Page[row]{Data: out}
	if len(out) == limit {
		next := out[len(out)-1].ID
		page.NextCursor = &next
	}
	a.store.db.QueryRow(`SELECT COUNT(*) FROM alerts`).Scan(&page.Total)
	writeJSON(w, page)
}

func capFirst(s string) string {
	if s == "" {
		return "Vehicle"
	}
	return strings.ToUpper(s[:1]) + s[1:]
}
