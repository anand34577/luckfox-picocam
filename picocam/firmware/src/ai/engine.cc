#include "engine.h"

#include <sys/prctl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <functional>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "app/events.h"
#include "app/faces.h"
#include "app/live.h"
#include "core/audit.h"
#include "core/log.h"
#include "core/util.h"
#include "detectors.h"
#include "dmabuf.h"
#include "imgproc.h"
#include "media/isp.h"
#include "media/video.h"
#include "models.h"
#include "motion.h"
#include "plugin.h"
#include "rawstage.h"
#include "net/http.h"
#include "textvote.h"
#include "tracker.h"

namespace ai {

static const char *TAG = "ai";

// ---------------------------------------------------------------- pipeline definition

struct Stage {
    std::string name, type, model_id;
    std::vector<std::string> on;  // parent classes this stage runs on (empty = all)
    float conf = 0.3f;
    int min_px = 0, interval_ms = 200;
    float votes = 3.f;            // vote score at which an attribute counts as resolved
    std::string format = "none";  // ocr: indian | none
    std::string charset;
    float threshold = 0.45f;  // embed: face match threshold (cosine similarity)
    bool gallery = true;      // embed: look the face up in the enrolled people
    std::string url, verify = "system", pin;  // raw: where the tensors go
    json meta;
    // loaded model (one of these)
    std::unique_ptr<Yolov5> det;
    std::unique_ptr<PlateOcr> ocr;
    std::unique_ptr<Classifier> cls;
    std::unique_ptr<FaceEmbedder> emb;
    std::unique_ptr<RawModel> raw;
    std::unique_ptr<Stage> then;
};

struct Rule {
    std::string id, name, type;
    bool enabled = true;
    std::vector<std::string> classes;
    std::vector<std::pair<float, float>> zone;  // normalised polygon
    std::pair<float, float> la{0, 0}, lb{0, 0};  // line
    bool has_line = false;
    std::string direction = "any";
    float dwell_s = 10, cooldown_s = 10;
    int threshold = 1;
    json attribute;  // {"name","op","values"}
    int64_t last_fire = 0;
    int count_in = 0, count_out = 0;  // line counters
    bool reset_daily = false;         // line counters go back to 0 at local midnight
    // "track" rules: skip a record whose attribute value was already logged within dedupe_s
    float dedupe_s = 0;
    std::string dedupe_attr;
    std::map<std::string, int64_t> recent;
};

struct AttrVote {
    std::map<std::string, float> votes;
    std::string best;
    float best_score = 0;
    bool resolved = false;
    void add(const std::string &v, float s, float need) {
        float &x = votes[v];
        x += s;
        if (x > best_score) best_score = x, best = v;
        if (best_score >= need) resolved = true;
    }
    // text readings: a consensus per character position instead of a vote between whole strings
    std::vector<std::pair<std::string, float>> reads;
    void add_text(const std::string &v, float s, float need) {
        reads.push_back({v, s});
        if (reads.size() > 24) reads.erase(reads.begin());
        int support = 0;
        best = text_consensus(reads, &best_score, &support);
        if (best_score >= need && support >= 2) resolved = true;
    }
};

struct TrackState {
    std::map<std::string, AttrVote> attrs;
    std::map<std::string, int64_t> last_run;  // stage name -> ms
    std::map<std::string, std::string> crops;  // name -> JPEG (best observation)
    std::map<std::string, float> crop_score;
    std::vector<float> embedding;
    RectF part;  // last part box (e.g. plate), for the overlay
    int64_t part_ms = 0;
    std::map<std::string, int64_t> zone_enter;  // rule id -> ms entered zone
    std::map<std::string, bool> fired;          // rule id -> already fired for this track
    bool completed = false;
    float last_bx = -1, last_by = -1;  // previous anchor point (normalised) for line crossing
    float prev_cx = -1, prev_cy = -1;  // box centre (frame pixels) and time of the previous observation
    int64_t prev_ms = 0;
    float speed = 0;  // px/s, smoothed
};

struct Pipeline {
    json cfg;
    std::string det_model, det_task;
    json det_meta;
    std::unique_ptr<Yolov5> yolo;
    std::unique_ptr<RetinaFace> face;
    std::vector<std::string> classes;  // allowed detector classes (empty = all)
    std::vector<std::pair<float, float>> roi;  // detection area (normalised polygon); empty = whole picture
    int tiles_x = 1, tiles_y = 1;              // the area is analysed as overlapping tiles (small objects)
    std::vector<std::unique_ptr<Stage>> stages;
    IouTracker tracker;
    std::map<int, TrackState> st;
    std::vector<Rule> rules;
};

// ---------------------------------------------------------------- state

static std::mutex g_mu;  // pipeline (re)configuration vs the AI thread
static std::unique_ptr<Pipeline> g_pipe;
static std::thread g_th;
static std::atomic<bool> g_run{false};
static std::string g_error;
static int g_hook = 0;

// frame handoff from the capture thread
static std::mutex g_fmu;
static std::condition_variable g_fcv;
static std::vector<uint8_t> g_fbuf;
static DmaBuf g_fdma;  // the same frame copy in media memory, so the RGA can read it (preferred over g_fbuf)
static Frame g_frame;
static bool g_busy = false, g_ready = false;
static int64_t g_last_take = 0;
static std::atomic<int> g_max_fps{10};
static std::atomic<bool> g_gate{false};  // run the NPU only while motion is seen
static std::atomic<int> g_gate_hold_ms{5000};
static std::atomic<int64_t> g_last_proc{0};
static std::mutex g_reconf_mu;  // one full restart of video + AI at a time (settings, scheduler)
static std::string g_cur_profile;
static std::thread g_sched;
static std::atomic<bool> g_sched_run{false};

// overlay drawn into the sub stream
struct OvItem {
    RectF box;  // normalised
    OverlayColor color;
    std::string label;
};
static std::mutex g_ovmu;
static std::vector<OvItem> g_overlay;
static int64_t g_overlay_ms = 0;
static std::atomic<bool> g_burn{false};

static std::atomic<float> g_fps{0}, g_ms{0};
static std::atomic<int> g_tracks{0};
static int64_t g_frames = 0;

// ---------------------------------------------------------------- helpers

static bool in_list(const std::vector<std::string> &l, const std::string &v) {
    return l.empty() || std::find(l.begin(), l.end(), v) != l.end();
}

static std::vector<std::string> str_list(const json &j) {
    std::vector<std::string> v;
    if (j.is_array())
        for (auto &x : j)
            if (x.is_string()) v.push_back(x.get<std::string>());
    return v;
}

static bool point_in_poly(float x, float y, const std::vector<std::pair<float, float>> &p) {
    bool in = false;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
        if (((p[i].second > y) != (p[j].second > y)) &&
            (x < (p[j].first - p[i].first) * (y - p[i].second) / (p[j].second - p[i].second) + p[i].first))
            in = !in;
    }
    return in;
}

static float cross(std::pair<float, float> a, std::pair<float, float> b, float x, float y) {
    return (b.first - a.first) * (y - a.second) - (b.second - a.second) * (x - a.first);
}

// segment p1-p2 intersects segment a-b
static bool seg_cross(float x1, float y1, float x2, float y2, std::pair<float, float> a, std::pair<float, float> b) {
    float d1 = cross(a, b, x1, y1), d2 = cross(a, b, x2, y2);
    if ((d1 > 0) == (d2 > 0) || d1 == 0) return false;
    std::pair<float, float> p{x1, y1}, q{x2, y2};
    float d3 = cross(p, q, a.first, a.second), d4 = cross(p, q, b.first, b.second);
    return (d3 > 0) != (d4 > 0);
}

static std::string fmt_plate(const std::string &raw, const std::string &format, bool *valid) {
    if (format == "indian") return normalize_indian_plate(raw, valid);
    std::string s;
    for (char c : raw)
        if (isalnum((unsigned char)c)) s += (char)toupper((unsigned char)c);
    *valid = s.size() >= 4;
    return s;
}

// ---------------------------------------------------------------- loading

static float clampf01(float v) { return std::max(0.f, std::min(1.f, v)); }

// numbers of a possibly nested array, flattened ([[10,13,16,30,...],[...]] or [10,13,...])
static void flatten(const json &j, std::vector<float> &out) {
    if (j.is_number()) out.push_back(j.get<float>());
    else if (j.is_array())
        for (auto &x : j) flatten(x, out);
}

// What a model says about itself (its manifest, the sidecar json) decides how its outputs are read
static std::string apply_detector_meta(Yolov5 &y, const json &meta) {
    y.decoder = jget<std::string>(meta, "decoder", "auto");
    y.anchors.clear();
    if (meta.contains("anchors")) {
        flatten(meta["anchors"], y.anchors);
        if (y.anchors.size() != 18) y.anchors.clear();  // 3 heads x 3 anchors x (w, h)
    }
    y.stretch = jget<bool>(meta, "stretch", false);
    y.class_agnostic = jget<bool>(meta, "class_agnostic", false);
    if (y.decoder == "plugin") {
        std::string e;
        y.plugin = load_plugin(jget<std::string>(meta, "plugin", ""), &e);
        if (!y.plugin) return e;
    }
    return "";
}

