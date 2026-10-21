#pragma once
// Which Luckfox Pico (or compatible RV1103/RV1106 board) are we running on? Everything here is detected at run time, so one
// firmware package serves Pico, Pico Mini/Plus/Pro/Max/Ultra, Pico Zero and friends; nothing is keyed on a board name.
#include <string>

#include "config.h"

namespace board {

struct Info {
    std::string model;        // /proc/device-tree/model
    std::string soc;          // "RV1103", "RV1106" or "RV110x"
    std::string boot;         // "spi-nand", "emmc", "sd" or "unknown"
    long ram_mb = 0;          // installed DRAM, rounded up to 64/128/256/512
    bool flash_limited = false;  // boot flash is raw NAND: frequent small writes wear it, keep chatty data in RAM
    bool ethernet = false;
    bool wifi = false;
    std::string sensor;       // as reported by the ISP, empty until isp::start()
    int sensor_w = 0, sensor_h = 0, sensor_fps = 0;
};

const Info &info();                                      // detected once (the hardware does not change while running)
void set_sensor(const char *name, int w, int h, int fps);  // called by the ISP once it knows the camera module
int max_width(const char *stream);                       // largest picture the sensor/ISP can deliver for "main" / "sub"
int max_height(const char *stream);
json to_json();
// SD card block device (partition 1, else the whole device) or "" if no card is present. Works whatever mmcblk number the
// board's device tree gives the slot, and never returns the eMMC or the card the system booted from.
std::string find_sd_device();
// media-memory (CMA) sizes worth offering on this much RAM
json cma_options();

}  // namespace board
