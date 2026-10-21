#include "events.h"

#include <dirent.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "app/live.h"
#include "app/storage.h"
#include "app/system.h"
#include "core/board.h"
#include "core/log.h"
#include "core/util.h"
#include "media/video.h"
#include "net/http.h"
#include "app/delivery.h"
#include "net/mqtt.h"

namespace events {

static const char *TAG = "events";

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<Event> g_q;
static std::deque<json> g_index;  // newest last
static std::atomic<bool> g_run{false};
static std::thread g_th;
static std::atomic<int64_t> g_total{0}, g_dropped{0};

static std::vector<std::function<void(const json &)>> g_listeners;

void add_listener(std::function<void(const json &rec)> fn) { g_listeners.push_back(std::move(fn)); }

std::string dir() {
    std::string r = storage::root();
    if (!r.empty() && jget<bool>(g_settings.get("events"), "store_on_sd", true)) return r + "/events";
    // No SD card. On eMMC boards the internal flash is fine; on raw SPI NAND every event picture is a flash write and
    // the partition is small, so events live in RAM (lost on reboot) unless "events.internal_flash" is switched on.
    if (board::info().flash_limited && !jget<bool>(g_settings.get("events"), "internal_flash", false)) return "/tmp/picocam/events";
    return g_paths.events();
}

static int keep_limit() {
    int keep = std::max(50, jget<int>(g_settings.get("events"), "keep", 2000));
    // internal flash / RAM is small: cap it
    return storage::root().empty() ? std::min(keep, board::info().flash_limited ? 100 : 300) : keep;
}

static std::vector<std::string> list_dir(const std::string &d) {
    std::vector<std::string> v;
    DIR *dir = opendir(d.c_str());
    if (!dir) return v;
    while (dirent *e = readdir(dir))
        if (e->d_name[0] != '.') v.push_back(e->d_name);
    closedir(dir);
    std::sort(v.begin(), v.end());
    return v;
}

static void load_index() {
    std::deque<json> idx;
    int keep = keep_limit();
    auto days = list_dir(dir());
    for (auto it = days.rbegin(); it != days.rend() && (int)idx.size() < keep; ++it) {
        std::string s;
        if (!read_file(dir() + "/" + *it + "/index.jsonl", s)) continue;
        std::vector<json> day;
        for (auto &l : split(s, '\n')) {
            if (l.empty()) continue;
            json j = json::parse(l, nullptr, false);
            if (j.is_object()) day.push_back(j);
        }
        for (auto d = day.rbegin(); d != day.rend() && (int)idx.size() < keep; ++d) idx.push_front(*d);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_index.swap(idx);
    LOGI(TAG, "%zu events in %s", g_index.size(), dir().c_str());
}

// drop whole days, oldest first, until we are under the limit
static void prune() {
    int keep = keep_limit();
    std::lock_guard<std::mutex> lk(g_mu);
    bool trimmed_today = false;
    while ((int)g_index.size() > keep) {
        std::string day = g_index.front().value("day", "");
        if (day.empty()) {
            g_index.pop_front();
            continue;
        }
        // keep today even when over the limit by a little
        if (day == format_time(time(nullptr), "%Y-%m-%d") && (int)g_index.size() < keep * 2) {
            // trim oldest individual events of today
            json &e = g_index.front();
            std::string base = dir() + "/" + day + "/" + e.value("id", "");
            for (auto &kv : e.value("images", json::object()).items()) unlink((dir() + "/" + day + "/" + kv.value().get<std::string>()).c_str());
            (void)base;
            g_index.pop_front();
            trimmed_today = true;
            continue;
        }
        remove_tree(dir() + "/" + day);
        while (!g_index.empty() && g_index.front().value("day", "") == day) g_index.pop_front();
        LOGI(TAG, "pruned events of %s", day.c_str());
    }
    if (trimmed_today) {
        // the day's index file still lists the trimmed events: rewrite it so they don't come back after a restart
        std::string today = format_time(time(nullptr), "%Y-%m-%d"), text;
        for (auto &e : g_index)
            if (e.value("day", "") == today) text += e.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
        write_file(dir() + "/" + today + "/index.jsonl", text);
    }
}

void emit(Event e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_q.size() >= 8) {
        ++g_dropped;
        return;
    }
    g_q.push_back(std::move(e));
    g_cv.notify_one();
}

static void store(Event &e) {
    json cfg = g_settings.get("events");
    int64_t ts = wall_ms();
    std::string day = format_time(ts / 1000, "%Y-%m-%d");
    std::string id = format_time(ts / 1000, "%Y%m%d-%H%M%S") + "-" + random_token(2);
    std::string d = dir() + "/" + day;
    mkdirs(d);
    json images = json::object();
    std::string snap;
    if (e.snapshot && jget<bool>(cfg, "snapshot", true) && video::snapshot(snap)) {
        if (write_file(d + "/" + id + ".jpg", snap)) images["snapshot"] = id + ".jpg";
    }
    for (auto &c : e.crops) {
        std::string name = id + "_" + c.first + ".jpg";
        if (write_file(d + "/" + name, c.second)) images[c.first] = name;
    }
    json rec = {{"id", id},
                {"ts", ts},
                {"time", iso8601(ts, false)},
                {"day", day},
                {"type", e.type},
                {"rule_id", e.rule_id},
                {"rule", e.rule_name},
                {"label", e.label},
                {"track_id", e.track_id},
                {"score", std::round(e.score * 100.0) / 100.0},
                {"box", e.box.is_null() ? json::array() : e.box},
                {"attributes", e.attributes},
                {"images", images}};
    json extra = e.extra;
    if (extra.contains("embedding")) extra.erase("embedding");  // big; delivered, not stored in the index
    if (!extra.empty()) rec["extra"] = extra;
    FILE *fp = fopen((d + "/index.jsonl").c_str(), "a");
    if (fp) {
        std::string line = rec.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
        fwrite(line.data(), 1, line.size(), fp);
        fclose(fp);
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_index.push_back(rec);
    }
    ++g_total;
    prune();
    LOGI(TAG, "%s %s %s %s", e.type.c_str(), e.rule_name.c_str(), e.label.c_str(), e.attributes.dump().c_str());
    live::publish("event", rec);
    for (auto &l : g_listeners) l(rec);
    storage::trigger(e.type);

    // reliable delivery (webhooks, MQTT, EdgeAI, FTP, mail, Telegram, GPIO): pictures are read from the store later
    json drec = rec;
    if (e.extra.contains("embedding")) drec["embedding"] = e.extra["embedding"];
    delivery::enqueue(drec);
}

static void loop() {
    prctl(PR_SET_NAME, "events", 0, 0, 0);
    load_index();
    while (g_run) {
        Event e;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_q.empty() || !g_run; });
            if (g_q.empty()) continue;
            e = std::move(g_q.front());
            g_q.pop_front();
        }
        store(e);
    }
}

