package main

import (
	"database/sql"
	"encoding/csv"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const schemaIntegration = `
CREATE TABLE IF NOT EXISTS vehicle_labels (
	plate      TEXT PRIMARY KEY,
	owner      TEXT NOT NULL DEFAULT '',
	label      TEXT NOT NULL DEFAULT '',
	watchlist  TEXT NOT NULL DEFAULT '',   -- '' | allow | block
	notes      TEXT NOT NULL DEFAULT '',
	updated_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS webhooks (
	id         INTEGER PRIMARY KEY AUTOINCREMENT,
	url        TEXT NOT NULL,
	secret     TEXT NOT NULL DEFAULT '',
	types      TEXT NOT NULL DEFAULT '',   -- comma list of face,vehicle; empty = all
	enabled    INTEGER NOT NULL DEFAULT 1,
	created_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_type_plate ON events(type, plate_text);
`

// ------------------------------------------------------------------ response shapes

type Images struct {
	Full    string `json:"full,omitempty"`    // whole camera frame
	Face    string `json:"face,omitempty"`    // cropped face (face detections)
	Plate   string `json:"plate,omitempty"`   // cropped number plate (vehicle detections)
	Vehicle string `json:"vehicle,omitempty"` // cropped vehicle (vehicle detections)
}

type PersonRef struct {
	ID    int64  `json:"id"`
	Name  string `json:"name"`
	Named bool   `json:"named"` // false until someone names this person
}

type PlateInfo struct {
	Text       string  `json:"text"`
	Valid      bool    `json:"valid"` // matches an Indian registration format
	Confidence float64 `json:"confidence"`
}

type VehicleRef struct {
	Type       string     `json:"type"`
	Confidence float64    `json:"confidence,omitempty"`
	Plate      *PlateInfo `json:"plate"` // null when no plate was read
	Owner      string     `json:"owner,omitempty"`
	Label      string     `json:"label,omitempty"`
	Watchlist  string     `json:"watchlist,omitempty"` // "", "allow" or "block"
}

// Detection is one face or vehicle sighting with everything needed to show it.
type Detection struct {
	ID       int64           `json:"id"`
	Type     string          `json:"type"` // face | vehicle
	TsMs     int64           `json:"ts_ms"`
	Time     string          `json:"time"`
	DeviceID string          `json:"device_id"`
	TrackID  int64           `json:"track_id"`
	Images   Images          `json:"images"`
	Person   *PersonRef      `json:"person,omitempty"`
	Vehicle  *VehicleRef     `json:"vehicle,omitempty"`
	Quality  float64         `json:"quality,omitempty"`
	Meta     json.RawMessage `json:"meta,omitempty"` // raw device metadata: boxes, landmarks, dwell time, ...
}

type PersonView struct {
	Person
	Named         bool   `json:"named"`
	FirstSeenTime string `json:"first_seen_time"`
	LastSeenTime  string `json:"last_seen_time"`
	Images        Images `json:"images"` // only "face" is set: the best face crop
}

type DeviceCount struct {
	DeviceID   string `json:"device_id"`
	Detections int64  `json:"detections"`
	LastSeen   int64  `json:"last_seen"`
}

type PersonDetail struct {
	PersonView
	Detections int64         `json:"detections"`
	Devices    []DeviceCount `json:"devices"`
	Recent     []Detection   `json:"recent"`
}

type VehicleView struct {
	Plate         string   `json:"plate"`
	PlateValid    bool     `json:"plate_valid"`
	VehicleTypes  []string `json:"vehicle_types"`
	Sightings     int64    `json:"sightings"`
	FirstSeen     int64    `json:"first_seen"`
	LastSeen      int64    `json:"last_seen"`
	FirstSeenTime string   `json:"first_seen_time"`
	LastSeenTime  string   `json:"last_seen_time"`
	Owner         string   `json:"owner"`
	Label         string   `json:"label"`
	Watchlist     string   `json:"watchlist"`
	Notes         string   `json:"notes"`
	Images        Images   `json:"images"` // from the most recent sighting
	LatestID      int64    `json:"latest_detection_id"`
}

type VehicleDetail struct {
	VehicleView
	Devices []DeviceCount `json:"devices"`
	Recent  []Detection   `json:"recent"`
}

type vehicleLabel struct{ Owner, Label, Watchlist, Notes string }

// ------------------------------------------------------------------ building views

func (a *App) base(r *http.Request) string {
	if a.publicURL != "" {
		return a.publicURL
	}
	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	return scheme + "://" + r.Host
}

func (s *Store) labels(plates []string) map[string]vehicleLabel {
	out := map[string]vehicleLabel{}
	if len(plates) == 0 {
		return out
	}
	args := make([]any, len(plates))
	for i, p := range plates {
		args[i] = p
	}
	rows, err := s.db.Query(`SELECT plate, owner, label, watchlist, notes FROM vehicle_labels WHERE plate IN (?`+
		strings.Repeat(",?", len(plates)-1)+`)`, args...)
	if err != nil {
		return out
	}
	defer rows.Close()
	for rows.Next() {
		var p string
		var l vehicleLabel
		if rows.Scan(&p, &l.Owner, &l.Label, &l.Watchlist, &l.Notes) == nil {
			out[p] = l
		}
	}
	return out
}

func (a *App) detections(base string, evs []EventRow) []Detection {
	plates := []string{}
	for _, e := range evs {
		if e.PlateText != "" {
			plates = append(plates, e.PlateText)
		}
	}
	labels := a.store.labels(plates)
	out := make([]Detection, 0, len(evs))
	for _, e := range evs {
		d := Detection{ID: e.ID, Type: e.Type, TsMs: e.TsMs, Time: isoTime(e.TsMs), DeviceID: e.DeviceID,
			TrackID: e.TrackID, Quality: e.Quality, Meta: e.Meta}
		d.Images.Full = a.img(base, e.FrameURL)
		var meta map[string]any
		json.Unmarshal(e.Meta, &meta)
		if e.Type == "face" {
			d.Images.Face = a.img(base, e.CropURL)
			if e.PersonID != 0 {
				d.Person = &PersonRef{ID: e.PersonID, Name: e.PersonName, Named: e.PersonName != ""}
			}
		} else {
			d.Images.Plate = a.img(base, e.CropURL)
			d.Images.Vehicle = a.img(base, e.ExtraURL)
			v := &VehicleRef{Type: e.VehicleType}
			v.Confidence, _ = meta["vehicle_conf"].(float64)
			if e.PlateText != "" {
				v.Plate = &PlateInfo{Text: e.PlateText, Valid: e.PlateValid, Confidence: e.PlateScore}
				l := labels[e.PlateText]
				v.Owner, v.Label, v.Watchlist = l.Owner, l.Label, l.Watchlist
			}
			d.Vehicle = v
		}
		out = append(out, d)
	}
	return out
}

func (a *App) personView(base string, p Person) PersonView {
	return PersonView{Person: Person{ID: p.ID, Name: p.Name, FirstSeen: p.FirstSeen, LastSeen: p.LastSeen,
		Visits: p.Visits, CoverURL: p.CoverURL}, Named: p.Name != "", FirstSeenTime: isoTime(p.FirstSeen),
		LastSeenTime: isoTime(p.LastSeen), Images: Images{Face: a.img(base, p.CoverURL)}}
}

// ------------------------------------------------------------------ detection queries

type DetQuery struct {
	Type, Device, Plate, VehicleType string
	PlateExact                       bool
	PersonID                         int64
	Named, PlateValid                string // "" | "true" | "false"
	From, To                         int64
	MinQuality                       float64
	Cursor                           int64
	Limit                            int
}

func (q DetQuery) where() (string, []any) {
	w, args := []string{"1=1"}, []any{}
	add := func(cond string, a ...any) { w = append(w, cond); args = append(args, a...) }
	if q.Type != "" {
		add("e.type = ?", q.Type)
	}
	if q.Device != "" {
		add("e.device_id = ?", q.Device)
	}
	if q.Plate != "" && q.PlateExact {
		add("e.plate_text = ?", q.Plate)
	} else if q.Plate != "" {
		add("e.plate_text LIKE ?", "%"+q.Plate+"%")
	}
	if q.VehicleType != "" {
		add("e.vehicle_type = ?", q.VehicleType)
	}
	if q.PersonID != 0 {
		add("e.person_id = ?", q.PersonID)
	}
	switch q.Named {
	case "true":
		add("COALESCE(p.name,'') <> ''")
	case "false":
		add("e.type = 'face' AND COALESCE(p.name,'') = ''")
	}
	switch q.PlateValid {
	case "true":
		add("e.plate_valid = 1")
	case "false":
		add("e.plate_text <> '' AND e.plate_valid = 0")
	}
	if q.From > 0 {
		add("e.ts_ms >= ?", q.From)
	}
	if q.To > 0 {
		add("e.ts_ms < ?", q.To)
	}
	if q.MinQuality > 0 {
		add("e.quality >= ?", q.MinQuality)
	}
	return strings.Join(w, " AND "), args
}

const detCols = `e.id,e.ts_ms,e.device_id,e.type,COALESCE(e.track_id,0),COALESCE(e.vehicle_type,''),
	COALESCE(e.plate_text,''),COALESCE(e.plate_valid,0),COALESCE(e.plate_score,0),COALESCE(e.person_id,0),
	COALESCE(p.name,''),COALESCE(e.quality,0),COALESCE(e.frame_path,''),COALESCE(e.crop_path,''),
	COALESCE(e.extra_path,''),COALESCE(e.meta,'')`

func scanEvent(sc interface{ Scan(...any) error }) (EventRow, error) {
	var e EventRow
	var meta string
	err := sc.Scan(&e.ID, &e.TsMs, &e.DeviceID, &e.Type, &e.TrackID, &e.VehicleType, &e.PlateText, &e.PlateValid,
		&e.PlateScore, &e.PersonID, &e.PersonName, &e.Quality, &e.FrameURL, &e.CropURL, &e.ExtraURL, &meta)
	if meta != "" && json.Valid([]byte(meta)) {
		e.Meta = json.RawMessage(meta)
	}
	return e, err
}

func (s *Store) QueryDetections(q DetQuery) ([]EventRow, int64, error) {
	where, args := q.where()
	var total int64
	if err := s.db.QueryRow(`SELECT COUNT(*) FROM events e LEFT JOIN persons p ON p.id = e.person_id WHERE `+where,
		args...).Scan(&total); err != nil {
		return nil, 0, err
	}
	if q.Cursor > 0 {
		where += " AND e.id < ?"
		args = append(args, q.Cursor)
	}
	args = append(args, q.Limit)
	rows, err := s.db.Query(`SELECT `+detCols+` FROM events e LEFT JOIN persons p ON p.id = e.person_id WHERE `+
		where+` ORDER BY e.id DESC LIMIT ?`, args...)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	out := []EventRow{}
	for rows.Next() {
		e, err := scanEvent(rows)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, e)
	}
	return out, total, rows.Err()
}

