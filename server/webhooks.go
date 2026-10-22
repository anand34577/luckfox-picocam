package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Webhook pushes every new detection to the integrator's URL. The body is signed with HMAC-SHA256 of the
// raw bytes using the webhook secret, sent as "X-EdgeAI-Signature: sha256=<hex>".
type Webhook struct {
	ID        int64    `json:"id"`
	URL       string   `json:"url"`
	Secret    string   `json:"secret,omitempty"` // shown once, on creation
	Types     []string `json:"types"`            // [] = every type
	Enabled   bool     `json:"enabled"`
	CreatedMs int64    `json:"created_ms"`
}

func (s *Store) listWebhooks(onlyEnabled bool) ([]Webhook, []string) {
	rows, err := s.db.Query(`SELECT id, url, secret, types, enabled, created_ms FROM webhooks ORDER BY id`)
	if err != nil {
		return nil, nil
	}
	defer rows.Close()
	var out []Webhook
	var secrets []string
	for rows.Next() {
		var w Webhook
		var types, secret string
		if rows.Scan(&w.ID, &w.URL, &secret, &types, &w.Enabled, &w.CreatedMs) != nil || (onlyEnabled && !w.Enabled) {
			continue
		}
		w.Types = []string{}
		if types != "" {
			w.Types = strings.Split(types, ",")
		}
		out = append(out, w)
		secrets = append(secrets, secret)
	}
	return out, secrets
}

func validTypes(ts []string) bool {
	for _, t := range ts {
		if t != "face" && t != "vehicle" {
			return false
		}
	}
	return true
}

func validWebhookURL(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && (u.Scheme == "http" || u.Scheme == "https") && u.Host != ""
}

func (a *App) listWebhooksH(w http.ResponseWriter, r *http.Request) {
	hs, _ := a.store.listWebhooks(false)
	if hs == nil {
		hs = []Webhook{}
	}
	writeJSON(w, map[string]any{"data": hs})
}

func (a *App) createWebhook(w http.ResponseWriter, r *http.Request) {
	var body struct {
		URL     string   `json:"url"`
		Types   []string `json:"types"`
		Enabled *bool    `json:"enabled"`
		Secret  string   `json:"secret"`
	}
	if json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10)).Decode(&body) != nil || !validWebhookURL(body.URL) {
		apiError(w, http.StatusBadRequest, "bad_request", "body must be JSON with an http(s) url")
		return
	}
	if !validTypes(body.Types) {
		apiError(w, http.StatusBadRequest, "bad_request", `types may only contain "face" and "vehicle"`)
		return
	}
	if body.Secret == "" {
		b := make([]byte, 24)
		rand.Read(b)
		body.Secret = hex.EncodeToString(b)
	}
	enabled := body.Enabled == nil || *body.Enabled
	res, err := a.store.db.Exec(`INSERT INTO webhooks(url, secret, types, enabled, created_ms) VALUES(?,?,?,?,?)`,
		body.URL, body.Secret, strings.Join(body.Types, ","), enabled, time.Now().UnixMilli())
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	id, _ := res.LastInsertId()
	if body.Types == nil {
		body.Types = []string{}
	}
	writeStatus(w, http.StatusCreated, Webhook{ID: id, URL: body.URL, Secret: body.Secret, Types: body.Types,
		Enabled: enabled, CreatedMs: time.Now().UnixMilli()})
}

func (a *App) deleteWebhook(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, http.StatusBadRequest, "bad_request", "id must be a positive integer")
		return
	}
	res, _ := a.store.db.Exec(`DELETE FROM webhooks WHERE id = ?`, id)
	if n, _ := res.RowsAffected(); n == 0 {
		apiError(w, http.StatusNotFound, "not_found", "no such webhook")
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

// testWebhook sends a sample "webhook.test" delivery and reports what the receiver answered.
func (a *App) testWebhook(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, http.StatusBadRequest, "bad_request", "id must be a positive integer")
		return
	}
	hs, secrets := a.store.listWebhooks(false)
	for i, h := range hs {
		if h.ID != id {
			continue
		}
		status, err := deliver(h.URL, secrets[i], "webhook.test", map[string]any{"message": "hello from edgeai"})
		if err != nil {
			writeJSON(w, map[string]any{"ok": false, "error": err.Error()})
			return
		}
		writeJSON(w, map[string]any{"ok": status/100 == 2, "status": status})
		return
	}
	apiError(w, http.StatusNotFound, "not_found", "no such webhook")
}

// Event names carried in the "event" field of a delivery.
const evtDetection = "detection.created"

func deliver(target, secret, event string, data any) (int, error) {
	idb := make([]byte, 8)
	rand.Read(idb)
	body, _ := json.Marshal(map[string]any{"event": event, "delivery_id": hex.EncodeToString(idb),
		"created_ms": time.Now().UnixMilli(), "data": data})
	req, err := http.NewRequest(http.MethodPost, target, bytes.NewReader(body))
	if err != nil {
		return 0, err
	}
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write(body)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "edgeai-webhook/"+apiVersion)
	req.Header.Set("X-EdgeAI-Event", event)
	req.Header.Set("X-EdgeAI-Signature", "sha256="+hex.EncodeToString(mac.Sum(nil)))
	c := http.Client{Timeout: 5 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	resp, err := c.Do(req)
	if err != nil {
		return 0, err
	}
	resp.Body.Close()
	return resp.StatusCode, nil
}

// notifyWebhooks fans a stored event out to every matching webhook without blocking ingest.
// Failed deliveries are retried 3 times (1s, 5s, 25s) and then dropped; use GET /detections to catch up.
// dispatch hands a stored event to the webhooks and the alert rules.
func (a *App) dispatch(base string, ev EventRow) {
	det := a.detections(base, []EventRow{ev})[0]
	a.notifyWebhooks(ev, det)
	a.alerts.onDetection(ev, det)
}

func (a *App) notifyWebhooks(ev EventRow, data Detection) {
	hs, secrets := a.store.listWebhooks(true)
	if len(hs) == 0 {
		return
	}
	for i, h := range hs {
		if len(h.Types) > 0 && !contains(h.Types, ev.Type) {
			continue
		}
		go func(h Webhook, secret string) {
			delay := time.Second
			for attempt := 1; attempt <= 4; attempt++ {
				status, err := deliver(h.URL, secret, evtDetection, data)
				if err == nil && status/100 == 2 {
					return
				}
				if attempt == 4 {
					log.Printf("webhook %d: giving up on detection %d (%s)", h.ID, ev.ID, failure(status, err))
					return
				}
				time.Sleep(delay)
				delay *= 5
			}
		}(h, secrets[i])
	}
}

func failure(status int, err error) string {
	if err != nil {
		return err.Error()
	}
	return fmt.Sprintf("HTTP %d", status)
}

func contains(xs []string, x string) bool {
	for _, v := range xs {
		if v == x {
			return true
		}
	}
	return false
}
