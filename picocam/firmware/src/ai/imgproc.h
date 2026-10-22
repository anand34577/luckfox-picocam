#pragma once
#include <string>
#include <vector>

#include "aicommon.h"

// Maps a point in model-input space back to frame space.
struct Letterbox {
    RectF roi;  // source region in the frame
    float sx = 1, sy = 1;
    int pad_x = 0, pad_y = 0;
    int out_w = 0, out_h = 0;  // size of the scaled picture inside the model input (the rest is padding)
    float to_frame_x(float mx) const { return roi.x1 + (mx - pad_x) / sx; }
    float to_frame_y(float my) const { return roi.y1 + (my - pad_y) / sy; }
    RectF to_frame(const RectF &r) const { return {to_frame_x(r.x1), to_frame_y(r.y1), to_frame_x(r.x2), to_frame_y(r.y2)}; }
};

// Where `roi` lands in a dst_w x dst_h model input: the source region (snapped to even pixels for NV12), the scale
// and the padding. Every preprocessing path (CPU, RGA) uses this, so boxes map back identically.
Letterbox plan_letterbox(const Frame &f, RectF roi, int dst_w, int dst_h, bool keep_aspect);

// Crop `roi` from an NV12 frame, scale it into dst (dst_w x dst_h RGB/BGR, row stride in bytes).
// keep_aspect pads with grey (YOLO letterbox); otherwise the crop is stretched.
Letterbox nv12_to_rgb(const Frame &f, RectF roi, uint8_t *dst, int dst_w, int dst_h, int dst_stride,
                      bool keep_aspect, bool bgr = false);

// Warps a face so that its 5 landmarks (eyes, nose, mouth corners; x,y pairs in frame pixels) land on the standard
// 112x112 recognition template (scaled to dst_w x dst_h). false when the landmarks are unusable.
bool align_face(const Frame &f, const float lm[10], uint8_t *dst, int dst_w, int dst_h, int dst_stride, bool bgr);

// JPEG of a frame region, downscaled to at most max_w wide (0 = no limit). Small crops are upscaled to min_w.
bool jpeg_encode(const Frame &f, RectF roi, int max_w, int min_w, int quality, std::vector<uint8_t> &out);

// Drawing directly on an NV12 buffer (overlay stream).
void draw_box(uint8_t *nv12, int w, int h, int stride, const RectF &r, OverlayColor c, int thick);
void draw_label(uint8_t *nv12, int w, int h, int stride, int x, int y, const std::string &text, OverlayColor c);
