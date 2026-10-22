package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// seed stores an event with the given picture files (field -> content) at an exact time.
func seed(t *testing.T, a *App, e EventRow, files map[string]string) EventRow {
	t.Helper()
	day := time.UnixMilli(e.TsMs).Format("2006-01-02")
	put := func(suffix, content string) string {
		rel := day + "/" + e.DeviceID + "_" + strconv.FormatInt(e.TsMs, 10) + "_" + strconv.FormatInt(e.TrackID, 10) + suffix
		p := filepath.Join(a.dataDir, "images", filepath.FromSlash(rel))
		os.MkdirAll(filepath.Dir(p), 0o755)
		os.WriteFile(p, []byte(content), 0o644)
		return "/images/" + rel
	}
	if c, ok := files["frame"]; ok {
		e.FrameURL = put("_frame.jpg", c)
	}
	if c, ok := files["crop"]; ok {
		e.CropURL = put("_crop.jpg", c)
	}
	if c, ok := files["extra"]; ok {
		e.ExtraURL = put("_extra.jpg", c)
	}
	if e.Meta == nil {
		e.Meta = json.RawMessage(`{}`)
	}
	if err := a.store.InsertEvent(&e); err != nil {
		t.Fatal(err)
	}
	return e
}

func exists(a *App, url string) bool {
	_, err := os.Stat(filepath.Join(a.dataDir, "images", filepath.FromSlash(strings.TrimPrefix(url, "/images/"))))
	return err == nil
}

func TestKeysSessionsAndSignedImages(t *testing.T) {
	a, h := newTestAppFull(t, "master")
	ev := seed(t, a, EventRow{TsMs: time.Now().UnixMilli(), DeviceID: "c1", Type: "vehicle", PlateText: "MH12AB1234"},
		map[string]string{"frame": "FRAME"})

	// keys can only be managed with admin rights
	if rr := call(h, "POST", "/api/v1/keys", `{"name":"x"}`); rr.Code != 401 {
		t.Fatalf("anonymous key create: %d", rr.Code)
	}
	var k KeyInfo
	rr := call(h, "POST", "/api/v1/keys", `{"name":"reporting","role":"read","expires_in_days":30}`, "X-API-Key", "master")
	if rr.Code != 201 {
		t.Fatalf("create: %d %s", rr.Code, rr.Body)
	}
	decode(t, rr, &k)
	if !strings.HasPrefix(k.Key, "ek_") || k.ExpiresMs == 0 {
		t.Fatalf("key: %+v", k)
	}
	var list struct{ Data []KeyInfo }
	decode(t, call(h, "GET", "/api/v1/keys", "", "X-API-Key", "master"), &list)
	if len(list.Data) != 1 || list.Data[0].Key != "" || list.Data[0].Prefix != k.Key[:9] {
		t.Fatalf("list must not reveal keys: %+v", list.Data)
	}

	// a read key reads, but cannot write or reach admin areas
	rk := []string{"Authorization", "Bearer " + k.Key}
	if rr := call(h, "GET", "/api/v1/detections", "", rk...); rr.Code != 200 {
		t.Fatalf("read key read: %d", rr.Code)
	}
	for _, c := range [][2]string{{"DELETE", "/api/v1/detections/" + itoa(ev.ID)}, {"GET", "/api/v1/keys"}, {"GET", "/api/v1/backup"},
		{"GET", "/api/v1/audit"}, {"PATCH", "/api/v1/vehicles/MH12AB1234"}} {
		if rr := call(h, c[0], c[1], `{}`, rk...); rr.Code != 403 {
			t.Fatalf("%s %s with a read key: %d", c[0], c[1], rr.Code)
		}
	}

	// expired and revoked keys stop working
	a.store.db.Exec(`UPDATE api_keys SET expires_ms = ? WHERE id = ?`, time.Now().UnixMilli()-1000, k.ID)
	if rr := call(h, "GET", "/api/v1/detections", "", rk...); rr.Code != 401 {
		t.Fatalf("expired key: %d", rr.Code)
	}
	a.store.db.Exec(`UPDATE api_keys SET expires_ms = 0 WHERE id = ?`, k.ID)
	if rr := call(h, "DELETE", "/api/v1/keys/"+itoa(k.ID), "", "X-API-Key", "master"); rr.Code != 200 {
		t.Fatalf("revoke: %d", rr.Code)
	}
	if rr := call(h, "GET", "/api/v1/detections", "", rk...); rr.Code != 401 {
		t.Fatalf("revoked key: %d", rr.Code)
	}

	// image links in responses are signed, work without the key, and cannot be tampered with
	var d Detection
	decode(t, call(h, "GET", "/api/v1/detections/"+itoa(ev.ID), "", "X-API-Key", "master"), &d)
	if !strings.Contains(d.Images.Full, "?exp=") {
		t.Fatalf("expected a signed link: %s", d.Images.Full)
	}
	path := strings.TrimPrefix(d.Images.Full, "http://cams.test:8090")
	if rr := call(h, "GET", path, ""); rr.Code != 200 || rr.Body.String() != "FRAME" {
		t.Fatalf("signed link: %d %q", rr.Code, rr.Body)
	}
	if rr := call(h, "GET", strings.Replace(path, "sig=", "sig=0", 1), ""); rr.Code != 401 {
		t.Fatalf("tampered signature accepted: %d", rr.Code)
	}
	if rr := call(h, "GET", strings.Split(path, "?")[0], ""); rr.Code != 401 {
		t.Fatalf("unsigned link accepted: %d", rr.Code)
	}
	other := strings.Replace(path, "_frame.jpg", "_other.jpg", 1) // valid signature, different file
	if rr := call(h, "GET", other, ""); rr.Code != 401 {
		t.Fatalf("signature reused for another file: %d", rr.Code)
	}
	a.imageTTL = -time.Hour
	decode(t, call(h, "GET", "/api/v1/detections/"+itoa(ev.ID), "", "X-API-Key", "master"), &d)
	if rr := call(h, "GET", strings.TrimPrefix(d.Images.Full, "http://cams.test:8090"), ""); rr.Code != 401 {
		t.Fatalf("expired link accepted: %d", rr.Code)
	}
}

