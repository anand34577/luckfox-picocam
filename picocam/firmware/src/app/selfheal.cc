// Self-healing and hardening: hardware watchdog, SSH/ADB switches, root password sync, network change
// confirmation and firmware rollback state. (Part of namespace sys, declared in system.h.)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "core/audit.h"
#include "core/log.h"
#include "core/util.h"
#include "media/video.h"
#include "system.h"

namespace sys {

static const char *TAG = "system";

// ---------------------------------------------------------------- watchdog

static int g_wd = -1;
static std::thread g_wd_th;
static std::atomic<bool> g_wd_run{false};
static std::atomic<int64_t> g_loop_beat{0};
static std::mutex g_wd_mu;

void loop_beat() { g_loop_beat = now_ms(); }

static void watchdog_loop() {
    int64_t last_warn = 0;
    while (g_wd_run) {
        int64_t beat_age = now_ms() - g_loop_beat, stalled = video::stalled_ms();
        if (beat_age < 20000 && stalled < 30000) {
            if (write(g_wd, "\0", 1) < 0) ioctl(g_wd, 0x80045705 /* WDIOC_KEEPALIVE */, 0);
        } else if (now_ms() - last_warn > 10000) {
            last_warn = now_ms();
            LOGE(TAG, "watchdog not fed (event loop %lld ms, video %lld ms): the board resets soon", (long long)beat_age,
                 (long long)stalled);
        }
        for (int i = 0; i < 50 && g_wd_run; ++i) usleep(100 * 1000);
    }
}

void watchdog_apply(bool on) {
    std::lock_guard<std::mutex> lk(g_wd_mu);
    if (on && g_wd < 0) {
        if (!file_exists("/dev/watchdog")) {
            LOGW(TAG, "no /dev/watchdog on this board: hardware watchdog unavailable");
            return;
        }
        g_wd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
        if (g_wd < 0) return;
        int timeout = 60;
        ioctl(g_wd, 0xC0045706 /* WDIOC_SETTIMEOUT */, &timeout);
        g_loop_beat = now_ms();
        g_wd_run = true;
        g_wd_th = std::thread(watchdog_loop);
        LOGI(TAG, "hardware watchdog armed (60 s)");
    } else if (!on && g_wd >= 0) {
        g_wd_run = false;
        if (g_wd_th.joinable()) g_wd_th.join();
        if (write(g_wd, "V", 1) < 0) {  // magic close: disarm
        }
        close(g_wd);
        g_wd = -1;
        LOGI(TAG, "hardware watchdog disarmed");
    }
}

void watchdog_shutdown() { watchdog_apply(false); }

// ---------------------------------------------------------------- security switches

std::string validate_system(json &s) {
    for (const char *k : {"watchdog", "ssh", "adb", "root_sync"})
        if (s.contains(k) && !s[k].is_boolean()) return std::string(k) + " must be true or false";
    return "";
}

static void marker(const char *name, bool on) {
    std::string p = g_paths.data + "/" + name;
    if (on) write_file(p, "1");
    else unlink(p.c_str());
}

void apply_system(const json &s, const json &before) {
    watchdog_apply(jget<bool>(s, "watchdog", true));
    bool ssh = jget<bool>(s, "ssh", true), adb = jget<bool>(s, "adb", true);
    // the supervisor (a shell script) re-applies these at every start, so they are marker files, not config
    marker("ssh.off", !ssh);
    marker("adb.off", !adb);
    if (!before.is_null() && jget<bool>(before, "ssh", true) != ssh) {
        if (!ssh)
            run_cmd("killall sshd dropbear 2>/dev/null; true");
        else
            run_cmd("pidof sshd dropbear >/dev/null || for s in /etc/init.d/S*sshd /etc/init.d/S*dropbear; do "
                    "[ -x $s ] && { $s start; break; }; done >/dev/null 2>&1; true");
    }
    if (!before.is_null() && jget<bool>(before, "adb", true) != adb && !adb) run_cmd("killall adbd 2>/dev/null; true");
}

static std::atomic<int> g_root_sync_state{0};  // 0 never, 1 ok, -1 failed

bool sync_root_password(const std::string &password) {
    if (!jget<bool>(g_settings.get("system"), "root_sync", true)) return false;
    RootWrite rw;  // /etc/shadow is on the system partition
    // the password goes through stdin, never through a shell command line
    FILE *p = popen("chpasswd 2>/dev/null", "w");
    if (!p) {
        g_root_sync_state = -1;
        return false;
    }
    fprintf(p, "root:%s\n", password.c_str());
    bool ok = pclose(p) == 0;
    g_root_sync_state = ok ? 1 : -1;
    if (ok) LOGI(TAG, "root password set to the admin password");
    else LOGW(TAG, "could not change the root password (chpasswd missing?)");
    return ok;
}

json security_status() {
    json s = g_settings.get("system");
    return {{"watchdog", jget<bool>(s, "watchdog", true)},
            {"watchdog_armed", g_wd >= 0},
            {"ssh", jget<bool>(s, "ssh", true)},
            {"adb", jget<bool>(s, "adb", true)},
            {"root_sync", jget<bool>(s, "root_sync", true)},
            {"root_synced", g_root_sync_state.load()},
            {"ssh_running", !trim(run_cmd("pidof sshd dropbear 2>/dev/null")).empty()}};
}

// ---------------------------------------------------------------- network change confirmation

static std::mutex g_net_mu;
static json g_net_prev;
static int64_t g_net_deadline = 0;
static int g_net_gen = 0;

static bool net_affects_link(const json &a, const json &b, std::initializer_list<const char *> keys) {
    for (const char *k : keys)
        if (a.value(k, json()) != b.value(k, json())) return true;
    return false;
}

void network_changed(const json &now, const json &before) {
    // a changed address on the interface you are connected through cuts you off: ask for confirmation (60 s, then revert)
    bool link = (file_exists("/sys/class/net/eth0") && net_affects_link(now, before, {"mode", "ip", "netmask", "gateway"})) ||
                (file_exists("/sys/class/net/usb0") && net_affects_link(now, before, {"usb_ip", "usb_netmask"}));
    apply_network(now);
    if (!link) return;
    int gen;
    {
        std::lock_guard<std::mutex> lk(g_net_mu);
        g_net_prev = before;
        g_net_deadline = now_ms() + 60000;
        gen = ++g_net_gen;
    }
    LOGW(TAG, "network settings changed: reverting in 60 s unless confirmed");
    std::thread([gen] {
        for (int i = 0; i < 600; ++i) {
            usleep(100 * 1000);
            std::lock_guard<std::mutex> lk(g_net_mu);
            if (g_net_gen != gen) return;  // confirmed or superseded
        }
        json prev;
        {
            std::lock_guard<std::mutex> lk(g_net_mu);
            if (g_net_gen != gen) return;
            prev = g_net_prev;
            g_net_deadline = 0;
            ++g_net_gen;
        }
        LOGW(TAG, "network change not confirmed: restoring the previous settings");
        g_settings.revert("network", prev);
        apply_network(prev);
    }).detach();
}

void network_confirm() {
    std::lock_guard<std::mutex> lk(g_net_mu);
    if (g_net_deadline) LOGI(TAG, "network change confirmed");
    g_net_deadline = 0;
    ++g_net_gen;
}

int network_pending_s() {
    std::lock_guard<std::mutex> lk(g_net_mu);
    if (!g_net_deadline) return 0;
    return (int)std::max<int64_t>(1, (g_net_deadline - now_ms() + 999) / 1000);
}

// ---------------------------------------------------------------- firmware state

json update_state() {
    std::string prev;
    read_file(g_paths.app + ".prev/manifest.json", prev);
    json pm = json::parse(prev.empty() ? "{}" : prev, nullptr, false);
    auto flag = [](const char *n) {
        std::string s;
        return read_file(g_paths.data + "/" + n, s) ? trim(s) : std::string();
    };
    return {{"version", version()},
            {"previous_version", pm.is_object() ? pm.value("version", "") : ""},
            {"pending", file_exists(g_paths.data + "/update-pending")},
            {"rolled_back", flag("rolled-back")},
            {"config_recovered", flag("recovered")}};
}

std::string rollback_update() {
    if (!file_exists(g_paths.app + ".prev/picocam")) return "no previous version is stored";
    remove_tree(g_paths.app + ".bad");
    if (rename(g_paths.app.c_str(), (g_paths.app + ".bad").c_str()) != 0) return "could not move the current version";
    if (rename((g_paths.app + ".prev").c_str(), g_paths.app.c_str()) != 0) {
        rename((g_paths.app + ".bad").c_str(), g_paths.app.c_str());
        return "could not restore the previous version";
    }
    unlink((g_paths.data + "/update-pending").c_str());
    sync();
    return "";
}

}  // namespace sys
