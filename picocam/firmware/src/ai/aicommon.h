#pragma once
// Geometry and frame types shared by the AI modules (ported from the edgeai app).
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/util.h"

struct RectF {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float w() const { return x2 - x1; }
    float h() const { return y2 - y1; }
    float area() const { return std::max(0.f, w()) * std::max(0.f, h()); }
    float cx() const { return (x1 + x2) * 0.5f; }
    float cy() const { return (y1 + y2) * 0.5f; }
    bool empty() const { return w() <= 1 || h() <= 1; }
    // grow by fraction of width/height on each side
    RectF expand(float fx, float fy) const { return {x1 - w() * fx, y1 - h() * fy, x2 + w() * fx, y2 + h() * fy}; }
    RectF clip(float W, float H) const {
        return {std::max(0.f, x1), std::max(0.f, y1), std::min(W, x2), std::min(H, y2)};
    }
};

float iou(const RectF &a, const RectF &b);

struct Det {
    RectF box;
    float score = 0;
    int cls = 0;
    bool has_lm = false;
    float lm[10] = {0};  // 5 landmarks (x,y), face only
};

// keeps highest-score boxes; per_class: only suppress boxes of the same class
void nms(std::vector<Det> &dets, float thr, bool per_class = true);

// One NV12 frame. `buf` owns the pixels when the frame must outlive the capture buffer.
struct Frame {
    const uint8_t *nv12 = nullptr;
    int w = 0, h = 0, stride = 0;
    int fd = -1;        // dma-buf holding nv12 (media memory, the RGA can read it); -1: ordinary memory, CPU only
    int64_t ts_ms = 0;  // wall clock at capture
    int64_t pts_us = 0;  // encoder-clock time of the picture (matches the video stream packets)
    std::shared_ptr<std::vector<uint8_t>> buf;
    size_t bytes() const { return (size_t)stride * h * 3 / 2; }
};

// Deep copy (tightly packed).
Frame clone_frame(const Frame &f);

// Colours for boxes burnt into the sub stream.
enum OverlayColor : uint8_t { C_GREEN, C_YELLOW, C_RED, C_BLUE, C_WHITE, C_ORANGE };
