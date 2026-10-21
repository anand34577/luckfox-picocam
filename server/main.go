// edgeai-server: collects events from Luckfox Pico edge devices and serves the dashboard.
//
//	edgeai-server -addr :8080 -data ./data -web ./web -mediamtx http://127.0.0.1:9997
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

var errNotFound = errors.New("not found")

type App struct {
	store      *Store
	gallery    *Gallery
	devices    *Devices
	hub        *Hub
	dataDir    string
	webrtcPort string
}

func main() {
	addr := flag.String("addr", ":8080", "listen address")
	dataDir := flag.String("data", "./data", "database and image directory")
	webDir := flag.String("web", "./web", "built dashboard (web/dist)")
	mtx := flag.String("mediamtx", "http://127.0.0.1:9997", "MediaMTX API for live video (empty to disable)")
	webrtcPort := flag.String("webrtc-port", "8889", "MediaMTX WebRTC port the browser connects to")
	faceThr := flag.Float64("face-threshold", 0.55, "cosine similarity needed to treat two faces as the same person")
	flag.Parse()

	if err := os.MkdirAll(filepath.Join(*dataDir, "images"), 0o755); err != nil {
		log.Fatal(err)
	}
	store, err := OpenStore(filepath.Join(*dataDir, "edgeai.db"))
	if err != nil {
		log.Fatal(err)
	}
	gallery, err := NewGallery(store, *faceThr)
	if err != nil {
		log.Fatal(err)
	}
	app := &App{store: store, gallery: gallery, devices: NewDevices(store, *mtx), hub: NewHub(), dataDir: *dataDir,
		webrtcPort: *webrtcPort}

	mux := http.NewServeMux()
	// device API
	mux.HandleFunc("POST /api/v1/events", app.postEvent)
	mux.HandleFunc("POST /api/v1/heartbeat", app.postHeartbeat)
	// dashboard API
	mux.HandleFunc("GET /api/v1/config", app.getConfig)
	mux.HandleFunc("GET /api/v1/events", app.getEvents)
	mux.HandleFunc("GET /api/v1/devices", func(w http.ResponseWriter, r *http.Request) { writeJSON(w, app.devices.List()) })
	mux.HandleFunc("GET /api/v1/persons", app.getPersons)
	mux.HandleFunc("PATCH /api/v1/persons/{id}", app.patchPerson)
	mux.HandleFunc("DELETE /api/v1/persons/{id}", app.deletePerson)
	mux.HandleFunc("POST /api/v1/persons/{id}/merge/{from}", app.mergePerson)
	mux.HandleFunc("GET /api/v1/stats", app.getStats)
	mux.HandleFunc("GET /api/v1/ws", app.hub.ServeWS)
	mux.Handle("GET /images/", http.StripPrefix("/images/", http.FileServer(http.Dir(filepath.Join(*dataDir, "images")))))
	mux.Handle("GET /", spa(*webDir))

	log.Printf("edgeai-server on %s (data %s)", *addr, *dataDir)
	log.Fatal(http.ListenAndServe(*addr, logRequests(mux)))
}

// ------------------------------------------------------------------ device endpoints

func (a *App) postEvent(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(8 << 20); err != nil {
		http.Error(w, "bad multipart: "+err.Error(), http.StatusBadRequest)
		return
	}
	var meta struct {
		Type        string    `json:"type"`
		DeviceID    string    `json:"device_id"`
		TrackID     int64     `json:"track_id"`
		TsMs        int64     `json:"ts_ms"`
		VehicleType string    `json:"vehicle_type"`
		PlateText   string    `json:"plate_text"`
		PlateValid  bool      `json:"plate_valid"`
		PlateScore  float64   `json:"plate_score"`
		Quality     float64   `json:"quality"`
		Embedding   []float32 `json:"embedding"`
	}
	rawMeta := r.FormValue("meta")
	if err := json.Unmarshal([]byte(rawMeta), &meta); err != nil || meta.DeviceID == "" {
		http.Error(w, "bad meta", http.StatusBadRequest)
		return
	}
	now := time.Now().UnixMilli()
	if meta.TsMs < 1_600_000_000_000 || meta.TsMs > now+60_000 {
		meta.TsMs = now // device clock not synced yet
	}

	ev := &EventRow{TsMs: meta.TsMs, DeviceID: meta.DeviceID, Type: meta.Type, TrackID: meta.TrackID,
		VehicleType: meta.VehicleType, PlateText: meta.PlateText, PlateValid: meta.PlateValid,
		PlateScore: meta.PlateScore, Quality: meta.Quality}
	// keep the embedding out of the stored/broadcast meta (large, not useful in the UI)
	var m map[string]any
	json.Unmarshal([]byte(rawMeta), &m)
	delete(m, "embedding")
	ev.Meta, _ = json.Marshal(m)

	stamp := fmt.Sprintf("%s/%s_%d_%d", time.UnixMilli(meta.TsMs).Format("2006-01-02"), safeName(meta.DeviceID),
		meta.TsMs, meta.TrackID)
	ev.FrameURL = a.saveImage(r, "frame", stamp+"_frame.jpg")
	if meta.Type == "face" {
		ev.CropURL = a.saveImage(r, "crop_face", stamp+"_face.jpg")
	} else {
		ev.CropURL = a.saveImage(r, "crop_plate", stamp+"_plate.jpg")
		ev.ExtraURL = a.saveImage(r, "crop_vehicle", stamp+"_vehicle.jpg")
	}

	resp := map[string]any{"ok": true, "server_time_ms": time.Now().UnixMilli()}
	if meta.Type == "face" && len(meta.Embedding) > 0 {
		mr, err := a.gallery.Match(meta.DeviceID, meta.TrackID, meta.Embedding, meta.Quality, meta.TsMs, ev.CropURL)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		ev.PersonID, ev.PersonName = mr.PersonID, mr.Name
		resp["person_id"], resp["name"], resp["is_new"], resp["similarity"] = mr.PersonID, mr.Name, mr.IsNew, mr.Similarity
	}
	if err := a.store.InsertEvent(ev); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	resp["event_id"] = ev.ID
	a.hub.Broadcast("event", ev)
	writeJSON(w, resp)
}

