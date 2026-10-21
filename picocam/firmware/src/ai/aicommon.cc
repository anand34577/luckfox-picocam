#include "aicommon.h"

#include <cstring>

float iou(const RectF &a, const RectF &b) {
    float iw = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
    float ih = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    if (iw <= 0 || ih <= 0) return 0.f;
    float inter = iw * ih;
    float u = a.area() + b.area() - inter;
    return u <= 0 ? 0.f : inter / u;
}

void nms(std::vector<Det> &dets, float thr, bool per_class) {
    std::sort(dets.begin(), dets.end(), [](const Det &a, const Det &b) { return a.score > b.score; });
    std::vector<Det> keep;
    std::vector<bool> removed(dets.size(), false);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (removed[i]) continue;
        keep.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (removed[j] || (per_class && dets[j].cls != dets[i].cls)) continue;
            if (iou(dets[i].box, dets[j].box) > thr) removed[j] = true;
        }
    }
    dets.swap(keep);
}

Frame clone_frame(const Frame &f) {
    Frame c;
    c.w = f.w;
    c.h = f.h;
    c.stride = f.w;
    c.ts_ms = f.ts_ms;
    c.buf = std::make_shared<std::vector<uint8_t>>((size_t)f.w * f.h * 3 / 2);
    uint8_t *dst = c.buf->data();
    for (int y = 0; y < f.h * 3 / 2; ++y) memcpy(dst + (size_t)y * f.w, f.nv12 + (size_t)y * f.stride, f.w);
    c.nv12 = dst;
    return c;
}