func TestLoginSession(t *testing.T) {
	_, h := newTestAppFull(t, "master")
	var s map[string]any
	decode(t, call(h, "GET", "/api/v1/session", ""), &s)
	if s["authenticated"] != false || s["auth_required"] != true {
		t.Fatalf("session before login: %v", s)
	}
	if rr := call(h, "POST", "/api/v1/login", `{"key":"nope"}`); rr.Code != 401 {
		t.Fatalf("bad login: %d", rr.Code)
	}
	rr := call(h, "POST", "/api/v1/login", `{"key":"master"}`)
	if rr.Code != 200 {
		t.Fatalf("login: %d %s", rr.Code, rr.Body)
	}
	var cookie *http.Cookie
	for _, c := range rr.Result().Cookies() {
		if c.Name == cookieName {
			cookie = c
		}
	}
	if cookie == nil || !cookie.HttpOnly || cookie.SameSite != http.SameSiteStrictMode {
		t.Fatalf("cookie: %+v", cookie)
	}
	cv := cookie.Name + "=" + cookie.Value
	if rr := call(h, "GET", "/api/v1/detections", "", "Cookie", cv); rr.Code != 200 {
		t.Fatalf("cookie read: %d", rr.Code)
	}
	if rr := call(h, "POST", "/api/v1/webhooks", `{"url":"http://x.test/h"}`, "Cookie", cv, "Origin", "http://cams.test:8090"); rr.Code != 201 {
		t.Fatalf("same-origin write: %d %s", rr.Code, rr.Body)
	}
	if rr := call(h, "POST", "/api/v1/webhooks", `{"url":"http://x.test/h"}`, "Cookie", cv, "Origin", "http://evil.test"); rr.Code != 403 {
		t.Fatalf("cross-site write accepted: %d", rr.Code)
	}
	if rr := call(h, "GET", "/api/v1/detections", "", "Cookie", cookie.Name+"="+cookie.Value+"x"); rr.Code != 401 {
		t.Fatalf("forged cookie: %d", rr.Code)
	}
	// brute force is throttled
	for i := 0; i < 9; i++ {
		call(h, "POST", "/api/v1/login", `{"key":"wrong"}`)
	}
	if rr := call(h, "POST", "/api/v1/login", `{"key":"master"}`); rr.Code != 429 {
		t.Fatalf("expected throttling, got %d", rr.Code)
	}
}

func TestAuditLog(t *testing.T) {
	_, h := newTestAppFull(t, "master")
	call(h, "POST", "/api/v1/webhooks", `{"url":"http://x.test/h"}`, "X-API-Key", "master")
	time.Sleep(100 * time.Millisecond) // the audit row is written after the response
	var pg struct {
		Data []struct{ Actor, Method, Path string }
	}
	decode(t, call(h, "GET", "/api/v1/audit", "", "X-API-Key", "master"), &pg)
	if len(pg.Data) == 0 || pg.Data[0].Method != "POST" || pg.Data[0].Path != "/api/v1/webhooks" || pg.Data[0].Actor != "master" {
		t.Fatalf("audit: %+v", pg.Data)
	}
}

