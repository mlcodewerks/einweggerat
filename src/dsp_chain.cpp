#include "dsp_chain.h"
#include <libretro_dspfilter.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <cctype>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C"
{
#define DSP_ENTRY(name) const dspfilter_implementation *name##_dspfilter_get_implementation(dspfilter_simd_mask_t);
    DSP_ENTRY(chorus)
    DSP_ENTRY(delta) DSP_ENTRY(echo) DSP_ENTRY(eq) DSP_ENTRY(iir)
        DSP_ENTRY(panning) DSP_ENTRY(phaser) DSP_ENTRY(reverb) DSP_ENTRY(tremolo) DSP_ENTRY(vibrato) DSP_ENTRY(wahwah)
            DSP_ENTRY(bitcrusher) DSP_ENTRY(overdrive) DSP_ENTRY(overdrive_guitar) DSP_ENTRY(earlyreverb) DSP_ENTRY(wsolapitchtempo)
#undef DSP_ENTRY
}

const std::vector<dsp_filter> &dsp_filters()
{
    using K = dsp_parameter_kind;
    static const std::vector<dsp_filter> filters = {
        {"chorus", "Chorus", {{"delay_ms", "Base delay (ms)", K::number, "25", 0, 1000}, {"depth_ms", "Delay depth (ms)", K::number, "1", 0, 1000}, 
        {"lfo_freq", "LFO frequency (Hz)", K::number, "0.5", 0.1f, 100}, {"drywet", "Wet mix", K::number, "0.8", 0, 1}}},
        {"crystalizer", "Crystalizer", {{"intensity", "Intensity", K::number, "5", 0, 10}}},
        {"echo", "Echo", {{"delay", "Tap delays (ms)", K::array, "200", 1, 5000}, {"feedback", "Tap feedback", K::array, "0.5", -0.99f, 0.99f}, 
        {"amp", "Echo amplification", K::number, "0.2", 0, 1}}},
        {"eq", "Equalizer (FFT)", {{"window_beta", "Kaiser window beta", K::number, "4", 0, 20}, {"block_size_log2", "Block size (power of 2)", K::integer, "8", 4, 11}, {"frequencies", "Band frequencies (Hz)", K::array, "0 24000", 0, 384000}, {"gains", "Band gains (dB)", K::array, "0 0", -60, 24}, {"impulse_response_output", "Impulse response output file", K::text, ""}}},
        {"iir", "IIR filter", {{"type", "Filter type", K::choice, "LPF", 0, 0, {"LPF", "HPF", "BPCSGF", "BPZPGF", "APF", "NOTCH", "RIAA_phono", "PEQ", "BBOOST", "LSH", "HSH", "RIAA_CD"}}, {"frequency", "Frequency (Hz)", K::number, "1024", 1, 192000}, {"quality", "Quality (Q)", K::number, "0.707", 0.01f, 100}, {"gain", "Gain (dB)", K::number, "0", -24, 24}}},
        {"panning", "Panning / channel mixer", {{"left_mix", "Left output: L / R gain", K::array, "1 0", -8, 8}, {"right_mix", "Right output: L / R gain", K::array, "0 1", -8, 8}}},
        {"phaser", "Phaser", {{"lfo_freq", "LFO frequency (Hz)", K::number, "0.4", 0.1f, 100}, {"lfo_start_phase", "LFO start phase (degrees)", K::number, "0", 0, 360}, {"feedback", "Feedback (%)", K::number, "0", -99, 99}, {"depth", "Depth", K::number, "0.4", 0, 1}, {"dry_wet", "Wet mix", K::number, "0.5", 0, 1}, {"stages", "Stages", K::integer, "2", 1, 24}}},
        {"reverb", "Reverb", {{"drytime", "Dry level", K::number, "0.43", 0, 1}, {"wettime", "Wet level", K::number, "0.4", 0, 1}, {"damping", "Damping", K::number, "0.8", 0, 1}, {"roomwidth", "Room width", K::number, "0.56", 0, 1}, {"roomsize", "Room size", K::number, "0.56", 0, 1}}},
        {"tremolo", "Tremolo", {{"freq", "Frequency (Hz)", K::number, "4", 0.1f, 100}, {"depth", "Depth", K::number, "0.9", 0, 1}}},
        {"vibrato", "Vibrato", {{"freq", "Frequency (Hz)", K::number, "5", 0.1f, 100}, {"depth", "Depth", K::number, "0.5", 0, 1}}},
        {"wahwah", "WahWah", {{"lfo_freq", "LFO frequency (Hz)", K::number, "1.5", 0.1f, 100}, {"lfo_start_phase", "LFO start phase (degrees)", K::number, "0", 0, 360}, {"freq_offset", "Frequency offset", K::number, "0.3", 0, 1}, {"depth", "Depth", K::number, "0.7", 0, 1}, {"resonance", "Resonance", K::number, "2.5", 0.1f, 20}}},
        {"bitcrusher", "Bitcrusher", {{"bits", "Bit depth", K::integer, "8", 2, 24}, {"downsample", "Sample hold factor", K::integer, "4", 1, 64}, {"drywet", "Wet mix", K::number, "1", 0, 1}}},
        {"overdrive", "Distortion / Overdrive", {{"drive_db", "Drive (dB)", K::number, "18", 0, 42}, {"tone_hz", "Tone (Hz)", K::number, "6500", 200, 172800}, {"drywet", "Wet mix", K::number, "1", 0, 1}, {"level_db", "Output level (dB)", K::number, "-4", -30, 12}, {"asymmetry", "Asymmetry", K::number, "0.12", 0, 1}}},
        {"overdrive_guitar", "Guitar Distortion / Tube Overdrive", {
            {"drive_db", "Drive (dB)", K::number, "18", 0, 42},
            {"bass_hz", "Bass crossover (Hz)", K::number, "180", 35, 1400},
            {"bass_tight", "Bass tightness", K::number, "0.78", 0, 1},
            {"tone_hz", "Tone (Hz)", K::number, "5600", 1200, 172800},
            {"presence", "Presence", K::number, "0.18", 0, 1},
            {"drywet", "Wet mix", K::number, "1", 0, 1},
            {"level_db", "Output level (dB)", K::number, "-5", -30, 12},
            {"asymmetry", "Asymmetry", K::number, "0.20", -1, 1}}},
        {"earlyreverb", "Early Reflection Reverb", {{"predelay_ms", "Predelay (ms)", K::number, "18", 0, 250}, {"room_size", "Room size", K::number, "1", 0.45f, 2.25f}, {"decay_sec", "Decay (seconds)", K::number, "2.6", 0.2f, 20}, {"damping", "Damping", K::number, "0.42", 0, 0.98f}, {"diffusion", "Diffusion", K::number, "0.72", 0, 1}, {"early_mix", "Early reflections", K::number, "0.42", 0, 1.5f}, {"late_mix", "Late reflections", K::number, "0.78", 0, 1.5f}, {"width", "Stereo width", K::number, "1.15", 0, 2}, {"drywet", "Wet mix", K::number, "0.35", 0, 1}}},
        {"wsola", "WSOLA pitch / tempo", {{"pitch", "Pitch (semitones)", K::number, "0", -12, 12}, {"tempo", "Tempo change (%)", K::number, "0", -50, 100}, {"rate", "Playback rate (%)", K::number, "1.0", 0.25f, 2.0f}}},

    };
    return filters;
}

