#include "log.h"

#include <cstdio>
#include <deque>
#include <mutex>

#include "audit.h"
#include "util.h"

static LogLevel g_level = LOG_INFO;
static std::mutex g_mu;
static std::deque<LogLine> g_ring;
static const size_t kRing = 400;

void log_set_level(LogLevel lv) { g_level = lv; }

void log_write(LogLevel lv, const char *tag, const char *fmt, ...) {
    if (lv < g_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static const char kLv[] = {'D', 'I', 'W', 'E'};
    int64_t now = wall_ms();
    std::string ts = format_time(now / 1000, "%H:%M:%S");
    fprintf(stderr, "%s.%03d [%c] %s: %s\n", ts.c_str(), (int)(now % 1000), kLv[lv], tag, buf);
    // warnings and errors also go to flash: after a crash or reboot the RAM log is gone
    if (lv >= LOG_WARN)
        audit::persist_log_line(format_time(now / 1000, "%Y-%m-%d %H:%M:%S") + " [" + kLv[lv] + "] " + tag + ": " + buf);
    std::lock_guard<std::mutex> lk(g_mu);
    g_ring.push_back({now, lv, tag, buf});
    if (g_ring.size() > kRing) g_ring.pop_front();
}

std::vector<LogLine> log_tail(size_t max) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t n = std::min(max, g_ring.size());
    return std::vector<LogLine>(g_ring.end() - n, g_ring.end());
}
