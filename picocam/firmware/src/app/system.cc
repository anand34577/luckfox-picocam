#include "system.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "core/board.h"
#include "core/log.h"
#include "core/util.h"

#ifndef PICOCAM_VERSION
#define PICOCAM_VERSION "dev"
#endif

namespace sys {

static const char *TAG = "system";
static std::atomic<bool> g_run{false};
static std::thread g_th;
static std::mutex g_mu;
static float g_cpu = 0;
static int64_t g_start_ms = now_ms();
static std::atomic<int64_t> g_ntp_ok_ms{0};
static std::atomic<bool> g_ntp_now{false};

const char *version() { return PICOCAM_VERSION; }

std::string board_model() { return board::info().model; }

// ---------------------------------------------------------------- stats

static bool read_cpu(uint64_t &idle, uint64_t &total) {
    std::string s;
    if (!read_file("/proc/stat", s)) return false;
    unsigned long long u, n, sy, id, io, irq, sirq, st;
    if (sscanf(s.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq, &st) < 4)
        return false;
    idle = id + io;
    total = u + n + sy + id + io + irq + sirq + st;
    return true;
}

static float temperature() {
    std::string s;
    for (const char *p : {"/sys/class/thermal/thermal_zone0/temp", "/sys/class/hwmon/hwmon0/temp1_input"})
        if (read_file(p, s)) return atoi(s.c_str()) / 1000.f;
    return -1;
}

json stats() {
    std::string up;
    read_file("/proc/uptime", up);
    std::string la;
    read_file("/proc/loadavg", la);
    float l1 = 0, l5 = 0, l15 = 0;
    sscanf(la.c_str(), "%f %f %f", &l1, &l5, &l15);
    long total = meminfo_kb("MemTotal"), avail = meminfo_kb("MemAvailable");
    long cma_total = meminfo_kb("CmaTotal"), cma_used = meminfo_kb("CmaAllocated");
    std::string rss;
    long rss_kb = 0;
    if (read_file("/proc/self/status", rss)) {
        size_t p = rss.find("VmRSS:");
        if (p != std::string::npos) rss_kb = atol(rss.c_str() + p + 6);
    }
    std::string npu;
    read_file("/sys/kernel/debug/rknpu/load", npu);
    int npu_load = -1;
    size_t p = npu.find("Core0:");
    if (p != std::string::npos) npu_load = atoi(npu.c_str() + p + 6);
    std::lock_guard<std::mutex> lk(g_mu);
    return {{"cpu_pct", std::round(g_cpu)},
            {"load", {l1, l5, l15}},
            {"mem_total_kb", total},
            {"mem_avail_kb", avail},
            {"media_mem_total_kb", cma_total},
            {"media_mem_used_kb", cma_used},
            {"app_rss_kb", rss_kb},
            {"temp_c", std::round(temperature() * 10.0) / 10.0},
            {"npu_load_pct", npu_load},
            {"uptime_s", (int64_t)atof(up.c_str())},
            {"app_uptime_s", (now_ms() - g_start_ms) / 1000},
            {"time_ms", wall_ms()},
            {"ntp_synced", g_ntp_ok_ms.load() > 0}};
}

json device_info() {
    json d = g_settings.get("device");
    std::string kv;
    static std::string uname = trim(run_cmd("uname -r"));  // fixed for the life of the process
    std::string mac = iface_mac("eth0");
    if (mac.empty()) mac = iface_mac("usb0");
    return {{"name", d.value("name", "PicoCam")},
            {"id", d.value("id", "")},
            {"location", d.value("location", "")},
            {"model", board_model()},
            {"soc", "Rockchip " + board::info().soc},
            {"sensor", board::info().sensor.empty() ? "unknown" : board::info().sensor + " (" + std::to_string(board::info().sensor_w) + "x" + std::to_string(board::info().sensor_h) + ")"},
            {"board", board::to_json()},
            {"firmware", version()},
            {"kernel", uname},
            {"mac", mac},
            {"serial", d.value("id", "")}};
}

static std::string iface_mask(const std::string &ifname) {
    std::string out = run_cmd("ip -4 addr show " + ifname + " 2>/dev/null");
    size_t p = out.find("inet ");
    if (p == std::string::npos) return "";
    size_t s = out.find('/', p);
    if (s == std::string::npos) return "";
    int bits = atoi(out.c_str() + s + 1);
    uint32_t m = bits ? 0xffffffffu << (32 - bits) : 0;
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", m >> 24, (m >> 16) & 255, (m >> 8) & 255, m & 255);
    return buf;
}

static json network_status_uncached();

// `ip` / route lookups fork a shell: serve a 5 s old answer instead of forking on the event loop per request
json network_status() {
    static std::mutex mu;
    static json cache;
    static int64_t at = -100000;
    std::lock_guard<std::mutex> lk(mu);
    if (now_ms() - at > 5000) {
        cache = network_status_uncached();
        at = now_ms();
    }
    return cache;
}

static json network_status_uncached() {
    json ifs = json::array();
    for (const char *n : {"eth0", "usb0", "wlan0"}) {
        if (!file_exists(std::string("/sys/class/net/") + n)) continue;
        std::string carrier, speed;
        read_file(std::string("/sys/class/net/") + n + "/carrier", carrier);
        read_file(std::string("/sys/class/net/") + n + "/speed", speed);
        ifs.push_back({{"name", n},
                       {"ip", iface_ipv4(n)},
                       {"netmask", iface_mask(n)},
                       {"mac", iface_mac(n)},
                       {"link", trim(carrier) == "1"},
                       {"speed_mbps", atoi(speed.c_str())}});
    }
    std::string route = run_cmd("ip route show default 2>/dev/null");
    std::string gw;
    size_t p = route.find("via ");
    if (p != std::string::npos) gw = split(route.substr(p + 4), ' ')[0];
    std::string resolv;
    read_file("/etc/resolv.conf", resolv);
    json dns = json::array();
    for (auto &l : split(resolv, '\n'))
        if (starts_with(trim(l), "nameserver")) dns.push_back(trim(trim(l).substr(10)));
    char host[64] = {0};
    gethostname(host, sizeof(host) - 1);
    return {{"interfaces", ifs}, {"gateway", gw}, {"dns", dns}, {"hostname", host}, {"primary", primary_iface()}};
}

// ---------------------------------------------------------------- network

static bool valid_ip(const std::string &s) {
    in_addr a;
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

std::string validate_network(json &n) {
    std::string mode = jget<std::string>(n, "mode", "dhcp");
    if (mode != "dhcp" && mode != "static") return "mode must be dhcp or static";
    if (mode == "static") {
        if (!valid_ip(jget<std::string>(n, "ip", ""))) return "invalid IP address";
        if (!valid_ip(jget<std::string>(n, "netmask", ""))) return "invalid netmask";
        std::string gw = jget<std::string>(n, "gateway", "");
        if (!gw.empty() && !valid_ip(gw)) return "invalid gateway";
    }
    if (n.contains("dns"))
        for (auto &d : n["dns"])
            if (!d.is_string() || (!d.get<std::string>().empty() && !valid_ip(d.get<std::string>()))) return "invalid DNS server";
    // address of the USB (RNDIS) link: the PC at the other end of the cable needs an address in the same subnet
    std::string uip = jget<std::string>(n, "usb_ip", "172.32.0.93"), umask = jget<std::string>(n, "usb_netmask", "255.255.255.0");
    in_addr ua;
    if (!valid_ip(uip) || inet_pton(AF_INET, uip.c_str(), &ua) != 1 || (ntohl(ua.s_addr) >> 24) == 0 || (ntohl(ua.s_addr) >> 24) >= 224)
        return "invalid USB IP address";
    if (!valid_ip(umask)) return "invalid USB netmask";
    std::string h = jget<std::string>(n, "hostname", "picocam");
    if (h.empty() || h.size() > 63) return "invalid hostname";
    for (char c : h)
        if (!isalnum((unsigned char)c) && c != '-') return "hostname may contain letters, digits and '-'";
    return "";
}

static int mask_bits(const std::string &m) {
    in_addr a;
    if (inet_pton(AF_INET, m.c_str(), &a) != 1) return 24;
    return __builtin_popcount(ntohl(a.s_addr));
}

// The USB gadget (usb0) appears a little after boot and the vendor scripts give it 172.32.0.93: put the configured address
// on it. Does nothing when it already has that address, so it is cheap to call repeatedly.
void apply_usb(const json &n) {
    if (!file_exists("/sys/class/net/usb0")) return;
    std::string ip = jget<std::string>(n, "usb_ip", "172.32.0.93"), mask = jget<std::string>(n, "usb_netmask", "255.255.255.0");
    if (!valid_ip(ip) || !valid_ip(mask)) return;
    if (iface_ipv4("usb0") == ip && iface_mask("usb0") == mask) return;
    run_cmd("ip link set usb0 up; ip addr flush dev usb0; ip addr add " + ip + "/" + std::to_string(mask_bits(mask)) + " dev usb0");
    LOGI(TAG, "usb0 address %s/%d", ip.c_str(), mask_bits(mask));
}

void apply_network(const json &n) {
    std::string h = jget<std::string>(n, "hostname", "picocam");
    run_cmd("hostname '" + h + "'");
    write_file("/etc/hostname", h + "\n");
    apply_usb(n);
    // Ethernet settings apply on boards that have it (Plus, Pro, Max, Ultra); the USB address was handled above
    if (!file_exists("/sys/class/net/eth0")) {
        LOGI(TAG, "no eth0: Ethernet settings stored, applied when Ethernet exists");
        return;
    }
    std::string mode = jget<std::string>(n, "mode", "dhcp");
    run_cmd("kill $(cat /var/run/udhcpc.eth0.pid 2>/dev/null) 2>/dev/null; killall -q udhcpc 2>/dev/null; true");
    run_cmd("ip link set eth0 up");
    if (mode == "static") {
        std::string ip = jget<std::string>(n, "ip", ""), gw = jget<std::string>(n, "gateway", "");
        run_cmd("ip addr flush dev eth0");
        run_cmd("ip addr add " + ip + "/" + std::to_string(mask_bits(jget<std::string>(n, "netmask", "255.255.255.0"))) +
                " dev eth0");
        if (!gw.empty()) run_cmd("ip route replace default via " + gw + " dev eth0");
        std::string resolv;
        for (auto &d : n.value("dns", json::array()))
            if (d.is_string() && !d.get<std::string>().empty()) resolv += "nameserver " + d.get<std::string>() + "\n";
        if (!resolv.empty()) write_file("/etc/resolv.conf", resolv);
        LOGI(TAG, "eth0 static %s gw %s", ip.c_str(), gw.c_str());
    } else {
        run_cmd("udhcpc -i eth0 -b -p /var/run/udhcpc.eth0.pid >/dev/null 2>&1 &");
        LOGI(TAG, "eth0 DHCP");
    }
}

// ---------------------------------------------------------------- time

std::string validate_time(json &t) {
    std::string tz = jget<std::string>(t, "timezone", "IST-5:30");
    if (tz.empty() || tz.size() > 48) return "invalid timezone";
    for (char c : tz)
        if (!isalnum((unsigned char)c) && !strchr("+-:,./<>", c)) return "invalid timezone";
    std::string srv = jget<std::string>(t, "ntp_server", "pool.ntp.org");
    for (char c : srv)
        if (!isalnum((unsigned char)c) && c != '.' && c != '-') return "invalid NTP server";
    int iv = jget<int>(t, "ntp_interval_min", 60);
    if (iv < 5 || iv > 1440) return "NTP interval must be 5..1440 minutes";
    return "";
}

void apply_time(const json &t) {
    std::string tz = jget<std::string>(t, "timezone", "IST-5:30");
    setenv("TZ", tz.c_str(), 1);
    tzset();
    write_file("/etc/TZ", tz + "\n");
    if (jget<bool>(t, "ntp", true)) g_ntp_now = true;
}

bool set_clock(int64_t ms) {
    timeval tv{(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
    if (settimeofday(&tv, nullptr) != 0) return false;
    run_cmd("hwclock -w -u 2>/dev/null");
    LOGI(TAG, "clock set to %s", iso8601(ms).c_str());
    return true;
}

void ntp_sync_now() { g_ntp_now = true; }

// SNTP (RFC 4330) query; the board's ntpd is the full daemon and not suited for one-shot syncs
static bool sntp_query(const std::string &server, int64_t *server_ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server.c_str(), "123", &hints, &res) != 0 || !res) return false;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    timeval to{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    uint8_t pkt[48] = {0x23};  // LI 0, version 4, mode 3 (client)
    int64_t t0 = wall_ms();
    bool ok = sendto(fd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) == (ssize_t)sizeof(pkt) &&
              recv(fd, pkt, sizeof(pkt), 0) == (ssize_t)sizeof(pkt);
    int64_t t1 = wall_ms();
    close(fd);
    freeaddrinfo(res);
    if (!ok || (pkt[0] & 7) != 4 || pkt[1] == 0) return false;  // must be a server reply with a stratum
    uint32_t sec = (pkt[40] << 24) | (pkt[41] << 16) | (pkt[42] << 8) | pkt[43];
    uint32_t frac = (pkt[44] << 24) | (pkt[45] << 16) | (pkt[46] << 8) | pkt[47];
    int64_t ms = ((int64_t)sec - 2208988800LL) * 1000 + (int64_t)(((uint64_t)frac * 1000) >> 32);
    *server_ms = ms + (t1 - t0) / 2;  // half the round trip
    return true;
}

static void ntp_sync(const std::string &server) {
    int64_t ms = 0;
    for (int i = 0; i < 3 && !sntp_query(server, &ms); ++i) usleep(500 * 1000);
    if (!ms) {
        LOGW(TAG, "NTP sync with %s failed", server.c_str());
        return;
    }
    if (std::llabs(ms - wall_ms()) > 500) set_clock(ms);
    g_ntp_ok_ms = wall_ms();
    LOGI(TAG, "time synced with %s", server.c_str());
}

// ---------------------------------------------------------------- sampler thread

static std::thread g_ntp_th;

static void ntp_loop() {
    int64_t next_ntp = now_ms() + 5000;
    while (g_run) {
        for (int i = 0; i < 10 && g_run && !g_ntp_now; ++i) usleep(100 * 1000);
        json t = g_settings.get("time");
        if (jget<bool>(t, "ntp", true) && (g_ntp_now || now_ms() >= next_ntp)) {
            g_ntp_now = false;
            ntp_sync(jget<std::string>(t, "ntp_server", "pool.ntp.org"));
            // retry soon while never synced (network may come up late)
            next_ntp = now_ms() + (g_ntp_ok_ms ? jget<int>(t, "ntp_interval_min", 60) * 60000 : 120000);
        }
    }
}

static void loop() {
    int usb_tick = 0;
    uint64_t pidle = 0, ptotal = 0;
    read_cpu(pidle, ptotal);
    while (g_run) {
        for (int i = 0; i < 20 && g_run; ++i) usleep(100 * 1000);
        // the vendor scripts may bring usb0 up after us: keep the configured address (not while a change awaits confirmation)
        if (++usb_tick % 5 == 0 && !network_pending_s()) apply_usb(g_settings.get("network"));
        uint64_t idle, total;
        if (read_cpu(idle, total) && total > ptotal) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_cpu = 100.f * (1.f - (float)(idle - pidle) / (float)(total - ptotal));
            pidle = idle;
            ptotal = total;
        }
    }
}

void start() {
    apply_time(g_settings.get("time"));
    g_run = true;
    g_th = std::thread(loop);
    g_ntp_th = std::thread(ntp_loop);
}

void stop() {
    g_run = false;
    if (g_th.joinable()) g_th.join();
    if (g_ntp_th.joinable()) g_ntp_th.join();
}

// ---------------------------------------------------------------- media memory (U-Boot env)

static const char *kEnvDev = "/dev/block/by-name/env";
static const size_t kEnvSize = 32 * 1024;

static uint32_t crc32(const uint8_t *d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// reads the env and checks its CRC; returns the offset of the "NN" in rk_dma_heap_cma=NNM or -1
static long env_load(std::string &env, std::string *err) {
    if (!file_exists(kEnvDev)) {
        *err = "the boot environment cannot be edited from Linux on this board (SPI-NAND boot). Change rk_dma_heap_cma in the "
               "SDK's env and reflash, or run `fw_setenv` if your image ships it. See docs/BOARDS.md";
        return -1;
    }
    std::string all;
    if (!read_file(kEnvDev, all) || all.size() < kEnvSize) {
        *err = "cannot read boot environment";
        return -1;
    }
    env = all.substr(0, kEnvSize);
    const uint8_t *d = (const uint8_t *)env.data();
    uint32_t stored = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
    if (crc32(d + 4, kEnvSize - 4) != stored) {
        *err = "boot environment checksum mismatch: not touching it";
        return -1;
    }
    size_t p = env.find("rk_dma_heap_cma=");
    if (p == std::string::npos) {
        *err = "rk_dma_heap_cma not found in the boot environment";
        return -1;
    }
    p += 16;
    if (p + 3 > env.size() || !isdigit((unsigned char)env[p]) || !isdigit((unsigned char)env[p + 1]) || env[p + 2] != 'M') {
        *err = "unexpected rk_dma_heap_cma format";
        return -1;
    }
    return (long)p;
}

json media_memory() {
    std::string cmd;
    read_file("/proc/cmdline", cmd);
    int active = 0;
    size_t p = cmd.find("rk_dma_heap_cma=");
    if (p != std::string::npos) active = atoi(cmd.c_str() + p + 16);
    std::string env, err;
    long off = env_load(env, &err);
    int configured = off >= 0 ? atoi(env.c_str() + off) : active;
    return {{"active_mb", active},
            {"configured_mb", configured},
            {"used_kb", meminfo_kb("CmaAllocated")},
            {"total_ram_kb", meminfo_kb("MemTotal")},
            {"changeable", off >= 0},
            {"note", off >= 0 ? "" : err},
            {"options", board::cma_options()}};
}

std::string set_media_memory(int mb) {
    bool allowed = false;
    for (auto &o : board::cma_options()) allowed |= o.get<int>() == mb;
    if (!allowed) return "this board's RAM does not allow " + std::to_string(mb) + " MB of media memory";
    std::string env, err;
    long off = env_load(env, &err);
    if (off < 0) return err;
    if (atoi(env.c_str() + off) == mb) return "";
    mkdirs(g_paths.data);
    std::string bak = g_paths.data + "/env.backup";
    if (!file_exists(bak) && !write_file(bak, env)) return "cannot back up the boot environment";
    char two[3];
    snprintf(two, sizeof(two), "%02d", mb);
    env[off] = two[0];
    env[off + 1] = two[1];
    uint32_t c = crc32((const uint8_t *)env.data() + 4, kEnvSize - 4);
    env[0] = (char)(c & 0xff), env[1] = (char)((c >> 8) & 0xff), env[2] = (char)((c >> 16) & 0xff), env[3] = (char)(c >> 24);
    FILE *fp = fopen(kEnvDev, "r+b");
    if (!fp) return "cannot open boot environment for writing";
    size_t w = fwrite(env.data(), 1, kEnvSize, fp);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    sync();
    std::string check, e2;
    if (w != kEnvSize || env_load(check, &e2) < 0 || check != env) {
        // put the original back
        std::string orig;
        if (read_file(bak, orig) && (fp = fopen(kEnvDev, "r+b"))) {
            fwrite(orig.data(), 1, kEnvSize, fp);
            fclose(fp);
            sync();
        }
        return "verification failed, original restored";
    }
    LOGW(TAG, "media memory set to %d MB (after reboot)", mb);
    return "";
}

void reboot(int delay_ms) {
    LOGW(TAG, "reboot requested");
    std::thread([delay_ms] {
        usleep(delay_ms * 1000);
        watchdog_shutdown();  // a clean reboot must not race the watchdog
        sync();
        run_cmd("reboot");
    }).detach();
}

}  // namespace sys
