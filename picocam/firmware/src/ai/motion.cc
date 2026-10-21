#include "motion.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "app/events.h"
#include "app/live.h"
#include "core/util.h"
#include "media/video.h"

namespace motion {

static const int GW = 80, GH = 45;
static std::mutex g_mu;
static bool g_enabled = false;
static float g_thr = 18, g_min_frac = 0.01f, g_cooldown_ms = 10000;
static std::vector<uint8_t> g_mask(GW *GH, 1);
static std::vector<float> g_bg;
static int64_t g_last_event = 0, g_last_pub = 0;
static int g_frame = 0;
static int g_warm = 0;  // frames analysed since the background was reset
static std::atomic<float> g_level{0};
static std::atomic<int64_t> g_events{0};
static std::atomic<int64_t> g_active_ms{0};  // last time the changed area reached the threshold

static bool pt_ok(const json &q) { return q.is_array() && q.size() == 2 && q[0].is_number() && q[1].is_number(); }

static std::string validate(json &m) {
    if (!m.contains("zones")) return "";
    if (!m["zones"].is_array() || m["zones"].size() > 8) return "zones must be a list of at most 8 polygons";
    for (auto &z : m["zones"]) {
        if (!z.is_array() || z.size() > 32) return "a motion zone is not a point list";
        for (auto &q : z)
            if (!pt_ok(q)) return "a motion zone has an invalid point";
    }
    return "";
}

static bool in_poly(float x, float y, const json &p) {
    bool in = false;
    size_t n = p.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        float xi = p[i][0].get<float>(), yi = p[i][1].get<float>(), xj = p[j][0].get<float>(), yj = p[j][1].get<float>();
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) in = !in;
    }
    return in;
}

static void apply(const json &m) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_enabled = jget<bool>(m, "enabled", false);
    int sens = std::max(0, std::min(100, jget<int>(m, "sensitivity", 60)));
    g_thr = 40.f - sens * 0.32f;  // luma difference a block must exceed
    g_min_frac = std::max(0.001f, jget<float>(m, "min_area_pct", 1.0f) / 100.f);
    g_cooldown_ms = std::max(1, jget<int>(m, "cooldown_s", 10)) * 1000.f;
    json zones = m.value("zones", json::array());
    json mm = m;
    std::string bad = validate(mm);
    if (!bad.empty()) zones = json::array();  // a bad stored zone must never take the camera down
    bool any = false;
    for (int r = 0; r < GH; ++r)
        for (int c = 0; c < GW; ++c) {
            float x = (c + 0.5f) / GW, y = (r + 0.5f) / GH;
            bool in = zones.empty();
            for (auto &z : zones)
                if (z.is_array() && z.size() >= 3 && in_poly(x, y, z)) in = true;
            g_mask[r * GW + c] = in;
            any |= in;
        }
    if (!any) std::fill(g_mask.begin(), g_mask.end(), 1);
    g_bg.clear();
}

static bool hook(video::FrameView &v) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_enabled || (++g_frame & 1)) return false;
    int bw = v.w / GW, bh = v.h / GH;
    if (bw < 2 || bh < 2) return false;
    std::vector<float> cur(GW * GH);
    for (int r = 0; r < GH; ++r)
        for (int c = 0; c < GW; ++c) {
            uint32_t sum = 0, n = 0;
            for (int y = r * bh; y < (r + 1) * bh; y += 2) {
                const uint8_t *p = v.nv12 + (size_t)y * v.stride + c * bw;
                for (int x = 0; x < bw; x += 2) sum += p[x], ++n;
            }
            cur[r * GW + c] = (float)sum / n;
        }
    if (g_bg.empty()) {
        g_bg = cur;
        g_warm = 0;
        return false;
    }
    // global brightness change (exposure step, lights switched) is not motion: compare after removing it
    double dmean = 0;
    for (int i = 0; i < GW * GH; ++i) dmean += cur[i] - g_bg[i];
    dmean /= GW * GH;
    int changed = 0, total = 0;
    for (int i = 0; i < GW * GH; ++i) {
        if (!g_mask[i]) continue;
        ++total;
        if (std::fabs(cur[i] - g_bg[i] - dmean) > g_thr) ++changed;
        g_bg[i] = g_bg[i] * 0.94f + cur[i] * 0.06f;  // adapt (~1 s at 15 fps)
    }
    float frac = total ? (float)changed / total : 0;
    g_level = frac;
    if (frac >= g_min_frac) g_active_ms = now_ms();
    int64_t t = now_ms();
    if (t - g_last_pub > 500) {
        g_last_pub = t;
        live::publish("motion", {{"level", std::round(frac * 1000.0) / 10.0}, {"active", frac >= g_min_frac}});
    }
    // let the background settle (exposure converging at start-up) before raising events
    if (++g_warm < 60) return false;
    if (frac >= g_min_frac && t - g_last_event >= g_cooldown_ms) {
        g_last_event = t;
        ++g_events;
        events::Event e;
        e.type = "motion";
        e.rule_name = "Motion";
        e.score = frac;
        e.extra = {{"area_pct", std::round(frac * 1000.0) / 10.0}};
        events::emit(std::move(e));
    }
    return false;
}

void init() {
    g_settings.on("motion", validate, [](const json &m, const json &) { apply(m); });
    apply(g_settings.get("motion"));
    video::add_frame_hook(hook);
}

bool active_within(int ms) {
    int64_t t = g_active_ms;
    return t && now_ms() - t < ms;
}

json status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return {{"enabled", g_enabled}, {"level_pct", std::round(g_level * 1000.0) / 10.0}, {"events", g_events.load()}};
}

}  // namespace motion