static void apply_ocr_meta(PlateOcr &o, const json &meta) {
    o.time_major = jget<std::string>(meta, "layout", "ct") == "tc";
    std::string b = jget<std::string>(meta, "blank", "last");
    o.blank = b == "first" ? 0 : (b == "last" ? -1 : atoi(b.c_str()));
}

// Tunable stage settings (everything except which models are loaded): also re-applied live, without a rebuild
static void stage_params(Stage &s, const json &j) {
    s.on = str_list(j.value("on", json::array()));
    s.conf = jget<float>(j, "conf", 0.3f);
    s.min_px = jget<int>(j, "min_px", 0);
    s.interval_ms = std::max(0, jget<int>(j, "interval_ms", 200));
    s.votes = jget<float>(j, "votes", 3.f);
    s.format = jget<std::string>(j, "format", "none");
    s.charset = jget<std::string>(j, "charset", jget<std::string>(s.meta, "charset", "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
    s.threshold = clampf01(jget<float>(j, "threshold", 0.45f));
    s.gallery = jget<bool>(j, "gallery", true);
    s.url = jget<std::string>(j, "url", "");
    s.verify = jget<std::string>(j, "verify", "system");
    s.pin = jget<std::string>(j, "pin", "");
    if (s.det) s.det->conf = s.conf;
    if (s.ocr) s.ocr->charset = s.charset;
}

static std::string load_stage(const json &j, std::unique_ptr<Stage> &out, std::vector<NpuModel *> &npu) {
    auto s = std::unique_ptr<Stage>(new Stage());
    s->name = jget<std::string>(j, "name", "");
    s->type = jget<std::string>(j, "type", "");
    s->model_id = jget<std::string>(j, "model", "");
    if (s->name.empty()) s->name = s->type;
    std::string p = models::path(s->model_id);
    if (p.empty()) return "stage '" + s->name + "': model " + s->model_id + " not found";
    s->meta = models::meta(s->model_id);
    bool bgr = jget<std::string>(s->meta, "color", "rgb") == "bgr";
    stage_params(*s, j);
    if (s->type == "detect") {
        s->det.reset(new Yolov5());
        s->det->conf = s->conf;
        s->det->min_box_px = 6;
        s->det->bgr = bgr;
        std::string me = apply_detector_meta(*s->det, s->meta);
        if (!me.empty()) return me;
        if (!s->det->load(p)) return "cannot load " + s->model_id;
        npu.push_back(&s->det->model);
    } else if (s->type == "ocr") {
        s->ocr.reset(new PlateOcr());
        s->ocr->charset = s->charset;
        s->ocr->bgr = bgr;
        apply_ocr_meta(*s->ocr, s->meta);
        if (!s->ocr->load(p)) return "cannot load " + s->model_id;
        npu.push_back(&s->ocr->model);
    } else if (s->type == "classify") {
        s->cls.reset(new Classifier());
        s->cls->bgr = bgr;
        s->cls->center_crop = jget<bool>(s->meta, "center_crop", false);
        if (!s->cls->load(p)) return "cannot load " + s->model_id;
        npu.push_back(&s->cls->model);
    } else if (s->type == "embed") {
        s->emb.reset(new FaceEmbedder());
        s->emb->bgr = jget<std::string>(s->meta, "color", "bgr") == "bgr";
        if (!s->emb->load(p)) return "cannot load " + s->model_id;
        npu.push_back(&s->emb->model);
    } else if (s->type == "raw") {
        if (s->url.empty()) return "stage '" + s->name + "': raw stages need a URL";
        s->raw.reset(new RawModel());
        s->raw->bgr = bgr;
        if (!s->raw->load(p)) return "cannot load " + s->model_id;
        npu.push_back(&s->raw->model);
    } else {
        return "stage '" + s->name + "': unknown type " + s->type;
    }
    if (j.contains("then") && j["then"].is_object()) {
        std::string e = load_stage(j["then"], s->then, npu);
        if (!e.empty()) return e;
    }
    out = std::move(s);
    return "";
}

static bool is_pt(const json &p) { return p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number(); }

static std::vector<Rule> parse_rules(const json &rules) {
    std::vector<Rule> out;
    if (!rules.is_array()) return out;
    for (auto &j : rules) {
        try {
            Rule r;
            r.id = jget<std::string>(j, "id", random_token(4));
            r.name = jget<std::string>(j, "name", r.id);
            r.type = jget<std::string>(j, "type", "intrusion");
            r.enabled = jget<bool>(j, "enabled", true);
            if (r.type == "motion") continue;  // handled by the motion detector
            r.classes = str_list(j.value("classes", json::array()));
            if (j.contains("zone") && j["zone"].is_array())
                for (auto &p : j["zone"])
                    if (is_pt(p)) r.zone.push_back({p[0].get<float>(), p[1].get<float>()});
            if (j.contains("line") && j["line"].is_array() && j["line"].size() == 2 && is_pt(j["line"][0]) && is_pt(j["line"][1])) {
                r.la = {j["line"][0][0].get<float>(), j["line"][0][1].get<float>()};
                r.lb = {j["line"][1][0].get<float>(), j["line"][1][1].get<float>()};
                r.has_line = true;
            }
            r.direction = jget<std::string>(j, "direction", "any");
            r.dwell_s = jget<float>(j, "dwell_s", 10);
            r.cooldown_s = jget<float>(j, "cooldown_s", 10);
            r.threshold = jget<int>(j, "threshold", 1);
            r.reset_daily = jget<bool>(j, "reset_daily", false);
            r.dedupe_s = jget<float>(j, "dedupe_s", 0);
            r.dedupe_attr = jget<std::string>(j, "dedupe_attr", "");
            r.attribute = j.value("attribute", json());
            out.push_back(r);
        } catch (const std::exception &e) {
            LOGW(TAG, "skipping a malformed rule: %s", e.what());
        }
    }
    return out;
}

// ---- line counters survive restarts and rule edits (people counting is data, not state)
static std::string counters_path() { return g_paths.data + "/counters.json"; }
static json g_counters = json::object();  // rule id -> {a, b}; "_day" = date of the last daily reset
static int64_t g_counters_saved = 0;
static bool g_counters_dirty = false;

static void counters_load() {
    std::string s;
    g_counters = json::object();
    if (read_file(counters_path(), s)) {
        json j = json::parse(s, nullptr, false);
        if (j.is_object()) g_counters = j;
    }
}

static void counters_restore(std::vector<Rule> &rules) {
    for (auto &r : rules) {
        auto it = g_counters.find(r.id);
        if (it == g_counters.end() || !it->is_object()) continue;
        r.count_in = jget<int>(*it, "a", 0);
        r.count_out = jget<int>(*it, "b", 0);
    }
}

static void counters_save(const std::vector<Rule> &rules, bool force) {
    if (!g_counters_dirty || (!force && now_ms() - g_counters_saved < 20000)) return;
    for (auto &r : rules)
        if (r.type == "line_cross") g_counters[r.id] = {{"a", r.count_in}, {"b", r.count_out}};
    g_counters["_day"] = format_time(time(nullptr), "%Y-%m-%d");
    write_file(counters_path(), g_counters.dump());
    g_counters_saved = now_ms();
    g_counters_dirty = false;
}

// detection area and tiling from the detector settings
static void read_area(Pipeline &p, const json &d) {
    p.roi.clear();
    for (auto &q : d.value("roi", json::array()))
        if (is_pt(q)) p.roi.push_back({q[0].get<float>(), q[1].get<float>()});
    if (p.roi.size() < 3) p.roi.clear();
    json tiles = d.value("tiles", json::array({1, 1}));
    p.tiles_x = p.tiles_y = 1;
    if (tiles.is_array() && tiles.size() == 2 && tiles[0].is_number() && tiles[1].is_number()) {
        p.tiles_x = std::max(1, std::min(4, tiles[0].get<int>()));
        p.tiles_y = std::max(1, std::min(4, tiles[1].get<int>()));
    }
}

static std::string build(const json &cfg, std::unique_ptr<Pipeline> &out) {
    auto p = std::unique_ptr<Pipeline>(new Pipeline());
    p->cfg = cfg;
    json d = cfg.value("detector", json::object());
    p->det_model = jget<std::string>(d, "model", "");
    if (p->det_model.empty()) return "no detector model selected";
    std::string path = models::path(p->det_model);
    if (path.empty()) return "detector model " + p->det_model + " not found";
    p->det_meta = models::meta(p->det_model);
    p->det_task = jget<std::string>(p->det_meta, "task", "detect");
    bool bgr = jget<std::string>(p->det_meta, "color", "rgb") == "bgr";
    std::vector<NpuModel *> npu;
    if (p->det_task == "face_detect") {
        p->face.reset(new RetinaFace());
        p->face->conf = jget<float>(d, "conf", 0.6f);
        p->face->bgr = jget<std::string>(p->det_meta, "color", "bgr") == "bgr";
        json tiles = d.value("tiles", json::array({1, 1}));
        if (tiles.is_array() && tiles.size() == 2) p->face->tiles_x = tiles[0], p->face->tiles_y = tiles[1];
        if (!p->face->load(path)) return "cannot load detector " + p->det_model;
        npu.push_back(&p->face->model);
    } else {
        p->yolo.reset(new Yolov5());
        p->yolo->conf = jget<float>(d, "conf", 0.4f);
        p->yolo->nms_thr = jget<float>(d, "nms", 0.45f);
        p->yolo->min_box_px = jget<float>(d, "min_box_px", 12);
        p->yolo->bgr = bgr;
        std::string me = apply_detector_meta(*p->yolo, p->det_meta);
        if (!me.empty()) return me;
        if (!p->yolo->load(path)) return "cannot load detector " + p->det_model;
        npu.push_back(&p->yolo->model);
    }
    p->classes = str_list(d.value("classes", json::array()));
    read_area(*p, d);
    for (auto &sj : cfg.value("stages", json::array())) {
        if (!jget<bool>(sj, "enabled", true)) continue;
        std::unique_ptr<Stage> s;
        std::string e = load_stage(sj, s, npu);
        if (!e.empty()) return e;
        p->stages.push_back(std::move(s));
    }
    // models run one after another on this thread: share one activation buffer (saves media memory)
    if (!npu_share_internal(npu)) return "not enough media memory for these models (try smaller models or a lower main-stream resolution)";
    json tr = cfg.value("track", json::object());
    p->tracker.iou_thr = jget<float>(tr, "iou", 0.25f);
    p->tracker.max_misses = jget<int>(tr, "max_misses", 10);
    out = std::move(p);
    return "";
}

// ---------------------------------------------------------------- profiles

// Several pipelines can be defined ("profiles", e.g. ANPR by day, people at night). One is chosen by hand
// (active_profile) or by the weekly schedule grid of each profile.
static std::string profile_for_now(const json &ai) {
    json profiles = ai.value("profiles", json::array());
    if (!profiles.is_array() || profiles.empty()) return "";
    std::string act = jget<std::string>(ai, "active_profile", "auto");
    if (act != "auto")
        for (auto &p : profiles)
            if (jget<std::string>(p, "id", "") == act) return act;
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    int slot = tm.tm_wday * 24 + tm.tm_hour;
    for (auto &p : profiles) {
        json s = p.value("schedule", json::array());
        if (s.is_array() && s.size() == 168 && s[slot].is_number() && s[slot].get<int>() != 0) return jget<std::string>(p, "id", "");
    }
    for (auto &p : profiles)
        if (jget<bool>(p, "default", false)) return jget<std::string>(p, "id", "");
    return jget<std::string>(profiles[0], "id", "");
}

// the AI settings with the active profile's pipeline laid over them
static json effective(const json &ai) {
    std::string id = profile_for_now(ai);
    if (id.empty()) return ai;
    json e = ai;
    for (auto &p : ai.value("profiles", json::array()))
        if (jget<std::string>(p, "id", "") == id) {
            json c = p.value("config", json::object());
            for (const char *k : {"detector", "stages", "track", "max_fps", "run_when", "motion_hold_s"})
                if (c.contains(k)) e[k] = c[k];
        }
    e["_profile"] = id;
    return e;
}

static void apply_gate(const json &cfg) {
    g_gate = jget<std::string>(cfg, "run_when", "always") == "motion";
    g_gate_hold_ms = std::max(1, std::min(300, jget<int>(cfg, "motion_hold_s", 5))) * 1000;
}

// ---------------------------------------------------------------- live re-configuration

static json stage_sig(const json &j) {
    json o = {{"t", jget<std::string>(j, "type", "")}, {"m", jget<std::string>(j, "model", "")}, {"n", jget<std::string>(j, "name", "")}};
    if (j.contains("then") && j["then"].is_object()) o["then"] = stage_sig(j["then"]);
    return o;
}

// What forces a rebuild (models in memory, stage layout). Thresholds, classes, tracker and rates do not.
static json structure(const json &ai) {
    json d = ai.value("detector", json::object());
    json st = json::array();
    for (auto &sj : ai.value("stages", json::array()))
        if (jget<bool>(sj, "enabled", true)) st.push_back(stage_sig(sj));
    // the preprocessing backend decides whether an RGA frame buffer is part of the media memory plan
    return {{"en", jget<bool>(ai, "enabled", false)}, {"det", jget<std::string>(d, "model", "")}, {"stages", st},
            {"cpu", jget<std::string>(ai, "preprocess", "auto") == "cpu"}};
}

static void apply_stage_hot(Stage &s, const json &j) {
    stage_params(s, j);
    if (s.then && j.contains("then") && j["then"].is_object()) apply_stage_hot(*s.then, j["then"]);
}

static void hot_apply(Pipeline &p, const json &cfg) {
    json d = cfg.value("detector", json::object());
    if (p.face) {
        p.face->conf = jget<float>(d, "conf", 0.6f);
    } else if (p.yolo) {
        p.yolo->conf = jget<float>(d, "conf", 0.4f);
        p.yolo->nms_thr = jget<float>(d, "nms", 0.45f);
        p.yolo->min_box_px = jget<float>(d, "min_box_px", 12);
    }
    p.classes = str_list(d.value("classes", json::array()));
    if (p.yolo) read_area(p, d);
    size_t i = 0;
    for (auto &sj : cfg.value("stages", json::array())) {
        if (!jget<bool>(sj, "enabled", true) || i >= p.stages.size()) continue;
        apply_stage_hot(*p.stages[i++], sj);
    }
    json tr = cfg.value("track", json::object());
    p.tracker.iou_thr = jget<float>(tr, "iou", 0.25f);
    p.tracker.max_misses = jget<int>(tr, "max_misses", 10);
}

// ---------------------------------------------------------------- per-frame processing

static std::string det_label(Pipeline &p, int cls) {
    if (p.det_task == "face_detect") return "face";
    return models::label(p.det_meta, cls);
}

static void keep_crop(TrackState &ts, const std::string &name, const Frame &f, RectF r, float score, int min_w) {
    if (score <= ts.crop_score[name]) return;
    std::vector<uint8_t> jpg;
    if (jpeg_encode(f, r, 480, min_w, 88, jpg)) {
        ts.crops[name].assign(jpg.begin(), jpg.end());
        ts.crop_score[name] = score;
    }
}

// runs a stage on a region of the frame; returns true if it produced something
static void run_stage(Stage &s, const Frame &f, RectF roi, TrackState &ts, int64_t now, float parent_score, const Det *det = nullptr,
                      int track_id = 0) {
    ts.last_run[s.name] = now;
    if (s.type == "detect") {
        auto parts = s.det->detect(f, roi.expand(0.05f, 0.05f).clip((float)f.w, (float)f.h));
        if (parts.empty()) return;
        const Det &d = parts[0];
        if (s.min_px && d.box.w() < s.min_px) return;
        ts.part = d.box;
        ts.part_ms = now;
        keep_crop(ts, s.name, f, d.box.expand(0.08f, 0.15f), d.score * d.box.w(), 240);
        if (s.then) run_stage(*s.then, f, d.box, ts, now, d.score, nullptr, track_id);
        else ts.attrs[s.name].add("yes", d.score, s.votes);
    } else if (s.type == "ocr") {
        RectF pr = roi.expand(0.06f, 0.12f).clip((float)f.w, (float)f.h);
        float conf = 0;
        bool valid = false;
        std::string text = fmt_plate(s.ocr->read(f, pr, &conf), s.format, &valid);
        if (!valid && pr.w() / std::max(1.f, pr.h()) < 3.2f) {
            // two-row plate (bikes, autos): read each half and join
            RectF top{pr.x1, pr.y1, pr.x2, pr.cy() + pr.h() * 0.04f};
            RectF bot{pr.x1, pr.cy() - pr.h() * 0.04f, pr.x2, pr.y2};
            float c1 = 0, c2 = 0;
            bool v2 = false;
            std::string t2 = fmt_plate(s.ocr->read(f, top, &c1) + s.ocr->read(f, bot, &c2), s.format, &v2);
            if (v2 || (c1 + c2) / 2 > conf) text = t2, conf = (c1 + c2) / 2, valid = v2;
        }
        if (text.size() < 4 || conf < s.conf) return;
        ts.attrs[s.name].add_text(text, conf * parent_score * (valid ? 1.f : 0.3f), s.votes);
    } else if (s.type == "classify") {
        float prob = 0;
        int c = s.cls->classify(f, roi, &prob);
        if (c < 0 || prob < s.conf) return;
        ts.attrs[s.name].add(models::label(s.meta, c), prob, s.votes);
    } else if (s.type == "embed") {
        Det d;
        if (det) d = *det;  // landmarks from the face detector align the face before the embedding
        d.box = roi;
        auto e = s.emb->embed(f, d);
        if (!e.empty()) {
            ts.embedding = e;
            ts.attrs[s.name].add("ok", 1.f, 1.f);
            faces::offer(e, track_id);  // an enrolment waiting for a face takes it
            faces::Match m;
            if (s.gallery && !faces::empty()) {
                if (faces::match(e, s.threshold, &m)) ts.attrs["identity"].add(m.name, m.score, 1.f);
                else ts.attrs["identity"].add("unknown", 0.3f, 1.f);
            }
        }
    } else if (s.type == "raw") {
        json body = s.raw->run(f, roi);
        if (body.is_null()) return;
        body["model"] = s.model_id;
        body["track_id"] = track_id;
        body["box"] = {roi.x1 / f.w, roi.y1 / f.h, roi.x2 / f.w, roi.y2 / f.h};
        body["ts"] = f.ts_ms;
        body["device"] = g_settings.get("device").value("id", "");
        rawstage::send(s.url, s.verify, s.pin, track_id, std::move(body));
    }
}

static json track_attrs(const TrackState &ts) {
    json a = json::object();
    for (auto &kv : ts.attrs)
        if (!kv.second.best.empty() && kv.second.best != "ok") a[kv.first] = kv.second.best;
    return a;
}

static bool attr_match(const Rule &r, const TrackState &ts) {
    if (!r.attribute.is_object() || jget<std::string>(r.attribute, "name", "").empty()) return true;
    std::string name = jget<std::string>(r.attribute, "name", ""), op = jget<std::string>(r.attribute, "op", "exists");
    auto it = ts.attrs.find(name);
    bool have = it != ts.attrs.end() && it->second.resolved;
    if (op == "exists") return have;
    if (op == "missing") return !have;
    if (!have) return false;
    std::string v = it->second.best;
    bool listed = false;
    // fuzzy: tolerate that many misread characters (look-alikes such as 0/O or 8/B count for less)
    float fuzzy = jget<float>(r.attribute, "fuzzy", 0.f);
    for (auto &x : r.attribute.value("values", json::array()))
        if (x.is_string() && (to_lower(x.get<std::string>()) == to_lower(v) ||
                              (fuzzy > 0 && text_distance(x.get<std::string>(), v) <= fuzzy + 0.01f)))
            listed = true;
    return op == "in" ? listed : !listed;
}

static void fire(Rule &r, const Track &t, TrackState &ts, const std::string &label, const Frame &f, json extra = json::object()) {
    events::Event e;
    e.type = r.type;
    e.rule_id = r.id;
    e.rule_name = r.name;
    e.label = label;
    e.track_id = t.id;
    e.score = t.score;
    e.box = {t.box.x1 / f.w, t.box.y1 / f.h, t.box.x2 / f.w, t.box.y2 / f.h};
    e.attributes = track_attrs(ts);
    if (!ts.embedding.empty()) e.extra["embedding"] = ts.embedding;
    for (auto &kv : ts.crops) e.crops.push_back(kv);
    // a crop of the object itself, from this frame
    std::vector<uint8_t> jpg;
    if (jpeg_encode(f, t.box.expand(0.1f, 0.1f), 480, 160, 85, jpg)) e.crops.push_back({"object", std::string(jpg.begin(), jpg.end())});
    // "ai": the picture of the event is the frame the AI saw (the main-stream snapshot is taken after the AI delay and may
    // miss a fast mover); lower resolution but exactly the moment of the detection
    if (jget<std::string>(g_settings.get("events"), "snapshot_source", "main") == "ai") {
        std::vector<uint8_t> whole;
        if (jpeg_encode(f, RectF{}, 0, 0, 85, whole)) {
            e.crops.push_back({"snapshot", std::string(whole.begin(), whole.end())});
            e.snapshot = false;
        }
    }
    for (auto it = extra.begin(); it != extra.end(); ++it) e.extra[it.key()] = *it;
    events::emit(std::move(e));
    r.last_fire = now_ms();
}

static void eval_rules(Pipeline &p, const Frame &f, int64_t now) {
    std::map<std::string, int> zone_counts;
    for (auto &t : p.tracker.tracks()) {
        if (!t.confirmed()) continue;
        TrackState &ts = p.st[t.id];
        std::string label = det_label(p, t.cls);
        // anchor: bottom centre (feet, wheels) is what crosses a line on the ground
        float bx = t.box.cx() / f.w, by = t.box.y2 / f.h;
        for (auto &r : p.rules) {
            if (!r.enabled || !in_list(r.classes, label)) continue;
            bool cooled = now - r.last_fire >= r.cooldown_s * 1000;
            bool inside = r.zone.size() >= 3 ? point_in_poly(bx, by, r.zone) : true;
            if (r.type == "intrusion") {
                // a rule in cooldown leaves the object un-fired, so it still alerts once the cooldown ends
                if (inside && t.visible() && cooled && !ts.fired[r.id] && attr_match(r, ts)) {
                    ts.fired[r.id] = true;
                    fire(r, t, ts, label, f);
                }
            } else if (r.type == "loitering") {
                if (inside && t.visible()) {
                    if (!ts.zone_enter[r.id]) ts.zone_enter[r.id] = now;
                    if (!ts.fired[r.id] && now - ts.zone_enter[r.id] >= r.dwell_s * 1000 && attr_match(r, ts)) {
                        ts.fired[r.id] = true;
                        fire(r, t, ts, label, f, {{"dwell_s", (now - ts.zone_enter[r.id]) / 1000}});
                    }
                } else if (!inside) {
                    ts.zone_enter[r.id] = 0;
                }
            } else if (r.type == "line_cross" && r.has_line) {
                if (ts.last_bx >= 0 && t.visible() && seg_cross(ts.last_bx, ts.last_by, bx, by, r.la, r.lb)) {
                    bool a_to_b = cross(r.la, r.lb, ts.last_bx, ts.last_by) > 0;  // left side -> right side
                    std::string dir = a_to_b ? "a_to_b" : "b_to_a";
                    (a_to_b ? r.count_in : r.count_out)++;
                    g_counters_dirty = true;
                    if ((r.direction == "any" || r.direction == dir) && attr_match(r, ts))
                        fire(r, t, ts, label, f, {{"direction", dir}, {"count_a_to_b", r.count_in}, {"count_b_to_a", r.count_out}});
                }
            } else if (r.type == "count") {
                if (inside && t.visible()) zone_counts[r.id]++;
            } else if (r.type == "presence") {
                if (inside && t.visible() && cooled && attr_match(r, ts) && !ts.fired[r.id]) {
                    ts.fired[r.id] = true;
                    fire(r, t, ts, label, f);
                }
            }
        }
        if (t.visible()) ts.last_bx = bx, ts.last_by = by;
    }
    {
        std::string today = format_time(time(nullptr), "%Y-%m-%d");
        if (g_counters.value("_day", today) != today) {
            for (auto &r : p.rules)
                if (r.reset_daily) r.count_in = r.count_out = 0;
            g_counters_dirty = true;
            counters_save(p.rules, true);
        }
        counters_save(p.rules, false);
    }
    for (auto &r : p.rules) {
        if (!r.enabled || r.type != "count") continue;
        int n = zone_counts[r.id];
        if (n >= r.threshold && now - r.last_fire >= r.cooldown_s * 1000) {
            events::Event e;
            e.type = "count";
            e.rule_id = r.id;
            e.rule_name = r.name;
            e.extra = {{"count", n}, {"threshold", r.threshold}};
            events::emit(std::move(e));
            r.last_fire = now;
        }
    }
}

// "track" rules: one record per object once its attributes are read (or when it leaves), e.g. ANPR logs
static void complete_track(Pipeline &p, const Track &t, TrackState &ts, const Frame &f) {
    if (ts.completed) return;
    ts.completed = true;
    std::string label = det_label(p, t.cls);
    int64_t now = now_ms();
    for (auto &r : p.rules) {
        if (!(r.enabled && r.type == "track" && in_list(r.classes, label) && attr_match(r, ts) &&
              (r.zone.size() < 3 || ts.last_bx < 0 || point_in_poly(ts.last_bx, ts.last_by, r.zone))))
            continue;
        if (r.dedupe_s > 0) {
            // the same plate (or other attribute value) seen again within dedupe_s is one visit, not two
            std::string key;
            for (auto &kv : ts.attrs) {
                if (!r.dedupe_attr.empty() && kv.first != r.dedupe_attr) continue;
                if (!kv.second.best.empty() && kv.second.best != "ok") {
                    key = kv.second.best;
                    break;
                }
            }
            if (!key.empty()) {
                for (auto it = r.recent.begin(); it != r.recent.end();)
                    it = now - it->second > r.dedupe_s * 1000 ? r.recent.erase(it) : std::next(it);
                if (r.recent.count(key)) {
                    r.recent[key] = now;  // still around: extend the window
                    continue;
                }
                r.recent[key] = now;
            }
        }
        fire(r, t, ts, label, f, {{"dwell_ms", t.last_ms - t.first_ms}});
    }
}

// Runs the detector over the configured area (a polygon's bounding box, or the whole picture), optionally as overlapping
// tiles so small objects get more of the model's resolution; results outside the polygon are dropped.
static std::vector<Det> detect_yolo(Pipeline &p, const Frame &f) {
    RectF area{0, 0, (float)f.w, (float)f.h};
    if (p.roi.size() >= 3) {
        float x1 = 1, y1 = 1, x2 = 0, y2 = 0;
        for (auto &q : p.roi) x1 = std::min(x1, q.first), y1 = std::min(y1, q.second), x2 = std::max(x2, q.first), y2 = std::max(y2, q.second);
        area = RectF{x1 * f.w, y1 * f.h, x2 * f.w, y2 * f.h}.clip((float)f.w, (float)f.h);
        if (area.empty()) area = RectF{0, 0, (float)f.w, (float)f.h};
    }
    std::vector<Det> all;
    if (p.tiles_x * p.tiles_y <= 1) {
        all = p.yolo->detect(f, area);
    } else {
        float tw = area.w() / p.tiles_x, th = area.h() / p.tiles_y;
        float ox = tw * 0.15f, oy = th * 0.15f;
        for (int ty = 0; ty < p.tiles_y; ++ty)
            for (int tx = 0; tx < p.tiles_x; ++tx) {
                RectF r{area.x1 + tx * tw - ox, area.y1 + ty * th - oy, area.x1 + (tx + 1) * tw + ox, area.y1 + (ty + 1) * th + oy};
                auto d = p.yolo->detect(f, r.clip((float)f.w, (float)f.h));
                all.insert(all.end(), d.begin(), d.end());
            }
        nms(all, p.yolo->nms_thr, !p.yolo->class_agnostic);
    }
    if (p.roi.size() >= 3) {
        std::vector<Det> in;
        for (auto &d : all)
            if (point_in_poly(d.box.cx() / f.w, d.box.y2 / f.h, p.roi)) in.push_back(d);  // feet / wheels inside the area
        all.swap(in);
    }
    return all;
}

static void process(Pipeline &p, const Frame &f) {
    int64_t now = now_ms();
    std::vector<Det> dets;
    if (p.face) {
        dets = p.face->detect(f);
    } else {
        for (auto &d : detect_yolo(p, f))
            if (in_list(p.classes, det_label(p, d.cls))) dets.push_back(d);
    }
    // answers from raw stages (off-box decoding) join the objects' attributes
    for (auto &r : rawstage::take_results()) {
        auto it = p.st.find(r.first);
        if (it == p.st.end()) continue;
        for (auto kv = r.second.begin(); kv != r.second.end(); ++kv)
            it->second.attrs[kv.key()].add(kv->is_string() ? kv->get<std::string>() : kv->dump(), 1.f, 1.f);
    }
    for (auto &lost : p.tracker.update(dets, now)) {
        auto it = p.st.find(lost.id);
        if (it == p.st.end()) continue;
        if (lost.hits >= 3) complete_track(p, lost, it->second, f);
        p.st.erase(it);
    }

    // stages: bounded work per frame so the AI rate stays steady
    int budget = 3;
    for (auto &t : p.tracker.tracks()) {
        if (!t.confirmed() || !t.visible()) continue;
        TrackState &ts = p.st[t.id];
        std::string label = det_label(p, t.cls);
        bool all_resolved = true;
        for (auto &s : p.stages) {
            if (!in_list(s->on, label)) continue;
            const std::string &leaf = s->then ? s->then->name : s->name;
            bool resolved = ts.attrs.count(leaf) && ts.attrs[leaf].resolved;
            all_resolved &= resolved;
            if (resolved || budget <= 0 || now - ts.last_run[s->name] < s->interval_ms) continue;
            if (s->min_px && t.box.h() < s->min_px && s->type != "detect") continue;
            --budget;
            run_stage(*s, f, t.box, ts, now, t.score, &t.det, t.id);
        }
        if (all_resolved && !p.stages.empty()) complete_track(p, t, ts, f);
    }
    eval_rules(p, f, now);
    g_tracks = (int)p.tracker.tracks().size();

    // the exposure controller meters on what is being tracked and limits the shutter by how fast it moves
    {
        std::vector<std::array<float, 4>> boxes;
        float fastest = 0;
        for (auto &t : p.tracker.tracks()) {
            if (!t.confirmed() || !t.visible()) continue;
            TrackState &ts = p.st[t.id];
            float cx = t.box.cx(), cy = t.box.cy();
            if (ts.prev_ms && now > ts.prev_ms) {
                float v = std::hypot(cx - ts.prev_cx, cy - ts.prev_cy) * 1000.f / (float)(now - ts.prev_ms);
                ts.speed = ts.speed * 0.5f + v * 0.5f;
            }
            ts.prev_cx = cx, ts.prev_cy = cy, ts.prev_ms = now;
            fastest = std::max(fastest, ts.speed);
            if (boxes.size() < 16) boxes.push_back({t.box.x1 / f.w, t.box.y1 / f.h, t.box.x2 / f.w, t.box.y2 / f.h});
        }
        isp::set_subject(boxes, fastest);
    }

    // overlay + live metadata
    std::vector<OvItem> ov;
    json objs = json::array();
    for (auto &t : p.tracker.tracks()) {
        if (!t.confirmed() || !t.visible()) continue;
        TrackState &ts = p.st[t.id];
        std::string label = det_label(p, t.cls);
        json attrs = track_attrs(ts);
        std::string text = label + " #" + std::to_string(t.id);
        for (auto it = attrs.begin(); it != attrs.end(); ++it) text += " " + it->get<std::string>();
        RectF nb{t.box.x1 / f.w, t.box.y1 / f.h, t.box.x2 / f.w, t.box.y2 / f.h};
        ov.push_back({nb, C_GREEN, text});
        json o = {{"id", t.id}, {"label", label}, {"score", std::round(t.score * 100.0) / 100.0},
                  {"box", {nb.x1, nb.y1, nb.x2, nb.y2}}, {"attrs", attrs}};
        if (now - ts.part_ms < 600) {
            RectF pb{ts.part.x1 / f.w, ts.part.y1 / f.h, ts.part.x2 / f.w, ts.part.y2 / f.h};
            ov.push_back({pb, C_YELLOW, ""});
            o["part"] = {pb.x1, pb.y1, pb.x2, pb.y2};
        }
        objs.push_back(o);
    }
    {
        std::lock_guard<std::mutex> lk(g_ovmu);
        g_overlay.swap(ov);
        g_overlay_ms = now;
    }
    live::publish("detections", {{"ts", f.ts_ms}, {"pts_us", f.pts_us}, {"w", f.w}, {"h", f.h}, {"objects", objs}});
}

// no frames for a while (motion gate): age every track out so each gets its "lost" handling
static void flush_idle(Pipeline &p, const Frame &f) {
    int64_t now = now_ms();
    for (int i = 0; i < p.tracker.max_misses + 3 && !p.tracker.tracks().empty(); ++i)
        for (auto &lost : p.tracker.update({}, now)) {
            auto it = p.st.find(lost.id);
            if (it == p.st.end()) continue;
            if (lost.hits >= 3) complete_track(p, lost, it->second, f);
            p.st.erase(it);
        }
    g_tracks = 0;
    std::lock_guard<std::mutex> lk(g_ovmu);
    g_overlay.clear();
}

// ---------------------------------------------------------------- threads

static void loop() {
    prctl(PR_SET_NAME, "ai", 0, 0, 0);
    int n = 0;
    double busy = 0;
    int64_t t0 = now_ms();
    while (g_run) {
        Frame f;
        {
            std::unique_lock<std::mutex> lk(g_fmu);
            g_busy = false;
            g_fcv.wait_for(lk, std::chrono::milliseconds(500), [] { return g_ready || !g_run; });
            if (!g_run) break;
            if (!g_ready) {
                // motion-gated and quiet for a while: close the tracks that were open (their records are written) so
                // objects don't linger half-tracked until the next movement
                if (g_gate && g_last_proc && now_ms() - g_last_proc > 3000 && g_frame.nv12) {
                    g_busy = true;
                    Frame last = g_frame;
                    lk.unlock();
                    {
                        std::lock_guard<std::mutex> lk2(g_mu);
                        if (g_pipe) flush_idle(*g_pipe, last);
                    }
                    g_last_proc = 0;
                }
                continue;
            }
            g_ready = false;
            f = g_frame;
        }
        int64_t ts = now_ms();
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_pipe) process(*g_pipe, f);
        }
        g_last_proc = now_ms();
        busy += now_ms() - ts;
        ++g_frames;
        if (++n == 10) {
            int64_t t = now_ms();
            g_fps = 10000.f / std::max<int64_t>(1, t - t0);
            g_ms = (float)(busy / n);
            t0 = t;
            n = 0;
            busy = 0;
        }
    }
}