// ---------------------------------------------------------------- lifecycle / API

void start() {
    mkdirs(dir());
    g_run = true;
    g_th = std::thread(loop);
    delivery::start();
    g_settings.on(
        "integrations", [](json &i) { return delivery::validate(i); }, [](const json &i, const json &) { delivery::apply(i); });
}

void stop() {
    g_run = false;
    g_cv.notify_all();
    if (g_th.joinable()) g_th.join();
    delivery::stop();
}

json status() {
    json d = delivery::status();
    std::lock_guard<std::mutex> lk(g_mu);
    return {{"stored", g_index.size()}, {"total", g_total.load()}, {"dropped", g_dropped.load()}, {"dir", dir()},
            {"integrations", d["integrations"]}, {"queued", d["queued"]}, {"delivery_dropped", d["dropped"]}, {"mqtt", mqtt::status()}};
}

static bool matches(const json &e, const HttpRequest &req) {
    std::string type = req.param("type"), label = req.param("label"), rule = req.param("rule"), q = to_lower(req.param("q"));
    std::string day = req.param("date");
    if (!type.empty() && e.value("type", "") != type) return false;
    if (!label.empty() && e.value("label", "") != label) return false;
    if (!rule.empty() && e.value("rule_id", "") != rule) return false;
    if (!day.empty() && e.value("day", "") != day) return false;
    if (!q.empty()) {
        std::string hay = to_lower(e.value("rule", "") + " " + e.value("label", "") + " " + e.value("attributes", json::object()).dump());
        if (hay.find(q) == std::string::npos) return false;
    }
    return true;
}

