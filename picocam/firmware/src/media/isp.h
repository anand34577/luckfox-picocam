#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/config.h"

// Camera ISP (Rockchip AIQ): 3A start/stop, image settings and the Smart Exposure controller.
namespace isp {

bool start(const std::string &iq_dir, int fps);
void stop();
bool running();

// validates the "image" section (applies scene presets, clamps ranges); returns "" or a message
std::string validate(json &image);

// apply the whole "image" settings section (cheap: only changed values reach the AIQ)
void apply(const json &image);

// A sub-stream NV12 frame (chroma right after the luma plane). The Smart Exposure controller samples its luma, the
// health monitor its chroma, a few times a second.
void feed_frame(const uint8_t *nv12, int w, int h, int stride);

// Called (from the ISP controller thread) when the picture is broken in a way only a pipeline restart cures:
// green cast, frozen AE, AIQ not answering. The callback must not restart synchronously. Rate-limited with back-off.
void on_fault(std::function<void(const std::string &why)> cb);

// Privacy masks (normalised x,y,w,h): kept out of the exposure statistics.
void set_masks(const json &masks);

// From the AI: boxes of the objects being tracked (normalised x1,y1,x2,y2) and the fastest one's speed in
// sub-frame pixels per second. Smart Exposure can meter on them and cap the shutter so they don't blur.
void set_subject(const std::vector<std::array<float, 4>> &boxes, float speed_px_s);

// live state for the UI: exposure, gain, luma, histogram, what the controller is doing
json status();

// Captures the picture at three exposure offsets (-1, 0, +1 EV) with histograms, for tuning. Blocks ~6 s.
json exposure_test();

}  // namespace isp
