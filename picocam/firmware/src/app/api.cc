#include "api.h"

#include <sys/statvfs.h>
#include <unistd.h>

#include <thread>

#include "ai/engine.h"
#include "ai/motion.h"
#include "app/auth.h"
#include "app/events.h"
#include "app/storage.h"
#include "app/live.h"
#include "app/system.h"
#include "core/audit.h"
#include "core/log.h"
#include "core/util.h"
#include "media/isp.h"
#include "media/video.h"
#include "net/http.h"
#include "net/ipfilter.h"
#include "net/tls.h"
#include "net/rtsp.h"

namespace api {

static const char *TAG = "api";
static const char *kMask = "********";

// sections an operator may change; everything else needs admin
static bool operator_section(const std::string &s) {
    static const char *ok[] = {"video", "image", "osd", "masks", "ai", "rules", "motion", "events", "recording", "device"};
    for (auto *o : ok)
        if (s == o) return true;
    return false;
}

static bool known_section(const std::string &s) { return !g_settings.get(s).is_null(); }

// secrets never leave the device; the UI sends the mask back for "unchanged"
static void mask_secrets(json &j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if ((it.key() == "password" || it.key() == "secret" || it.key() == "token") && it->is_string() &&
                !it->get<std::string>().empty())
                *it = kMask;
            else
                mask_secrets(*it);
        }
    } else if (j.is_array()) {
        for (auto &x : j) mask_secrets(x);
    }
}

static void unmask(json &patch, const json &current) {
    if (!patch.is_object() || !current.is_object()) {
        if (patch.is_array() && current.is_array())
            for (size_t i = 0; i < patch.size(); ++i) {
                // list entries are matched by name (webhooks...) so deleting or reordering one cannot hand its secret to another
                const json *match = i < current.size() ? &current[i] : nullptr;
                if (patch[i].is_object() && patch[i].contains("name") && patch[i]["name"].is_string()) {
                    match = nullptr;
                    for (auto &c : current)
                        if (c.is_object() && c.value("name", std::string("(none)")) == patch[i]["name"].get<std::string>()) match = &c;
                }
                if (match) unmask(patch[i], *match);
            }
        return;
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it->is_string() && it->get<std::string>() == kMask && current.contains(it.key())) *it = current[it.key()];
        else if (current.contains(it.key())) unmask(*it, current[it.key()]);
    }
}

static void set_session_cookie(HttpResponse &res, const HttpRequest &req, const std::string &tok, int max_age) {
    res.headers["Set-Cookie"] = "pc_session=" + tok + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(max_age) +
                                (req.tls ? "; Secure" : "");
}

