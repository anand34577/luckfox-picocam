#include "system.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
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
static float g_cpu = 0, g_cpu_user = 0, g_cpu_sys = 0, g_app_cpu = 0;  // % of the CPU over the last 2 s
static int64_t g_start_ms = now_ms();
static std::atomic<int64_t> g_ntp_ok_ms{0};
static std::atomic<bool> g_ntp_now{false};

const char *version() { return PICOCAM_VERSION; }

std::string board_model() { return board::info().model; }

// ---------------------------------------------------------------- stats

struct CpuTimes {
    uint64_t idle = 0, total = 0, user = 0, sys = 0, app = 0;
};

static bool read_cpu(CpuTimes &c) {
    std::string s;
    if (!read_file("/proc/stat", s)) return false;
    unsigned long long u, n, sy, id, io = 0, irq = 0, sirq = 0, st = 0;
    if (sscanf(s.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq, &st) < 4)
        return false;
    c.idle = id + io;
    c.total = u + n + sy + id + io + irq + sirq + st;
    c.user = u + n;
    c.sys = sy + irq + sirq;
    // this process (all threads): utime + stime, fields 14 and 15, counted after the ")" that ends the name
    std::string ps;
    c.app = 0;
    if (read_file("/proc/self/stat", ps)) {
        size_t e = ps.rfind(')');
        unsigned long long ut = 0, stt = 0;
        if (e != std::string::npos &&
            sscanf(ps.c_str() + e + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &stt) == 2)
            c.app = ut + stt;
    }
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
    // hardware engine load from the vendor drivers: "NPU load: 16%" (procfs; debugfs is not mounted on these boards)
    // and the RGA's "load = 1%". Never read /proc/rknpu/volt: the driver dereferences a missing regulator (kernel oops).
    auto pct_after = [](const char *path, std::initializer_list<const char *> keys) {
        std::string t;
        if (!read_file(path, t)) return -1;
        for (const char *k : keys) {
            size_t at = t.find(k);
            if (at != std::string::npos) return atoi(t.c_str() + at + strlen(k));
        }
        return -1;
    };
    int npu_load = pct_after("/proc/rknpu/load", {"NPU load:", "Core0:"});
    if (npu_load < 0) npu_load = pct_after("/sys/kernel/debug/rknpu/load", {"NPU load:", "Core0:"});
    int rga_load = pct_after("/proc/rkrga/load", {"load = "});
    std::string f;
    int cpu_mhz = read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", f) ? atoi(f.c_str()) / 1000 : -1;
    std::lock_guard<std::mutex> lk(g_mu);
    return {{"cpu_pct", std::round(g_cpu)},
            {"cpu_user_pct", std::round(g_cpu_user)},
            {"cpu_sys_pct", std::round(g_cpu_sys)},
            {"app_cpu_pct", std::round(g_app_cpu * 10.0) / 10.0},
            {"cpu_mhz", cpu_mhz},
            {"rga_load_pct", rga_load},
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
    write_system_file("/etc/hostname", h + "\n");
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
        if (!resolv.empty()) {
            // a link into /tmp on these images: write its target, the system partition may be read-only
            char lnk[256];
            ssize_t n = readlink("/etc/resolv.conf", lnk, sizeof(lnk) - 1);
            std::string to = n > 0 ? std::string(lnk, n) : "";
            if (!to.empty() && to[0] != '/') to = "/etc/" + to;
            if (!to.empty()) write_file(to, resolv);
            else write_system_file("/etc/resolv.conf", resolv);
        }
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
    write_system_file("/etc/TZ", tz + "\n");
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
static bool sntp_query(const std::string &server, int64_t *server_ms, std::string *why) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server.c_str(), "123", &hints, &res) != 0 || !res) {
        *why = "cannot resolve the name (no DNS server reachable?)";
        return false;
    }
    *why = "no reply";
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }
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

static int g_ntp_fails = 0;  // consecutive failed syncs (retries back off, the warning is not repeated each time)

