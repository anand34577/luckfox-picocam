#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aicommon.h"
#include "core/config.h"
#include "imgproc.h"
#include "npu.h"

// "Raw" stage: runs ANY model on an object (or the whole picture) and ships the raw output tensors to an HTTP endpoint,
// which decodes them and may answer {"attributes": {"name": "value", ...}}. Those attributes join the object's other
// attributes (and can drive rules). The escape hatch for models the camera cannot decode itself and for logic that
// belongs on a server.
struct RawModel {
    NpuModel model;
    bool bgr = false;
    bool load(const std::string &path) { return model.load(path); }
    // runs the model on the region and returns the output tensors as JSON (dims, type, scale, zero point, base64 data)
    json run(const Frame &f, RectF roi);
    // average and best time of n inference runs (input conversion included), milliseconds
    void bench(const Frame &f, int n, double *avg_ms, double *min_ms);
};

namespace rawstage {

// queue a request (non-blocking; the oldest is dropped when the sender is behind)
void send(const std::string &url, const std::string &verify, const std::string &pin, int track_id, json body);
// answers received since the last call: (track id, attributes)
std::vector<std::pair<int, json>> take_results();
void stop();

}  // namespace rawstage
