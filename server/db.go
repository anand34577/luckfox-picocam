package main

import (
	"database/sql"
	"encoding/json"
	"strings"

	_ "modernc.org/sqlite"
)

const schema = `
CREATE TABLE IF NOT EXISTS events (
	id           INTEGER PRIMARY KEY AUTOINCREMENT,
	ts_ms        INTEGER NOT NULL,
	device_id    TEXT NOT NULL,
	type         TEXT NOT NULL,           -- vehicle | face
	track_id     INTEGER,
	vehicle_type TEXT,
	plate_text   TEXT,
	plate_valid  INTEGER,
	plate_score  REAL,
	person_id    INTEGER,
	quality      REAL,
	frame_path   TEXT,
	crop_path    TEXT,                    -- plate or face crop
	extra_path   TEXT,                    -- vehicle crop
	meta         TEXT
);
CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts_ms);
CREATE INDEX IF NOT EXISTS idx_events_plate ON events(plate_text);
CREATE INDEX IF NOT EXISTS idx_events_person ON events(person_id);

CREATE TABLE IF NOT EXISTS persons (
	id         INTEGER PRIMARY KEY AUTOINCREMENT,
	name       TEXT NOT NULL DEFAULT '',
	first_seen INTEGER NOT NULL,
	last_seen  INTEGER NOT NULL,
	visits     INTEGER NOT NULL DEFAULT 0,
	cover_path TEXT
);
CREATE TABLE IF NOT EXISTS person_embeddings (
	id        INTEGER PRIMARY KEY AUTOINCREMENT,
	person_id INTEGER NOT NULL,
	emb       TEXT NOT NULL,              -- JSON float array
	quality   REAL NOT NULL,
	ts_ms     INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS devices (
	id        TEXT PRIMARY KEY,
	profile   TEXT,
	ip        TEXT,
	last_seen INTEGER,
	status    TEXT
);
`

type Store struct{ db *sql.DB }

func OpenStore(path string) (*Store, error) {
	db, err := sql.Open("sqlite", path+"?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)")
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1) // sqlite: one writer keeps things simple and safe
	if _, err := db.Exec(schema); err != nil {
		return nil, err
	}
	return &Store{db: db}, nil
}

type EventRow struct {
	ID          int64           `json:"id"`
	TsMs        int64           `json:"ts_ms"`
	DeviceID    string          `json:"device_id"`
	Type        string          `json:"type"`
	TrackID     int64           `json:"track_id"`
	VehicleType string          `json:"vehicle_type,omitempty"`
	PlateText   string          `json:"plate_text,omitempty"`
	PlateValid  bool            `json:"plate_valid"`
	PlateScore  float64         `json:"plate_score,omitempty"`
	PersonID    int64           `json:"person_id,omitempty"`
	PersonName  string          `json:"person_name,omitempty"`
	Quality     float64         `json:"quality,omitempty"`
	FrameURL    string          `json:"frame_url,omitempty"`
	CropURL     string          `json:"crop_url,omitempty"`
	ExtraURL    string          `json:"extra_url,omitempty"`
	Meta        json.RawMessage `json:"meta,omitempty"`
}

