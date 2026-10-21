package main

import (
	"encoding/json"
	"math"
	"sync"
	"time"
)

// Gallery assigns stable person ids to face embeddings (cosine similarity, in memory, persisted in sqlite).
type Gallery struct {
	mu        sync.Mutex
	store     *Store
	threshold float64 // min cosine similarity to call it the same person
	persons   map[int64]*galleryPerson
	tracks    map[trackKey]trackRef // device track -> person, so updates of one track stay one person
}

type galleryPerson struct {
	id   int64
	name string
	embs [][]float32
	qual []float64
}

type trackKey struct {
	device string
	track  int64
}
type trackRef struct {
	person int64
	at     time.Time
}

const maxEmbeddingsPerPerson = 12

func NewGallery(s *Store, threshold float64) (*Gallery, error) {
	g := &Gallery{store: s, threshold: threshold, persons: map[int64]*galleryPerson{}, tracks: map[trackKey]trackRef{}}
	rows, err := s.db.Query(`SELECT p.id, p.name, e.emb, e.quality FROM persons p JOIN person_embeddings e ON e.person_id = p.id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() {
		var id int64
		var name, emb string
		var q float64
		if err := rows.Scan(&id, &name, &emb, &q); err != nil {
			return nil, err
		}
		var v []float32
		if json.Unmarshal([]byte(emb), &v) != nil {
			continue
		}
		p := g.persons[id]
		if p == nil {
			p = &galleryPerson{id: id, name: name}
			g.persons[id] = p
		}
		p.embs = append(p.embs, v)
		p.qual = append(p.qual, q)
	}
	return g, rows.Err()
}

func cosine(a, b []float32) float64 {
	if len(a) != len(b) {
		return -1
	}
	var dot, na, nb float64
	for i := range a {
		dot += float64(a[i]) * float64(b[i])
		na += float64(a[i]) * float64(a[i])
		nb += float64(b[i]) * float64(b[i])
	}
	return dot / (math.Sqrt(na*nb) + 1e-9)
}

type MatchResult struct {
	PersonID   int64   `json:"person_id"`
	Name       string  `json:"name"`
	IsNew      bool    `json:"is_new"`
	Similarity float64 `json:"similarity"`
}

// Match finds (or creates) the person for an embedding and learns from it.
func (g *Gallery) Match(device string, track int64, emb []float32, quality float64, ts int64, coverURL string) (MatchResult, error) {
	g.mu.Lock()
	defer g.mu.Unlock()

	// forget track bindings older than 10 minutes
	for k, v := range g.tracks {
		if time.Since(v.at) > 10*time.Minute {
			delete(g.tracks, k)
		}
	}

	best, bestSim := int64(0), -1.0
	for id, p := range g.persons {
		for _, e := range p.embs {
			if s := cosine(emb, e); s > bestSim {
				best, bestSim = id, s
			}
		}
	}
	key := trackKey{device, track}
	res := MatchResult{Similarity: bestSim}

	if ref, ok := g.tracks[key]; ok && (best == ref.person || bestSim < g.threshold+0.1) {
		// same track as before: stick with its person unless another one is clearly better
		res.PersonID = ref.person
	} else if best != 0 && bestSim >= g.threshold {
		res.PersonID = best
	}

	if res.PersonID == 0 {
		r, err := g.store.db.Exec(`INSERT INTO persons(name, first_seen, last_seen, visits, cover_path) VALUES('', ?, ?, 0, ?)`,
			ts, ts, coverURL)
		if err != nil {
			return res, err
		}
		res.PersonID, _ = r.LastInsertId()
		res.IsNew = true
		g.persons[res.PersonID] = &galleryPerson{id: res.PersonID}
	}
	p := g.persons[res.PersonID]
	res.Name = p.name

	_, seenTrack := g.tracks[key]
	g.tracks[key] = trackRef{res.PersonID, time.Now()}
	visit := 0
	if !seenTrack {
		visit = 1
	}
	g.store.db.Exec(`UPDATE persons SET last_seen = MAX(last_seen, ?), visits = visits + ? WHERE id = ?`, ts, visit, res.PersonID)

	g.learn(p, emb, quality, ts, coverURL)
	return res, nil
}

// learn keeps a small, diverse set of good embeddings per person.
func (g *Gallery) learn(p *galleryPerson, emb []float32, quality float64, ts int64, coverURL string) {
	for _, e := range p.embs {
		if cosine(emb, e) > 0.92 {
			return // nearly identical view already stored
		}
	}
	if len(p.embs) >= maxEmbeddingsPerPerson {
		// replace the lowest-quality sample if this one is better
		worst := 0
		for i := range p.qual {
			if p.qual[i] < p.qual[worst] {
				worst = i
			}
		}
		if quality <= p.qual[worst] {
			return
		}
		p.embs = append(p.embs[:worst], p.embs[worst+1:]...)
		p.qual = append(p.qual[:worst], p.qual[worst+1:]...)
		g.store.db.Exec(`DELETE FROM person_embeddings WHERE id = (SELECT id FROM person_embeddings WHERE person_id = ?
			ORDER BY quality ASC LIMIT 1)`, p.id)
	}
	best := true
	for _, q := range p.qual {
		if q >= quality {
			best = false
		}
	}
	p.embs = append(p.embs, emb)
	p.qual = append(p.qual, quality)
	b, _ := json.Marshal(emb)
	g.store.db.Exec(`INSERT INTO person_embeddings(person_id, emb, quality, ts_ms) VALUES(?,?,?,?)`, p.id, string(b), quality, ts)
	if best && coverURL != "" {
		g.store.db.Exec(`UPDATE persons SET cover_path = ? WHERE id = ?`, coverURL, p.id)
	}
}

func (g *Gallery) Rename(id int64, name string) error {
	g.mu.Lock()
	defer g.mu.Unlock()
	if p := g.persons[id]; p != nil {
		p.name = name
	}
	_, err := g.store.db.Exec(`UPDATE persons SET name = ? WHERE id = ?`, name, id)
	return err
}

// Merge moves everything of `from` into `into` (fixes a person split into two ids).
func (g *Gallery) Merge(into, from int64) error {
	g.mu.Lock()
	defer g.mu.Unlock()
	a, b := g.persons[into], g.persons[from]
	if a == nil || b == nil || into == from {
		return errNotFound
	}
	tx, err := g.store.db.Begin()
	if err != nil {
		return err
	}
	tx.Exec(`UPDATE person_embeddings SET person_id = ? WHERE person_id = ?`, into, from)
	tx.Exec(`UPDATE events SET person_id = ? WHERE person_id = ?`, into, from)
	tx.Exec(`UPDATE persons SET visits = visits + (SELECT visits FROM persons WHERE id = ?),
		first_seen = MIN(first_seen, (SELECT first_seen FROM persons WHERE id = ?)),
		last_seen = MAX(last_seen, (SELECT last_seen FROM persons WHERE id = ?)) WHERE id = ?`, from, from, from, into)
	tx.Exec(`DELETE FROM persons WHERE id = ?`, from)
	if err := tx.Commit(); err != nil {
		return err
	}
	a.embs = append(a.embs, b.embs...)
	a.qual = append(a.qual, b.qual...)
	delete(g.persons, from)
	for k, v := range g.tracks {
		if v.person == from {
			g.tracks[k] = trackRef{into, v.at}
		}
	}
	return nil
}

func (g *Gallery) Delete(id int64) error {
	g.mu.Lock()
	defer g.mu.Unlock()
	delete(g.persons, id)
	g.store.db.Exec(`DELETE FROM person_embeddings WHERE person_id = ?`, id)
	g.store.db.Exec(`UPDATE events SET person_id = NULL WHERE person_id = ?`, id)
	_, err := g.store.db.Exec(`DELETE FROM persons WHERE id = ?`, id)
	return err
}
