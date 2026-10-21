#include "audit.h"

#include <sys/stat.h>

#include <mutex>

#include "util.h"

namespace audit {

static std::mutex g_mu;
static const size_t kMaxBytes = 192 * 1024;  // per file, two generations

static std::string audit_path() { return g_paths.data + "/audit.jsonl"; }
static std::string log_path() { return g_paths.data + "/picocam-warn.log"; }

// append a line, rotating to <path>.1 when the file grows past the cap
static void append(const std::string &path, const std::string &line) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && (size_t)st.st_size > kMaxBytes) rename(path.c_str(), (path + ".1").c_str());
    FILE *fp = fopen(path.c_str(), "a");
    if (!fp) return;
    fwrite(line.data(), 1, line.size(), fp);
    fputc('\n', fp);
    fclose(fp);
}

void init() { mkdirs(g_paths.data); }

void record(const std::string &who, const std::string &peer, const std::string &action, const std::string &detail) {
    json j = {{"ts", wall_ms()}, {"user", who.empty() ? "-" : who}, {"peer", peer}, {"action", action}, {"detail", detail}};
    std::lock_guard<std::mutex> lk(g_mu);
    append(audit_path(), j.dump(-1, ' ', false, json::error_handler_t::replace));
}

json tail(size_t max) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::string> lines;
    for (const std::string &p : {audit_path() + ".1", audit_path()}) {
        std::string s;
        if (!read_file(p, s)) continue;
        for (auto &l : split(s, '\n'))
            if (!l.empty()) lines.push_back(l);
    }
    json out = json::array();
    for (auto it = lines.rbegin(); it != lines.rend() && out.size() < max; ++it) {
        json j = json::parse(*it, nullptr, false);
        if (j.is_object()) out.push_back(j);
    }
    return out;
}

void persist_log_line(const std::string &line) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_paths.data.empty()) return;
    // a warning that repeats every few seconds (broker down, port scan) must not wear out the flash
    static std::string last;
    static int64_t last_ms = 0;
    std::string body = line.substr(line.find(']') == std::string::npos ? 0 : line.find(']'));
    if (body == last && now_ms() - last_ms < 60000) return;
    last = body;
    last_ms = now_ms();
    append(log_path(), line);
}

std::string persistent_log_tail(size_t max_bytes) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string all, s;
    if (read_file(log_path() + ".1", s)) all += s;
    s.clear();
    if (read_file(log_path(), s)) all += s;
    return all.size() > max_bytes ? all.substr(all.size() - max_bytes) : all;
}

}  // namespace audit