static void ntp_sync(const std::string &server) {
    int64_t ms = 0;
    std::string why;
    for (int i = 0; i < 3 && !sntp_query(server, &ms, &why); ++i) usleep(500 * 1000);
    if (!ms) {
        static std::string last_why;
        if (g_ntp_fails++ == 0 || why != last_why || g_ntp_fails % 12 == 0)
            LOGW(TAG, "NTP sync with %s failed: %s (attempt %d)", server.c_str(), why.c_str(), g_ntp_fails);
        last_why = why;
        return;
    }
    g_ntp_fails = 0;
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
            // while failing: 2, 4, 8 .. 30 min (the network may come up late; a missing DNS server is not fixed by hammering)
            int64_t retry = std::min<int64_t>(30 * 60000, 120000LL << std::min(4, std::max(0, g_ntp_fails - 1)));
            next_ntp = now_ms() + (g_ntp_fails == 0 ? jget<int>(t, "ntp_interval_min", 60) * 60000 : retry);
        }
    }
}

static void loop() {
    int usb_tick = 0;
    CpuTimes prev;
    read_cpu(prev);
    while (g_run) {
        for (int i = 0; i < 20 && g_run; ++i) usleep(100 * 1000);
        // the vendor scripts may bring usb0 up after us: keep the configured address (not while a change awaits confirmation)
        if (++usb_tick % 5 == 0 && !network_pending_s()) apply_usb(g_settings.get("network"));
        CpuTimes c;
        if (read_cpu(c) && c.total > prev.total) {
            float dt = (float)(c.total - prev.total);
            std::lock_guard<std::mutex> lk(g_mu);
            g_cpu = std::max(0.f, std::min(100.f, 100.f * (1.f - (float)(c.idle - prev.idle) / dt)));
            g_cpu_user = 100.f * (float)(c.user - prev.user) / dt;
            g_cpu_sys = 100.f * (float)(c.sys - prev.sys) / dt;
            g_app_cpu = c.app >= prev.app ? std::min(100.f, 100.f * (float)(c.app - prev.app) / dt) : 0.f;
            prev = c;
        }
    }
}

static void prepare_media_partition();

