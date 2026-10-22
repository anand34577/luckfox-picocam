package main

import (
	"archive/zip"
	"database/sql"
	"encoding/csv"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
	_ "time/tzdata" // so IANA zone names work on Windows and minimal Linux images
)

// ------------------------------------------------------------------ time ranges

type reportRange struct {
	From, To time.Time
	Loc      *time.Location
}

const maxReportDays = 93

// parseRange reads from/to/tz. Default: today in the zone. A date-only "2026-10-22" means that local day.
func parseRange(r *http.Request) (reportRange, string) {
	v := r.URL.Query()
	loc := time.Local
	if tz := v.Get("tz"); tz != "" {
		l, err := time.LoadLocation(tz)
		if err != nil {
			return reportRange{}, "tz must be an IANA name such as Asia/Kolkata"
		}
		loc = l
	}
	parse := func(s string) (time.Time, bool, bool) { // time, isDateOnly, ok
		if t, err := time.ParseInLocation("2006-01-02", s, loc); err == nil {
			return t, true, true
		}
		if ms, ok := parseTime(s); ok && ms > 0 {
			return time.UnixMilli(ms).In(loc), false, true
		}
		return time.Time{}, false, false
	}
	y, m, d := time.Now().In(loc).Date()
	rg := reportRange{Loc: loc, From: time.Date(y, m, d, 0, 0, 0, 0, loc)}
	rg.To = rg.From.AddDate(0, 0, 1)
	if s := v.Get("from"); s != "" {
		t, _, ok := parse(s)
		if !ok {
			return rg, "from must be YYYY-MM-DD, epoch or RFC 3339"
		}
		rg.From = t
		rg.To = time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, loc).AddDate(0, 0, 1)
		if s := v.Get("to"); s == "" {
			if _, dateOnly, _ := parse(v.Get("from")); !dateOnly {
				rg.To = t.Add(24 * time.Hour)
			}
		}
	}
	if s := v.Get("to"); s != "" {
		t, dateOnly, ok := parse(s)
		if !ok {
			return rg, "to must be YYYY-MM-DD, epoch or RFC 3339"
		}
		if dateOnly {
			t = t.AddDate(0, 0, 1) // "to=2026-10-22" includes that whole day
		}
		rg.To = t
	}
	if !rg.To.After(rg.From) {
		return rg, "to must be after from"
	}
	if rg.To.Sub(rg.From) > maxReportDays*24*time.Hour+time.Hour {
		return rg, fmt.Sprintf("range is limited to %d days", maxReportDays)
	}
	return rg, ""
}

// days splits the range into local calendar days (DST-safe: each day is [midnight, next midnight)).
func (rg reportRange) days() [][2]time.Time {
	var out [][2]time.Time
	d := time.Date(rg.From.Year(), rg.From.Month(), rg.From.Day(), 0, 0, 0, 0, rg.Loc)
	for d.Before(rg.To) {
		next := d.AddDate(0, 0, 1)
		lo, hi := d, next
		if lo.Before(rg.From) {
			lo = rg.From
		}
		if hi.After(rg.To) {
			hi = rg.To
		}
		out = append(out, [2]time.Time{lo, hi})
		d = next
	}
	return out
}

// ------------------------------------------------------------------ summary report

type periodCount struct {
	Period   string `json:"period"`
	Faces    int64  `json:"faces"`
	Vehicles int64  `json:"vehicles"`
}

