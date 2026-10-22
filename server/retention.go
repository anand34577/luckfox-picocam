package main

import (
	"encoding/json"
	"io/fs"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// Retention decides what the janitor deletes. Zero means "keep forever" / "no limit".
type Retention struct {
	Days          int     `json:"days"`           // delete unprotected detections older than this
	ProtectedDays int     `json:"protected_days"` // same for protected ones (named people, labelled or watchlisted plates)
	MaxDiskGB     float64 `json:"max_disk_gb"`    // cap for the images folder; oldest unprotected go first
}

// protectedSQL matches events worth keeping longer: faces of named people and plates someone labelled.
const protectedSQL = `(COALESCE(e.person_id IN (SELECT id FROM persons WHERE name <> ''), 0) OR
	COALESCE(e.plate_text IN (SELECT plate FROM vehicle_labels WHERE watchlist <> '' OR owner <> '' OR label <> ''), 0))`

type CleanupResult struct {
	StartedMs      int64  `json:"started_ms"`
	DurationMs     int64  `json:"duration_ms"`
	EventsDeleted  int64  `json:"events_deleted"`
	PersonsDeleted int64  `json:"persons_deleted"`
	BytesFreed     int64  `json:"bytes_freed"`
	Note           string `json:"note,omitempty"`
}

type Janitor struct {
	a    *App
	mu   sync.Mutex // one cleanup at a time
	last *CleanupResult
}

func (s *Store) retention() Retention {
	var r Retention
	if v, ok := s.getSetting("retention"); ok {
		json.Unmarshal([]byte(v), &r)
	}
	return r
}

func (r Retention) valid() bool {
	return r.Days >= 0 && r.Days <= 36500 && r.ProtectedDays >= 0 && r.ProtectedDays <= 36500 &&
		r.MaxDiskGB >= 0 && r.MaxDiskGB <= 1_000_000
}

// Start runs a cleanup shortly after boot and then every hour.
func (j *Janitor) Start() {
	go func() {
		time.Sleep(time.Minute)
		for {
			if r := j.Run(); r.EventsDeleted > 0 || r.PersonsDeleted > 0 {
				log.Printf("cleanup: %d detections, %d persons, %.1f MB freed", r.EventsDeleted, r.PersonsDeleted,
					float64(r.BytesFreed)/1e6)
			}
			time.Sleep(time.Hour)
		}
	}()
}

func (j *Janitor) Run() CleanupResult {
	j.mu.Lock()
	defer j.mu.Unlock()
	t0 := time.Now()
	res := CleanupResult{StartedMs: t0.UnixMilli()}
	cfg := j.a.store.retention()
	now := time.Now().UnixMilli()

	if cfg.Days > 0 {
		cut := now - int64(cfg.Days)*86_400_000
		j.sweep(&res, `e.ts_ms < ? AND NOT `+protectedSQL, []any{cut}, 0, nil)
	}
	if cfg.ProtectedDays > 0 {
		cut := now - int64(cfg.ProtectedDays)*86_400_000
		j.sweep(&res, `e.ts_ms < ? AND `+protectedSQL, []any{cut}, 0, nil)
	}
	if cfg.MaxDiskGB > 0 {
		limit := int64(cfg.MaxDiskGB * 1e9)
		size := j.a.imagesSize()
		if size > limit {
			target := limit * 9 / 10 // free a little extra so the next hour does not trip again
			j.sweep(&res, `NOT `+protectedSQL, nil, size-target, nil)
			if left := j.a.imagesSize(); left > limit {
				res.Note = "images folder is still over the limit; only named people and labelled plates are left to delete"
			}
		}
	}
	// faces that lost every detection and never got a name are noise
	rows, err := j.a.store.db.Query(`SELECT id FROM persons WHERE name = '' AND id NOT IN
		(SELECT DISTINCT person_id FROM events WHERE person_id IS NOT NULL)`)
	if err == nil {
		var ids []int64
		for rows.Next() {
			var id int64
			if rows.Scan(&id) == nil {
				ids = append(ids, id)
			}
		}
		rows.Close()
		for _, id := range ids {
			if j.a.gallery.Delete(id) == nil {
				res.PersonsDeleted++
			}
		}
	}
	j.a.pruneEmptyDirs()
	res.DurationMs = time.Since(t0).Milliseconds()
	j.last = &res
	return res
}

// sweep deletes the oldest matching events in small batches until none are left or, when
// freeBytes > 0, that many bytes of images were removed.
func (j *Janitor) sweep(res *CleanupResult, cond string, args []any, freeBytes int64, _ any) {
	var freed int64
	for {
		rows, err := j.a.store.db.Query(`SELECT e.id, COALESCE(e.frame_path,''), COALESCE(e.crop_path,''),
			COALESCE(e.extra_path,'') FROM events e WHERE `+cond+` ORDER BY e.id LIMIT 300`, args...)
		if err != nil {
			log.Printf("cleanup: %v", err)
			return
		}
		type ev struct {
			id   int64
			urls [3]string
		}
		var batch []ev
		for rows.Next() {
			var e ev
			if rows.Scan(&e.id, &e.urls[0], &e.urls[1], &e.urls[2]) == nil {
				batch = append(batch, e)
			}
		}
		rows.Close()
		if len(batch) == 0 {
			return
		}
		for _, e := range batch {
			if _, err := j.a.store.db.Exec(`DELETE FROM events WHERE id = ?`, e.id); err != nil {
				log.Printf("cleanup: %v", err)
				return
			}
			res.EventsDeleted++
			for _, u := range e.urls {
				n := j.a.removeImageSized(u)
				freed += n
				res.BytesFreed += n
			}
			if freeBytes > 0 && freed >= freeBytes {
				return
			}
		}
		time.Sleep(50 * time.Millisecond) // leave the single database connection to ingest between batches
	}
}

func (a *App) imagesRoot() string {
	root, _ := filepath.Abs(filepath.Join(a.dataDir, "images"))
	return root
}

func (a *App) imagesSize() int64 {
	var n int64
	filepath.WalkDir(a.imagesRoot(), func(_ string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			if fi, err := d.Info(); err == nil {
				n += fi.Size()
			}
		}
		return nil
	})
	return n
}