func (s *Store) GetEvent(id int64) (EventRow, error) {
	return scanEvent(s.db.QueryRow(`SELECT `+detCols+` FROM events e LEFT JOIN persons p ON p.id = e.person_id
		WHERE e.id = ?`, id))
}

func detQueryFrom(r *http.Request) (DetQuery, string) {
	v := r.URL.Query()
	q := DetQuery{Type: v.Get("type"), Device: v.Get("device"), Plate: normPlate(v.Get("plate")),
		PlateExact: truthy(v.Get("exact")), VehicleType: v.Get("vehicle_type"), Named: v.Get("named"),
		PlateValid: v.Get("plate_valid")}
	if q.Type != "" && q.Type != "face" && q.Type != "vehicle" {
		return q, "type must be face or vehicle"
	}
	for _, f := range []string{q.Named, q.PlateValid} {
		if f != "" && f != "true" && f != "false" {
			return q, "named and plate_valid must be true or false"
		}
	}
	var ok bool
	if q.From, ok = parseTime(v.Get("from")); !ok {
		return q, "from must be epoch seconds/milliseconds or RFC 3339"
	}
	if q.To, ok = parseTime(v.Get("to")); !ok {
		return q, "to must be epoch seconds/milliseconds or RFC 3339"
	}
	if s := v.Get("person_id"); s != "" {
		if q.PersonID, ok = parseID(s); !ok {
			return q, "person_id must be a positive integer"
		}
	}
	q.MinQuality, _ = strconv.ParseFloat(v.Get("min_quality"), 64)
	q.Cursor, _ = strconv.ParseInt(v.Get("cursor"), 10, 64)
	q.Limit = limitParam(v.Get("limit"), 50, 500)
	return q, ""
}

