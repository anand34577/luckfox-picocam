#include "faces.h"

#include <cmath>
#include <mutex>

#include "core/log.h"
#include "core/util.h"

namespace faces {

static const char *TAG = "faces";

struct Person {
    std::string id, name;
    int64_t created = 0;
    std::vector<std::vector<float>> emb;
};

static std::mutex g_mu;
static std::vector<Person> g_people;

// enrolment state
static bool g_armed = false;
static std::string g_name;
static int g_shots = 3, g_got = 0, g_track = -1;
static int64_t g_last = 0, g_armed_at = 0;
static std::vector<std::vector<float>> g_pending;

static std::string path() { return g_paths.data + "/faces.json"; }

static void save_locked() {
    json arr = json::array();
    for (auto &p : g_people) {
        json e = json::array();
        for (auto &v : p.emb) {
            json row = json::array();
            for (float x : v) row.push_back(std::round(x * 10000.f) / 10000.f);
            e.push_back(row);
        }
        arr.push_back({{"id", p.id}, {"name", p.name}, {"created", p.created}, {"emb", e}});
    }
    write_file(path(), json{{"people", arr}}.dump());
}

void load() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_people.clear();
    std::string s;
    if (!read_file(path(), s)) return;
    json j = json::parse(s, nullptr, false);
    if (!j.is_object()) return;
    for (auto &p : j.value("people", json::array())) {
        Person x;
        x.id = p.value("id", "");
        x.name = p.value("name", "");
        x.created = p.value("created", (int64_t)0);
        for (auto &row : p.value("emb", json::array())) {
            std::vector<float> v;
            for (auto &n : row)
                if (n.is_number()) v.push_back(n.get<float>());
            if (!v.empty()) x.emb.push_back(v);
        }
        if (!x.id.empty() && !x.emb.empty()) g_people.push_back(x);
    }
    LOGI(TAG, "%zu enrolled people", g_people.size());
}

json list() {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    for (auto &p : g_people) out.push_back({{"id", p.id}, {"name", p.name}, {"shots", p.emb.size()}, {"created", p.created}});
    return out;
}

std::string remove(const std::string &id) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_people.begin(); it != g_people.end(); ++it)
        if (it->id == id) {
            g_people.erase(it);
            save_locked();
            return "";
        }
    return "no such person";
}

std::string rename(const std::string &id, const std::string &name) {
    if (name.empty() || name.size() > 48) return "name must be 1..48 characters";
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &p : g_people)
        if (p.id == id) {
            p.name = name;
            save_locked();
            return "";
        }
    return "no such person";
}

std::string enroll_start(const std::string &name, int shots) {
    if (name.empty() || name.size() > 48) return "name must be 1..48 characters";
    std::lock_guard<std::mutex> lk(g_mu);
    g_armed = true;
    g_name = name;
    g_shots = std::max(1, std::min(8, shots));
    g_got = 0;
    g_track = -1;
    g_last = 0;
    g_armed_at = now_ms();
    g_pending.clear();
    return "";
}

void enroll_cancel() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_armed = false;
    g_pending.clear();
}

json enroll_status() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_armed && now_ms() - g_armed_at > 120000) g_armed = false;  // nobody showed up
    return {{"active", g_armed}, {"name", g_name}, {"got", g_got}, {"shots", g_shots}};
}

float cosine(const std::vector<float> &a, const std::vector<float> &b) {
    if (a.size() != b.size() || a.empty()) return 0;
    double d = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) d += (double)a[i] * b[i], na += (double)a[i] * a[i], nb += (double)b[i] * b[i];
    return na > 0 && nb > 0 ? (float)(d / std::sqrt(na * nb)) : 0.f;
}

bool offer(const std::vector<float> &e, int track_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_armed) return false;
    if (now_ms() - g_armed_at > 120000) return g_armed = false;
    // the first face seen is locked in; the shots must all come from that one person (same track), spaced out so they
    // differ a little in pose and light
    if (g_track < 0) g_track = track_id;
    if (track_id != g_track) return false;
    if (g_got > 0 && now_ms() - g_last < 700) return true;
    g_pending.push_back(e);
    g_last = now_ms();
    ++g_got;
    if (g_got >= g_shots) {
        Person p;
        p.id = random_token(4);
        p.name = g_name;
        p.created = wall_ms();
        p.emb = g_pending;
        g_people.push_back(p);
        save_locked();
        LOGI(TAG, "enrolled %s (%d shots)", g_name.c_str(), g_got);
        g_armed = false;
        g_pending.clear();
    }
    return true;
}

bool match(const std::vector<float> &e, float threshold, Match *m) {
    std::lock_guard<std::mutex> lk(g_mu);
    float best = -1;
    const Person *who = nullptr;
    for (auto &p : g_people)
        for (auto &v : p.emb) {
            float s = cosine(e, v);
            if (s > best) best = s, who = &p;
        }
    if (!who || best < threshold) return false;
    if (m) m->name = who->name, m->score = best;
    return true;
}

bool empty() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_people.empty();
}

}  // namespace faces