static bool frame_hook(video::FrameView &v) {
    // hand a copy to the AI thread when it is idle and the rate limit allows
    if (g_run) {
        int64_t t = now_ms();
        std::lock_guard<std::mutex> lk(g_fmu);
        bool gated = g_gate && !motion::active_within(g_gate_hold_ms);  // nothing moves: leave the NPU idle
        if (!gated && !g_busy && t - g_last_take >= 1000 / std::max(1, g_max_fps.load())) {
            g_last_take = t;
            size_t sz = (size_t)v.stride * v.h * 3 / 2;
            if (g_fdma.fd() >= 0 && sz <= g_fdma.size()) {
                // the RGA copies capture buffer -> AI buffer (both media memory); the CPU only if that fails
                if (rga_copy_nv12(v.fd, g_fdma.fd(), v.stride, v.h)) {
                    g_fdma.cpu_read_sync();
                    static bool checked = false;
                    if (!checked) {  // first copy: compare sample rows with the CPU's view of the capture buffer
                        checked = true;
                        bool same = true;
                        for (int r = 0; r < v.h * 3 / 2 && same; r += 37)
                            same = !memcmp(g_fdma.data() + (size_t)r * v.stride, v.nv12 + (size_t)r * v.stride, v.w);
                        if (!same) rga_copy_disable("the copy differs from the picture");
                        else LOGI("ai", "AI frames are copied by the RGA (checked against the picture)");
                    }
                } else {
                    g_fdma.begin_cpu_write();
                    memcpy(g_fdma.data(), v.nv12, sz);
                    g_fdma.end_cpu_write();
                }
                g_frame.nv12 = g_fdma.data();
                g_frame.fd = g_fdma.fd();
            } else {
                g_fbuf.resize(sz);
                memcpy(g_fbuf.data(), v.nv12, sz);
                g_frame.nv12 = g_fbuf.data();
                g_frame.fd = -1;
            }
            g_frame.w = v.w;
            g_frame.h = v.h;
            g_frame.stride = v.stride;
            g_frame.ts_ms = wall_ms();
            g_frame.pts_us = v.pts_us;
            g_busy = g_ready = true;
            g_fcv.notify_one();
        }
    }
    // optional: burn the latest boxes into the sub stream (for NVRs/players without the web overlay)
    if (!g_burn) return false;
    std::vector<OvItem> ov;
    {
        std::lock_guard<std::mutex> lk(g_ovmu);
        if (now_ms() - g_overlay_ms > 1200) return false;
        ov = g_overlay;
    }
    int thick = v.h >= 720 ? 3 : 2;
    for (auto &o : ov) {
        RectF b{o.box.x1 * v.w, o.box.y1 * v.h, o.box.x2 * v.w, o.box.y2 * v.h};
        draw_box(v.nv12, v.w, v.h, v.stride, b, o.color, thick);
        if (!o.label.empty()) draw_label(v.nv12, v.w, v.h, v.stride, (int)b.x1, (int)b.y1, o.label, o.color);
    }
    return !ov.empty();
}

