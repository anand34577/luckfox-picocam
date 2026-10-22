package main

import "time"

// initAccess prepares the signing secret and the small in-memory helpers of the access layer.
func (a *App) initAccess() {
	a.alerts = newAlerts(a)
	a.janitor = &Janitor{a: a}
	a.secret = a.store.secret()
	a.touched = map[int64]time.Time{}
	a.limiter = &loginLimiter{fail: map[string][]time.Time{}}
	if a.imageTTL <= 0 {
		a.imageTTL = 24 * time.Hour
	}
}
