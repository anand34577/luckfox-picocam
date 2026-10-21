// PicoCam - IP camera firmware for Luckfox Pico / Pico Plus (RV1103/RV1106 + SC3336)
//   picocam [-a /oem/picocam] [-d /userdata/picocam] [--iq /oem/usr/share/iqfiles] [-v]
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstring>

#include "ai/engine.h"
#include "ai/motion.h"
#include "app/api.h"
#include "app/auth.h"
#include "app/events.h"
#include "app/live.h"
#include "app/storage.h"
#include "app/system.h"
#include "core/audit.h"
#include "core/config.h"
#include "core/log.h"
#include "core/util.h"
#include "media/isp.h"
#include "media/video.h"
#include "net/http.h"
#include "net/ipfilter.h"
#include "net/tls.h"
#include "net/loop.h"
#include "net/onvif.h"
#include "net/rtsp.h"

static const char *TAG = "main";

static void on_signal(int) { g_loop.stop(); }

static void register_settings() {
    g_settings.on(
        "video", [](json &v) { return video::validate(v); },
        [](const json &, const json &before) {
            // encoders are rebuilt (in the media-memory-safe order); RTSP clients reconnect and get the new SDP
            g_loop.post([] { rtsp::kick_all(); });
            ai::restart_video(before);
        });
    g_settings.on("image", [](json &v) { return isp::validate(v); }, [](const json &v, const json &) { isp::apply(v); });
    g_settings.on("osd", nullptr, [](const json &v, const json &) { video::apply_osd(v); });
    g_settings.on("device", nullptr, [](const json &, const json &) { video::apply_osd(g_settings.get("osd")); });
    g_settings.on(
        "masks", [](json &v) { return v.is_array() && v.size() <= 8 ? std::string() : std::string("up to 8 masks"); },
        [](const json &v, const json &) {
            video::apply_masks(v);
            isp::set_masks(v);  // masked areas stay out of the exposure statistics
        });
    g_settings.on(
        "rtsp",
        [](json &v) {
            std::string a = jget<std::string>(v, "auth", "digest");
            if (a != "digest" && a != "basic" && a != "none") return std::string("auth must be digest, basic or none");
            int p = jget<int>(v, "port", 554);
            if (p < 1 || p > 65535) return std::string("invalid port");
            return std::string();
        },
        [](const json &v, const json &before) {
            rtsp::configure(jget<std::string>(v, "auth", "digest"));
            if (jget<int>(v, "port", 554) != jget<int>(before, "port", 554) || jget<bool>(v, "enabled", true) != jget<bool>(before, "enabled", true)) {
                g_loop.post([v] {
                    rtsp::stop();
                    if (jget<bool>(v, "enabled", true)) rtsp::start(jget<int>(v, "port", 554));
                });
            }
        });
    g_settings.on("network", [](json &v) { return sys::validate_network(v); },
                  [](const json &v, const json &before) { sys::network_changed(v, before); });
    g_settings.on(
        "security", [](json &v) { return ipfilter::validate(v.value("allow", json::array())); },
        [](const json &v, const json &) { ipfilter::configure(v.value("allow", json::array())); });
    g_settings.on(
        "web",
        [](json &v) {
            int p = jget<int>(v, "port", 80);
            json h = v.value("https", json::object());
            int hp = jget<int>(h, "port", 443);
            if (p < 1 || p > 65535 || hp < 1 || hp > 65535) return std::string("invalid port");
            if (p == hp && jget<bool>(h, "enabled", true)) return std::string("HTTP and HTTPS need different ports");
            int rp = jget<int>(g_settings.get("rtsp"), "port", 554);
            if (p == rp || hp == rp) return std::string("the web port must differ from the RTSP port (") + std::to_string(rp) + ")";
            int hrs = jget<int>(v, "session_hours", 12);
            if (hrs < 1 || hrs > 720) return std::string("session length must be 1..720 hours");
            return std::string();
        },
        [](const json &v, const json &before) {
            json h = v.value("https", json::object()), hb = before.value("https", json::object());
            int port = jget<int>(v, "port", 80), hport = jget<int>(h, "port", 443);
            g_http.redirect_https = jget<bool>(h, "redirect", false) && tls::available() && jget<bool>(h, "enabled", true);
            g_http.https_port = hport;
            // listeners are re-opened on the loop thread; if the new port cannot be opened the old one comes back
            g_loop.post([v, before, port, hport, h, hb] {
                if (port != jget<int>(before, "port", 80)) {
                    g_http.stop_plain();
                    if (!g_http.start(port, g_paths.www())) {
                        LOGE(TAG, "cannot listen on web port %d: back to %d", port, jget<int>(before, "port", 80));
                        g_http.start(jget<int>(before, "port", 80), g_paths.www());
                        g_settings.revert("web", before);
                    }
                }
                if (jget<bool>(h, "enabled", true) != jget<bool>(hb, "enabled", true) || hport != jget<int>(hb, "port", 443)) {
                    g_http.stop_tls();
                    if (jget<bool>(h, "enabled", true) && tls::available() && !g_http.start_tls(hport)) LOGE(TAG, "cannot listen on HTTPS port %d", hport);
                }
            });
        });
    g_settings.on("system", [](json &v) { return sys::validate_system(v); },
                  [](const json &v, const json &before) { sys::apply_system(v, before); });
    g_settings.on("time", [](json &v) { return sys::validate_time(v); }, [](const json &v, const json &) { sys::apply_time(v); });
}