static void free_rga_frames() {
    std::lock_guard<std::mutex> fl(g_fmu);
    g_fdma.release();
}

// keep_frames: start_with_video() has just taken the RGA frame buffer for the next pipeline
static void stop_engine(bool keep_frames = false) {
    g_run = false;
    g_fcv.notify_all();
    if (g_th.joinable()) g_th.join();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pipe) {
        g_counters_dirty = true;
        counters_save(g_pipe->rules, true);
    }
    g_pipe.reset();  // frees the NPU memory
    {
        // media memory goes back before the video restarts (its encoders need large contiguous blocks)
        std::lock_guard<std::mutex> fl(g_fmu);
        g_frame = Frame();
        if (!keep_frames) g_fdma.release();
        npu_rga_frames(false);
    }
    std::lock_guard<std::mutex> lk2(g_ovmu);
    g_overlay.clear();
}

// Size of one sub-stream NV12 frame, the picture the AI works on
static size_t ai_frame_bytes() {
    json sub = g_settings.get("video").value("sub", json::object());
    return (size_t)(jget<int>(sub, "width", 960) & ~15) * jget<int>(sub, "height", 540) * 3 / 2;
}

// AI frames in media memory let the RGA prepare the model inputs; without room for them the CPU does it.
// start_with_video() takes the buffer before the encoders and the NPU (media memory fills up and fragments after);
// this only covers a start that skipped that step.
static void alloc_rga_frames(const json &cfg) {
    if (!jget<bool>(cfg, "enabled", false) || jget<std::string>(cfg, "preprocess", "auto") == "cpu") return;
    std::lock_guard<std::mutex> fl(g_fmu);
    if (g_fdma.fd() >= 0 || g_fdma.alloc(ai_frame_bytes())) {
        npu_rga_frames(true);
        return;
    }
    npu_rga_frames(false, "no media memory left for the RGA's frame buffer: the CPU prepares the model inputs");
    LOGW(TAG, "no media memory for an RGA frame buffer (%zu KB): CPU preprocessing", ai_frame_bytes() / 1024);
}