func TestRetention(t *testing.T) {
	a, h := newTestAppFull(t, "")
	now := time.Now()
	old := now.Add(-40 * 24 * time.Hour).UnixMilli()
	vOld := seed(t, a, EventRow{TsMs: old, DeviceID: "c1", Type: "vehicle", PlateText: "OLD1", TrackID: 1}, map[string]string{"frame": "F", "crop": "P"})
	vFlag := seed(t, a, EventRow{TsMs: old, DeviceID: "c1", Type: "vehicle", PlateText: "FLAG1", TrackID: 2}, map[string]string{"frame": "F"})
	vNew := seed(t, a, EventRow{TsMs: now.UnixMilli(), DeviceID: "c1", Type: "vehicle", PlateText: "NEW1", TrackID: 3}, map[string]string{"frame": "F"})
	call(h, "PATCH", "/api/v1/vehicles/FLAG1", `{"watchlist":"block"}`)
	// two faces: one named person, one unnamed; both old
	r1 := a.mustFace(t, old, 10, 1)
	r2 := a.mustFace(t, old, 11, 2)
	a.gallery.Rename(r1, "Alice")
	_ = r2

	if rr := call(h, "PUT", "/api/v1/settings/retention", `{"days":-1}`); rr.Code != 400 {
		t.Fatalf("negative days: %d", rr.Code)
	}
	if rr := call(h, "PUT", "/api/v1/settings/retention", `{"days":30,"protected_days":0}`); rr.Code != 200 {
		t.Fatalf("set: %d", rr.Code)
	}
	res := a.janitor.Run()
	if res.EventsDeleted != 2 { // OLD1 + the unnamed old face
		t.Fatalf("deleted %d: %+v", res.EventsDeleted, res)
	}
	if exists(a, vOld.FrameURL) || exists(a, vOld.CropURL) {
		t.Fatal("old pictures still on disk")
	}
	if !exists(a, vFlag.FrameURL) || !exists(a, vNew.FrameURL) {
		t.Fatal("protected or recent pictures were deleted")
	}
	if res.PersonsDeleted != 1 {
		t.Fatalf("the unnamed face's person should be gone: %+v", res)
	}
	var n int
	a.store.db.QueryRow(`SELECT COUNT(*) FROM persons`).Scan(&n)
	if n != 1 {
		t.Fatalf("persons left: %d", n)
	}
	// protected ones follow their own, longer limit
	call(h, "PUT", "/api/v1/settings/retention", `{"days":30,"protected_days":20}`)
	if res := a.janitor.Run(); res.EventsDeleted != 2 {
		t.Fatalf("protected sweep: %+v", res)
	}
	if !exists(a, vNew.FrameURL) {
		t.Fatal("recent picture lost")
	}
}

// mustFace stores a face event for a fresh person and returns the person id.
func (a *App) mustFace(t *testing.T, ts int64, track int64, axis int) int64 {
	t.Helper()
	emb := make([]float32, 8)
	emb[axis] = 1
	m, err := a.gallery.Match("c", track, emb, 0.5, ts, "")
	if err != nil {
		t.Fatal(err)
	}
	seed(t, a, EventRow{TsMs: ts, DeviceID: "c", Type: "face", TrackID: track, PersonID: m.PersonID}, map[string]string{"frame": "F", "crop": "C"})
	return m.PersonID
}

func TestDiskQuota(t *testing.T) {
	a, h := newTestAppFull(t, "")
	big := strings.Repeat("x", 400_000)
	base := time.Now().Add(-48 * time.Hour).UnixMilli()
	var evs []EventRow
	for i := 0; i < 5; i++ {
		evs = append(evs, seed(t, a, EventRow{TsMs: base + int64(i), DeviceID: "c", Type: "vehicle", PlateText: "Q" + strconv.Itoa(i), TrackID: int64(i)},
			map[string]string{"frame": big}))
	}
	flagged := seed(t, a, EventRow{TsMs: base - 1000, DeviceID: "c", Type: "vehicle", PlateText: "KEEP", TrackID: 99}, map[string]string{"frame": big})
	call(h, "PATCH", "/api/v1/vehicles/KEEP", `{"owner":"Boss"}`)
	// ~2.4 MB on disk, limit 1 MB: the oldest unprotected go first, the labelled plate stays
	call(h, "PUT", "/api/v1/settings/retention", `{"max_disk_gb":0.001}`)
	res := a.janitor.Run()
	if res.EventsDeleted == 0 || res.EventsDeleted > 4 {
		t.Fatalf("quota sweep: %+v", res)
	}
	if !exists(a, flagged.FrameURL) {
		t.Fatal("protected picture was deleted by the quota")
	}
	if exists(a, evs[0].FrameURL) {
		t.Fatal("oldest picture should be deleted first")
	}
	if !exists(a, evs[4].FrameURL) {
		t.Fatal("newest picture should survive")
	}
	if size := a.imagesSize(); size > 1_000_000 {
		t.Fatalf("still %d bytes", size)
	}
}

