#pragma once
#include "core/config.h"

// Pixel-change motion detection on the sub stream (no NPU): an 80x45 grid of block averages compared with a
// slowly adapting background. Works alongside (or instead of) AI detection.
//   settings "motion": enabled, sensitivity 0..100, min_area_pct, cooldown_s, zones (normalised polygons)
namespace motion {

void init();
json status();
// true if motion was seen within the last `ms` (lets the NPU run only while something moves)
bool active_within(int ms);

}  // namespace motion
