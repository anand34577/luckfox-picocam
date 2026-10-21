package main

import (
	"encoding/json"
	"log"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// Hub pushes live messages ({"kind": "event"|"device", "data": ...}) to every dashboard tab.
type Hub struct {
	mu      sync.Mutex
	clients map[*websocket.Conn]chan []byte
}

func NewHub() *Hub { return &Hub{clients: map[*websocket.Conn]chan []byte{}} }

var upgrader = websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}

func (h *Hub) Broadcast(kind string, data any) {
	b, err := json.Marshal(map[string]any{"kind": kind, "data": data})
	if err != nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, ch := range h.clients {
		select {
		case ch <- b:
		default: // slow client: drop rather than block ingest
		}
	}
}

func (h *Hub) ServeWS(w http.ResponseWriter, r *http.Request) {
	c, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	ch := make(chan []byte, 64)
	h.mu.Lock()
	h.clients[c] = ch
	h.mu.Unlock()
	defer func() {
		h.mu.Lock()
		delete(h.clients, c)
		h.mu.Unlock()
		c.Close()
	}()
	go func() { // reader: only to notice disconnects
		for {
			if _, _, err := c.ReadMessage(); err != nil {
				c.Close()
				return
			}
		}
	}()
	ping := time.NewTicker(25 * time.Second)
	defer ping.Stop()
	for {
		select {
		case b := <-ch:
			c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if err := c.WriteMessage(websocket.TextMessage, b); err != nil {
				return
			}
		case <-ping.C:
			c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if err := c.WriteMessage(websocket.PingMessage, nil); err != nil {
				log.Printf("ws ping: %v", err)
				return
			}
		}
	}
}
