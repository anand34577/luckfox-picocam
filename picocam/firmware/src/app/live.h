#pragma once
#include <string>

#include "core/config.h"

// Browser live view and the realtime JSON bus.
//   /ws/live?stream=main|sub : text {"type":"init",codec,width,height} then binary frames
//                              [u8 flags(1=key)][u64 pts_us BE][Annex-B access unit]  -> WebCodecs in the UI
//   /ws/events               : JSON messages {"topic": "...", ...} (detections, events, status)
namespace live {

void register_routes();
// thread-safe; delivered to every /ws/events client subscribed to `topic` (or to all when they asked for "*")
void publish(const std::string &topic, const json &msg);
int viewers();

}  // namespace live
