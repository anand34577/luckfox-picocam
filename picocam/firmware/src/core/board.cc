#include "board.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "util.h"

namespace board {

static std::mutex g_mu;
static Info g_info;
static bool g_ready = false;

static std::string cmdline_value(const std::string &cmd, const char *key) {
    size_t p = cmd.find(key);
    if (p == std::string::npos) return "";
    p += strlen(key);
    size_t e = cmd.find_first_of(" \n", p);
    return cmd.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static Info detect() {
    Info i;
    std::string s;
    if (read_file("/proc/device-tree/model", s)) i.model = trim(std::string(s.c_str()));  // strip the trailing NUL
    if (i.model.empty()) i.model = "Luckfox Pico";

    std::string compat;
    read_file("/proc/device-tree/compatible", compat);  // NUL-separated strings
    for (char &c : compat) if (c == 0) c = ' ';
    compat = to_lower(compat);
    i.soc = compat.find("rv1106") != std::string::npos ? "RV1106" : compat.find("rv1103") != std::string::npos ? "RV1103" : "RV110x";

    // MemTotal excludes the kernel and the media (CMA) pool; round the sum up to the DRAM chip size
    long kb = std::max(0L, meminfo_kb("MemTotal")) + std::max(0L, meminfo_kb("CmaTotal"));
    long mb = kb / 1024, sz = 64;
    while (sz < mb && sz < 2048) sz *= 2;
    i.ram_mb = sz;

    std::string cmd;
    read_file("/proc/cmdline", cmd);
    std::string media = to_lower(cmdline_value(cmd, "storagemedia="));
    std::string mtd;
    read_file("/proc/mtd", mtd);
    mtd = to_lower(mtd);
    bool has_mtd = mtd.find("\"") != std::string::npos;  // /proc/mtd lists named partitions only when raw flash is present
    if (media.find("sd") == 0) i.boot = "sd";
    else if (media.find("emmc") == 0 || file_exists("/dev/mmcblk0boot0")) i.boot = "emmc";
    else if (media.find("mtd") == 0 || media.find("nand") != std::string::npos || has_mtd) i.boot = "spi-nand";
    else i.boot = "unknown";
    // NOR/NAND on MTD (UBIFS) wears with small frequent writes; eMMC and SD have their own wear levelling
    i.flash_limited = i.boot == "spi-nand";

    i.ethernet = file_exists("/sys/class/net/eth0");
    i.wifi = file_exists("/sys/class/net/wlan0");
    return i;
}

const Info &info() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ready) {
        g_info = detect();
        g_ready = true;
    }
    return g_info;
}

void set_sensor(const char *name, int w, int h, int fps) {
    info();
    std::lock_guard<std::mutex> lk(g_mu);
    g_info.sensor = name ? name : "";
    g_info.sensor_w = w;
    g_info.sensor_h = h;
    g_info.sensor_fps = fps;
}

// The main channel is the ISP's full output; the sub channel is scaled down and never above 1080p.
int max_width(const char *stream) {
    const Info &i = info();
    int w = i.sensor_w > 0 ? i.sensor_w : 2304;  // 2304 = the SC3336 default until the ISP has said otherwise
    return !strcmp(stream, "main") ? w : std::min(w, 1920);
}

int max_height(const char *stream) {
    const Info &i = info();
    int h = i.sensor_h > 0 ? i.sensor_h : 1296;
    return !strcmp(stream, "main") ? h : std::min(h, 1080);
}

std::string find_sd_device() {
    std::string cmd;
    read_file("/proc/cmdline", cmd);
    bool boot_sd = cmd.find("storagemedia=sd") != std::string::npos;
    for (int n = 0; n < 4; ++n) {
        std::string base = "/sys/block/mmcblk" + std::to_string(n), type;
        if (!file_exists(base)) continue;
        // "SD" for cards, "MMC" for the on-board eMMC; boot0/boot1 are not under /sys/block/mmcblkN itself
        if (!read_file(base + "/device/type", type)) continue;
        if (trim(type) != "SD") continue;
        if (boot_sd && n == 0) continue;  // the system card: handled as "boot_sd" by the storage module
        std::string dev = "/dev/mmcblk" + std::to_string(n);
        if (file_exists(dev + "p1")) return dev + "p1";
        if (file_exists(dev)) return dev;
    }
    return "";
}

json cma_options() {
    long ram = info().ram_mb;
    if (ram >= 256) return {24, 28, 32, 48, 64};
    if (ram >= 128) return {24, 28, 32, 40};
    return {24, 28, 32};
}

json to_json() {
    const Info &i = info();
    return {{"model", i.model},
            {"soc", i.soc},
            {"ram_mb", i.ram_mb},
            {"boot", i.boot},
            {"flash_limited", i.flash_limited},
            {"ethernet", i.ethernet},
            {"wifi", i.wifi},
            {"sensor", i.sensor},
            {"sensor_width", i.sensor_w},
            {"sensor_height", i.sensor_h},
            {"sensor_fps", i.sensor_fps}};
}

}  // namespace board
