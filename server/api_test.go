package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

func newTestApp(t *testing.T, apiKey string) http.Handler {
	_, h := newTestAppFull(t, apiKey)
	return h
}

func newTestAppFull(t *testing.T, apiKey string) (*App, http.Handler) {
	t.Helper()
	dir := t.TempDir()
	os.MkdirAll(filepath.Join(dir, "images"), 0o755)
	store, err := OpenStore(filepath.Join(dir, "t.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { store.db.Close() })
	g, _ := NewGallery(store, 0.55)
	a := &App{store: store, gallery: g, devices: NewDevices(store, ""), hub: NewHub(), dataDir: dir, apiKey: apiKey}
	a.initAccess()
	mux := a.routes(dir)
	return a, a.withAccess(mux, "")
}

func post(t *testing.T, h http.Handler, meta map[string]any, files map[string]string) map[string]any {
	t.Helper()
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	b, _ := json.Marshal(meta)
	mw.WriteField("meta", string(b))
	for field, content := range files {
		fw, _ := mw.CreateFormFile(field, field+".jpg")
		fw.Write([]byte(content))
	}
	mw.Close()
	req := httptest.NewRequest("POST", "/api/v1/events", &buf)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	if rr.Code != 200 {
		t.Fatalf("post event: %d %s", rr.Code, rr.Body)
	}
	var out map[string]any
	json.Unmarshal(rr.Body.Bytes(), &out)
	return out
}

func call(h http.Handler, method, path, body string, hdr ...string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, path, strings.NewReader(body))
	req.Host = "cams.test:8090"
	for i := 0; i+1 < len(hdr); i += 2 {
		req.Header.Set(hdr[i], hdr[i+1])
	}
	rr := httptest.NewRecorder()
	h.ServeHTTP(rr, req)
	return rr
}

func decode(t *testing.T, rr *httptest.ResponseRecorder, v any) {
	t.Helper()
	if err := json.Unmarshal(rr.Body.Bytes(), v); err != nil {
		t.Fatalf("bad json %q: %v", rr.Body, err)
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }

func TestVehicleAndPersonFlow(t *testing.T) {
	h := newTestApp(t, "")
	now := time.Now().UnixMilli()
	for i, plate := range []string{"MH12AB1234", "MH12AB1234", "KA01XY9999"} {
		post(t, h, map[string]any{"type": "vehicle", "device_id": "cam1", "track_id": i + 1, "ts_ms": now + int64(i),
			"vehicle_type": "car", "vehicle_conf": 0.9, "plate_text": plate, "plate_valid": true, "plate_score": 0.8},
			map[string]string{"frame": "F", "crop_plate": "P", "crop_vehicle": "V"})
	}
	post(t, h, map[string]any{"type": "vehicle", "device_id": "cam1", "track_id": 9, "ts_ms": now + 5, "vehicle_type": "truck"},
		map[string]string{"frame": "F"})
	emb := make([]float32, 8)
	emb[0] = 1
	r := post(t, h, map[string]any{"type": "face", "device_id": "cam2", "track_id": 1, "ts_ms": now, "quality": 0.7,
		"embedding": emb}, map[string]string{"frame": "F", "crop_face": "FACE"})
	pid := int64(r["person_id"].(float64))

	// a vehicle detection has all three images and a plate object
	var page Page[Detection]
	decode(t, call(h, "GET", "/api/v1/detections?type=vehicle&plate=mh-12%20ab1234", ""), &page)
	if page.Total != 2 || len(page.Data) != 2 {
		t.Fatalf("plate filter: total %d len %d", page.Total, len(page.Data))
	}
	d := page.Data[0]
	if d.Vehicle == nil || d.Vehicle.Plate == nil || d.Vehicle.Plate.Text != "MH12AB1234" || d.Vehicle.Confidence != 0.9 {
		t.Fatalf("vehicle ref: %+v", d.Vehicle)
	}
	if !strings.HasPrefix(d.Images.Plate, "http://cams.test:8090/images/") || d.Images.Full == "" || d.Images.Vehicle == "" {
		t.Fatalf("images: %+v", d.Images)
	}
	// the absolute URL really serves the crop
	rr := call(h, "GET", strings.TrimPrefix(d.Images.Plate, "http://cams.test:8090"), "")
	if body, _ := io.ReadAll(rr.Body); string(body) != "P" {
		t.Fatalf("plate image body %q", body)
	}

	// cursor paging
	decode(t, call(h, "GET", "/api/v1/detections?limit=2", ""), &page)
	if page.NextCursor == nil || len(page.Data) != 2 {
		t.Fatalf("expected a next cursor: %+v", page)
	}
	var page2 Page[Detection]
	decode(t, call(h, "GET", "/api/v1/detections?limit=100&cursor="+itoa(*page.NextCursor), ""), &page2)
	if page2.NextCursor != nil || len(page2.Data) != 3 {
		t.Fatalf("second page: %d next=%v", len(page2.Data), page2.NextCursor)
	}

	// vehicles aggregate by plate; the plate-less truck is excluded
	var vp Page[VehicleView]
	decode(t, call(h, "GET", "/api/v1/vehicles", ""), &vp)
	if vp.Total != 2 || len(vp.Data) != 2 {
		t.Fatalf("vehicles: %+v", vp)
	}
	var v VehicleDetail
	decode(t, call(h, "GET", "/api/v1/vehicles/mh12-ab%201234", ""), &v)
	if v.Sightings != 2 || len(v.Recent) != 2 || v.Images.Plate == "" || v.Devices[0].DeviceID != "cam1" {
		t.Fatalf("vehicle detail: %+v", v)
	}
	if rr := call(h, "GET", "/api/v1/vehicles/ZZ99ZZ9999", ""); rr.Code != 404 {
		t.Fatalf("unknown plate: %d", rr.Code)
	}

	// labels: partial update, validation, and they show up on detections
	if rr := call(h, "PATCH", "/api/v1/vehicles/MH12AB1234", `{"owner":"Alice","watchlist":"block"}`); rr.Code != 200 {
		t.Fatalf("patch: %d %s", rr.Code, rr.Body)
	}
	call(h, "PATCH", "/api/v1/vehicles/MH12AB1234", `{"label":"Van"}`)
	if rr := call(h, "PATCH", "/api/v1/vehicles/MH12AB1234", `{"watchlist":"nope"}`); rr.Code != 400 {
		t.Fatalf("bad watchlist accepted: %d", rr.Code)
	}
	decode(t, call(h, "GET", "/api/v1/vehicles?watchlist=block", ""), &vp)
	if len(vp.Data) != 1 || vp.Data[0].Owner != "Alice" || vp.Data[0].Label != "Van" {
		t.Fatalf("watchlist filter: %+v", vp.Data)
	}
	decode(t, call(h, "GET", "/api/v1/detections/"+itoa(d.ID), ""), &d)
	if d.Vehicle.Watchlist != "block" || d.Vehicle.Owner != "Alice" {
		t.Fatalf("label on detection: %+v", d.Vehicle)
	}

	// people: unnamed, then named, with face crop and full frame
	var people Page[PersonView]
	decode(t, call(h, "GET", "/api/v1/people?named=false", ""), &people)
	if len(people.Data) != 1 || people.Data[0].Named || people.Data[0].Images.Face == "" {
		t.Fatalf("people: %+v", people)
	}
	call(h, "PATCH", "/api/v1/persons/"+itoa(pid), `{"name":"Alice Johnson"}`)
	var pd PersonDetail
	decode(t, call(h, "GET", "/api/v1/people/"+itoa(pid), ""), &pd)
	if !pd.Named || pd.Name != "Alice Johnson" || pd.Detections != 1 || len(pd.Recent) != 1 ||
		pd.Recent[0].Images.Full == "" || pd.Recent[0].Images.Face == "" || pd.Recent[0].Person.Name != "Alice Johnson" {
		t.Fatalf("person detail: %+v", pd)
	}
	decode(t, call(h, "GET", "/api/v1/detections?named=true", ""), &page)
	if len(page.Data) != 1 || page.Data[0].Type != "face" {
		t.Fatalf("named filter: %+v", page.Data)
	}

	// validation
	for _, p := range []string{"/api/v1/detections?type=bike", "/api/v1/detections?from=yesterday", "/api/v1/detections?person_id=-1"} {
		if rr := call(h, "GET", p, ""); rr.Code != 400 {
			t.Fatalf("%s -> %d", p, rr.Code)
		}
	}
	if rr := call(h, "GET", "/api/v1/detections/99999", ""); rr.Code != 404 {
		t.Fatalf("missing detection: %d", rr.Code)
	}

	// csv: header + 3 plated vehicles + the plate-less truck
	rr = call(h, "GET", "/api/v1/export/detections.csv?type=vehicle", "")
	if lines := strings.Split(strings.TrimSpace(rr.Body.String()), "\n"); len(lines) != 5 || !strings.Contains(lines[1], "http://cams.test:8090/images/") {
		t.Fatalf("csv:\n%s", rr.Body)
	}

	// delete removes the row
	if rr := call(h, "DELETE", "/api/v1/detections/"+itoa(d.ID), ""); rr.Code != 200 {
		t.Fatalf("delete: %d", rr.Code)
	}
	if rr := call(h, "GET", "/api/v1/detections/"+itoa(d.ID), ""); rr.Code != 404 {
		t.Fatalf("deleted detection still there")
	}
}

func TestAPIKey(t *testing.T) {
	h := newTestApp(t, "s3cret")
	if rr := call(h, "GET", "/api/v1/detections", ""); rr.Code != 401 {
		t.Fatalf("no key: %d", rr.Code)
	}
	if rr := call(h, "GET", "/api/v1/detections", "", "Authorization", "Bearer wrong"); rr.Code != 401 {
		t.Fatalf("wrong key: %d", rr.Code)
	}
	for _, hdr := range [][]string{{"Authorization", "Bearer s3cret"}, {"X-API-Key", "s3cret"}} {
		if rr := call(h, "GET", "/api/v1/detections", "", hdr...); rr.Code != 200 {
			t.Fatalf("%v: %d", hdr, rr.Code)
		}
	}
	if rr := call(h, "GET", "/api/v1/detections?api_key=s3cret", ""); rr.Code != 200 {
		t.Fatalf("query key: %d", rr.Code)
	}
	if rr := call(h, "GET", "/images/x.jpg", ""); rr.Code != 401 {
		t.Fatalf("images must need the key: %d", rr.Code)
	}
	if rr := call(h, "GET", "/api/v1/health", ""); rr.Code != 200 {
		t.Fatalf("health must be open: %d", rr.Code)
	}
	// devices keep posting without a key
	post(t, h, map[string]any{"type": "vehicle", "device_id": "cam1", "ts_ms": time.Now().UnixMilli(), "plate_text": "AB12CD3456"}, nil)
}

func TestWebhookDelivery(t *testing.T) {
	h := newTestApp(t, "")
	got := make(chan []byte, 4)
	sig := make(chan string, 4)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		sig <- r.Header.Get("X-EdgeAI-Signature")
		got <- b
	}))
	defer srv.Close()
	if rr := call(h, "POST", "/api/v1/webhooks", `{"url":"`+srv.URL+`","types":["vehicle"],"secret":"k"}`); rr.Code != 201 {
		t.Fatalf("create: %d %s", rr.Code, rr.Body)
	}
	if rr := call(h, "POST", "/api/v1/webhooks", `{"url":"ftp://x"}`); rr.Code != 400 {
		t.Fatalf("bad url accepted")
	}
	post(t, h, map[string]any{"type": "face", "device_id": "c", "ts_ms": time.Now().UnixMilli(), "embedding": []float32{1, 0}}, nil)
	post(t, h, map[string]any{"type": "vehicle", "device_id": "c", "ts_ms": time.Now().UnixMilli(), "plate_text": "MH12AB1234"}, nil)
	select {
	case body := <-got:
		mac := hmac.New(sha256.New, []byte("k"))
		mac.Write(body)
		if want := "sha256=" + hex.EncodeToString(mac.Sum(nil)); <-sig != want {
			t.Fatal("bad signature")
		}
		var m struct {
			Event string
			Data  Detection
		}
		json.Unmarshal(body, &m)
		if m.Event != "detection.created" || m.Data.Type != "vehicle" || m.Data.Vehicle.Plate.Text != "MH12AB1234" {
			t.Fatalf("payload: %s", body)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("no delivery")
	}
	select {
	case b := <-got:
		t.Fatalf("second delivery (face leaked through the vehicle-only filter?): %s", b)
	case <-time.After(300 * time.Millisecond):
	}
}
