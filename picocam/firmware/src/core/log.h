#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

// Leveled logging to stderr plus an in-memory ring the web UI reads (System > Logs).
enum LogLevel { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };

void log_set_level(LogLevel lv);
void log_write(LogLevel lv, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

struct LogLine {
    int64_t ts_ms;
    LogLevel level;
    std::string tag, msg;
};
// most recent lines, oldest first (at most `max`)
std::vector<LogLine> log_tail(size_t max);

#define LOGD(tag, fmt, ...) log_write(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOGI(tag, fmt, ...) log_write(LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define LOGW(tag, fmt, ...) log_write(LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define LOGE(tag, fmt, ...) log_write(LOG_ERROR, tag, fmt, ##__VA_ARGS__)