func (a *App) reportSummary(w http.ResponseWriter, r *http.Request) {
	rg, bad := parseRange(r)
	if bad != "" {
		apiError(w, 400, "bad_request", bad)
		return
	}
	group := r.URL.Query().Get("group")
	if group == "" {
		group = "day"
		if rg.To.Sub(rg.From) <= 48*time.Hour {
			group = "hour"
		}
	}
	if group != "hour" && group != "day" {
		apiError(w, 400, "bad_request", "group must be hour or day")
		return
	}
	if group == "hour" && rg.To.Sub(rg.From) > 7*24*time.Hour+time.Hour {
		apiError(w, 400, "bad_request", "group=hour is limited to 7 days; use group=day")
		return
	}
	lo, hi := rg.From.UnixMilli(), rg.To.UnixMilli()
	db := a.store.db
	out := map[string]any{"range": map[string]any{"from": rg.From.Format(time.RFC3339), "to": rg.To.Format(time.RFC3339),
		"tz": rg.Loc.String(), "group": group}}

	var faces, vehicles, plates, uniqPeople, newPeople, unnamed, uniqPlates int64
	db.QueryRow(`SELECT COALESCE(SUM(type='face'),0), COALESCE(SUM(type='vehicle'),0),
		COALESCE(SUM(type='vehicle' AND plate_text <> ''),0), COUNT(DISTINCT person_id), COUNT(DISTINCT NULLIF(plate_text,''))
		FROM events WHERE ts_ms >= ? AND ts_ms < ?`, lo, hi).Scan(&faces, &vehicles, &plates, &uniqPeople, &uniqPlates)
	db.QueryRow(`SELECT COUNT(*) FROM persons WHERE first_seen >= ? AND first_seen < ?`, lo, hi).Scan(&newPeople)
	db.QueryRow(`SELECT COUNT(DISTINCT e.person_id) FROM events e JOIN persons p ON p.id = e.person_id
		WHERE e.ts_ms >= ? AND e.ts_ms < ? AND p.name = ''`, lo, hi).Scan(&unnamed)
	out["totals"] = map[string]int64{"detections": faces + vehicles, "faces": faces, "vehicles": vehicles,
		"plates_read": plates, "unique_people": uniqPeople, "new_people": newPeople, "unnamed_people": unnamed,
		"unique_plates": uniqPlates}

	var periods []periodCount
	step := func(t time.Time) time.Time { return t.AddDate(0, 0, 1) }
	label := "2006-01-02"
	start := time.Date(rg.From.Year(), rg.From.Month(), rg.From.Day(), 0, 0, 0, 0, rg.Loc)
	if group == "hour" {
		step = func(t time.Time) time.Time { return t.Add(time.Hour) }
		label = "2006-01-02T15:00"
		start = rg.From.Truncate(time.Hour)
		if _, off := rg.From.Zone(); off%3600 != 0 { // half-hour zones: align to the local hour
			start = time.Date(rg.From.Year(), rg.From.Month(), rg.From.Day(), rg.From.Hour(), 0, 0, 0, rg.Loc)
		}
	}
	for t := start; t.Before(rg.To); t = step(t) {
		a1, b1 := t, step(t)
		if a1.Before(rg.From) {
			a1 = rg.From
		}
		if b1.After(rg.To) {
			b1 = rg.To
		}
		var f, v int64
		db.QueryRow(`SELECT COALESCE(SUM(type='face'),0), COALESCE(SUM(type='vehicle'),0) FROM events
			WHERE ts_ms >= ? AND ts_ms < ?`, a1.UnixMilli(), b1.UnixMilli()).Scan(&f, &v)
		periods = append(periods, periodCount{t.Format(label), f, v})
	}
	out["by_period"] = periods

	byType := map[string]int64{}
	if rows, err := db.Query(`SELECT COALESCE(NULLIF(vehicle_type,''),'unknown'), COUNT(*) FROM events
		WHERE type='vehicle' AND ts_ms >= ? AND ts_ms < ? GROUP BY 1`, lo, hi); err == nil {
		for rows.Next() {
			var k string
			var n int64
			rows.Scan(&k, &n)
			byType[k] = n
		}
		rows.Close()
	}
	out["by_vehicle_type"] = byType
	byDev := map[string]map[string]int64{}
	if rows, err := db.Query(`SELECT device_id, COALESCE(SUM(type='face'),0), COALESCE(SUM(type='vehicle'),0) FROM events
		WHERE ts_ms >= ? AND ts_ms < ? GROUP BY device_id`, lo, hi); err == nil {
		for rows.Next() {
			var k string
			var f, v int64
			rows.Scan(&k, &f, &v)
			byDev[k] = map[string]int64{"faces": f, "vehicles": v}
		}
		rows.Close()
	}
	out["by_device"] = byDev

	type topPlate struct {
		Plate     string `json:"plate"`
		Sightings int64  `json:"sightings"`
		Owner     string `json:"owner,omitempty"`
		Label     string `json:"label,omitempty"`
		Watchlist string `json:"watchlist,omitempty"`
	}
	tops := []topPlate{}
	if rows, err := db.Query(`SELECT plate_text, COUNT(*) FROM events WHERE type='vehicle' AND plate_text <> ''
		AND ts_ms >= ? AND ts_ms < ? GROUP BY plate_text ORDER BY COUNT(*) DESC, plate_text LIMIT 10`, lo, hi); err == nil {
		for rows.Next() {
			var t topPlate
			rows.Scan(&t.Plate, &t.Sightings)
			tops = append(tops, t)
		}
		rows.Close()
	}
	pl := make([]string, len(tops))
	for i, t := range tops {
		pl[i] = t.Plate
	}
	lb := a.store.labels(pl)
	for i := range tops {
		l := lb[tops[i].Plate]
		tops[i].Owner, tops[i].Label, tops[i].Watchlist = l.Owner, l.Label, l.Watchlist
	}
	out["top_plates"] = tops
	writeJSON(w, out)
}