int main(int argc, char **argv) {
    std::string iq = "/oem/usr/share/iqfiles";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) g_paths.app = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) g_paths.data = argv[++i];
        else if (!strcmp(argv[i], "--iq") && i + 1 < argc) iq = argv[++i];
        else if (!strcmp(argv[i], "-v")) log_set_level(LOG_DEBUG);
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setpriority(PRIO_PROCESS, 0, -5);  // video before housekeeping

    mkdirs(g_paths.data);
    audit::init();
    g_settings.load();
    auth::load();
    LOGI(TAG, "PicoCam %s on %s (%s)", sys::version(), sys::board_model().c_str(),
         g_settings.get("device").value("id", "").c_str());
    if (!g_loop.init()) {
        LOGE(TAG, "event loop init failed");
        return 1;
    }
    register_settings();
    sys::start();
    ipfilter::configure(g_settings.get("security").value("allow", json::array()));
    sys::apply_system(g_settings.get("system"), json());  // watchdog, SSH/ADB markers

    json v = g_settings.get("video");
    video::sys_init();
    if (!isp::start(iq, jget<int>(v, "sensor_fps", 25))) {
        sys::stop();
        return 2;
    }
    isp::set_masks(g_settings.get("masks"));
    isp::apply(g_settings.get("image"));
    ai::init();
    if (!ai::start_with_video()) {
        LOGE(TAG, "video pipeline failed to start");
        ai::shutdown();
        isp::stop();
        sys::stop();
        return 3;
    }

    api::register_routes();
    live::register_routes();
    storage::register_routes();
    storage::start();
    events::register_routes();
    events::start();
    ai::register_routes();
    motion::init();
    onvif::start();
    json web = g_settings.get("web"), rt = g_settings.get("rtsp");
    // HTTPS: the self-signed certificate is created on the first start (about a second)
    bool tls_ok = tls::init();
    int web_port = jget<int>(web, "port", 80);
    if (!g_http.start(web_port, g_paths.www())) {
        // a port that cannot be opened must never leave the camera without a portal
        LOGE(TAG, "web server failed to start on port %d: trying 80, then 8080", web_port);
        if (!g_http.start(80, g_paths.www()) && !g_http.start(8080, g_paths.www())) LOGE(TAG, "web server failed to start");
    }
    json https = web.value("https", json::object());
    g_http.https_port = jget<int>(https, "port", 443);
    g_http.redirect_https = tls_ok && jget<bool>(https, "enabled", true) && jget<bool>(https, "redirect", false);
    if (tls_ok && jget<bool>(https, "enabled", true) && !g_http.start_tls(g_http.https_port))
        LOGE(TAG, "HTTPS failed to start on port %d", g_http.https_port);
    rtsp::configure(jget<std::string>(rt, "auth", "digest"));
    if (jget<bool>(rt, "enabled", true)) rtsp::start(jget<int>(rt, "port", 554));
    sys::apply_network(g_settings.get("network"));

    // drop connections that went silent (the HTTP/RTSP ports are open to the whole LAN)
    g_loop.every(5000, [] { Conn::sweep_idle(60000); });
    g_loop.every(1000, [] { sys::loop_beat(); });  // watchdog liveness

    // push a status heartbeat to UI clients
    g_loop.every(2000, [] {
        live::publish("status", {{"system", sys::stats()}, {"video", video::status()}, {"isp", isp::status()},
                                 {"rtsp_sessions", rtsp::sessions()}, {"ai", ai::status()}, {"motion", motion::status()},
                                 {"storage", storage::status()}, {"network_pending_s", sys::network_pending_s()}});
    });

    LOGI(TAG, "ready: http://%s:%d  rtsp://%s:%d/live/main", iface_ipv4(primary_iface()).c_str(), jget<int>(web, "port", 80),
         iface_ipv4(primary_iface()).c_str(), jget<int>(rt, "port", 554));
    g_loop.run();

    LOGI(TAG, "shutting down");
    onvif::stop();
    ai::shutdown();
    events::stop();
    storage::stop();
    rtsp::stop();
    g_http.stop();
    video::stop();
    isp::stop();
    sys::stop();
    sys::watchdog_shutdown();  // clean exit: disarm last, so a hang during shutdown still resets the board
    return 0;
}