static void configure(const json &cfg, const json &rules) {
    stop_engine(true);
    g_error.clear();
    g_burn = jget<bool>(g_settings.get("video").value("sub", json::object()), "ai_overlay", false);
    g_max_fps = std::max(1, std::min(30, jget<int>(cfg, "max_fps", 10)));
    g_cur_profile = jget<std::string>(cfg, "_profile", "");
    npu_set_preprocess(jget<std::string>(cfg, "preprocess", "auto"));
    apply_gate(cfg);
    if (!jget<bool>(cfg, "enabled", false)) {
        LOGI(TAG, "AI disabled");
        free_rga_frames();
        return;
    }
    long cma0 = meminfo_kb("CmaAllocated");
    std::unique_ptr<Pipeline> p;
    std::string err = build(cfg, p);
    if (!err.empty()) {
        g_error = err;
        LOGE(TAG, "%s", err.c_str());
        free_rga_frames();
        return;
    }
    counters_load();
    p->rules = parse_rules(rules);
    counters_restore(p->rules);
    LOGI(TAG, "pipeline ready: %s + %zu stage(s), %zu rule(s), NPU memory %ld KB", p->det_model.c_str(), p->stages.size(),
         p->rules.size(), meminfo_kb("CmaAllocated") - cma0);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pipe = std::move(p);
    }
    alloc_rga_frames(cfg);
    g_run = true;
    g_th = std::thread(loop);
}