const dsp_filter *dsp_find_filter(const std::string &id)
{
    const std::string canonical = (id == "native_wsola" || id == "wsolapitchtempo") ? "wsola" : id;
    for (const auto &filter : dsp_filters())
        if (filter.id == canonical)
            return &filter;
    return nullptr;
}

dsp_stage dsp_default_stage(const std::string &id)
{
    dsp_stage stage{id, true, {}};
    if (const auto *filter = dsp_find_filter(id))
    {
        stage.filter = filter->id;
        for (const auto &p : filter->parameters)
            stage.parameters[p.key] = p.initial;
    }
    return stage;
}

namespace
{
    std::string trim(const std::string &s)
    {
        const auto first = s.find_first_not_of(" \t\r\n");
        return first == s.npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }
    std::vector<float> numbers(const std::string &s)
    {
        std::istringstream stream(s);
        stream.imbue(std::locale::classic());
        std::vector<float> result;
        std::string token;
        while (stream >> token)
        {
            std::istringstream number(token);
            number.imbue(std::locale::classic());
            float v;
            if (!(number >> v) || !number.eof() || !std::isfinite(v) || result.size() == 64)
                throw std::runtime_error("Expected at most 64 finite numbers separated by spaces.");
            result.push_back(v);
        }
        if (!stream.eof() || result.empty())
            throw std::runtime_error("Expected numbers separated by spaces.");
        return result;
    }
    std::string value(const dsp_stage &stage, const char *key)
    {
        const auto it = stage.parameters.find(key);
        if (it != stage.parameters.end())
            return it->second;
        for (const auto &p : dsp_find_filter(stage.filter)->parameters)
            if (std::strcmp(p.key, key) == 0)
                return p.initial;
        throw std::runtime_error("Unknown DSP parameter.");
    }
    void validate(const dsp_settings &settings, float rate, bool check_rate = true)
    {
        if (settings.stages.size() > 32)
            throw std::runtime_error("A chain supports up to 32 filters.");
        if (!std::isfinite(rate) || rate < 1000 || rate > 384000)
            throw std::runtime_error("DSP requires an input rate between 1000 and 384000 Hz.");
        double duration_ratio = 1;
        for (size_t i = 0; i < settings.stages.size(); ++i)
        {
            const auto &stage = settings.stages[i];
            try
            {
                const auto *filter = dsp_find_filter(stage.filter);
                if (!filter)
                    throw std::runtime_error("Unknown filter: " + stage.filter);
                for (const auto &entry : stage.parameters)
                    if (std::none_of(filter->parameters.begin(), filter->parameters.end(),
                                     [&](const dsp_parameter &p)
                                     { return entry.first == p.key; }))
                        throw std::runtime_error("Unknown parameter: " + entry.first);
                for (const auto &p : filter->parameters)
                {
                    const auto s = value(stage, p.key);
                    if (s.size() > 4096 || s.find_first_of("\r\n\"") != s.npos)
                        throw std::runtime_error(std::string(p.label) + ": invalid text.");
                    if (p.kind == dsp_parameter_kind::text)
                        continue;
                    if (p.kind == dsp_parameter_kind::choice)
                    {
                        if (std::none_of(p.choices.begin(), p.choices.end(), [&](const char *c)
                                         { return s == c; }))
                            throw std::runtime_error("Unknown filter type: " + s);
                        continue;
                    }
                    const auto vals = numbers(s);
                    if (p.kind != dsp_parameter_kind::array && vals.size() != 1)
                        throw std::runtime_error(std::string(p.label) + ": expected one number.");
                    for (float v : vals)
                        if (v < p.minimum || v > p.maximum || (p.kind == dsp_parameter_kind::integer && v != std::floor(v)))
                            throw std::runtime_error(std::string(p.label) + ": outside the supported range.");
                }
                auto list = [&](const char *key)
                { return numbers(value(stage, key)); };
                auto scalar = [&](const char *key)
                { return list(key).front(); };
                if (settings.enabled && stage.enabled && std::strcmp(filter->id, "wsolapitchtempo") == 0)
                {
                    duration_ratio /= 1.0 + scalar("tempo") / 100.0;
                    if (duration_ratio > 16)
                        throw std::runtime_error("Combined tempo expansion exceeds 16 times the input duration.");
                }
                if (stage.filter == "echo")
                {
                    if (list("delay").size() != list("feedback").size())
                        throw std::runtime_error("Each echo delay needs a matching feedback value.");
                    double total = 0;
                    for (float d : list("delay"))
                        total += d;
                    if (check_rate && total * rate / 1000 > 2000000)
                        throw std::runtime_error("Combined echo delay exceeds 2 million frames.");
                }
                if (stage.filter == "eq" && list("frequencies").size() != list("gains").size())
                    throw std::runtime_error("Each EQ frequency needs a matching gain.");
                if (stage.filter == "panning" && (list("left_mix").size() != 2 || list("right_mix").size() != 2))
                    throw std::runtime_error("Each channel mix needs exactly two gains (L R).");
                if (stage.filter == "chorus" && (scalar("depth_ms") > scalar("delay_ms") ||
                                                 (check_rate && (scalar("delay_ms") + scalar("depth_ms")) * rate / 1000 >= 4094)))
                    throw std::runtime_error("Chorus depth must not exceed delay; delay + depth must be below 4094 samples at this rate.");
                if (check_rate && stage.filter == "iir" && scalar("frequency") >= rate * 0.5f)
                    throw std::runtime_error("IIR frequency must be below half the input sample rate.");
            }
            catch (const std::exception &e)
            {
                throw std::runtime_error("Filter " + std::to_string(i + 1) + " (" + stage.filter + "): " + e.what());
            }
        }
    }

