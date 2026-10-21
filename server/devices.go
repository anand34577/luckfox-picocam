package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"
)

type Device struct {
	ID       string          `json:"id"`
	Profile  string          `json:"profile"`
	IP       string          `json:"ip"`
	LastSeen int64           `json:"last_seen"`
	Online   bool            `json:"online"`
	Status   json.RawMessage `json:"status"`
	// MediaMTX path names for the browser player (WebRTC/WHEP)
	CleanPath   string `json:"clean_path"`
	OverlayPath string `json:"overlay_path"`
}

type Devices struct {
	mu       sync.Mutex
	store    *Store
	devices  map[string]*Device
	mediamtx string            // MediaMTX API base, e.g. http://127.0.0.1:9997 ("" = disabled)
	sources  map[string]string // path -> rtsp source already registered
}

func NewDevices(s *Store, mediamtxAPI string) *Devices {
	d := &Devices{store: s, devices: map[string]*Device{}, mediamtx: strings.TrimRight(mediamtxAPI, "/"),
		sources: map[string]string{}}
	rows, err := s.db.Query(`SELECT id, COALESCE(profile,''), COALESCE(ip,''), COALESCE(last_seen,0), COALESCE(status,'{}') FROM devices`)
	if err == nil {
		for rows.Next() {
			var dev Device
			var st string
			rows.Scan(&dev.ID, &dev.Profile, &dev.IP, &dev.LastSeen, &st)
			dev.Status = json.RawMessage(st)
			dev.CleanPath, dev.OverlayPath = pathNames(dev.ID)
			d.devices[dev.ID] = &dev
		}
		rows.Close()
	}
	// re-register known streams once MediaMTX is up
	go func() {
		time.Sleep(2 * time.Second)
		d.mu.Lock()
		defer d.mu.Unlock()
		for _, dev := range d.devices {
			if dev.IP != "" {
				d.registerStreams(dev)
			}
		}
	}()
	return d
}

func pathNames(id string) (string, string) {
	safe := strings.Map(func(r rune) rune {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' {
			return r
		}
		return '_'
	}, id)
	return safe + "_clean", safe + "_overlay"
}

// Heartbeat records a device status report and makes sure its streams are available to browsers.
func (d *Devices) Heartbeat(raw []byte, remoteIP string) (*Device, error) {
	var hb struct {
		DeviceID string `json:"device_id"`
		Profile  string `json:"profile"`
		IP       string `json:"ip"`
	}
	if err := json.Unmarshal(raw, &hb); err != nil || hb.DeviceID == "" {
		return nil, fmt.Errorf("bad heartbeat")
	}
	if hb.IP == "" {
		hb.IP = remoteIP
	}
	now := time.Now().UnixMilli()
	d.mu.Lock()
	defer d.mu.Unlock()
	dev := d.devices[hb.DeviceID]
	if dev == nil {
		dev = &Device{ID: hb.DeviceID}
		dev.CleanPath, dev.OverlayPath = pathNames(dev.ID)
		d.devices[hb.DeviceID] = dev
	}
	dev.Profile, dev.IP, dev.LastSeen, dev.Online, dev.Status = hb.Profile, hb.IP, now, true, json.RawMessage(raw)
	d.store.db.Exec(`INSERT INTO devices(id, profile, ip, last_seen, status) VALUES(?,?,?,?,?)
		ON CONFLICT(id) DO UPDATE SET profile=excluded.profile, ip=excluded.ip, last_seen=excluded.last_seen,
		status=excluded.status`, dev.ID, dev.Profile, dev.IP, now, string(raw))
	d.registerStreams(dev)
	cp := *dev
	return &cp, nil
}

func (d *Devices) List() []Device {
	d.mu.Lock()
	defer d.mu.Unlock()
	out := make([]Device, 0, len(d.devices))
	now := time.Now().UnixMilli()
	for _, dev := range d.devices {
		dev.Online = now-dev.LastSeen < 35_000
		out = append(out, *dev)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// registerStreams adds/updates on-demand MediaMTX paths pulling the device's RTSP streams.
func (d *Devices) registerStreams(dev *Device) {
	if d.mediamtx == "" || dev.IP == "" {
		return
	}
	for path, src := range map[string]string{
		dev.CleanPath:   "rtsp://" + dev.IP + "/live/clean",
		dev.OverlayPath: "rtsp://" + dev.IP + "/live/overlay",
	} {
		if d.sources[path] == src {
			continue
		}
		body, _ := json.Marshal(map[string]any{"source": src, "sourceOnDemand": true, "rtspTransport": "tcp",
			"sourceOnDemandCloseAfter": "20s"})
		ok := mediamtxCall("POST", d.mediamtx+"/v3/config/paths/add/"+path, body) ||
			mediamtxCall("PATCH", d.mediamtx+"/v3/config/paths/patch/"+path, body)
		if ok {
			d.sources[path] = src
			log.Printf("stream %s -> %s", path, src)
		}
	}
}

func mediamtxCall(method, url string, body []byte) bool {
	req, _ := http.NewRequest(method, url, bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	c := http.Client{Timeout: 3 * time.Second}
	resp, err := c.Do(req)
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode/100 == 2
}
