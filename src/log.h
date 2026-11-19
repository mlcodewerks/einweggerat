#pragma once
#include "libretro.h"
#include <string>
#include <vector>

enum class LogSource { Frontend, Core };
struct LogEntry {
    std::string text;
    LogSource source = LogSource::Frontend;
    retro_log_level level = RETRO_LOG_INFO;
};
void append_log(const std::string &text, LogSource source = LogSource::Frontend,
                retro_log_level level = RETRO_LOG_INFO, bool error_stream = false);
std::vector<LogEntry> snapshot_log();
std::string plain_log();
void clear_log();