    struct config_context
    {
        const dsp_stage &stage;
        bool allocation_failed = false;
        bool export_impulse = false;
    };
    int get_float(void *data, const char *key, float *out, float fallback)
    {
        const auto &p = static_cast<config_context *>(data)->stage.parameters;
        const auto it = p.find(key);
        *out = it == p.end() ? fallback : numbers(it->second).front();
        return it != p.end();
    }
    int get_int(void *data, const char *key, int *out, int fallback)
    {
        float f;
        int found = get_float(data, key, &f, static_cast<float>(fallback));
        *out = static_cast<int>(f);
        return found;
    }
    template <class T>
    int get_array(void *data, const char *key, T **out, unsigned *count, const T *fallback, unsigned n)
    {
        auto &context = *static_cast<config_context *>(data);
        const auto it = context.stage.parameters.find(key);
        std::vector<float> vals;
        if (it != context.stage.parameters.end())
            vals = numbers(it->second);
        *count = it == context.stage.parameters.end() ? n : static_cast<unsigned>(vals.size());
        *out = static_cast<T *>(std::malloc(*count * sizeof(T)));
        if (!*out)
        {
            context.allocation_failed = true;
            *count = 0;
            return 0;
        }
        for (unsigned i = 0; i < *count; ++i)
            (*out)[i] = it == context.stage.parameters.end() ? fallback[i] : static_cast<T>(vals[i]);
        return it != context.stage.parameters.end();
    }
    int get_string(void *data, const char *key, char **out, const char *fallback)
    {
        auto &context = *static_cast<config_context *>(data);
        const auto it = context.stage.parameters.find(key);
        bool found = it != context.stage.parameters.end();
        // Restarting the audio stream must not rewrite an exported response file.
        if (std::strcmp(key, "impulse_response_output") == 0 && !context.export_impulse)
            found = false;
        const std::string s = found ? it->second : fallback;
        *out = static_cast<char *>(std::malloc(s.size() + 1));
        if (!*out)
        {
            context.allocation_failed = true;
            return 0;
        }
        std::memcpy(*out, s.c_str(), s.size() + 1);
        return found && !s.empty();
    }
    const dspfilter_config callbacks{get_float, get_int, get_array<float>, get_array<int>, get_string, std::free};
    const dspfilter_implementation *implementation(const std::string &id)
    {
        static const dspfilter_get_implementation_t entries[] = {
            chorus_dspfilter_get_implementation, delta_dspfilter_get_implementation, echo_dspfilter_get_implementation,
            eq_dspfilter_get_implementation, iir_dspfilter_get_implementation, panning_dspfilter_get_implementation,
            phaser_dspfilter_get_implementation, reverb_dspfilter_get_implementation, tremolo_dspfilter_get_implementation,
            vibrato_dspfilter_get_implementation, wahwah_dspfilter_get_implementation,
            bitcrusher_dspfilter_get_implementation, overdrive_dspfilter_get_implementation,
            overdrive_guitar_dspfilter_get_implementation, earlyreverb_dspfilter_get_implementation,
            wsolapitchtempo_dspfilter_get_implementation};
        const auto *filter = dsp_find_filter(id);
        for (size_t i = 0; filter && i < dsp_filters().size(); ++i)
            if (filter == &dsp_filters()[i])
                return entries[i](0);
        return nullptr;
    }
}