type sink struct {
	mu   sync.Mutex
	msgs []map[string]any
}

func (s *sink) handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var m map[string]any
		json.NewDecoder(r.Body).Decode(&m)
		s.mu.Lock()
		s.msgs = append(s.msgs, m)
		s.mu.Unlock()
	})
}

func (s *sink) wait(t *testing.T, n int) []map[string]any {
	t.Helper()
	for i := 0; i < 100; i++ {
		s.mu.Lock()
		got := len(s.msgs)
		s.mu.Unlock()
		if got >= n {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	time.Sleep(60 * time.Millisecond) // anything extra would have arrived by now
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]map[string]any(nil), s.msgs...)
}

func TestRulesAndAlerts(t *testing.T) {
	a, h := newTestAppFull(t, "")
	s := &sink{}
	srv := httptest.NewServer(s.handler())
	defer srv.Close()

	var ch Channel
	rr := call(h, "POST", "/api/v1/channels", `{"name":"ops","kind":"http","config":{"url":"`+srv.URL+`","secret":"shh"}}`)
	if rr.Code != 201 {
		t.Fatalf("channel: %d %s", rr.Code, rr.Body)
	}
	decode(t, rr, &ch)
	if ch.Config["secret"] != masked {
		t.Fatalf("secret must be masked: %v", ch.Config)
	}
	for _, bad := range []string{`{"name":"x","kind":"fax","config":{}}`, `{"name":"x","kind":"telegram","config":{"bot_token":"t"}}`,
		`{"name":"x","kind":"http","config":{"url":"ftp://h"}}`, `{"name":"x","kind":"email","config":{"host":"h","from":"a","to":"b","port":"99999"}}`,
		`{"name":"x","kind":"http","config":{"url":"http://h","nope":"1"}}`} {
		if rr := call(h, "POST", "/api/v1/channels", bad); rr.Code != 400 {
			t.Fatalf("accepted %s: %d", bad, rr.Code)
		}
	}
	// editing with the masked value keeps the stored secret
	if rr := call(h, "PATCH", "/api/v1/channels/"+itoa(ch.ID), `{"name":"ops2","config":{"url":"`+srv.URL+`","secret":"********"}}`); rr.Code != 200 {
		t.Fatalf("patch channel: %d %s", rr.Code, rr.Body)
	}
	_, chans := a.alerts.snapshot()
	if chans[ch.ID].Config["secret"] != "shh" || chans[ch.ID].Name != "ops2" {
		t.Fatalf("stored channel: %+v", chans[ch.ID])
	}

	cid := itoa(ch.ID)
	for _, bad := range []string{
		`{"name":"r","on":"detection","channels":[` + cid + `],"match":{"type":"face","watchlist":"block"}}`,
		`{"name":"r","on":"detection","channels":[999]}`,
		`{"name":"r","on":"detection","channels":[` + cid + `],"match":{"hours":"25:00-06:00"}}`,
		`{"name":"r","on":"device_health","channels":[` + cid + `],"match":{"metric":"voltage","op":">","value":1}}`,
		`{"name":"r","on":"detection","channels":[` + cid + `],"typo":1}`,
		`{"name":"","on":"detection","channels":[` + cid + `]}`,
	} {
		if rr := call(h, "POST", "/api/v1/rules", bad); rr.Code != 400 {
			t.Fatalf("accepted rule %s: %d %s", bad, rr.Code, rr.Body)
		}
	}
	rr = call(h, "POST", "/api/v1/rules", `{"name":"Blocked plate","on":"detection","channels":[`+cid+`],"cooldown_s":300,"match":{"type":"vehicle","watchlist":"block"}}`)
	if rr.Code != 201 {
		t.Fatalf("rule: %d %s", rr.Code, rr.Body)
	}
	if rr := call(h, "POST", "/api/v1/rules", `{"name":"Unknown faces","on":"detection","channels":[`+cid+`],"cooldown_s":0,"match":{"type":"face","face":"unknown"}}`); rr.Code != 201 {
		t.Fatalf("rule 2: %d %s", rr.Code, rr.Body)
	}
	call(h, "PATCH", "/api/v1/vehicles/KA01XY9999", `{"watchlist":"block"}`)

	car := func(plate string, track int) {
		post(t, h, map[string]any{"type": "vehicle", "device_id": "gate", "track_id": track, "ts_ms": time.Now().UnixMilli(),
			"vehicle_type": "car", "plate_text": plate, "plate_valid": true}, map[string]string{"frame": "F"})
	}
	car("MH12AB1234", 1) // not on the watchlist: silent
	car("KA01XY9999", 2) // blocked: alert
	car("KA01XY9999", 3) // same plate within the cooldown: silent
	got := s.wait(t, 1)
	if len(got) != 1 {
		t.Fatalf("expected exactly one alert, got %d: %v", len(got), got)
	}
	data := got[0]["data"].(map[string]any)
	if !strings.Contains(data["message"].(string), "BLOCKED vehicle") || !strings.Contains(data["message"].(string), "KA01XY9999") ||
		data["rule"] != "Blocked plate" || data["detection"] == nil {
		t.Fatalf("alert payload: %v", data)
	}

	// an unknown face fires, once the person gets a name the same rule stays quiet
	emb := make([]float32, 8)
	emb[3] = 1
	face := func(track int) map[string]any {
		return post(t, h, map[string]any{"type": "face", "device_id": "lobby", "track_id": track, "ts_ms": time.Now().UnixMilli(),
			"quality": 0.8, "embedding": emb}, map[string]string{"frame": "F", "crop_face": "C"})
	}
	r := face(1)
	got = s.wait(t, 2)
	if len(got) != 2 || !strings.Contains(got[1]["data"].(map[string]any)["message"].(string), "Unknown person") {
		t.Fatalf("unknown face alert: %v", got)
	}
	call(h, "PATCH", "/api/v1/persons/"+itoa(int64(r["person_id"].(float64))), `{"name":"Alice"}`)
	face(2)
	if got = s.wait(t, 3); len(got) != 2 {
		t.Fatalf("a named face must not alert: %v", got)
	}

	// history records both deliveries
	var pg struct {
		Data []struct {
			RuleName  string `json:"rule_name"`
			Delivered int
			Failed    int
		}
	}
	decode(t, call(h, "GET", "/api/v1/alerts", ""), &pg)
	if len(pg.Data) != 2 || pg.Data[0].Delivered != 1 || pg.Data[0].Failed != 0 {
		t.Fatalf("alert history: %+v", pg.Data)
	}

	// a dry run explains itself
	var tr map[string]any
	var pageD Page[Detection]
	decode(t, call(h, "GET", "/api/v1/detections?plate=MH12AB1234", ""), &pageD)
	decode(t, call(h, "POST", "/api/v1/rules/1/test", `{"detection_id":`+itoa(pageD.Data[0].ID)+`}`), &tr)
	if tr["matches"] != false || tr["reason"] != "watchlist differs" {
		t.Fatalf("dry run: %v", tr)
	}
	// a channel in use cannot be deleted
	if rr := call(h, "DELETE", "/api/v1/channels/"+cid, ""); rr.Code != 400 {
		t.Fatalf("deleted a channel that a rule uses: %d", rr.Code)
	}
	if rr := call(h, "POST", "/api/v1/channels/"+cid+"/test", ""); rr.Code != 200 || !strings.Contains(rr.Body.String(), `"ok":true`) {
		t.Fatalf("channel test: %d %s", rr.Code, rr.Body)
	}
}