void register_routes() {
    // ------------------------------------------------------------ session
    g_http.route("GET", "/api/session", auth::NONE, [](HttpRequest &req, HttpResponse &res) {
        res.send_json({{"activated", auth::activated()},
                       {"user", req.user},
                       {"role", auth::role_name(req.role)},
                       {"ip", req.peer},
                       {"device", g_settings.get("device").value("name", "PicoCam")},
                       {"model", sys::board_model()},
                       {"firmware", sys::version()}});
    });

    g_http.route("POST", "/api/activate", auth::NONE, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string pw = jget<std::string>(b, "password", "");
        std::string err = auth::activate(pw);
        if (!err.empty()) return res.error(400, err);
        audit::record("admin", req.peer, "activate", "device activated");
        sys::sync_root_password(pw);  // the shell is protected by the same password
        std::string tok = auth::create_session("admin", auth::ADMIN, req.peer);
        set_session_cookie(res, req, tok, 12 * 3600);
        res.send_json({{"ok", true}, {"user", "admin"}, {"role", "admin"}});
    });

    g_http.route("POST", "/api/login", auth::NONE, [](HttpRequest &req, HttpResponse &res) {
        if (auth::locked_out(req.peer)) return res.error(429, "too many failed attempts, try again later");
        json b = req.json_body();
        std::string user = jget<std::string>(b, "username", ""), pass = jget<std::string>(b, "password", "");
        auth::Role r = auth::check_password(user, pass);
        if (r == auth::NONE) {
            auth::note_failure(req.peer);
            audit::record(user, req.peer, "login_failed");
            // no sleep here: this runs on the event loop (it also carries the video); the lockout throttles guessing
            return res.error(401, "wrong user name or password");
        }
        auth::note_success(req.peer);
        int hours = jget<int>(g_settings.get("web"), "session_hours", 12);
        std::string tok = auth::create_session(user, r, req.peer);
        set_session_cookie(res, req, tok, hours * 3600);
        LOGI(TAG, "login %s from %s", user.c_str(), req.peer.c_str());
        audit::record(user, req.peer, "login");
        res.send_json({{"ok", true}, {"user", user}, {"role", auth::role_name(r)}, {"token", tok}});
    });

    g_http.route("POST", "/api/logout", auth::NONE, [](HttpRequest &req, HttpResponse &res) {
        for (auto &kv : split(req.header("cookie"), ';')) {
            std::string t = trim(kv);
            if (starts_with(t, "pc_session=")) auth::drop_session(t.substr(11));
        }
        set_session_cookie(res, req, "", 0);
        res.ok();
    });

    // ------------------------------------------------------------ status
    g_http.route("GET", "/api/status", auth::VIEWER, [](HttpRequest &, HttpResponse &res) {
        res.send_json({{"device", sys::device_info()},
                       {"system", sys::stats()},
                       {"video", video::status()},
                       {"isp", isp::status()},
                       {"rtsp_sessions", rtsp::sessions()},
                       {"rtsp_port", jget<int>(g_settings.get("rtsp"), "port", 554)},
                       {"ai", ai::status()},
                       {"motion", motion::status()},
                       {"storage", storage::status()},
                       {"events", events::status()},
                       {"network", sys::network_status()},
                       {"network_pending_s", sys::network_pending_s()},
                       {"update", sys::update_state()}});
    });

    // three pictures at -1 / 0 / +1 EV with histograms, to judge the exposure settings (takes ~6 s)
    g_http.route("POST", "/api/image/exposure-test", auth::OPERATOR,
                 [](HttpRequest &, HttpResponse &res) { res.send_json(isp::exposure_test()); }, true);
    g_http.route("GET", "/api/image/status", auth::VIEWER,
                 [](HttpRequest &, HttpResponse &res) { res.send_json(isp::status()); });

    // ------------------------------------------------------------ settings
    g_http.route("GET", "/api/config", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) {
        json all = g_settings.all();
        mask_secrets(all);
        res.send_json(all);
    });

    g_http.route("GET", "/api/config/defaults", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) {
        res.send_json(g_settings.defaults());
    });

    g_http.route("GET", "/api/config/", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1 || !known_section(req.path_args[0])) return res.error(404, "unknown section");
        json s = g_settings.get(req.path_args[0]);
        mask_secrets(s);
        res.send_json(s);
    });

    auto put_section = [](HttpRequest &req, HttpResponse &res, bool replace) {
        if (req.path_args.size() != 1 || !known_section(req.path_args[0])) return res.error(404, "unknown section");
        const std::string &sec = req.path_args[0];
        if (req.role < auth::ADMIN && !operator_section(sec)) return res.error(403, "admin only");
        json patch = req.json_body();
        unmask(patch, g_settings.get(sec));
        if (sec == "security" && patch.contains("allow")) {
            // an allow-list that excludes the person saving it would lock them out
            std::string bad = ipfilter::validate(patch["allow"]);
            if (!bad.empty()) return res.error(400, bad);
            json saved = g_settings.get("security");
            ipfilter::configure(patch["allow"]);
            bool me = ipfilter::allowed(req.peer);
            ipfilter::configure(saved.value("allow", json::array()));
            if (!me) return res.error(400, "your own address (" + req.peer + ") is not in the list: you would lock yourself out");
        }
        bool persist = req.param("persist", "1") != "0";
        std::string err = replace ? g_settings.replace(sec, patch, persist) : g_settings.update(sec, patch, persist);
        if (!err.empty()) return res.error(400, err);
        if (persist) {
            LOGI(TAG, "%s changed %s", req.user.c_str(), sec.c_str());
            audit::record(req.user, req.peer, "config", sec);
        }
        json s = g_settings.get(sec);
        mask_secrets(s);
        res.send_json(s);
    };
    // PATCH-like merge (PUT) and full replace (POST) — slow appliers (encoder restart) run off the loop
    g_http.route("PUT", "/api/config/", auth::OPERATOR, [put_section](HttpRequest &q, HttpResponse &r) { put_section(q, r, false); }, true);
    g_http.route("POST", "/api/config/", auth::OPERATOR, [put_section](HttpRequest &q, HttpResponse &r) { put_section(q, r, true); }, true);

    // ------------------------------------------------------------ snapshot
    auto snap = [](HttpRequest &, HttpResponse &res) {
        std::string jpg;
        if (!video::snapshot(jpg)) return res.error(503, "snapshot unavailable");
        res.headers["Content-Type"] = "image/jpeg";
        res.body.swap(jpg);
    };
    g_http.route("GET", "/api/snapshot.jpg", auth::VIEWER, snap, true);
    g_http.route("GET", "/snapshot.jpg", auth::VIEWER, snap, true);  // NVR / ONVIF friendly
    g_http.route("GET", "/ISAPI/Streaming/channels/101/picture", auth::VIEWER, snap, true);

    // ------------------------------------------------------------ users and API keys (admin)
    g_http.route("GET", "/api/users", auth::ADMIN, [](HttpRequest &, HttpResponse &res) { res.send_json(auth::list_users()); });
    g_http.route("POST", "/api/users", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string err = auth::set_user(jget<std::string>(b, "name", ""), jget<std::string>(b, "password", ""),
                                         jget<std::string>(b, "role", "viewer"));
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "user_set", jget<std::string>(b, "name", "") + " (" + jget<std::string>(b, "role", "") + ")");
        if (jget<std::string>(b, "name", "") == "admin" && !jget<std::string>(b, "password", "").empty())
            sys::sync_root_password(jget<std::string>(b, "password", ""));
        res.send_json(auth::list_users());
    });
    g_http.route("DELETE", "/api/users/", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "user name required");
        if (req.path_args[0] == req.user) return res.error(400, "you cannot delete yourself");
        std::string err = auth::delete_user(req.path_args[0]);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "user_delete", req.path_args[0]);
        res.send_json(auth::list_users());
    });
    // any user may change their own password
    g_http.route("POST", "/api/password", auth::VIEWER, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        if (auth::check_password(req.user, jget<std::string>(b, "current", "")) == auth::NONE)
            return res.error(400, "current password is wrong");
        std::string role;
        for (auto &u : auth::list_users())
            if (u.value("name", "") == req.user) role = u.value("role", "viewer");
        std::string err = auth::set_user(req.user, jget<std::string>(b, "password", ""), role);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "password_change");
        if (req.user == "admin") sys::sync_root_password(jget<std::string>(b, "password", ""));
        res.ok();
    });
    g_http.route("GET", "/api/keys", auth::ADMIN, [](HttpRequest &, HttpResponse &res) { res.send_json(auth::list_keys()); });
    g_http.route("POST", "/api/keys", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string key;
        std::string err = auth::create_key(jget<std::string>(b, "name", ""), jget<std::string>(b, "role", "viewer"), &key);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "key_create", jget<std::string>(b, "name", ""));
        res.send_json({{"key", key}, {"keys", auth::list_keys()}});
    });
    g_http.route("DELETE", "/api/keys/", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "key name required");
        std::string err = auth::delete_key(req.path_args[0]);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "key_delete", req.path_args[0]);
        res.send_json(auth::list_keys());
    });

    // ------------------------------------------------------------ system
    g_http.route("POST", "/api/system/reboot", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        audit::record(req.user, req.peer, "reboot");
        sys::reboot();
        res.ok();
    });
    g_http.route("POST", "/api/system/restart", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        audit::record(req.user, req.peer, "restart_service");
        // the supervisor restarts the service
        std::thread([] {
            usleep(1000 * 1000);
            g_loop.stop();
        }).detach();
        res.ok();
    });
    g_http.route("POST", "/api/system/factory-reset", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        bool keep_net = jget<bool>(b, "keep_network", true);
        g_settings.factory_reset(keep_net);
        if (jget<bool>(b, "users", false)) unlink((g_paths.data + "/users.json").c_str());
        LOGW(TAG, "factory reset by %s", req.user.c_str());
        audit::record(req.user, req.peer, "factory_reset", jget<bool>(b, "users", false) ? "users removed" : "");
        sys::reboot(2000);
        res.ok();
    });
    g_http.route("GET", "/api/system/media-memory", auth::OPERATOR,
                 [](HttpRequest &, HttpResponse &res) { res.send_json(sys::media_memory()); });
    g_http.route("POST", "/api/system/media-memory", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string err = sys::set_media_memory(jget<int>(b, "mb", 24));
        if (!err.empty()) return res.error(400, err);
        LOGW(TAG, "%s changed media memory to %d MB", req.user.c_str(), jget<int>(b, "mb", 24));
        audit::record(req.user, req.peer, "media_memory", std::to_string(jget<int>(b, "mb", 24)) + " MB");
        if (jget<bool>(b, "reboot", false)) sys::reboot(2000);
        res.send_json(sys::media_memory());
    });
    g_http.route("GET", "/api/system/root-mode", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) { res.send_json(sys::root_mode()); });
    g_http.route("POST", "/api/system/root-mode", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        bool ro = jget<bool>(req.json_body(), "read_only", true);
        std::string err = sys::set_root_mode(ro);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "root_mode", ro ? "read-only" : "writable");
        res.send_json(sys::root_mode());
    });
    g_http.route("GET", "/api/system/logs", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        size_t n = (size_t)std::max(1, std::min(400, atoi(req.param("n", "200").c_str())));
        json out = json::array();
        static const char *lv[] = {"debug", "info", "warn", "error"};
        for (auto &l : log_tail(n)) out.push_back({{"ts", l.ts_ms}, {"level", lv[l.level]}, {"tag", l.tag}, {"msg", l.msg}});
        res.send_json(out);
    });
    g_http.route("POST", "/api/system/time", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        if (b.contains("epoch_ms") && !sys::set_clock(b["epoch_ms"].get<int64_t>())) return res.error(500, "cannot set clock");
        if (jget<bool>(b, "ntp_now", false)) sys::ntp_sync_now();
        res.send_json({{"time_ms", wall_ms()}});
    });
    g_http.route("GET", "/api/system/backup", auth::ADMIN, [](HttpRequest &, HttpResponse &res) {
        json all = g_settings.all();
        mask_secrets(all);
        res.send_json({{"picocam_backup", 1}, {"firmware", sys::version()}, {"created", iso8601(wall_ms())}, {"config", all}});
        res.headers["Content-Disposition"] = "attachment; filename=\"picocam-config.json\"";
    });
    g_http.route("POST", "/api/system/restore", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        if (!b.contains("config") || !b["config"].is_object()) return res.error(400, "not a PicoCam backup file");
        json errs = json::object();
        for (auto it = b["config"].begin(); it != b["config"].end(); ++it) {
            if (!known_section(it.key()) || it.key() == "device") continue;
            json v = *it;
            unmask(v, g_settings.get(it.key()));
            std::string err = g_settings.replace(it.key(), v);
            if (!err.empty()) errs[it.key()] = err;
        }
        res.send_json({{"ok", errs.empty()}, {"errors", errs}});
    }, true);

    // signed-in sessions: see who is logged in and kick them out
    auto current_token = [](const HttpRequest &req) {
        for (auto &kv : split(req.header("cookie"), ';')) {
            std::string t = trim(kv);
            if (starts_with(t, "pc_session=")) return t.substr(11);
        }
        return std::string();
    };
    g_http.route("GET", "/api/sessions", auth::ADMIN, [current_token](HttpRequest &req, HttpResponse &res) {
        res.send_json(auth::list_sessions(current_token(req)));
    });
    g_http.route("DELETE", "/api/sessions/", auth::ADMIN, [current_token](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "session id required");
        if (req.path_args[0] == "others") {
            int n = auth::revoke_other_sessions(current_token(req));
            audit::record(req.user, req.peer, "sessions_revoked", std::to_string(n) + " other sessions");
        } else if (!auth::revoke_session(req.path_args[0])) {
            return res.error(404, "no such session");
        } else {
            audit::record(req.user, req.peer, "session_revoked", req.path_args[0]);
        }
        res.send_json(auth::list_sessions(current_token(req)));
    });
    // HTTPS certificate
    g_http.route("GET", "/api/system/tls", auth::ADMIN, [](HttpRequest &, HttpResponse &res) {
        json j = tls::info();
        json h = g_settings.get("web").value("https", json::object());
        j["enabled"] = jget<bool>(h, "enabled", true);
        j["port"] = jget<int>(h, "port", 443);
        j["redirect"] = jget<bool>(h, "redirect", false);
        res.send_json(j);
    });
    g_http.route("POST", "/api/system/tls/regenerate", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        std::string e = tls::regenerate();
        if (!e.empty()) return res.error(500, e);
        audit::record(req.user, req.peer, "tls_regenerate");
        res.send_json(tls::info());
    }, true);
    g_http.route("POST", "/api/system/tls/upload", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string e = tls::install(jget<std::string>(b, "cert", ""), jget<std::string>(b, "key", ""));
        if (!e.empty()) return res.error(400, e);
        audit::record(req.user, req.peer, "tls_upload");
        res.send_json(tls::info());
    });
    g_http.route("GET", "/api/system/security", auth::ADMIN, [](HttpRequest &, HttpResponse &res) { res.send_json(sys::security_status()); });
    g_http.route("GET", "/api/system/audit", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        res.send_json(audit::tail((size_t)std::max(1, std::min(500, atoi(req.param("n", "200").c_str())))));
    });
    g_http.route("POST", "/api/system/network/confirm", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        sys::network_confirm();
        audit::record(req.user, req.peer, "network_confirm");
        res.ok();
    });
    g_http.route("GET", "/api/system/update-state", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) { res.send_json(sys::update_state()); });
    g_http.route("POST", "/api/system/rollback", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        std::string err = sys::rollback_update();
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "firmware_rollback");
        LOGW(TAG, "firmware rolled back by %s, restarting", req.user.c_str());
        std::thread([] {
            usleep(1000 * 1000);
            g_loop.stop();
        }).detach();
        res.ok();
    });
    // everything support needs in one file: versions, status, masked config, logs (RAM and flash), crash reports, audit
    g_http.route("GET", "/api/system/diagnostics", auth::ADMIN, [](HttpRequest &, HttpResponse &res) {
        json all = g_settings.all();
        mask_secrets(all);
        json logs = json::array();
        static const char *lv[] = {"debug", "info", "warn", "error"};
        for (auto &l : log_tail(400)) logs.push_back({{"ts", l.ts_ms}, {"level", lv[l.level]}, {"tag", l.tag}, {"msg", l.msg}});
        std::string crash;
        read_file(g_paths.data + "/crash.log", crash);
        if (crash.size() > 65536) crash = crash.substr(crash.size() - 65536);
        res.send_json({{"created", iso8601(wall_ms())},
                       {"device", sys::device_info()},
                       {"update", sys::update_state()},
                       {"security", sys::security_status()},
                       {"status", {{"system", sys::stats()}, {"video", video::status()}, {"isp", isp::status()}, {"ai", ai::status()},
                                   {"storage", storage::status()}, {"events", events::status()}, {"network", sys::network_status()}}},
                       {"config", all},
                       {"log", logs},
                       {"warnings_on_flash", audit::persistent_log_tail(65536)},
                       {"crash_log", crash},
                       {"audit", audit::tail(100)}});
        res.headers["Content-Disposition"] = "attachment; filename=\"picocam-diagnostics.json\"";
    });

    // firmware/app update: a picocam-*.tar.gz built by build.sh
    g_http.route("POST", "/api/system/update", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        std::string pkg = req.body_file;
        if (pkg.empty()) {
            if (req.body.empty()) return res.error(400, "no package uploaded");
            pkg = g_paths.data + "/tmp/update.tgz";
            mkdirs(g_paths.data + "/tmp");
            write_file(pkg, req.body);
        }
        std::string stage = g_paths.app + ".new";
        remove_tree(stage);
        // The partition holds the running version, the previous one (rollback) and the unpacked new one. Small flash
        // partitions (SPI NAND boards) cannot hold all three: check first, give up the rollback copy if that is what it takes.
        {
            struct statvfs sv;
            int64_t pkg_sz = file_size(pkg), need = pkg_sz * 3 + (4 << 20);  // unpacked is roughly 1.6x the .tar.gz
            std::string parent = g_paths.app.substr(0, g_paths.app.find_last_of('/'));
            if (parent.empty()) parent = "/";
            auto freeb = [&]() { return statvfs(parent.c_str(), &sv) == 0 ? (int64_t)sv.f_bavail * (int64_t)sv.f_frsize : INT64_MAX; };
            if (freeb() < need && file_exists(g_paths.app + ".prev")) {
                LOGW(TAG, "low space on %s: dropping the previous version (no rollback for this update)", parent.c_str());
                remove_tree(g_paths.app + ".prev");
            }
            if (freeb() < need) {
                return res.error(507, "not enough free space on " + parent + " (" + std::to_string(freeb() >> 20) + " MB free, about " +
                                          std::to_string(need >> 20) + " MB needed). Use the \"lite\" package or remove bundled models");
            }
        }
        mkdirs(stage);
        int st = -1;
        // busybox tar has no -z
        std::string out = run_cmd("gunzip -c '" + pkg + "' | tar -x -C '" + stage + "' 2>&1 && test -f '" + stage + "/picocam'", &st);
        if (st != 0 || !file_exists(stage + "/picocam") || !file_exists(stage + "/manifest.json")) {
            remove_tree(stage);
            return res.error(400, "invalid package: " + trim(out));
        }
        std::string man;
        read_file(stage + "/manifest.json", man);
        json m = json::parse(man, nullptr, false);
        // the binary must be an ARM executable whose checksum matches the manifest
        std::string bin;
        read_file(stage + "/picocam", bin);
        if (bin.size() < 4096 || bin.compare(0, 4, "\x7f" "ELF") != 0 || (unsigned char)bin[18] != 40) {
            remove_tree(stage);
            return res.error(400, "invalid package: picocam is not an ARM executable");
        }
        if (m.is_object() && m.contains("sha256") && m["sha256"].is_object() && m["sha256"].contains("picocam")) {
            if (sha256_hex(bin) != m["sha256"]["picocam"].get<std::string>()) {
                remove_tree(stage);
                return res.error(400, "invalid package: checksum mismatch (corrupt upload?)");
            }
        } else {
            LOGW(TAG, "update package has no checksum");
        }
        bin.clear();
        // carry over the service switch and any models that were added to the app dir
        run_cmd("chmod +x '" + stage + "/picocam'; [ -f '" + g_paths.app + "/enabled' ] && touch '" + stage + "/enabled'; [ -d '" +
                g_paths.app + "/models' ] && mkdir -p '" + stage + "/models' && cp -an '" + g_paths.app + "/models/.' '" + stage +
                "/models/' 2>/dev/null; true");
        remove_tree(g_paths.app + ".prev");
        if (rename(g_paths.app.c_str(), (g_paths.app + ".prev").c_str()) != 0 ||
            rename(stage.c_str(), g_paths.app.c_str()) != 0) {
            rename((g_paths.app + ".prev").c_str(), g_paths.app.c_str());
            return res.error(500, "could not install package");
        }
        // probation: the supervisor puts the previous version back if the new one keeps crashing
        write_file(g_paths.data + "/update-pending", m.value("version", "?"));
        unlink((g_paths.data + "/rolled-back").c_str());
        // make sure the boot stub is the one that runs service.sh (older installs had the whole supervisor in it)
        sys::RootWrite rw;
        run_cmd("[ -f '" + g_paths.app + "/S95picocam' ] && ! grep -q service.sh /etc/init.d/S95picocam && tr -d '\\r' < '" +
                g_paths.app + "/S95picocam' > /etc/init.d/S95picocam && chmod +x /etc/init.d/S95picocam; true");
        sync();
        audit::record(req.user, req.peer, "firmware_update", m.value("version", "?"));
        LOGW(TAG, "updated to %s by %s, restarting", m.value("version", "?").c_str(), req.user.c_str());
        res.send_json({{"ok", true}, {"version", m.value("version", "?")}});
        // let the answer go out, then exit: the supervisor starts the new binary
        std::thread([] {
            usleep(1000 * 1000);
            g_loop.stop();
        }).detach();
    }, true);
}

}  // namespace api