// ------------------------------------------------------------------ detections handlers

func (a *App) listDetections(w http.ResponseWriter, r *http.Request) {
	q, bad := detQueryFrom(r)
	if bad != "" {
		apiError(w, http.StatusBadRequest, "bad_request", bad)
		return
	}
	evs, total, err := a.store.QueryDetections(q)
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	page := Page[Detection]{Data: a.detections(a.base(r), evs), Total: total}
	if len(evs) == q.Limit {
		next := evs[len(evs)-1].ID
		page.NextCursor = &next
	}
	writeJSON(w, page)
}

func (a *App) getDetection(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, http.StatusBadRequest, "bad_request", "id must be a positive integer")
		return
	}
	e, err := a.store.GetEvent(id)
	if err == sql.ErrNoRows {
		apiError(w, http.StatusNotFound, "not_found", "no such detection")
		return
	} else if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	writeJSON(w, a.detections(a.base(r), []EventRow{e})[0])
}

func (a *App) deleteDetection(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, http.StatusBadRequest, "bad_request", "id must be a positive integer")
		return
	}
	e, err := a.store.GetEvent(id)
	if err == sql.ErrNoRows {
		apiError(w, http.StatusNotFound, "not_found", "no such detection")
		return
	} else if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	if _, err := a.store.db.Exec(`DELETE FROM events WHERE id = ?`, id); err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	for _, u := range []string{e.FrameURL, e.CropURL, e.ExtraURL} {
		a.removeImage(u)
	}
	writeJSON(w, map[string]any{"ok": true})
}