func TestRuleTimeWindow(t *testing.T) {
	loc, _ := time.LoadLocation("Asia/Kolkata")
	at := func(h, m int) time.Time { return time.Date(2026, 10, 21, h, m, 0, 0, loc) } // a Wednesday
	night := RuleMatch{Hours: "22:00-06:00", Tz: "Asia/Kolkata"}
	for _, c := range []struct {
		t  time.Time
		ok bool
	}{{at(23, 0), true}, {at(2, 30), true}, {at(5, 59), true}, {at(6, 0), false}, {at(12, 0), false}, {at(21, 59), false}, {at(22, 0), true}} {
		if night.timeOK(c.t) != c.ok {
			t.Fatalf("%v: want %v", c.t.Format("15:04"), c.ok)
		}
	}
	// the zone matters: 23:00 Kolkata is 17:30 UTC
	if !night.timeOK(at(23, 0).UTC()) {
		t.Fatal("zone conversion")
	}
	weekend := RuleMatch{Days: []int{0, 6}, Tz: "Asia/Kolkata"}
	if weekend.timeOK(at(12, 0)) || !weekend.timeOK(at(12, 0).AddDate(0, 0, 3)) {
		t.Fatal("days filter")
	}
}

func TestHealthAlerts(t *testing.T) {
	a, h := newTestAppFull(t, "")
	s := &sink{}
	srv := httptest.NewServer(s.handler())
	defer srv.Close()
	var ch Channel
	decode(t, call(h, "POST", "/api/v1/channels", `{"name":"ops","kind":"http","config":{"url":"`+srv.URL+`"}}`), &ch)
	cid := itoa(ch.ID)
	for _, r := range []string{
		`{"name":"offline","on":"device_offline","channels":[` + cid + `]}`,
		`{"name":"back","on":"device_online","channels":[` + cid + `]}`,
		`{"name":"hot","on":"device_health","cooldown_s":600,"channels":[` + cid + `],"match":{"metric":"temp_c","op":">","value":75}}`,
		`{"name":"cma","on":"device_health","cooldown_s":600,"channels":[` + cid + `],"match":{"metric":"cma_pct","op":">","value":90}}`,
	} {
		if rr := call(h, "POST", "/api/v1/rules", r); rr.Code != 201 {
			t.Fatalf("rule: %d %s", rr.Code, rr.Body)
		}
	}
	offlineGrace = 0
	defer func() { offlineGrace = 60 * time.Second }()

	beat := func(body string) {
		if _, err := a.devices.Heartbeat([]byte(body), "10.0.0.5"); err != nil {
			t.Fatal(err)
		}
	}
	beat(`{"device_id":"cam1","profile":"x","temp_c":81.5,"cma_used_kb":95,"cma_total_kb":100}`)
	a.alerts.checkHealth()
	got := s.wait(t, 2)
	if len(got) != 2 {
		t.Fatalf("expected temperature and media-memory alerts: %v", got)
	}
	a.alerts.checkHealth() // cooldown: no repeat
	if got = s.wait(t, 3); len(got) != 2 {
		t.Fatalf("health alert repeated inside the cooldown: %v", got)
	}

	// silence for 2 minutes -> offline once
	a.devices.mu.Lock()
	a.devices.devices["cam1"].LastSeen = time.Now().Add(-2 * time.Minute).UnixMilli()
	a.devices.mu.Unlock()
	a.alerts.checkHealth()
	a.alerts.checkHealth()
	got = s.wait(t, 3)
	if len(got) != 3 || !strings.Contains(got[2]["data"].(map[string]any)["message"].(string), "offline") {
		t.Fatalf("offline alert: %v", got)
	}
	beat(`{"device_id":"cam1","profile":"x","temp_c":40,"cma_used_kb":10,"cma_total_kb":100}`)
	a.alerts.checkHealth()
	got = s.wait(t, 4)
	if len(got) != 4 || !strings.Contains(got[3]["data"].(map[string]any)["message"].(string), "back online") {
		t.Fatalf("online alert: %v", got)
	}
}

