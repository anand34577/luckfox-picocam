#pragma once
#include <string>

#include "core/config.h"

// SD card and recording.
//   Pico (boots from SD): recordings go to the SD card's large root partition.
//   Pico Plus (boots from SPI NAND): an inserted SD card is mounted at /mnt/sdcard (formatting from the UI).
// Recording modes: off | continuous | events (pre/post buffer around events) | schedule (weekly hour grid).
// Files: <root>/picocam/rec/YYYY-MM-DD/HHMMSS-HHMMSS_<stream>[_e].mp4 (fragmented MP4).
namespace storage {

void start();
void stop();
std::string root();  // "" when no storage is available
json status();
void register_routes();
// an event happened: event/schedule recording keeps recording for post_s
void trigger(const std::string &reason);

}  // namespace storage