// removeImage deletes a stored image unless another record still points at it (e.g. a person's cover).
func (a *App) removeImage(url string) {
	rel := strings.TrimPrefix(url, "/images/")
	if rel == "" || rel == url {
		return
	}
	var n int
	a.store.db.QueryRow(`SELECT (SELECT COUNT(*) FROM persons WHERE cover_path = ?) +
		(SELECT COUNT(*) FROM events WHERE frame_path = ? OR crop_path = ? OR extra_path = ?)`, url, url, url, url).Scan(&n)
	if n > 0 {
		return
	}
	root, _ := filepath.Abs(filepath.Join(a.dataDir, "images"))
	p, _ := filepath.Abs(filepath.Join(root, filepath.FromSlash(rel)))
	if strings.HasPrefix(p, root+string(filepath.Separator)) {
		os.Remove(p)
	}
}

func (a *App) exportDetections(w http.ResponseWriter, r *http.Request) {
	q, bad := detQueryFrom(r)
	if bad != "" {
		apiError(w, http.StatusBadRequest, "bad_request", bad)
		return
	}
	q.Cursor = 0
	q.Limit = limitParam(r.URL.Query().Get("limit"), 10000, 100000)
	evs, _, err := a.store.QueryDetections(q)
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	base := a.base(r)
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", `attachment; filename="detections.csv"`)
	cw := csv.NewWriter(w)
	cw.Write([]string{"id", "type", "time", "ts_ms", "device_id", "person_id", "person_name", "vehicle_type", "plate",
		"plate_valid", "plate_confidence", "quality", "full_image", "face_image", "plate_image", "vehicle_image"})
	for _, e := range evs {
		face, plate, veh := "", "", ""
		if e.Type == "face" {
			face = a.img(base, e.CropURL)
		} else {
			plate, veh = a.img(base, e.CropURL), a.img(base, e.ExtraURL)
		}
		pid := ""
		if e.PersonID != 0 {
			pid = strconv.FormatInt(e.PersonID, 10)
		}
		cw.Write([]string{strconv.FormatInt(e.ID, 10), e.Type, isoTime(e.TsMs), strconv.FormatInt(e.TsMs, 10),
			e.DeviceID, pid, csvSafe(e.PersonName), e.VehicleType, e.PlateText, strconv.FormatBool(e.PlateValid),
			strconv.FormatFloat(e.PlateScore, 'f', 3, 64), strconv.FormatFloat(e.Quality, 'f', 3, 64),
			a.img(base, e.FrameURL), face, plate, veh})
	}
	cw.Flush()
}

