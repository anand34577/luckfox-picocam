package main

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// ------------------------------------------------------------------ errors and envelopes

// apiError writes {"error":{"code","message"}} so integrators can branch on a stable code.
func apiError(w http.ResponseWriter, status int, code, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{"code": code, "message": msg}})
}

func writeStatus(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

// Page is the list envelope of every integration endpoint.
type Page[T any] struct {
	Data       []T    `json:"data"`
	Total      int64  `json:"total"`
	NextCursor *int64 `json:"next_cursor"` // pass as ?cursor= for the next page; null on the last page
}

// ------------------------------------------------------------------ small parsers

func parseID(s string) (int64, bool) {
	id, err := strconv.ParseInt(s, 10, 64)
	return id, err == nil && id > 0
}

func limitParam(q string, def, max int) int {
	n, err := strconv.Atoi(q)
	if err != nil || n <= 0 {
		return def
	}
	if n > max {
		return max
	}
	return n
}

// parseTime accepts epoch milliseconds, epoch seconds or RFC 3339 and returns epoch milliseconds (0 = unset).
func parseTime(s string) (int64, bool) {
	if s == "" {
		return 0, true
	}
	if n, err := strconv.ParseInt(s, 10, 64); err == nil {
		if n < 100_000_000_000 { // seconds
			n *= 1000
		}
		return n, true
	}
	t, err := time.Parse(time.RFC3339, s)
	if err != nil {
		return 0, false
	}
	return t.UnixMilli(), true
}

func isoTime(ms int64) string {
	if ms <= 0 {
		return ""
	}
	return time.UnixMilli(ms).UTC().Format(time.RFC3339)
}

// normPlate upper-cases and strips everything but letters and digits, matching what the devices store.
func normPlate(s string) string {
	return strings.Map(func(r rune) rune {
		switch {
		case r >= 'a' && r <= 'z':
			return r - 32
		case r >= 'A' && r <= 'Z', r >= '0' && r <= '9':
			return r
		}
		return -1
	}, s)
}

func truthy(s string) bool { return s == "1" || strings.EqualFold(s, "true") }