func TestReportsAndAttendance(t *testing.T) {
	a, h := newTestAppFull(t, "")
	loc, _ := time.LoadLocation("Asia/Kolkata")
	day := time.Date(2026, 10, 20, 0, 0, 0, 0, loc)
	at := func(hh, mm int) int64 {
		return day.Add(time.Duration(hh)*time.Hour + time.Duration(mm)*time.Minute).UnixMilli()
	}

	alice := a.mustFace(t, at(9, 5), 1, 1)
	a.gallery.Rename(alice, "Alice")
	a.mustFaceAgain(t, alice, at(17, 40), 2, 1)
	bob := a.mustFace(t, at(11, 0), 3, 2) // unnamed
	_ = bob
	seed(t, a, EventRow{TsMs: at(10, 0), DeviceID: "gate", Type: "vehicle", VehicleType: "car", PlateText: "MH12AB1234", TrackID: 5}, nil)
	seed(t, a, EventRow{TsMs: at(10, 30), DeviceID: "gate", Type: "vehicle", VehicleType: "car", PlateText: "MH12AB1234", TrackID: 6}, nil)
	seed(t, a, EventRow{TsMs: at(23, 59), DeviceID: "gate", Type: "vehicle", VehicleType: "truck", PlateText: "KA01XY9999", TrackID: 7}, nil)
	// 00:10 the next local day belongs to the next report day
	seed(t, a, EventRow{TsMs: day.AddDate(0, 0, 1).Add(10 * time.Minute).UnixMilli(), DeviceID: "gate", Type: "vehicle", VehicleType: "car", PlateText: "NEXTDAY", TrackID: 8}, nil)

	var sum struct {
		Totals   map[string]int64 `json:"totals"`
		ByPeriod []periodCount    `json:"by_period"`
		ByType   map[string]int64 `json:"by_vehicle_type"`
		TopPlate []struct {
			Plate     string
			Sightings int64
		} `json:"top_plates"`
		ByDevice map[string]map[string]int64 `json:"by_device"`
	}
	decode(t, call(h, "GET", "/api/v1/reports/summary?from=2026-10-20&tz=Asia/Kolkata", ""), &sum)
	want := map[string]int64{"detections": 6, "faces": 3, "vehicles": 3, "plates_read": 3, "unique_people": 2, "unique_plates": 2, "unnamed_people": 1}
	for k, v := range want {
		if sum.Totals[k] != v {
			t.Fatalf("totals[%s] = %d, want %d (%v)", k, sum.Totals[k], v, sum.Totals)
		}
	}
	if len(sum.ByPeriod) != 24 || sum.ByPeriod[9].Faces != 1 || sum.ByPeriod[10].Vehicles != 2 || sum.ByPeriod[23].Vehicles != 1 {
		t.Fatalf("hourly buckets: %+v", sum.ByPeriod)
	}
	if sum.ByType["car"] != 2 || sum.ByType["truck"] != 1 || sum.TopPlate[0].Plate != "MH12AB1234" || sum.TopPlate[0].Sightings != 2 {
		t.Fatalf("breakdowns: %+v %+v", sum.ByType, sum.TopPlate)
	}
	// two days, grouped by day
	decode(t, call(h, "GET", "/api/v1/reports/summary?from=2026-10-20&to=2026-10-21&tz=Asia/Kolkata&group=day", ""), &sum)
	if len(sum.ByPeriod) != 2 || sum.ByPeriod[0].Vehicles != 3 || sum.ByPeriod[1].Vehicles != 1 {
		t.Fatalf("daily buckets: %+v", sum.ByPeriod)
	}
	for _, bad := range []string{"?tz=Mars/Base", "?from=2026-10-20&to=2026-10-19", "?from=2026-01-01&to=2026-12-31", "?group=week", "?from=banana"} {
		if rr := call(h, "GET", "/api/v1/reports/summary"+bad, ""); rr.Code != 400 {
			t.Fatalf("summary%s -> %d", bad, rr.Code)
		}
	}

	var att struct{ Data []attendanceRow }
	decode(t, call(h, "GET", "/api/v1/reports/attendance?kind=person&from=2026-10-20&tz=Asia/Kolkata&named=true", ""), &att)
	if len(att.Data) != 1 || att.Data[0].Name != "Alice" || att.Data[0].FirstSeen != "2026-10-20T09:05:00+05:30" ||
		att.Data[0].LastSeen != "2026-10-20T17:40:00+05:30" || att.Data[0].DurationMin != 515 || att.Data[0].Sightings != 2 {
		t.Fatalf("attendance: %+v", att.Data)
	}
	decode(t, call(h, "GET", "/api/v1/reports/attendance?kind=vehicle&from=2026-10-20&to=2026-10-21&tz=Asia/Kolkata", ""), &att)
	if len(att.Data) != 4 { // MH12 and KA01 on the 20th, NEXTDAY on the 21st (+ nothing else)
		if len(att.Data) != 3 {
			t.Fatalf("vehicle attendance: %+v", att.Data)
		}
	}
	rr := call(h, "GET", "/api/v1/reports/attendance?kind=person&from=2026-10-20&tz=Asia/Kolkata&format=csv", "")
	if !strings.HasPrefix(rr.Header().Get("Content-Type"), "text/csv") || !strings.Contains(rr.Body.String(), "Alice") {
		t.Fatalf("csv: %s", rr.Body)
	}
}

