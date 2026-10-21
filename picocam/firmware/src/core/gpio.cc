#include "gpio.h"

#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>

#include "util.h"

namespace gpio {

// sysfs wants a plain write, not write_file() (temp file + rename)
static bool sysfs_write(const std::string &path, const std::string &v) {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return false;
    fputs(v.c_str(), f);
    fclose(f);
    return true;
}

bool set(int pin, bool value) {
    if (pin < 0 || pin > 511) return false;
    std::string base = "/sys/class/gpio/gpio" + std::to_string(pin);
    if (!file_exists(base + "/value")) {
        sysfs_write("/sys/class/gpio/export", std::to_string(pin));
        usleep(100 * 1000);
        sysfs_write(base + "/direction", "out");
    }
    return sysfs_write(base + "/value", value ? "1" : "0");
}

void pulse(int pin, bool active_high, int ms) {
    if (!set(pin, active_high)) return;
    std::thread([pin, active_high, ms] {
        usleep((useconds_t)std::max(50, std::min(60000, ms)) * 1000);
        set(pin, !active_high);
    }).detach();
}

}  // namespace gpio