// ------------------------------------------------------------------ attendance (first in / last seen per day)

type attendanceRow struct {
	Date        string   `json:"date"`
	Kind        string   `json:"kind"`
	ID          int64    `json:"person_id,omitempty"`
	Name        string   `json:"name,omitempty"`
	Plate       string   `json:"plate,omitempty"`
	Label       string   `json:"label,omitempty"`
	FirstSeen   string   `json:"first_seen"`
	LastSeen    string   `json:"last_seen"`
	DurationMin int64    `json:"duration_min"`
	Sightings   int64    `json:"sightings"`
	Devices     []string `json:"devices"`
}

func (a *App) reportAttendance(w http.ResponseWriter, r *http.Request) {
	rg, bad := parseRange(r)
	if bad != "" {
		apiError(w, 400, "bad_request", bad)
		return
	}
	kind := r.URL.Query().Get("kind")
	if kind == "" {
		kind = "person"
	}
	if kind != "person" && kind != "vehicle" {
		apiError(w, 400, "bad_request", "kind must be person or vehicle")
		return
	}
	namedOnly := truthy(r.URL.Query().Get("named"))
	var out []attendanceRow
	for _, day := range rg.days() {
		date := day[0].In(rg.Loc).Format("2006-01-02")
		var rows *sql.Rows
		var err error
		if kind == "person" {
			q := `SELECT e.person_id, p.name, MIN(e.ts_ms), MAX(e.ts_ms), COUNT(*), GROUP_CONCAT(DISTINCT e.device_id)
				FROM events e JOIN persons p ON p.id = e.person_id WHERE e.type='face' AND e.ts_ms >= ? AND e.ts_ms < ?`
			if namedOnly {
				q += ` AND p.name <> ''`
			}
			rows, err = a.store.db.Query(q+` GROUP BY e.person_id ORDER BY MIN(e.ts_ms)`, day[0].UnixMilli(), day[1].UnixMilli())
		} else {
			rows, err = a.store.db.Query(`SELECT 0, e.plate_text, MIN(e.ts_ms), MAX(e.ts_ms), COUNT(*), GROUP_CONCAT(DISTINCT e.device_id)
				FROM events e WHERE e.type='vehicle' AND e.plate_text <> '' AND e.ts_ms >= ? AND e.ts_ms < ?
				GROUP BY e.plate_text ORDER BY MIN(e.ts_ms)`, day[0].UnixMilli(), day[1].UnixMilli())
		}
		if err != nil {
			apiError(w, 500, "internal", err.Error())
			return
		}
		start := len(out)
		for rows.Next() {
			x := attendanceRow{Date: date, Kind: kind}
			var first, last int64
			var name, devs string
			if rows.Scan(&x.ID, &name, &first, &last, &x.Sightings, &devs) != nil {
				continue
			}
			if kind == "person" {
				x.Name = name
			} else {
				x.Plate = name
			}
			x.FirstSeen = time.UnixMilli(first).In(rg.Loc).Format(time.RFC3339)
			x.LastSeen = time.UnixMilli(last).In(rg.Loc).Format(time.RFC3339)
			x.DurationMin = (last - first) / 60000
			x.Devices = strings.Split(devs, ",")
			sort.Strings(x.Devices)
			out = append(out, x)
		}
		rows.Close()
		if kind == "vehicle" { // labels after the cursor is closed (one sqlite connection)
			pl := []string{}
			for _, x := range out[start:] {
				pl = append(pl, x.Plate)
			}
			lb := a.store.labels(pl)
			for i := start; i < len(out); i++ {
				l := lb[out[i].Plate]
				out[i].Label = strings.TrimSpace(l.Owner + " " + l.Label)
			}
		}
	}
	if out == nil {
		out = []attendanceRow{}
	}
	if r.URL.Query().Get("format") == "csv" {
		w.Header().Set("Content-Type", "text/csv; charset=utf-8")
		w.Header().Set("Content-Disposition", `attachment; filename="attendance.csv"`)
		cw := csv.NewWriter(w)
		cw.Write([]string{"date", "kind", "person_id", "name", "plate", "label", "first_seen", "last_seen", "duration_min", "sightings", "devices"})
		for _, x := range out {
			id := ""
			if x.ID != 0 {
				id = strconv.FormatInt(x.ID, 10)
			}
			cw.Write([]string{x.Date, x.Kind, id, csvSafe(x.Name), x.Plate, csvSafe(x.Label), x.FirstSeen, x.LastSeen,
				strconv.FormatInt(x.DurationMin, 10), strconv.FormatInt(x.Sightings, 10), strings.Join(x.Devices, " ")})
		}
		cw.Flush()
		return
	}
	writeJSON(w, map[string]any{"range": map[string]any{"from": rg.From.Format(time.RFC3339), "to": rg.To.Format(time.RFC3339),
		"tz": rg.Loc.String()}, "data": out})
}

