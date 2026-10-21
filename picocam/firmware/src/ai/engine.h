#pragma once
#include <string>

#include "core/config.h"

// AI engine: runs the configured pipeline on sub-stream frames.
//
//   detector (YOLOv5/v8/11 or RetinaFace)  ->  tracker  ->  per-object stages  ->  rules  ->  events
//
// Stages run on each tracked object of the listed classes, at most every interval_ms:
//   detect   : find a part inside the object (e.g. a number plate), may chain another stage ("then")
//   ocr      : read text (CTC model), voted over the track, optional plate-format correction
//   classify : label the object crop (vehicle type, colour, helmet, ...), voted over the track
//   embed    : feature vector (face recognition on a server)
// Rules (zones / lines / dwell / counts / attributes) turn tracks into events.
namespace ai {

void init();  // registers settings hooks and the frame hook
// Starts the video pipeline and the AI in the order that lets both get contiguous media memory.
bool start_with_video();
// restart video (and the AI) after a video settings change; rolls back to `before` if the new settings don't fit
void restart_video(const json &before);
void shutdown();
json status();
void register_routes();

}  // namespace ai