// The NPU allocations a pipeline will make: weights of every model, its I/O tensors, and one shared working buffer
static std::vector<size_t> memory_blocks(const json &cfg) {
    std::vector<size_t> blocks;
    if (!jget<bool>(cfg, "enabled", false)) return blocks;
    std::vector<std::string> ids{jget<std::string>(cfg.value("detector", json::object()), "model", "")};
    std::function<void(const json &)> add = [&](const json &st) {
        if (!jget<bool>(st, "enabled", true)) return;
        ids.push_back(jget<std::string>(st, "model", ""));
        if (st.contains("then") && st["then"].is_object()) add(st["then"]);
    };
    for (auto &st : cfg.value("stages", json::array())) add(st);
    size_t internal = 0;
    for (auto &id : ids) {
        std::string p = models::path(id);
        if (p.empty()) continue;
        json io = models::meta(id).value("io", json());
        if (!io.is_object()) io = models::inspect(p);
        blocks.push_back(jget<size_t>(io, "weights_kb", file_size(p) / 1024) * 1024 + 4096);
        blocks.push_back(512 * 1024);  // input + output tensors
        internal = std::max(internal, jget<size_t>(io, "internal_kb", 1600) * 1024);
    }
    blocks.push_back(internal + 4096);
    return blocks;
}

bool start_with_video() {
    json a = effective(g_settings.get("ai"));
    json v = g_settings.get("video"), osd = g_settings.get("osd"), masks = g_settings.get("masks");
    bool ok = false;
    if (jget<bool>(a, "enabled", false)) {
        // AI first: its models and the RGA frame buffer pack into the empty media memory, and the encoders then take
        // the large free block after them. (Reserving holes for the AI around the video leaves the order to chance:
        // media memory is first-fit and the NPU's own allocations land in whichever hole fits first.)
        if (jget<std::string>(a, "preprocess", "auto") != "cpu") {
            std::lock_guard<std::mutex> fl(g_fmu);
            if (g_fdma.fd() < 0) g_fdma.alloc(ai_frame_bytes());
        }
        configure(a, g_settings.get("rules"));
        ok = video::start(v, osd, masks);
        if (!ok && g_run) {
            // the video comes first: give it the memory and load the AI around it (CPU preprocessing)
            LOGW(TAG, "video did not start with the AI loaded first: starting the video first");
            stop_engine();
        }
    }
    if (!ok) {
        auto blocks = memory_blocks(a);
        if (!blocks.empty()) video::cma_reserve(blocks);
        ok = video::start(v, osd, masks);
        video::cma_release();
        if (!g_run) configure(a, g_settings.get("rules"));
    }
    if (jget<bool>(a, "enabled", false) && !g_run && g_error.find("memory") == std::string::npos && g_error.find("cannot load") != std::string::npos)
        g_error += " — not enough media memory with the current video settings. Lower the main-stream resolution, use smaller "
                   "models, or increase media memory in System > Performance.";
    return ok;
}