// csvSafe stops a name starting with = + - @ from running as a formula when the file is opened in a spreadsheet.
func csvSafe(s string) string {
	if s != "" && strings.ContainsRune("=+-@\t\r", rune(s[0])) {
		return "'" + s
	}
	return s
}

// ------------------------------------------------------------------ persons handlers

func (a *App) listPeople(w http.ResponseWriter, r *http.Request) {
	ps, err := a.store.ListPersons(strings.TrimSpace(r.URL.Query().Get("q")))
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	named := r.URL.Query().Get("named")
	base := a.base(r)
	out := []PersonView{}
	for _, p := range ps {
		if (named == "true" && p.Name == "") || (named == "false" && p.Name != "") {
			continue
		}
		out = append(out, a.personView(base, p))
	}
	writeJSON(w, Page[PersonView]{Data: out, Total: int64(len(out))})
}

func (s *Store) GetPerson(id int64) (Person, error) {
	var p Person
	err := s.db.QueryRow(`SELECT id,name,first_seen,last_seen,visits,COALESCE(cover_path,'') FROM persons WHERE id = ?`,
		id).Scan(&p.ID, &p.Name, &p.FirstSeen, &p.LastSeen, &p.Visits, &p.CoverURL)
	return p, err
}

func (a *App) getPerson(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r.PathValue("id"))
	if !ok {
		apiError(w, http.StatusBadRequest, "bad_request", "id must be a positive integer")
		return
	}
	p, err := a.store.GetPerson(id)
	if err == sql.ErrNoRows {
		apiError(w, http.StatusNotFound, "not_found", "no such person")
		return
	} else if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	base := a.base(r)
	d := PersonDetail{PersonView: a.personView(base, p), Devices: a.store.deviceCounts(`e.person_id = ?`, id)}
	for _, dc := range d.Devices {
		d.Detections += dc.Detections
	}
	evs, _, err := a.store.QueryDetections(DetQuery{PersonID: id, Limit: limitParam(r.URL.Query().Get("recent"), 10, 100)})
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	d.Recent = a.detections(base, evs)
	writeJSON(w, d)
}

func (s *Store) deviceCounts(cond string, arg any) []DeviceCount {
	out := []DeviceCount{}
	rows, err := s.db.Query(`SELECT e.device_id, COUNT(*), MAX(e.ts_ms) FROM events e WHERE `+cond+
		` GROUP BY e.device_id ORDER BY MAX(e.ts_ms) DESC`, arg)
	if err != nil {
		return out
	}
	defer rows.Close()
	for rows.Next() {
		var d DeviceCount
		if rows.Scan(&d.DeviceID, &d.Detections, &d.LastSeen) == nil {
			out = append(out, d)
		}
	}
	return out
}

// ------------------------------------------------------------------ vehicles handlers

// vehicleSelect groups vehicle events by plate; the caller adds filters via where.
const vehicleSelect = `SELECT e.plate_text, COUNT(*), MIN(e.ts_ms), MAX(e.ts_ms), MAX(e.id), MAX(e.plate_valid),
	COALESCE(GROUP_CONCAT(DISTINCT NULLIF(e.vehicle_type,'')),'')
	FROM events e WHERE e.type = 'vehicle' AND e.plate_text <> ''`