func (a *App) pruneEmptyDirs() {
	root := a.imagesRoot()
	ents, _ := os.ReadDir(root)
	for _, e := range ents {
		if e.IsDir() {
			os.Remove(filepath.Join(root, e.Name())) // fails (and is ignored) unless the folder is empty
		}
	}
}

// removeImageSized is removeImage that reports how many bytes it freed.
func (a *App) removeImageSized(url string) int64 {
	rel := strings.TrimPrefix(url, "/images/")
	if rel == "" || rel == url {
		return 0
	}
	var n int
	a.store.db.QueryRow(`SELECT (SELECT COUNT(*) FROM persons WHERE cover_path = ?) +
		(SELECT COUNT(*) FROM events WHERE frame_path = ? OR crop_path = ? OR extra_path = ?)`, url, url, url, url).Scan(&n)
	if n > 0 {
		return 0
	}
	root := a.imagesRoot()
	p, _ := filepath.Abs(filepath.Join(root, filepath.FromSlash(rel)))
	if !strings.HasPrefix(p, root+string(filepath.Separator)) {
		return 0
	}
	fi, err := os.Stat(p)
	if err != nil || os.Remove(p) != nil {
		return 0
	}
	return fi.Size()
}

// ------------------------------------------------------------------ endpoints

func (a *App) getRetention(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, a.store.retention())
}

func (a *App) putRetention(w http.ResponseWriter, r *http.Request) {
	var cfg Retention
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4<<10))
	dec.DisallowUnknownFields()
	if dec.Decode(&cfg) != nil || !cfg.valid() {
		apiError(w, 400, "bad_request", "body: {days, protected_days, max_disk_gb}, all >= 0 (0 = keep forever / no limit)")
		return
	}
	b, _ := json.Marshal(cfg)
	if err := a.store.setSetting("retention", string(b)); err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	writeJSON(w, cfg)
}

func (a *App) runMaintenance(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, a.janitor.Run())
}

func (a *App) maintenanceStatus(w http.ResponseWriter, r *http.Request) {
	free, total := diskSpace(a.imagesRoot())
	out := map[string]any{"retention": a.store.retention(), "images_bytes": a.imagesSize(),
		"disk_free_bytes": free, "disk_total_bytes": total, "last_cleanup": a.janitor.last}
	var n int64
	a.store.db.QueryRow(`SELECT COUNT(*) FROM events`).Scan(&n)
	out["detections"] = n
	writeJSON(w, out)
}