// ------------------------------------------------------------------ timelines across cameras

type Visit struct {
	DeviceID  string `json:"device_id"`
	StartMs   int64  `json:"start_ms"`
	EndMs     int64  `json:"end_ms"`
	Start     string `json:"start"`
	End       string `json:"end"`
	Sightings int    `json:"sightings"`
	FirstID   int64  `json:"first_detection_id"`
	LastID    int64  `json:"last_detection_id"`
	Images    Images `json:"images"` // from the first sighting of the visit
}

type Transition struct {
	From string `json:"from_device"`
	To   string `json:"to_device"`
	AtMs int64  `json:"at_ms"`
	GapS int64  `json:"gap_s"` // time between leaving one camera and appearing on the next
}

type Timeline struct {
	Visits      []Visit      `json:"visits"`
	Path        []string     `json:"path"` // cameras in the order visited
	Transitions []Transition `json:"transitions"`
	Truncated   bool         `json:"truncated"` // true when more than 5000 sightings matched; the oldest are left out
}

// buildTimeline merges consecutive sightings on one camera (no more than `gap` apart) into visits.
func (a *App) buildTimeline(base string, evs []EventRow, gap time.Duration) Timeline {
	sort.Slice(evs, func(i, j int) bool { return evs[i].TsMs < evs[j].TsMs })
	tl := Timeline{Visits: []Visit{}, Path: []string{}, Transitions: []Transition{}}
	for _, e := range evs {
		if n := len(tl.Visits); n > 0 {
			v := &tl.Visits[n-1]
			if v.DeviceID == e.DeviceID && time.Duration(e.TsMs-v.EndMs)*time.Millisecond <= gap {
				v.EndMs, v.End, v.LastID = e.TsMs, isoTime(e.TsMs), e.ID
				v.Sightings++
				continue
			}
		}
		im := Images{Full: a.img(base, e.FrameURL)}
		if e.Type == "face" {
			im.Face = a.img(base, e.CropURL)
		} else {
			im.Plate, im.Vehicle = a.img(base, e.CropURL), a.img(base, e.ExtraURL)
		}
		tl.Visits = append(tl.Visits, Visit{DeviceID: e.DeviceID, StartMs: e.TsMs, EndMs: e.TsMs, Start: isoTime(e.TsMs),
			End: isoTime(e.TsMs), Sightings: 1, FirstID: e.ID, LastID: e.ID, Images: im})
	}
	for i, v := range tl.Visits {
		if i == 0 || tl.Visits[i-1].DeviceID != v.DeviceID {
			tl.Path = append(tl.Path, v.DeviceID)
		}
		if i > 0 && tl.Visits[i-1].DeviceID != v.DeviceID {
			tl.Transitions = append(tl.Transitions, Transition{From: tl.Visits[i-1].DeviceID, To: v.DeviceID, AtMs: v.StartMs,
				GapS: (v.StartMs - tl.Visits[i-1].EndMs) / 1000})
		}
	}
	return tl
}