func (s *Store) InsertEvent(e *EventRow) error {
	res, err := s.db.Exec(`INSERT INTO events(ts_ms,device_id,type,track_id,vehicle_type,plate_text,plate_valid,
		plate_score,person_id,quality,frame_path,crop_path,extra_path,meta) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		e.TsMs, e.DeviceID, e.Type, e.TrackID, e.VehicleType, e.PlateText, e.PlateValid, e.PlateScore,
		nullInt(e.PersonID), e.Quality, e.FrameURL, e.CropURL, e.ExtraURL, string(e.Meta))
	if err != nil {
		return err
	}
	e.ID, _ = res.LastInsertId()
	return nil
}

func nullInt(v int64) any {
	if v == 0 {
		return nil
	}
	return v
}

type EventQuery struct {
	Type, Device, Plate string
	PersonID            int64
	Before              int64 // event id cursor
	Limit               int
}

func (s *Store) ListEvents(q EventQuery) ([]EventRow, error) {
	where := []string{"1=1"}
	args := []any{}
	if q.Type != "" {
		where = append(where, "e.type = ?")
		args = append(args, q.Type)
	}
	if q.Device != "" {
		where = append(where, "e.device_id = ?")
		args = append(args, q.Device)
	}
	if q.Plate != "" {
		where = append(where, "e.plate_text LIKE ?")
		args = append(args, "%"+strings.ToUpper(q.Plate)+"%")
	}
	if q.PersonID != 0 {
		where = append(where, "e.person_id = ?")
		args = append(args, q.PersonID)
	}
	if q.Before > 0 {
		where = append(where, "e.id < ?")
		args = append(args, q.Before)
	}
	if q.Limit <= 0 || q.Limit > 500 {
		q.Limit = 100
	}
	args = append(args, q.Limit)
	rows, err := s.db.Query(`SELECT e.id,e.ts_ms,e.device_id,e.type,COALESCE(e.track_id,0),COALESCE(e.vehicle_type,''),
		COALESCE(e.plate_text,''),COALESCE(e.plate_valid,0),COALESCE(e.plate_score,0),COALESCE(e.person_id,0),
		COALESCE(p.name,''),COALESCE(e.quality,0),COALESCE(e.frame_path,''),COALESCE(e.crop_path,''),
		COALESCE(e.extra_path,'')
		FROM events e LEFT JOIN persons p ON p.id = e.person_id
		WHERE `+strings.Join(where, " AND ")+` ORDER BY e.id DESC LIMIT ?`, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []EventRow{}
	for rows.Next() {
		var e EventRow
		if err := rows.Scan(&e.ID, &e.TsMs, &e.DeviceID, &e.Type, &e.TrackID, &e.VehicleType, &e.PlateText,
			&e.PlateValid, &e.PlateScore, &e.PersonID, &e.PersonName, &e.Quality, &e.FrameURL, &e.CropURL,
			&e.ExtraURL); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

type Person struct {
	ID        int64  `json:"id"`
	Name      string `json:"name"`
	FirstSeen int64  `json:"first_seen"`
	LastSeen  int64  `json:"last_seen"`
	Visits    int64  `json:"visits"`
	CoverURL  string `json:"cover_url"`
}

func (s *Store) ListPersons(q string) ([]Person, error) {
	rows, err := s.db.Query(`SELECT id,name,first_seen,last_seen,visits,COALESCE(cover_path,'') FROM persons
		WHERE (? = '' OR name LIKE ? OR CAST(id AS TEXT) = ?) ORDER BY last_seen DESC LIMIT 500`, q, "%"+q+"%", q)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Person{}
	for rows.Next() {
		var p Person
		if err := rows.Scan(&p.ID, &p.Name, &p.FirstSeen, &p.LastSeen, &p.Visits, &p.CoverURL); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

type Stats struct {
	VehiclesToday int64            `json:"vehicles_today"`
	PlatesToday   int64            `json:"plates_today"`
	FacesToday    int64            `json:"faces_today"`
	PersonsTotal  int64            `json:"persons_total"`
	NewToday      int64            `json:"new_persons_today"`
	ByVehicleType map[string]int64 `json:"by_vehicle_type"`
}

func (s *Store) Stats(dayStart int64) (*Stats, error) {
	st := &Stats{ByVehicleType: map[string]int64{}}
	s.db.QueryRow(`SELECT COUNT(*), COALESCE(SUM(plate_text <> ''),0) FROM events WHERE type='vehicle' AND ts_ms >= ?`,
		dayStart).Scan(&st.VehiclesToday, &st.PlatesToday)
	s.db.QueryRow(`SELECT COUNT(*) FROM events WHERE type='face' AND ts_ms >= ?`, dayStart).Scan(&st.FacesToday)
	s.db.QueryRow(`SELECT COUNT(*), COALESCE(SUM(first_seen >= ?),0) FROM persons`, dayStart).Scan(&st.PersonsTotal, &st.NewToday)
	rows, err := s.db.Query(`SELECT COALESCE(vehicle_type,'?'), COUNT(*) FROM events WHERE type='vehicle' AND ts_ms >= ?
		GROUP BY vehicle_type`, dayStart)
	if err == nil {
		for rows.Next() {
			var k string
			var v int64
			rows.Scan(&k, &v)
			st.ByVehicleType[k] = v
		}
		rows.Close()
	}
	return st, nil
}
