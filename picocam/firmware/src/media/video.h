#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/config.h"

// Camera -> encoders:
//   VI ch0 (full sensor res) --bind--> VENC0 main stream (+ VENC2 JPEG snapshots in combo mode)
//   VI ch1 (scaled)          --app---> frame hooks (AI, motion, exposure stats, overlay) --> VENC1 sub stream
// Encoded packets go to g_main_stream / g_sub_stream (see stream.h).
namespace video {

// A sub-stream frame handed to hooks. Pixels stay valid only during the hook call.
struct FrameView {
    uint8_t *nv12;
    int w, h, stride;
    int64_t pts_us;  // monotonic capture time
    int fd = -1;     // the capture buffer as a dma-buf (media memory: the RGA can read it), -1 if unknown
};
// Hooks run on the capture thread for every sub-stream frame, in registration order: keep them short.
// Return true if the hook modified the pixels (overlay drawing), so caches are flushed before encoding.
using FrameHook = std::function<bool(FrameView &)>;
int add_frame_hook(FrameHook h);
void remove_frame_hook(int id);

// Media memory (CMA) is allocated first-fit and the encoders and the NPU both need large contiguous blocks.
// To make them coexist: reserve a hole for the AI models, start ISP + video, release the hole, load the models.
void sys_init();
bool cma_reserve(const std::vector<size_t> &blocks);  // one hole per block, largest first
void cma_release();
bool start(const json &video, const json &osd, const json &masks);
void stop();
bool restart(const json &video, const json &osd, const json &masks);
bool running();
// how long the video path looks stuck (0 = healthy): an encoder that stopped producing packets, a start/stop
// that has not returned, or a start that failed. Used by the hardware watchdog.
int64_t stalled_ms();

// live changes that don't need an encoder restart
void apply_osd(const json &osd);
void apply_masks(const json &masks);
void request_idr(int stream);

// Hardware JPEG of the main stream (full resolution). Blocks up to timeout_ms.
bool snapshot(std::string &jpeg, int timeout_ms = 1500);

json status();

// validates a "video" settings section; returns "" or an error
std::string validate(json &video);

}  // namespace video
