#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct shader_chain_config;
struct shader_diagnostic {
    std::filesystem::path source;
    size_t line = 0; // One-based physical source line, zero when unmapped.
    bool warning = false;
    std::string message;
};
struct shader_diagnostic_report {
    uint64_t revision = 0;
    std::vector<shader_diagnostic> entries;
    // Exact normalized source snapshots used to reject stale editor markers.
    std::map<std::filesystem::path, std::string> sources;
    void clear() { ++revision; entries.clear(); sources.clear(); }
};

std::string shader_normalize_source(std::string text);
void shader_read_diagnostics(shader_diagnostic_report &, const std::string &error,
    const shader_chain_config &, bool enabled_only);