func (a *App) mustFaceAgain(t *testing.T, person int64, ts int64, track int64, axis int) {
	t.Helper()
	seed(t, a, EventRow{TsMs: ts, DeviceID: "c", Type: "face", TrackID: track, PersonID: person}, map[string]string{"frame": "F", "crop": "C"})
}

func TestTimelineSimilarMergeErase(t *testing.T) {
	a, h := newTestAppFull(t, "")
	base := time.Now().Add(-time.Hour).UnixMilli()
	min := int64(60_000)
	car := func(plate, dev string, off int64, track int64) EventRow {
		return seed(t, a, EventRow{TsMs: base + off*min, DeviceID: dev, Type: "vehicle", VehicleType: "car", PlateText: plate, TrackID: track},
			map[string]string{"frame": "F", "crop": "P", "extra": "V"})
	}
	car("MH12AB1234", "gate", 0, 1)
	car("MH12AB1234", "gate", 1, 2) // within 2 min: same visit
	car("MH12AB1234", "yard", 10, 3)
	car("MH12AB1234", "gate", 30, 4)
	car("MH12A81234", "gate", 40, 5) // B read as 8
	car("ZZ99ZZ9999", "gate", 41, 6)

	var tl struct {
		Subject  map[string]any
		Timeline Timeline
	}
	decode(t, call(h, "GET", "/api/v1/vehicles/mh12-ab1234/timeline", ""), &tl)
	if got := strings.Join(tl.Timeline.Path, ">"); got != "gate>yard>gate" || len(tl.Timeline.Visits) != 3 ||
		tl.Timeline.Visits[0].Sightings != 2 || len(tl.Timeline.Transitions) != 2 || tl.Timeline.Transitions[0].GapS != 540 ||
		tl.Timeline.Visits[0].Images.Plate == "" {
		t.Fatalf("timeline: %+v", tl.Timeline)
	}
	if rr := call(h, "GET", "/api/v1/vehicles/NOPE/timeline", ""); rr.Code != 404 {
		t.Fatalf("unknown plate timeline: %d", rr.Code)
	}

	var sim struct{ Similar []SimilarPlate }
	decode(t, call(h, "GET", "/api/v1/vehicles/MH12AB1234/similar", ""), &sim)
	if len(sim.Similar) != 1 || sim.Similar[0].Plate != "MH12A81234" || sim.Similar[0].Distance != 1 {
		t.Fatalf("similar: %+v", sim.Similar)
	}
	call(h, "PATCH", "/api/v1/vehicles/MH12A81234", `{"owner":"Misread"}`)
	if rr := call(h, "POST", "/api/v1/vehicles/MH12AB1234/merge/MH12A81234", ""); rr.Code != 200 {
		t.Fatalf("merge: %d %s", rr.Code, rr.Body)
	}
	var v VehicleDetail
	decode(t, call(h, "GET", "/api/v1/vehicles/MH12AB1234", ""), &v)
	if v.Sightings != 5 || v.Owner != "Misread" {
		t.Fatalf("after merge: %+v", v.VehicleView)
	}
	if rr := call(h, "GET", "/api/v1/vehicles/MH12A81234", ""); rr.Code != 404 {
		t.Fatalf("merged plate still exists: %d", rr.Code)
	}
	if rr := call(h, "POST", "/api/v1/vehicles/MH12AB1234/merge/MH12AB1234", ""); rr.Code != 400 {
		t.Fatalf("self merge: %d", rr.Code)
	}

	// erase: rows, pictures, label, and for a person also the face samples
	last := car("ERASE1", "gate", 50, 9)
	if rr := call(h, "POST", "/api/v1/vehicles/ERASE1/erase", ""); rr.Code != 200 || !strings.Contains(rr.Body.String(), `"detections_deleted":1`) {
		t.Fatalf("erase vehicle: %d %s", rr.Code, rr.Body)
	}
	if exists(a, last.FrameURL) || exists(a, last.CropURL) {
		t.Fatal("pictures of an erased vehicle remain")
	}
	pid := a.mustFace(t, base, 20, 4)
	var person Person
	person, _ = a.store.GetPerson(pid)
	if rr := call(h, "POST", "/api/v1/people/"+itoa(pid)+"/erase", ""); rr.Code != 200 {
		t.Fatalf("erase person: %d %s", rr.Code, rr.Body)
	}
	if _, err := a.store.GetPerson(pid); err == nil {
		t.Fatal("person still exists")
	}
	var n int
	a.store.db.QueryRow(`SELECT COUNT(*) FROM events WHERE person_id = ?`, pid).Scan(&n)
	a.store.db.QueryRow(`SELECT COUNT(*) FROM person_embeddings WHERE person_id = ?`, pid).Scan(&n)
	if n != 0 || len(a.gallery.persons) != 0 {
		t.Fatalf("leftovers: %d embeddings, %d in memory", n, len(a.gallery.persons))
	}
	_ = person
}