func (a *App) timelineFor(w http.ResponseWriter, r *http.Request, q DetQuery, subject any) {
	from, ok1 := parseTime(r.URL.Query().Get("from"))
	to, ok2 := parseTime(r.URL.Query().Get("to"))
	if !ok1 || !ok2 {
		apiError(w, 400, "bad_request", "from/to must be epoch seconds/milliseconds or RFC 3339")
		return
	}
	gap := limitParam(r.URL.Query().Get("gap"), 120, 86400)
	q.From, q.To, q.Limit = from, to, 5000
	evs, total, err := a.store.QueryDetections(q) // newest 5000
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	tl := a.buildTimeline(a.base(r), evs, time.Duration(gap)*time.Second)
	tl.Truncated = total > int64(len(evs))
	writeJSON(w, map[string]any{"subject": subject, "gap_s": gap, "total_sightings": total, "timeline": tl})
}

func (a *App) personTimeline(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, 400, "bad_request", "id must be a positive integer")
		return
	}
	p, err := a.store.GetPerson(id)
	if err != nil {
		apiError(w, 404, "not_found", "no such person")
		return
	}
	a.timelineFor(w, r, DetQuery{PersonID: id}, map[string]any{"type": "person", "id": p.ID, "name": p.Name})
}

func (a *App) vehicleTimeline(w http.ResponseWriter, r *http.Request) {
	plate := normPlate(r.PathValue("plate"))
	if plate == "" {
		apiError(w, 400, "bad_request", "plate must contain letters or digits")
		return
	}
	var n int
	a.store.db.QueryRow(`SELECT COUNT(*) FROM events WHERE type='vehicle' AND plate_text = ?`, plate).Scan(&n)
	if n == 0 {
		apiError(w, 404, "not_found", "this plate has not been seen")
		return
	}
	a.timelineFor(w, r, DetQuery{Type: "vehicle", Plate: plate, PlateExact: true}, map[string]any{"type": "vehicle", "plate": plate})
}

// ------------------------------------------------------------------ misread plates

func editDistance(a, b string, max int) int {
	if d := len(a) - len(b); d > max || -d > max {
		return max + 1
	}
	prev := make([]int, len(b)+1)
	cur := make([]int, len(b)+1)
	for j := range prev {
		prev[j] = j
	}
	for i := 1; i <= len(a); i++ {
		cur[0] = i
		best := cur[0]
		for j := 1; j <= len(b); j++ {
			c := 1
			if a[i-1] == b[j-1] {
				c = 0
			}
			cur[j] = min(prev[j]+1, cur[j-1]+1, prev[j-1]+c)
			best = min(best, cur[j])
		}
		if best > max {
			return max + 1
		}
		prev, cur = cur, prev
	}
	return prev[len(b)]
}

type SimilarPlate struct {
	Plate     string `json:"plate"`
	Distance  int    `json:"distance"` // single-character edits away
	Sightings int64  `json:"sightings"`
	LastSeen  int64  `json:"last_seen"`
}

func (a *App) similarVehicles(w http.ResponseWriter, r *http.Request) {
	plate := normPlate(r.PathValue("plate"))
	if plate == "" {
		apiError(w, 400, "bad_request", "plate must contain letters or digits")
		return
	}
	dist := limitParam(r.URL.Query().Get("distance"), 1, 2)
	rows, err := a.store.db.Query(`SELECT plate_text, COUNT(*), MAX(ts_ms) FROM events WHERE type='vehicle' AND plate_text <> ''
		AND LENGTH(plate_text) BETWEEN ? AND ? GROUP BY plate_text`, len(plate)-dist, len(plate)+dist)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	defer rows.Close()
	out := []SimilarPlate{}
	for rows.Next() {
		var s SimilarPlate
		if rows.Scan(&s.Plate, &s.Sightings, &s.LastSeen) != nil || s.Plate == plate {
			continue
		}
		if s.Distance = editDistance(plate, s.Plate, dist); s.Distance <= dist {
			out = append(out, s)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Distance != out[j].Distance {
			return out[i].Distance < out[j].Distance
		}
		return out[i].Sightings > out[j].Sightings
	})
	writeJSON(w, map[string]any{"plate": plate, "similar": out})
}

// mergeVehicle folds the sightings of a misread plate into the correct one.
func (a *App) mergeVehicle(w http.ResponseWriter, r *http.Request) {
	into, from := normPlate(r.PathValue("plate")), normPlate(r.PathValue("from"))
	if into == "" || from == "" || into == from {
		apiError(w, 400, "bad_request", "two different plates are required")
		return
	}
	tx, err := a.store.db.Begin()
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	defer tx.Rollback()
	res, err := tx.Exec(`UPDATE events SET plate_text = ? WHERE type='vehicle' AND plate_text = ?`, into, from)
	if err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		apiError(w, 404, "not_found", "the plate to merge has not been seen")
		return
	}
	// keep the label of the target; take over the source's when the target has none
	tx.Exec(`INSERT OR IGNORE INTO vehicle_labels(plate, owner, label, watchlist, notes, updated_ms)
		SELECT ?, owner, label, watchlist, notes, updated_ms FROM vehicle_labels WHERE plate = ?`, into, from)
	tx.Exec(`DELETE FROM vehicle_labels WHERE plate = ?`, from)
	if err := tx.Commit(); err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	writeJSON(w, map[string]any{"ok": true, "moved": n})
}