void restart_video(const json &before) {
    std::lock_guard<std::mutex> rl(g_reconf_mu);
    stop_engine();
    video::stop();
    if (start_with_video()) return;
    LOGE(TAG, "new video settings do not fit in media memory: back to the previous ones");
    g_settings.revert("video", before);
    video::stop();
    stop_engine();
    start_with_video();
    g_error = "The last video settings needed more media memory than available and were rolled back.";
}

void recover_video(const std::string &why) {
    // never from the caller's thread: the caller may be the ISP controller, which a video restart stops and joins
    static std::atomic<bool> busy{false};
    if (!g_sched_run || busy.exchange(true)) return;
    std::thread([why] {
        LOGW(TAG, "restarting the video pipeline: %s", why.c_str());
        audit::record("-", "", "video_recovered", why);
        {
            std::lock_guard<std::mutex> rl(g_reconf_mu);
            if (g_sched_run) {
                stop_engine();
                video::stop();
                if (!start_with_video()) LOGE(TAG, "video pipeline did not come back (the watchdog resets the board)");
            }
        }
        busy = false;
    }).detach();
}

// ---------------------------------------------------------------- API

static std::string validate_ai(json &a) {
    int fps = jget<int>(a, "max_fps", 10);
    if (fps < 1 || fps > 30) return "max_fps must be 1..30";
    std::string rw = jget<std::string>(a, "run_when", "always");
    if (rw != "always" && rw != "motion") return "run_when must be always or motion";
    std::string pre = jget<std::string>(a, "preprocess", "auto");
    if (pre != "auto" && pre != "cpu") return "preprocess must be auto or cpu";
    if (rw == "motion" && !jget<bool>(g_settings.get("motion"), "enabled", false)) return "turn on Motion detection first: the AI runs only while motion is seen";
    if (a.contains("profiles")) {
        if (!a["profiles"].is_array() || a["profiles"].size() > 4) return "at most 4 AI profiles";
        std::vector<std::string> ids;
        for (auto &pr : a["profiles"]) {
            std::string id = jget<std::string>(pr, "id", "");
            if (id.empty() || id == "auto" || std::find(ids.begin(), ids.end(), id) != ids.end()) return "each AI profile needs its own id";
            ids.push_back(id);
            if (pr.contains("schedule") && (!pr["schedule"].is_array() || (!pr["schedule"].empty() && pr["schedule"].size() != 168)))
                return "a profile schedule has 7x24 entries";
        }
    }
    json e = effective(a);
    if (jget<bool>(e, "enabled", false)) {
        std::string m = jget<std::string>(e.value("detector", json::object()), "model", "");
        if (m.empty()) return "choose a detector model before enabling AI";
        if (models::path(m).empty()) return "detector model not found: " + m;
    }
    return "";
}

static std::string validate_rules(json &r) {
    if (!r.is_array()) return "rules must be a list";
    if (r.size() > 32) return "at most 32 rules";
    static const char *types[] = {"intrusion", "loitering", "line_cross", "count", "presence", "track", "motion"};
    for (auto &x : r) {
        std::string t = jget<std::string>(x, "type", "");
        if (std::find_if(std::begin(types), std::end(types), [&](const char *s) { return t == s; }) == std::end(types))
            return "unknown rule type '" + t + "'";
        if (!x.contains("id") || jget<std::string>(x, "id", "").empty()) x["id"] = random_token(4);
        if (t == "line_cross" && (!x.contains("line") || !x["line"].is_array() || x["line"].size() != 2 || !is_pt(x["line"][0]) ||
                                  !is_pt(x["line"][1])))
            return "line rule '" + jget<std::string>(x, "name", "") + "' needs two points";
        if (x.contains("zone")) {
            if (!x["zone"].is_array() || x["zone"].size() > 32) return "zone of rule '" + jget<std::string>(x, "name", "") + "' is not a point list";
            for (auto &q : x["zone"])
                if (!is_pt(q)) return "zone of rule '" + jget<std::string>(x, "name", "") + "' has an invalid point";
        }
    }
    return "";
}

void init() {
    faces::load();
    g_settings.on("ai", validate_ai, [](const json &now_raw, const json &before_raw) {
        npu_set_preprocess(jget<std::string>(now_raw, "preprocess", "auto"));
        json now = effective(now_raw), before = effective(before_raw);
        // same models and stage layout: only thresholds etc. changed, apply them live (no video restart)
        if (structure(now) == structure(before)) {
            g_max_fps = std::max(1, std::min(30, jget<int>(now, "max_fps", 10)));
            apply_gate(now);
            if (!jget<bool>(now, "enabled", false)) return;
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_pipe) {
                hot_apply(*g_pipe, now);
                g_cur_profile = jget<std::string>(now, "_profile", "");
                LOGI(TAG, "AI settings applied live");
                return;
            }
        }
        // models changed: re-run the whole start sequence so encoders and NPU both get contiguous media memory
        std::lock_guard<std::mutex> rl(g_reconf_mu);
        stop_engine();
        video::stop();
        start_with_video();
    });
    // profile schedule: every 30 s see whether another profile is due
    g_sched_run = true;
    g_sched = std::thread([] {
        while (g_sched_run) {
            for (int i = 0; i < 30 && g_sched_run; ++i) usleep(1000 * 1000);
            json a = g_settings.get("ai");
            if (!g_sched_run || !jget<bool>(a, "enabled", false) || jget<std::string>(a, "active_profile", "auto") != "auto") continue;
            std::string id = profile_for_now(a);
            if (id.empty() || id == g_cur_profile) continue;
            LOGI(TAG, "schedule: switching to AI profile '%s'", id.c_str());
            std::lock_guard<std::mutex> rl(g_reconf_mu);
            stop_engine();
            video::stop();
            start_with_video();
        }
    });
    g_settings.on("rules", validate_rules, [](const json &r, const json &) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_pipe) {
            g_counters_dirty = true;
            counters_save(g_pipe->rules, true);  // keep line counters across the edit
            g_pipe->rules = parse_rules(r);
            counters_restore(g_pipe->rules);
            for (auto &kv : g_pipe->st) kv.second.fired.clear();
        }
    });
    g_settings.on("video", nullptr, [](const json &v, const json &) {
        g_burn = jget<bool>(v.value("sub", json::object()), "ai_overlay", false);
    });
    g_hook = video::add_frame_hook(frame_hook);
}

void shutdown() {
    g_sched_run = false;
    if (g_sched.joinable()) g_sched.join();
    rawstage::stop();
    if (g_hook) video::remove_frame_hook(g_hook);
    stop_engine();
}

json status() {
    json counters = json::array();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_pipe)
            for (auto &r : g_pipe->rules)
                if (r.type == "line_cross") counters.push_back({{"rule", r.id}, {"name", r.name}, {"a_to_b", r.count_in}, {"b_to_a", r.count_out}});
    }
    json profiles = json::array();
    for (auto &pr : g_settings.get("ai").value("profiles", json::array()))
        profiles.push_back({{"id", jget<std::string>(pr, "id", "")}, {"name", jget<std::string>(pr, "name", "")}});
    return {{"enabled", jget<bool>(g_settings.get("ai"), "enabled", false)},
            {"running", g_run.load()},
            {"profile", g_cur_profile},
            {"profiles", profiles},
            {"gated", g_gate.load()},
            {"error", g_error},
            {"fps", std::round(g_fps * 10.0) / 10.0},
            {"infer_ms", std::round(g_ms.load())},
            {"tracks", g_tracks.load()},
            {"frames", g_frames},
            {"npu", npu_stats()},
            {"counters", counters}};
}

// the next sub-stream frame, copied (model test and benchmark)
static bool grab_frame(Frame &f) {
    std::vector<uint8_t> buf;
    std::mutex m;
    std::condition_variable cv;
    bool got = false;
    int hid = video::add_frame_hook([&](video::FrameView &v) {
        std::lock_guard<std::mutex> lk(m);
        if (got) return false;
        buf.assign(v.nv12, v.nv12 + (size_t)v.stride * v.h * 3 / 2);
        f.w = v.w, f.h = v.h, f.stride = v.stride;
        got = true;
        cv.notify_one();
        return false;
    });
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(2), [&] { return got; });
    lk.unlock();
    video::remove_frame_hook(hid);
    if (!got) return false;
    f.buf = std::make_shared<std::vector<uint8_t>>(std::move(buf));
    f.nv12 = f.buf->data();
    f.ts_ms = wall_ms();
    return true;
}

// How fast is this model on this NPU (input conversion + inference)? Loads it temporarily.
static json bench_model(const std::string &id) {
    std::string p = models::path(id);
    if (p.empty()) return {{"error", "unknown model"}};
    Frame f;
    if (!grab_frame(f)) return {{"error", "no video frame"}};
    RawModel m;
    m.bgr = jget<std::string>(models::meta(id), "color", "rgb") == "bgr";
    if (!m.model.load(p, false)) return {{"error", "model failed to load (out of NPU memory? stop AI first)"}};
    double avg = 0, best = 0;
    m.bench(f, 20, &avg, &best);
    return {{"model", id}, {"runs", 20}, {"avg_ms", std::round(avg * 10) / 10}, {"best_ms", std::round(best * 10) / 10},
            {"max_fps", avg > 0 ? std::round(10000.0 / avg) / 10 : 0}, {"input", std::to_string(m.model.in_w()) + "x" + std::to_string(m.model.in_h())}};
}

