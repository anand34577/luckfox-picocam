#pragma once
#include <vector>

#include "aicommon.h"

struct Track {
    int id = 0;
    RectF box;
    Det det;  // latest matched detection (landmarks etc.)
    int cls = 0;
    float score = 0;
    int hits = 0, misses = 0;
    float vx = 0, vy = 0;  // box centre velocity, pixels per second
    int64_t first_ms = 0, last_ms = 0;
    bool confirmed() const { return hits >= 2; }
    bool visible() const { return misses == 0; }
};

// IoU tracker with constant-velocity prediction and global best-pair assignment: cheap enough for the A7 core.
class IouTracker {
public:
    float iou_thr = 0.25f;
    int max_misses = 10;  // AI frames a track survives without a match

    // Updates tracks with this frame's detections; returns ids of tracks that were dropped.
    std::vector<Track> update(const std::vector<Det> &dets, int64_t now);
    std::vector<Track> &tracks() { return tracks_; }

private:
    std::vector<Track> tracks_;
    int next_id_ = 1;
};