func TestBackup(t *testing.T) {
	a, h := newTestAppFull(t, "")
	seed(t, a, EventRow{TsMs: time.Now().UnixMilli(), DeviceID: "c", Type: "vehicle", PlateText: "BK1"}, map[string]string{"frame": "PIC"})
	for _, withImages := range []bool{false, true} {
		rr := call(h, "GET", "/api/v1/backup?images="+strconv.FormatBool(withImages), "")
		if rr.Code != 200 || rr.Header().Get("Content-Type") != "application/zip" {
			t.Fatalf("backup: %d", rr.Code)
		}
		zr, err := zip.NewReader(bytes.NewReader(rr.Body.Bytes()), int64(rr.Body.Len()))
		if err != nil {
			t.Fatal(err)
		}
		names := map[string]bool{}
		for _, f := range zr.File {
			names[f.Name] = true
		}
		hasImg := false
		for n := range names {
			hasImg = hasImg || strings.HasPrefix(n, "images/")
		}
		if !names["edgeai.db"] || hasImg != withImages {
			t.Fatalf("images=%v: %v", withImages, names)
		}
	}
	if left, _ := filepath.Glob(filepath.Join(a.dataDir, ".backup-*")); len(left) != 0 {
		t.Fatalf("temporary snapshot not removed: %v", left)
	}
}

func TestCleanErrHidesTokens(t *testing.T) {
	err := cleanErr(&url.Error{Op: "Post", URL: "https://api.telegram.org/bot123:SECRET/sendMessage", Err: errors.New("dial tcp: lookup failed")}, "123:SECRET")
	if strings.Contains(err.Error(), "SECRET") || strings.Contains(err.Error(), "telegram") {
		t.Fatalf("leaked: %v", err)
	}
	if !strings.Contains(err.Error(), "lookup failed") {
		t.Fatalf("lost the cause: %v", err)
	}
}