func (a *App) saveImage(r *http.Request, field, rel string) string {
	f, _, err := r.FormFile(field)
	if err != nil {
		return ""
	}
	defer f.Close()
	path := filepath.Join(a.dataDir, "images", filepath.FromSlash(rel))
	os.MkdirAll(filepath.Dir(path), 0o755)
	out, err := os.Create(path)
	if err != nil {
		log.Printf("save %s: %v", rel, err)
		return ""
	}
	defer out.Close()
	io.Copy(out, f)
	return "/images/" + rel
}

func (a *App) postHeartbeat(w http.ResponseWriter, r *http.Request) {
	body, _ := io.ReadAll(io.LimitReader(r.Body, 64<<10))
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	dev, err := a.devices.Heartbeat(body, host)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	a.hub.Broadcast("device", dev)
	writeJSON(w, map[string]any{"ok": true, "server_time_ms": time.Now().UnixMilli()})
}

// ------------------------------------------------------------------ dashboard endpoints

func (a *App) getConfig(w http.ResponseWriter, r *http.Request) {
	host := r.Host
	if h, _, err := net.SplitHostPort(r.Host); err == nil {
		host = h
	}
	writeJSON(w, map[string]any{"webrtc_base": "http://" + host + ":" + a.webrtcPort})
}

func (a *App) getEvents(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	pid, _ := strconv.ParseInt(q.Get("person_id"), 10, 64)
	before, _ := strconv.ParseInt(q.Get("before"), 10, 64)
	limit, _ := strconv.Atoi(q.Get("limit"))
	rows, err := a.store.ListEvents(EventQuery{Type: q.Get("type"), Device: q.Get("device"), Plate: q.Get("plate"),
		PersonID: pid, Before: before, Limit: limit})
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, rows)
}

func (a *App) getPersons(w http.ResponseWriter, r *http.Request) {
	p, err := a.store.ListPersons(strings.TrimSpace(r.URL.Query().Get("q")))
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, p)
}

func (a *App) patchPerson(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.ParseInt(r.PathValue("id"), 10, 64)
	var body struct {
		Name string `json:"name"`
	}
	if json.NewDecoder(r.Body).Decode(&body) != nil {
		http.Error(w, "bad body", http.StatusBadRequest)
		return
	}
	if err := a.gallery.Rename(id, strings.TrimSpace(body.Name)); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (a *App) deletePerson(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if err := a.gallery.Delete(id); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (a *App) mergePerson(w http.ResponseWriter, r *http.Request) {
	into, _ := strconv.ParseInt(r.PathValue("id"), 10, 64)
	from, _ := strconv.ParseInt(r.PathValue("from"), 10, 64)
	if err := a.gallery.Merge(into, from); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

func (a *App) getStats(w http.ResponseWriter, r *http.Request) {
	y, m, d := time.Now().Date()
	st, err := a.store.Stats(time.Date(y, m, d, 0, 0, 0, 0, time.Local).UnixMilli())
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, st)
}

// ------------------------------------------------------------------ helpers

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func safeName(s string) string {
	return strings.Map(func(r rune) rune {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' {
			return r
		}
		return '_'
	}, s)
}

// spa serves the built React app, falling back to index.html for client-side routes.
func spa(dir string) http.Handler {
	fs := http.FileServer(http.Dir(dir))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := filepath.Join(dir, filepath.FromSlash(filepath.Clean("/"+r.URL.Path)))
		if st, err := os.Stat(p); err != nil || st.IsDir() && r.URL.Path != "/" {
			http.ServeFile(w, r, filepath.Join(dir, "index.html"))
			return
		}
		fs.ServeHTTP(w, r)
	})
}

func logRequests(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/v1/events" && r.Method == http.MethodPost {
			log.Printf("event from %s (%s)", r.Header.Get("X-Device-Id"), r.RemoteAddr)
		}
		h.ServeHTTP(w, r)
	})
}