struct dsp_chain::runtime
{
    struct instance
    {
        const dspfilter_implementation *api;
        void *data = nullptr;
        ~instance()
        {
            if (data)
                api->free(data);
        }
    };
    std::vector<std::unique_ptr<instance>> stages;
    std::vector<float> a, b;
    runtime(const dsp_settings &settings, float rate, bool export_impulse)
    {
        validate(settings, rate);
        // Reserve the largest burst from the bounded EQ blocks across the chain.
        a.reserve(2 * (256 + 32 * 2048));
        b.reserve(a.capacity());
        if (!settings.enabled)
            return;
        for (const auto &stage : settings.stages)
        {
            if (!stage.enabled)
                continue;
            auto item = std::make_unique<instance>();
            item->api = implementation(stage.filter);
            config_context context{stage, false, export_impulse};
            dspfilter_info info{rate};
            item->data = item->api->init(&info, &callbacks, &context);
            if (!item->data || context.allocation_failed)
                throw std::runtime_error("Could not initialize " + stage.filter);
            stages.push_back(std::move(item));
        }
    }
};
dsp_chain::dsp_chain() = default;
dsp_chain::~dsp_chain() = default;
dsp_settings dsp_chain::settings() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}
std::string dsp_chain::error() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return last_error;
}
float dsp_chain::sample_rate() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return rate;
}
bool dsp_chain::configure(const dsp_settings &settings, std::string &error, bool export_impulse)
{
    std::lock_guard<std::mutex> lock(mutex);
    try
    {
        auto next = std::make_unique<runtime>(settings, rate, export_impulse);
        current = settings;
        active = std::move(next);
        last_error.clear();
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
void dsp_chain::reset(float sample_rate)
{
    std::lock_guard<std::mutex> lock(mutex);
    rate = sample_rate;
    try
    {
        active = std::make_unique<runtime>(current, rate, false);
        last_error.clear();
    }
    catch (const std::exception &e)
    {
        active.reset();
        last_error = std::string("DSP bypassed: ") + e.what();
    }
}
void dsp_chain::process(const int16_t *samples, size_t frames, std::vector<int16_t> &output)
{
    std::lock_guard<std::mutex> lock(mutex);
    output.clear();
    if (!samples || !frames)
        return;
    if (!active || active->stages.empty())
    {
        output.assign(samples, samples + frames * 2);
        return;
    }
    // Small calls also bound the upstream EQ's fixed output + overlap scratch buffer.
    for (size_t start = 0; start < frames; start += 256)
    {
        const size_t count = std::min<size_t>(256, frames - start) * 2;
        auto &a = active->a;
        auto &b = active->b;
        a.resize(count);
        for (size_t i = 0; i < count; ++i)
            a[i] = samples[start * 2 + i] / 32768.0f;
        for (auto &stage : active->stages)
        {
            b.clear();
            for (size_t pos = 0; pos < a.size(); pos += 512)
            {
                dspfilter_input in{a.data() + pos, static_cast<unsigned>(std::min<size_t>(512, a.size() - pos) / 2)};
                dspfilter_output out{};
                stage->api->process(stage->data, &out, &in);
                if (out.frames)
                    b.insert(b.end(), out.samples, out.samples + out.frames * 2);
            }
            a.swap(b);
        }
        for (float f : a)
        {
            if (!std::isfinite(f))
                f = 0;
            output.push_back(static_cast<int16_t>(std::lround(std::clamp(f * 32768.0f, -32768.0f, 32767.0f))));
        }
    }
}

bool dsp_load(const std::filesystem::path &path, dsp_settings &settings, std::string &error)
{
    try
    {
        std::ifstream file(path);
        if (!file)
            throw std::runtime_error("Cannot open DSP preset: " + path.u8string());
        std::map<std::string, std::string> entries;
        std::string line;
        size_t bytes = 0;
        while (std::getline(file, line))
        {
            bytes += line.size();
            if (bytes > 1024 * 1024)
                throw std::runtime_error("DSP preset is too large.");
            bool quoted = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"')
                    quoted = !quoted;
                if (line[i] == '#' && !quoted)
                {
                    line.resize(i);
                    break;
                }
            }
            if (trim(line).empty())
                continue;
            auto equal = line.find('=');
            if (equal == line.npos || quoted)
                throw std::runtime_error("Malformed DSP preset line.");
            auto v = trim(line.substr(equal + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            entries[trim(line.substr(0, equal))] = v;
        }
        if (file.bad())
            throw std::runtime_error("Cannot read DSP preset.");
        auto n = numbers(entries.at("filters"));
        if (n.size() != 1 || n[0] < 0 || n[0] > 32 || std::floor(n[0]) != n[0])
            throw std::runtime_error("Invalid DSP filter count (0-32).");
        auto flag = [&](const std::string &key)
        {
            auto it = entries.find(key);
            if (it == entries.end())
                return true;
            if (it->second == "true" || it->second == "1")
                return true;
            if (it->second == "false" || it->second == "0")
                return false;
            throw std::runtime_error("Invalid bypass flag: " + key);
        };
        dsp_settings result;
        result.enabled = flag("einweggerat_enabled");
        for (size_t i = 0; i < static_cast<size_t>(n[0]); ++i)
        {
            auto prefix = "filter" + std::to_string(i);
            auto id = entries.at(prefix);
            // RetroArch presets may name the shared library, including an extension.
            id = std::filesystem::path(id).stem().string();
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            auto *filter = dsp_find_filter(id);
            if (!filter)
                throw std::runtime_error("Unsupported DSP filter: " + id);
            auto stage = dsp_default_stage(id);
            stage.enabled = flag(prefix + "_enabled");
            for (const auto &p : filter->parameters)
            {
                auto it = entries.find(prefix + "_" + p.key);
                if (it == entries.end())
                    it = entries.find(id + "_" + p.key);
                if (it != entries.end())
                    stage.parameters[p.key] = it->second;
            }
            result.stages.push_back(std::move(stage));
        }
        // Actual rate-dependent checks occur when applying the preset.
        validate(result, 48000, false);
        settings = std::move(result);
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = std::string("Cannot load preset: ") + e.what();
        return false;
    }
}

bool dsp_save(const std::filesystem::path &path, const dsp_settings &settings, std::string &error)
{
    try
    {
        // Validation of sample-rate-dependent fields is handled by configure().
        validate(settings, 48000, false);
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot write " + temporary.u8string());
        file << "# libretro DSP chain; einweggerat_enabled and filterN_enabled preserve bypass state.\n";
        file << "filters = " << settings.stages.size() << "\neinweggerat_enabled = " << settings.enabled << '\n';
        for (size_t i = 0; i < settings.stages.size(); ++i)
        {
            const auto &s = settings.stages[i];
            if (!dsp_find_filter(s.filter))
                throw std::runtime_error("Unknown filter.");
            file << "\nfilter" << i << " = " << s.filter << "\nfilter" << i << "_enabled = " << s.enabled << '\n';
            for (const auto &p : dsp_find_filter(s.filter)->parameters)
            {
                auto v = value(s, p.key);
                if (v.find_first_of("\r\n\"") != v.npos)
                    throw std::runtime_error("Invalid parameter text.");
                file << "filter" << i << '_' << p.key << " = \"" << v << "\"\n";
            }
        }
        file.close();
        if (!file)
            throw std::runtime_error("Cannot finish writing DSP preset.");
#ifdef _WIN32
        // std::filesystem::rename cannot replace an existing file on Windows.
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace DSP preset.");
#else
        std::filesystem::rename(temporary, path);
#endif
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
