#include "tracker.h"

#include <algorithm>

std::vector<Track> IouTracker::update(const std::vector<Det> &dets, int64_t now) {
    std::vector<bool> used(dets.size(), false);
    std::vector<bool> matched(tracks_.size(), false);

    // Every (track, detection) pair that overlaps enough, scored against where the track *should* be now: its box moved
    // by its velocity. A fast vehicle at a low AI frame rate no longer loses its identity between frames.
    struct Cand {
        float iou;
        size_t t, d;
    };
    std::vector<Cand> cands;
    for (size_t ti = 0; ti < tracks_.size(); ++ti) {
        const Track &t = tracks_[ti];
        float dt = std::min(1.0f, std::max(0.f, (now - t.last_ms) / 1000.f));
        float dx = t.vx * dt, dy = t.vy * dt;
        // predict from the last *detection* (t.box is smoothed for display and lags behind a moving object)
        const RectF &lb = t.det.box;
        RectF pb{lb.x1 + dx, lb.y1 + dy, lb.x2 + dx, lb.y2 + dy};
        for (size_t d = 0; d < dets.size(); ++d) {
            float v = iou(pb, dets[d].box);
            if (dets[d].cls != t.cls) v *= 0.5f;  // a different class must overlap clearly better to take over a track
            if (v > iou_thr) cands.push_back({v, ti, d});
        }
    }
    // best pairs first, each track and detection used once (global, not in track order)
    std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.iou > b.iou; });
    for (auto &c : cands) {
        if (matched[c.t] || used[c.d]) continue;
        matched[c.t] = used[c.d] = true;
        Track &t = tracks_[c.t];
        const Det &d = dets[c.d];
        float dt = std::max(0.02f, (now - t.last_ms) / 1000.f);
        // velocity of the box centre, smoothed
        float vx = (d.box.cx() - t.det.box.cx()) / dt, vy = (d.box.cy() - t.det.box.cy()) / dt;
        t.vx = t.hits > 1 ? t.vx * 0.5f + vx * 0.5f : vx;
        t.vy = t.hits > 1 ? t.vy * 0.5f + vy * 0.5f : vy;
        // light smoothing keeps overlay boxes from jittering
        const float a = 0.6f;
        t.box = {a * d.box.x1 + (1 - a) * t.box.x1, a * d.box.y1 + (1 - a) * t.box.y1,
                 a * d.box.x2 + (1 - a) * t.box.x2, a * d.box.y2 + (1 - a) * t.box.y2};
        t.det = d;
        t.cls = d.cls;
        t.score = d.score;
        ++t.hits;
        t.misses = 0;
        t.last_ms = now;
    }
    for (size_t ti = 0; ti < tracks_.size(); ++ti)
        if (!matched[ti]) {
            ++tracks_[ti].misses;
            // keep coasting along the last velocity so the box is where the object will reappear
            // (last_ms is not touched: the prediction horizon is measured from the last real observation)
        }
    for (size_t d = 0; d < dets.size(); ++d) {
        if (used[d]) continue;
        Track t;
        t.id = next_id_++;
        t.box = dets[d].box;
        t.det = dets[d];
        t.cls = dets[d].cls;
        t.score = dets[d].score;
        t.hits = 1;
        t.first_ms = t.last_ms = now;
        tracks_.push_back(t);
    }
    std::vector<Track> lost;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        // unconfirmed one-frame blips die immediately
        if (it->misses > max_misses || (!it->confirmed() && it->misses > 1)) {
            lost.push_back(*it);
            it = tracks_.erase(it);
        } else {
            ++it;
        }
    }
    return lost;
}