// One-off inference on the current picture (model test in the UI). Loads the model temporarily.
static json test_model(const std::string &id, float conf) {
    std::string p = models::path(id);
    if (p.empty()) return {{"error", "unknown model"}};
    json meta = models::meta(id);
    std::string task = jget<std::string>(meta, "task", "detect");
    Frame f;
    if (!grab_frame(f)) return {{"error", "no video frame"}};
    bool bgr = jget<std::string>(meta, "color", "rgb") == "bgr";
    int64_t t0 = now_ms();
    json out = {{"task", task}, {"width", f.w}, {"height", f.h}};
    json res = json::array();
    if (task == "detect") {
        Yolov5 y;
        y.conf = conf;
        y.bgr = bgr;
        if (!y.model.load(p, false)) return {{"error", "model failed to load (out of NPU memory? stop AI first)"}};
        for (auto &d : y.detect(f, RectF{}))
            res.push_back({{"label", models::label(meta, d.cls)}, {"score", d.score},
                           {"box", {d.box.x1 / f.w, d.box.y1 / f.h, d.box.x2 / f.w, d.box.y2 / f.h}}});
    } else if (task == "face_detect") {
        RetinaFace r;
        r.conf = conf;
        r.tiles_x = r.tiles_y = 1;
        if (!r.model.load(p, false)) return {{"error", "model failed to load"}};
        for (auto &d : r.detect(f))
            res.push_back({{"label", "face"}, {"score", d.score}, {"box", {d.box.x1 / f.w, d.box.y1 / f.h, d.box.x2 / f.w, d.box.y2 / f.h}}});
    } else if (task == "classify") {
        Classifier c;
        c.bgr = bgr;
        if (!c.model.load(p, false)) return {{"error", "model failed to load"}};
        float prob = 0;
        int k = c.classify(f, RectF{}, &prob);
        res.push_back({{"label", models::label(meta, k)}, {"score", prob}});
    } else if (task == "ocr") {
        PlateOcr o;
        o.charset = jget<std::string>(meta, "charset", o.charset);
        o.bgr = bgr;
        if (!o.model.load(p, false)) return {{"error", "model failed to load"}};
        float c = 0;
        std::string t = o.read(f, RectF{0, 0, (float)f.w, (float)f.h}, &c);
        res.push_back({{"label", t}, {"score", c}});
    } else {
        return {{"error", "test not supported for task " + task}};
    }
    out["results"] = res;
    out["ms"] = now_ms() - t0;
    return out;
}

void register_routes() {
    g_http.route("GET", "/api/ai/status", auth::VIEWER, [](HttpRequest &, HttpResponse &res) { res.send_json(status()); });
    g_http.route("GET", "/api/ai/models", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) { res.send_json(models::list()); });
    // upload: raw body = the .rknn file, ?filename=..&name=..&task=..&labels=a,b,c&color=rgb
    g_http.route("POST", "/api/ai/models", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        std::string tmp = req.body_file;
        if (tmp.empty()) {
            if (req.body.size() < 64) return res.error(400, "no model file uploaded");
            tmp = g_paths.data + "/tmp/model-" + random_token(4);
            mkdirs(g_paths.data + "/tmp");
            write_file(tmp, req.body);
        }
        // a .tar.gz is a model package (model + manifest + labels [+ decoder plugin])
        {
            std::string head;
            if (FILE *fp = fopen(tmp.c_str(), "rb")) {
                char b[2] = {0, 0};
                if (fread(b, 1, 2, fp) == 2) head.assign(b, 2);
                fclose(fp);
            }
            if (head == "\x1f\x8b") {
                std::string id, err = models::install_package(tmp, req.role >= auth::ADMIN, &id);
                unlink(tmp.c_str());
                if (!err.empty()) return res.error(400, err);
                res.send_json({{"id", id}, {"meta", models::meta(id)}});
                return;
            }
        }
        json meta = json::object();
        for (const char *k : {"name", "task", "color", "charset", "description", "decoder", "layout", "blank"})
            if (!req.param(k).empty()) meta[k] = req.param(k);
        for (const char *k : {"stretch", "class_agnostic", "center_crop"})
            if (!req.param(k).empty()) meta[k] = req.param(k) == "true" || req.param(k) == "1";
        if (!req.param("plugin").empty()) {
            // a plugin runs inside the daemon: naming one needs the administrator
            if (req.role < auth::ADMIN) return res.error(403, "decoder plugins need an administrator");
            meta["plugin"] = req.param("plugin");
        }
        if (!req.param("anchors").empty()) {
            json a = json::array();
            for (auto &s : split(req.param("anchors"), ',')) a.push_back(atof(trim(s).c_str()));
            if (a.size() != 18) return res.error(400, "anchors: 18 numbers expected (3 heads x 3 anchors x w,h)");
            meta["anchors"] = a;
        }
        if (!req.param("labels").empty()) {
            json l = json::array();
            for (auto &s : split(req.param("labels"), ',')) l.push_back(trim(s));
            meta["labels"] = l;
        }
        std::string id;
        std::string err = models::install(req.param("filename", "model.rknn"), tmp, meta, &id);
        unlink(tmp.c_str());
        if (!err.empty()) return res.error(400, err);
        res.send_json({{"id", id}, {"meta", models::meta(id)}});
    }, true);
    g_http.route("PUT", "/api/ai/models/", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "model id required");
        std::string err = models::update_meta(req.path_args[0], req.json_body());
        if (!err.empty()) return res.error(400, err);
        res.send_json(models::meta(req.path_args[0]));
    });
    g_http.route("DELETE", "/api/ai/models/", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "model id required");
        std::string err = models::remove(req.path_args[0]);
        if (!err.empty()) return res.error(400, err);
        res.ok();
    });
    g_http.route("POST", "/api/ai/test", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        json r = test_model(jget<std::string>(b, "model", ""), jget<float>(b, "conf", 0.35f));
        if (r.contains("error")) return res.error(400, r["error"].get<std::string>());
        res.send_json(r);
    }, true);
    // ---- decoder plugins (shared libraries: administrators only)
    g_http.route("GET", "/api/ai/plugins", auth::ADMIN, [](HttpRequest &, HttpResponse &res) { res.send_json(plugin_list()); });
    g_http.route("POST", "/api/ai/plugins", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        std::string tmp = req.body_file;
        if (tmp.empty()) {
            if (req.body.size() < 64) return res.error(400, "no file uploaded");
            tmp = g_paths.data + "/tmp/plugin-" + random_token(4);
            mkdirs(g_paths.data + "/tmp");
            write_file(tmp, req.body);
        }
        std::string err = plugin_install(req.param("filename", "decoder.so"), tmp);
        unlink(tmp.c_str());
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "plugin_install", req.param("filename", ""));
        res.send_json(plugin_list());
    }, true);
    g_http.route("DELETE", "/api/ai/plugins/", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "plugin file required");
        std::string err = plugin_remove(req.path_args[0]);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "plugin_remove", req.path_args[0]);
        res.send_json(plugin_list());
    });

    // ---- enrolled faces (on-device recognition)
    g_http.route("GET", "/api/faces", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) {
        res.send_json({{"people", faces::list()}, {"enroll", faces::enroll_status()}});
    });
    g_http.route("POST", "/api/faces/enroll", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string err = faces::enroll_start(jget<std::string>(b, "name", ""), jget<int>(b, "shots", 3));
        if (!err.empty()) return res.error(400, err);
        res.send_json(faces::enroll_status());
    });
    g_http.route("POST", "/api/faces/cancel", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) {
        faces::enroll_cancel();
        res.send_json(faces::enroll_status());
    });
    g_http.route("PUT", "/api/faces/", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "id required");
        std::string err = faces::rename(req.path_args[0], jget<std::string>(req.json_body(), "name", ""));
        if (!err.empty()) return res.error(400, err);
        res.send_json(faces::list());
    });
    g_http.route("DELETE", "/api/faces/", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 1) return res.error(400, "id required");
        std::string err = faces::remove(req.path_args[0]);
        if (!err.empty()) return res.error(400, err);
        audit::record(req.user, req.peer, "face_delete", req.path_args[0]);
        res.send_json(faces::list());
    });

    g_http.route("POST", "/api/ai/benchmark", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        json r = bench_model(jget<std::string>(req.json_body(), "model", ""));
        if (r.contains("error")) return res.error(400, r["error"].get<std::string>());
        res.send_json(r);
    }, true);
    g_http.route("POST", "/api/ai/counters/reset", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_pipe)
            for (auto &r : g_pipe->rules) r.count_in = r.count_out = 0;
        g_counters = json::object();
        g_counters_dirty = true;
        if (g_pipe) counters_save(g_pipe->rules, true);
        res.ok();
    });
}

}  // namespace ai