static bool safe(const std::string &s) {
    if (s.empty() || s.size() > 80 || s.find("..") != std::string::npos) return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

void register_routes() {
    g_settings.on(
        "events",
        [](json &e) {
            int k = jget<int>(e, "keep", 2000);
            return k < 50 || k > 100000 ? std::string("keep must be 50..100000") : std::string();
        },
        nullptr);

    g_http.route("GET", "/api/events", auth::VIEWER, [](HttpRequest &req, HttpResponse &res) {
        size_t limit = (size_t)std::max(1, std::min(500, atoi(req.param("limit", "60").c_str())));
        std::string before = req.param("before");
        json out = json::array();
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto it = g_index.rbegin(); it != g_index.rend() && out.size() < limit; ++it) {
            if (!before.empty() && it->value("id", "") >= before) continue;
            if (matches(*it, req)) out.push_back(*it);
        }
        res.send_json(out);
    });

    g_http.route("GET", "/api/events/summary", auth::VIEWER, [](HttpRequest &, HttpResponse &res) {
        json by_type = json::object(), by_label = json::object(), hours = json::array();
        std::vector<int> h(24, 0);
        std::string today = format_time(time(nullptr), "%Y-%m-%d");
        int today_n = 0;
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &e : g_index) {
            if (e.value("day", "") != today) continue;
            ++today_n;
            by_type[e.value("type", "?")] = by_type.value(e.value("type", "?"), 0) + 1;
            if (!e.value("label", "").empty()) by_label[e.value("label", "")] = by_label.value(e.value("label", ""), 0) + 1;
            std::string t = e.value("time", "");
            if (t.size() >= 13) h[atoi(t.substr(11, 2).c_str()) % 24]++;
        }
        for (int v : h) hours.push_back(v);
        res.send_json({{"today", today_n}, {"stored", g_index.size()}, {"by_type", by_type}, {"by_label", by_label}, {"hours", hours}});
    });

    // CSV export (ANPR / counting logs)
    g_http.route("GET", "/api/events/export.csv", auth::VIEWER, [](HttpRequest &req, HttpResponse &res) {
        std::string csv = "id,time,type,rule,label,track,score,attributes\n";
        auto esc = [](std::string s) {
            for (auto &c : s)
                if (c == '"') c = '\'';
            if (!s.empty() && strchr("=+-@", s[0])) s = "'" + s;  // spreadsheet formula injection
            return "\"" + s + "\"";
        };
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &e : g_index) {
            if (!matches(e, req)) continue;
            std::string attrs;
            for (auto &kv : e.value("attributes", json::object()).items())
                attrs += (attrs.empty() ? "" : "; ") + kv.key() + "=" + (kv.value().is_string() ? kv.value().get<std::string>() : kv.value().dump());
            csv += e.value("id", "") + "," + e.value("time", "") + "," + e.value("type", "") + "," + esc(e.value("rule", "")) + "," +
                   esc(e.value("label", "")) + "," + std::to_string(e.value("track_id", 0)) + "," +
                   std::to_string(e.value("score", 0.0)) + "," + esc(attrs) + "\n";
        }
        res.headers["Content-Type"] = "text/csv";
        res.headers["Content-Disposition"] = "attachment; filename=\"picocam-events.csv\"";
        res.body = csv;
    });

    // /api/events/<id>/<image>.jpg
    g_http.route("GET", "/api/events/", auth::VIEWER, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() == 1) {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto &e : g_index)
                if (e.value("id", "") == req.path_args[0]) return res.send_json(e);
            return res.error(404, "no such event");
        }
        if (req.path_args.size() != 2 || !safe(req.path_args[0]) || !safe(req.path_args[1])) return res.error(400, "bad path");
        const std::string &id = req.path_args[0];
        if (id.size() < 8) return res.error(400, "bad id");
        std::string day = id.substr(0, 4) + "-" + id.substr(4, 2) + "-" + id.substr(6, 2);
        std::string p = dir() + "/" + day + "/" + req.path_args[1];
        if (!file_exists(p)) return res.error(404, "not found");
        res.headers["Content-Type"] = "image/jpeg";
        res.headers["Cache-Control"] = "private, max-age=86400";
        res.file = p;
        res.file_len = file_size(p);
    });

    g_http.route("DELETE", "/api/events", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto &d : list_dir(dir())) remove_tree(dir() + "/" + d);
        g_index.clear();
        LOGW(TAG, "all events deleted by %s", req.user.c_str());
        res.ok();
    }, true);  // async: removing thousands of files must not stall the video loop

    g_http.route("GET", "/api/integrations/status", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) { res.send_json(status()); });

    g_http.route("POST", "/api/integrations/test", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        Event e;
        e.type = "test";
        e.rule_name = "Test event";
        e.label = jget<std::string>(b, "label", "person");
        e.attributes = {{"note", "test event from the web UI"}};
        emit(std::move(e));
        res.ok();
    });
}

}  // namespace events
