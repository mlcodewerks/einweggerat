#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include "shader_diagnostics.h"

struct shader_parameter {
    std::string id, label;
    float initial = 0, minimum = 0, maximum = 1, step = 0.01f;
};
struct shader_pass {
    std::filesystem::path source;
    bool enabled = true;
    // Keys omit their numeric pass suffix; unknown preset options are preserved.
    std::map<std::string, std::string> options;
};
struct shader_chain_config {
    std::string name;
    bool enabled = true;
    std::vector<shader_pass> passes;
    std::map<std::string, std::string> globals;
    std::map<std::string, float> values;
    std::vector<shader_parameter> parameters;
};
struct shader_settings {
    bool enabled = true;
    std::vector<shader_chain_config> chains;
};
bool shader_import(const std::filesystem::path &, shader_chain_config &, std::string &);
bool shader_export(const std::filesystem::path &, const shader_chain_config &, std::string &);
bool shader_save(const std::filesystem::path &, const shader_settings &, std::string &);
bool shader_load(const std::filesystem::path &, shader_settings &, std::string &);
bool shader_save_source(const std::filesystem::path &, const std::string &, std::string &);
// Create a named source and preset without replacing existing files.
bool shader_create_preset(const std::filesystem::path &directory, const std::string &name,
    const std::string &source, shader_chain_config &, std::string &error);
std::vector<std::string> shader_pass_parameters(const std::filesystem::path &);
std::filesystem::path shader_temporary_preset();

// UI and renderers use this on the frontend thread. GPU state belongs to its backend.
struct shader_controller {
    shader_settings settings;
    uint64_t revision = 1;
    std::string error;
    shader_diagnostic_report diagnostics;
    std::filesystem::path directory;
    bool initialized = false;
    float source_fps = 60, source_aspect = 0;
    void init();
    void changed() { ++revision; diagnostics.clear(); }
    void compilation_failed(const shader_chain_config &chain, bool enabled_only = true) {
        shader_read_diagnostics(diagnostics, error, chain, enabled_only);
    }
    bool inspect(shader_chain_config &);
};
shader_controller &video_shaders();