// ------------------------------------------------------------------ erase (privacy)

func (a *App) eraseWhere(cond string, args []any) CleanupResult {
	a.janitor.mu.Lock()
	defer a.janitor.mu.Unlock()
	var res CleanupResult
	a.janitor.sweep(&res, cond, args, 0, nil)
	return res
}

// erasePerson removes a person completely: every detection, picture, face sample and the record itself.
func (a *App) erasePerson(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, 400, "bad_request", "id must be a positive integer")
		return
	}
	if _, err := a.store.GetPerson(id); err != nil {
		apiError(w, 404, "not_found", "no such person")
		return
	}
	// the cover image is referenced by the person row, so drop that reference before the images go
	a.store.db.Exec(`UPDATE persons SET cover_path = '' WHERE id = ?`, id)
	res := a.eraseWhere(`e.person_id = ?`, []any{id})
	if err := a.gallery.Delete(id); err != nil {
		apiError(w, 500, "internal", err.Error())
		return
	}
	a.alerts.forget("person:" + strconv.FormatInt(id, 10))
	writeJSON(w, map[string]any{"ok": true, "detections_deleted": res.EventsDeleted, "bytes_freed": res.BytesFreed})
}

func (a *App) eraseVehicle(w http.ResponseWriter, r *http.Request) {
	plate := normPlate(r.PathValue("plate"))
	if plate == "" {
		apiError(w, 400, "bad_request", "plate must contain letters or digits")
		return
	}
	res := a.eraseWhere(`e.type='vehicle' AND e.plate_text = ?`, []any{plate})
	a.store.db.Exec(`DELETE FROM vehicle_labels WHERE plate = ?`, plate)
	a.alerts.forget("plate:" + plate)
	writeJSON(w, map[string]any{"ok": true, "detections_deleted": res.EventsDeleted, "bytes_freed": res.BytesFreed})
}

// forget drops cooldown entries of an erased subject so nothing about it lingers in memory.
func (al *Alerts) forget(subject string) {
	al.cdMu.Lock()
	defer al.cdMu.Unlock()
	for k := range al.cooldown {
		if strings.HasSuffix(k, "|"+subject) {
			delete(al.cooldown, k)
		}
	}
}

// ------------------------------------------------------------------ backup

// backup streams a zip with a consistent copy of the database and, with ?images=true, the pictures.
// Files are stored without compression: JPEGs do not shrink, and it keeps CPU use low on small servers.
func (a *App) backup(w http.ResponseWriter, r *http.Request) {
	tmp := filepath.Join(a.dataDir, fmt.Sprintf(".backup-%d.db", time.Now().UnixNano()))
	defer os.Remove(tmp)
	if _, err := a.store.db.Exec(`VACUUM INTO '` + strings.ReplaceAll(tmp, `'`, `''`) + `'`); err != nil {
		apiError(w, 500, "internal", "database snapshot failed: "+err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="edgeai-backup-%s.zip"`, time.Now().Format("20060102-150405")))
	zw := zip.NewWriter(w)
	defer zw.Close()
	add := func(name, src string, mod time.Time) error {
		f, err := os.Open(src)
		if err != nil {
			return err
		}
		defer f.Close()
		zf, err := zw.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Store, Modified: mod})
		if err != nil {
			return err
		}
		_, err = io.Copy(zf, f)
		return err
	}
	if err := add("edgeai.db", tmp, time.Now()); err != nil {
		return
	}
	if truthy(r.URL.Query().Get("images")) {
		root := a.imagesRoot()
		filepath.WalkDir(root, func(p string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			if r.Context().Err() != nil {
				return r.Context().Err()
			}
			rel, _ := filepath.Rel(filepath.Dir(root), p)
			fi, e := d.Info()
			if e != nil {
				return nil
			}
			add(filepath.ToSlash(rel), p, fi.ModTime())
			return nil
		})
	}
}