func (a *App) vehicleViews(base string, rows *sql.Rows) ([]VehicleView, error) {
	var vs []VehicleView
	for rows.Next() {
		var v VehicleView
		var types string
		if err := rows.Scan(&v.Plate, &v.Sightings, &v.FirstSeen, &v.LastSeen, &v.LatestID, &v.PlateValid, &types); err != nil {
			return nil, err
		}
		v.VehicleTypes = []string{}
		if types != "" {
			v.VehicleTypes = strings.Split(types, ",")
			sort.Strings(v.VehicleTypes)
		}
		v.FirstSeenTime, v.LastSeenTime = isoTime(v.FirstSeen), isoTime(v.LastSeen)
		vs = append(vs, v)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows.Close() // single sqlite connection: finish this cursor before issuing more queries
	plates := make([]string, len(vs))
	for i, v := range vs {
		plates[i] = v.Plate
	}
	labels := a.store.labels(plates)
	for i := range vs {
		l := labels[vs[i].Plate]
		vs[i].Owner, vs[i].Label, vs[i].Watchlist, vs[i].Notes = l.Owner, l.Label, l.Watchlist, l.Notes
		if e, err := a.store.GetEvent(vs[i].LatestID); err == nil {
			vs[i].Images = Images{Full: a.img(base, e.FrameURL), Plate: a.img(base, e.CropURL),
				Vehicle: a.img(base, e.ExtraURL)}
		}
	}
	return vs, nil
}

func (a *App) listVehicles(w http.ResponseWriter, r *http.Request) {
	v := r.URL.Query()
	limit := limitParam(v.Get("limit"), 50, 500)
	where, args := "", []any{}
	if p := normPlate(v.Get("plate")); p != "" {
		where += " AND e.plate_text LIKE ?"
		args = append(args, "%"+p+"%")
	}
	if t := v.Get("vehicle_type"); t != "" {
		where += " AND e.vehicle_type = ?"
		args = append(args, t)
	}
	if truthy(v.Get("plate_valid")) {
		where += " AND e.plate_valid = 1"
	}
	from, ok1 := parseTime(v.Get("from"))
	to, ok2 := parseTime(v.Get("to"))
	if !ok1 || !ok2 {
		apiError(w, http.StatusBadRequest, "bad_request", "from/to must be epoch seconds/milliseconds or RFC 3339")
		return
	}
	if from > 0 {
		where += " AND e.ts_ms >= ?"
		args = append(args, from)
	}
	if to > 0 {
		where += " AND e.ts_ms < ?"
		args = append(args, to)
	}
	if wl := v.Get("watchlist"); wl != "" {
		if wl != "allow" && wl != "block" {
			apiError(w, http.StatusBadRequest, "bad_request", "watchlist must be allow or block")
			return
		}
		where += " AND e.plate_text IN (SELECT plate FROM vehicle_labels WHERE watchlist = ?)"
		args = append(args, wl)
	}
	var total int64
	a.store.db.QueryRow(`SELECT COUNT(DISTINCT e.plate_text) FROM events e WHERE e.type='vehicle' AND e.plate_text <> ''`+
		where, args...).Scan(&total)
	having := ""
	if c, _ := strconv.ParseInt(v.Get("cursor"), 10, 64); c > 0 {
		having = " HAVING MAX(e.id) < ?"
		args = append(args, c)
	}
	args = append(args, limit)
	rows, err := a.store.db.Query(vehicleSelect+where+` GROUP BY e.plate_text`+having+` ORDER BY MAX(e.id) DESC LIMIT ?`, args...)
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	vs, err := a.vehicleViews(a.base(r), rows)
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	if vs == nil {
		vs = []VehicleView{}
	}
	page := Page[VehicleView]{Data: vs, Total: total}
	if len(vs) == limit {
		next := vs[len(vs)-1].LatestID
		page.NextCursor = &next
	}
	writeJSON(w, page)
}

func (a *App) loadVehicle(r *http.Request) (*VehicleView, string, int, string) {
	plate := normPlate(r.PathValue("plate"))
	if plate == "" {
		return nil, "", http.StatusBadRequest, "plate must contain letters or digits"
	}
	rows, err := a.store.db.Query(vehicleSelect+` AND e.plate_text = ? GROUP BY e.plate_text`, plate)
	if err != nil {
		return nil, plate, http.StatusInternalServerError, err.Error()
	}
	vs, err := a.vehicleViews(a.base(r), rows)
	if err != nil {
		return nil, plate, http.StatusInternalServerError, err.Error()
	}
	if len(vs) == 0 {
		return nil, plate, http.StatusNotFound, "this plate has not been seen"
	}
	return &vs[0], plate, 0, ""
}

func (a *App) getVehicle(w http.ResponseWriter, r *http.Request) {
	v, plate, status, msg := a.loadVehicle(r)
	if v == nil {
		apiError(w, status, map[int]string{400: "bad_request", 404: "not_found", 500: "internal"}[status], msg)
		return
	}
	base := a.base(r)
	d := VehicleDetail{VehicleView: *v, Devices: a.store.deviceCounts(`e.type='vehicle' AND e.plate_text = ?`, plate)}
	evs, _, err := a.store.QueryDetections(DetQuery{Type: "vehicle", Plate: plate, PlateExact: true,
		Limit: limitParam(r.URL.Query().Get("recent"), 10, 100)})
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	d.Recent = a.detections(base, evs)
	writeJSON(w, d)
}

// patchVehicle upserts the owner/label/watchlist record for a plate. Absent fields are left unchanged.
func (a *App) patchVehicle(w http.ResponseWriter, r *http.Request) {
	plate := normPlate(r.PathValue("plate"))
	if plate == "" || len(plate) > 16 {
		apiError(w, http.StatusBadRequest, "bad_request", "plate must be 1-16 letters or digits")
		return
	}
	var body struct {
		Owner, Label, Watchlist, Notes *string
	}
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16<<10))
	if err := dec.Decode(&body); err != nil {
		apiError(w, http.StatusBadRequest, "bad_request", "body must be JSON with owner, label, watchlist, notes")
		return
	}
	if body.Watchlist != nil && *body.Watchlist != "" && *body.Watchlist != "allow" && *body.Watchlist != "block" {
		apiError(w, http.StatusBadRequest, "bad_request", `watchlist must be "", "allow" or "block"`)
		return
	}
	cur := a.store.labels([]string{plate})[plate]
	set := func(dst *string, src *string) {
		if src != nil {
			*dst = strings.TrimSpace(*src)
		}
	}
	set(&cur.Owner, body.Owner)
	set(&cur.Label, body.Label)
	set(&cur.Watchlist, body.Watchlist)
	set(&cur.Notes, body.Notes)
	_, err := a.store.db.Exec(`INSERT INTO vehicle_labels(plate, owner, label, watchlist, notes, updated_ms)
		VALUES(?,?,?,?,?,?) ON CONFLICT(plate) DO UPDATE SET owner=excluded.owner, label=excluded.label,
		watchlist=excluded.watchlist, notes=excluded.notes, updated_ms=excluded.updated_ms`,
		plate, cur.Owner, cur.Label, cur.Watchlist, cur.Notes, time.Now().UnixMilli())
	if err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	writeJSON(w, map[string]any{"plate": plate, "owner": cur.Owner, "label": cur.Label, "watchlist": cur.Watchlist,
		"notes": cur.Notes})
}

