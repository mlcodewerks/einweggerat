#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

enum class dsp_parameter_kind { number, integer, array, text, choice };
struct dsp_parameter {
    const char *key;
    const char *label;
    dsp_parameter_kind kind;
    const char *initial;
    float minimum = 0, maximum = 1;
    std::vector<const char *> choices;
};
struct dsp_filter {
    const char *id;
    const char *name;
    std::vector<dsp_parameter> parameters;
};
struct dsp_stage {
    std::string filter;
    bool enabled = true;
    std::map<std::string, std::string> parameters;
};
struct dsp_settings {
    bool enabled = true;
    std::vector<dsp_stage> stages;
};

// The catalogue is also the validation schema and the UI's parameter inventory.
const std::vector<dsp_filter> &dsp_filters();
const dsp_filter *dsp_find_filter(const std::string &id);
dsp_stage dsp_default_stage(const std::string &id);
bool dsp_load(const std::filesystem::path &path, dsp_settings &settings, std::string &error);
bool dsp_save(const std::filesystem::path &path, const dsp_settings &settings, std::string &error);

class dsp_chain {
public:
    dsp_chain();
    ~dsp_chain();
    dsp_settings settings() const;
    std::string error() const;
    float sample_rate() const;
    // Build replacement state before publishing; rejected edits retain the active chain.
    bool configure(const dsp_settings &settings, std::string &error, bool export_impulse = false);
    // Rebuild delay/FFT state at a new core rate or after restarting playback.
    void reset(float sample_rate);
    // Caller-owned PCM is never modified. Output may have a different frame count (EQ).
    void process(const int16_t *samples, size_t frames, std::vector<int16_t> &output);
private:
    struct runtime;
    mutable std::mutex mutex;
    dsp_settings current;
    std::unique_ptr<runtime> active;
    float rate = 48000;
    std::string last_error;
};
