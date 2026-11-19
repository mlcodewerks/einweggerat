#include "log.h"
#include <cstdio>
#include <deque>
#include <mutex>

namespace {
struct LogBuffer {
    std::mutex mutex;
    std::deque<LogEntry> entries;
    size_t bytes = 0;
};
LogBuffer &buffer() {
    static LogBuffer log;
    return log;
}
} // namespace
void append_log(const std::string &text, LogSource source, retro_log_level level, bool error_stream) {
    if (text.empty())
        return;
    auto &log = buffer();
    std::lock_guard<std::mutex> lock(log.mutex);
    auto retained = text.substr(text.size() > 131072 ? text.size() - 131072 : 0);
    log.bytes += retained.size();
    log.entries.push_back({std::move(retained), source, level});
    if (log.bytes > 262144 || log.entries.size() > 4096)
        while (log.bytes > 131072 || log.entries.size() > 2048) {
            log.bytes -= log.entries.front().text.size();
            log.entries.pop_front();
        }
    std::fwrite(text.data(), 1, text.size(), error_stream ? stderr : stdout);
}
std::vector<LogEntry> snapshot_log() {
    auto &log = buffer();
    std::lock_guard<std::mutex> lock(log.mutex);
    return {log.entries.begin(), log.entries.end()};
}
std::string plain_log() {
    auto &log = buffer();
    std::lock_guard<std::mutex> lock(log.mutex);
    std::string text;
    for (const auto &entry : log.entries)
        text += entry.text;
    return text;
}
void clear_log() {
    auto &log = buffer();
    std::lock_guard<std::mutex> lock(log.mutex);
    log.entries.clear();
    log.bytes = 0;
}