void start() {
    apply_time(g_settings.get("time"));
    g_run = true;
    g_th = std::thread(loop);
    g_ntp_th = std::thread(ntp_loop);
    prepare_media_partition();
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

// reads the env and checks its CRC
static bool env_read(std::string &env, std::string *err) {
    if (!file_exists(kEnvDev)) {
        *err = "the boot environment cannot be edited from Linux on this board (SPI-NAND boot). Change rk_dma_heap_cma in the "
               "SDK's env and reflash, or run `fw_setenv` if your image ships it. See docs/BOARDS.md";
        return false;
    }
    std::string all;
    if (!read_file(kEnvDev, all) || all.size() < kEnvSize) {
        *err = "cannot read boot environment";
        return false;
    }
    env = all.substr(0, kEnvSize);
    const uint8_t *d = (const uint8_t *)env.data();
    uint32_t stored = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
    if (crc32(d + 4, kEnvSize - 4) != stored) {
        *err = "boot environment checksum mismatch: not touching it";
        return false;
    }
    return true;
}

// reads the env; returns the offset of the "NN" in rk_dma_heap_cma=NNM or -1
static long env_load(std::string &env, std::string *err) {
    if (!env_read(env, err)) return -1;
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

// writes a changed env (new CRC), reads it back and puts the original back if anything differs; "" on success
static std::string env_write(std::string env) {
    std::string orig, err;
    if (!env_read(orig, &err)) return err;
    mkdirs(g_paths.data);
    std::string bak = g_paths.data + "/env.backup";
    if (!file_exists(bak) && !write_file(bak, orig)) return "cannot back up the boot environment";
    uint32_t c = crc32((const uint8_t *)env.data() + 4, kEnvSize - 4);
    env[0] = (char)(c & 0xff), env[1] = (char)((c >> 8) & 0xff), env[2] = (char)((c >> 16) & 0xff), env[3] = (char)(c >> 24);
    FILE *fp = fopen(kEnvDev, "r+b");
    if (!fp) return "cannot open boot environment for writing";
    size_t w = fwrite(env.data(), 1, kEnvSize, fp);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    sync();
    std::string check;
    if (w != kEnvSize || !env_read(check, &err) || check != env) {
        if ((fp = fopen(kEnvDev, "r+b"))) {
            fwrite(orig.data(), 1, kEnvSize, fp);
            fflush(fp);
            fsync(fileno(fp));
            fclose(fp);
            sync();
        }
        return "verification failed, original restored";
    }
    return "";
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
    char two[3];
    snprintf(two, sizeof(two), "%02d", mb);
    env[off] = two[0];
    env[off + 1] = two[1];
    std::string e = env_write(env);
    if (!e.empty()) return e;
    LOGW(TAG, "media memory set to %d MB (after reboot)", mb);
    return "";
}

// ---------------------------------------------------------------- system card layout
// Luckfox's SD image gives the system (root) partition a fixed 6 GB and leaves the rest of the card unpartitioned.
// The partitions come from blkdevparts= in the U-Boot env, so a "media" partition is added after the system one, running
// to the end of the card (after a restart). Recordings and events go there, which lets the system partition stay
// read-only: a power cut can then never damage the OS.

enum CardLayout { kCardNone, kCardMedia, kCardPending, kCardFullRoot };
static std::atomic<int> g_card_layout{kCardNone};
static std::atomic<int64_t> g_card_unused{0};  // bytes no partition uses (counted at start-up)

static int64_t sys_sectors(const std::string &path) {
    std::string s;
    return read_file(path, s) ? atoll(trim(s).c_str()) : -1;
}

static void prepare_media_partition() {
    std::string cmd;
    read_file("/proc/cmdline", cmd);
    if (cmd.find("storagemedia=sd") == std::string::npos) return;
    if (cmd.find("(media)") != std::string::npos) {
        g_card_layout = kCardMedia;
        return;
    }
    size_t p = cmd.find("root=/dev/");
    if (p == std::string::npos) return;
    std::string part = split(cmd.substr(p + 10), ' ')[0];  // mmcblk1p7
    size_t pp = part.rfind('p');
    if (part.compare(0, 6, "mmcblk") != 0 || pp == std::string::npos || pp < 7) return;
    std::string disk = part.substr(0, pp);
    int partno = atoi(part.c_str() + pp + 1);
    int64_t disk_s = sys_sectors("/sys/block/" + disk + "/size"), start_s = sys_sectors("/sys/class/block/" + part + "/start"),
            size_s = sys_sectors("/sys/class/block/" + part + "/size");
    if (disk_s <= 0 || start_s < 0 || size_s <= 0) return;
    int64_t unused = (disk_s - start_s - size_s) * 512;
    g_card_unused = unused > 0 ? unused : 0;

    std::string env, err;
    if (!env_read(env, &err)) return LOGW(TAG, "card: %s", err.c_str());
    size_t b = env.find("blkdevparts="), be = b == std::string::npos ? b : env.find('\0', b);
    if (be == std::string::npos) return;
    std::string val = env.substr(b + 12, be - b - 12);
    size_t ds = val.find(disk + ":");
    if (ds == std::string::npos) return;
    size_t de = val.find(';', ds);
    std::string list = val.substr(ds + disk.size() + 1, (de == std::string::npos ? val.size() : de) - ds - disk.size() - 1);
    auto entries = split(list, ',');
    // already added, waiting for the restart
    if ((int)entries.size() == partno + 1 && ends_with(list, "(media)")) {
        g_card_layout = kCardPending;
        return LOGI(TAG, "card: the media partition (%.1f GB) is used after the next restart", unused / 1e9);
    }
    // only touch what this boot was made from, and only when the system partition is the last one
    if (cmd.find("blkdevparts=" + val) == std::string::npos || (int)entries.size() != partno) return;
    if (entries.back().compare(0, 1, "-") == 0) {
        // the system partition already runs to the end of the card (older PicoCam test build or a custom image)
        g_card_layout = kCardFullRoot;
        return LOGW(TAG, "card: the system partition fills the whole card; re-flash the SD card for separate recordings storage and a read-only system");
    }
    if (unused < (1ll << 30)) return;
    size_t at = b + 12 + ds + disk.size() + 1 + list.size();  // end of this disk's list in env
    size_t used = env.find(std::string("\0\0", 2), 4);
    if (used == std::string::npos || used + 16 > kEnvSize) return LOGW(TAG, "card: no room in the boot environment");
    std::string grown = env.substr(0, at) + ",-(media)" + env.substr(at);
    grown.resize(kEnvSize, '\0');
    err = env_write(grown);
    if (!err.empty()) return LOGE(TAG, "could not add the media partition: %s", err.c_str());
    g_card_layout = kCardPending;
    LOGW(TAG, "card: %.1f GB were unused; a media partition for recordings is added on the next restart", unused / 1e9);
}

json card_layout() {
    static const char *names[] = {"none", "media", "pending", "full_root"};
    return {{"layout", names[g_card_layout.load()]}, {"unused_bytes", g_card_unused.load()}};
}

// ---------------------------------------------------------------- read-only system partition
// service.sh does the switching (also at boot); /etc/picocam.ro is the marker its boot hook reads. PicoCam's own writes
// to the system partition (hostname, time zone, root password, boot stub) open it for a moment with RootWrite.

static const char *kRoMarker = "/etc/picocam.ro";
static std::mutex g_rw_mu;
static int g_rw_depth = 0;

RootWrite::RootWrite() {
    std::lock_guard<std::mutex> lk(g_rw_mu);
    if (g_rw_depth++ == 0 && file_exists(kRoMarker)) run_cmd("mount -o remount,rw / 2>/dev/null");
}

RootWrite::~RootWrite() {
    std::lock_guard<std::mutex> lk(g_rw_mu);
    if (--g_rw_depth == 0 && file_exists(kRoMarker)) {
        ::sync();
        run_cmd("mount -o remount,ro / 2>/dev/null");
    }
}

bool write_system_file(const std::string &path, const std::string &data) {
    std::string cur;
    if (read_file(path, cur) && cur == data) return true;  // unchanged: no write at all
    RootWrite rw;
    return write_file(path, data);
}

static bool root_mounted_ro() {
    std::string m;
    read_file("/proc/mounts", m);
    for (auto &l : split(m, '\n')) {
        auto f = split(l, ' ');
        if (f.size() >= 4 && f[1] == "/") return f[3] == "ro" || f[3].compare(0, 3, "ro,") == 0;
    }
    return false;
}

json root_mode() {
    std::string cmd;
    read_file("/proc/cmdline", cmd);
    bool sd = cmd.find("storagemedia=sd") != std::string::npos, mmc = sd || cmd.find("storagemedia=emmc") != std::string::npos;
    std::string why;
    if (!mmc) why = "only for boards that boot from an SD card or eMMC";
    else if (sd && cmd.find("(media)") == std::string::npos)
        why = g_card_layout == kCardPending    ? "the media partition is ready after the next restart"
              : g_card_layout == kCardFullRoot ? "the system partition fills the whole card: re-flash the SD card first (docs/INSTALL.md)"
                                               : "recordings are on the system partition (card smaller than 8 GB?)";
    return {{"supported", why.empty()},
            {"reason", why},
            {"wanted", !file_exists(g_paths.data + "/root-rw")},
            {"read_only", root_mounted_ro()}};
}

std::string set_root_mode(bool read_only) {
    json m = root_mode();
    if (read_only && !jget<bool>(m, "supported", false)) return jget<std::string>(m, "reason", "not supported");
    std::string marker = g_paths.data + "/root-rw";
    if (read_only) unlink(marker.c_str());
    else if (!write_file(marker, "1")) return "cannot store the setting";
    int st = -1;
    std::string out = run_cmd("sh '" + g_paths.app + "/service.sh' rootmode 2>&1", &st);
    if (st != 0) return "could not switch: " + trim(out);
    LOGW(TAG, "system partition is now %s", root_mounted_ro() ? "read-only" : "writable");
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