func (a *App) deleteVehicleLabel(w http.ResponseWriter, r *http.Request) {
	plate := normPlate(r.PathValue("plate"))
	if _, err := a.store.db.Exec(`DELETE FROM vehicle_labels WHERE plate = ?`, plate); err != nil {
		apiError(w, http.StatusInternalServerError, "internal", err.Error())
		return
	}
	writeJSON(w, map[string]any{"ok": true})
}

// ------------------------------------------------------------------ devices, health

func (a *App) getDevice(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	for _, d := range a.devices.List() {
		if d.ID == id {
			writeJSON(w, d)
			return
		}
	}
	apiError(w, http.StatusNotFound, "not_found", "no such device")
}

func (a *App) health(w http.ResponseWriter, r *http.Request) {
	var n int64
	if err := a.store.db.QueryRow(`SELECT COUNT(*) FROM events`).Scan(&n); err != nil {
		apiError(w, http.StatusServiceUnavailable, "unavailable", err.Error())
		return
	}
	online := 0
	for _, d := range a.devices.List() {
		if d.Online {
			online++
		}
	}
	writeJSON(w, map[string]any{"status": "ok", "version": apiVersion, "time_ms": time.Now().UnixMilli(),
		"detections": n, "devices_online": online, "auth_required": a.apiKey != ""})
}
