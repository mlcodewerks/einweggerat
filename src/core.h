#pragma once
#include "audio.h"
#include "common.h"
#include "input.h"
#include "log.h"
#include "midi.h"
#include "video.h"
#include <map>
#include <mutex>
struct Option {
    std::string key, label, info, value;
    std::vector<std::string> values;
    std::vector<std::string> value_labels;
    std::string category, categorized_label, categorized_info;
    bool visible = true;
};
struct OptionCategory {
    std::string key, label, info;
};
class Core {
  public:
    Core(Video &video, Input &input, const fs::path &directory);
    ~Core();
    void load(const fs::path &library, const fs::path &content = {});
    void unload();
    void run();
    void reset();
    void save_state(unsigned slot);
    void load_state(unsigned slot);
    void set_option(Option &option, const std::string &value);
    bool loaded() const { return game_loaded; }
    const std::string &name() const { return title; }
    const fs::path &library_path() const { return core_path; }
    void save_state(const fs::path &path);
    void load_state(const fs::path &path);
    double fps = 60;
    bool paused = false, audio_enabled = true;
    Audio audio;
    Midi midi;
    std::vector<Option> options;
    std::vector<OptionCategory> option_categories;
    retro_disk_control_ext_callback disks{};
    std::string log_text();
    std::vector<LogEntry> log_entries();
    void log(const std::string &text, LogSource source = LogSource::Frontend,
             retro_log_level level = RETRO_LOG_INFO);
    uint64_t frames = 0;

  private:
    struct Api;
    std::unique_ptr<Api> api;
    Video &video;
    Input &input;
    fs::path directory, core_path, content_path, save_path;
    std::string system_directory, save_directory, assets_directory, core_path_utf8, content_path_utf8, title,
        callback_error;
    std::vector<uint8_t> content_bytes;
    std::map<std::string, std::string> saved_options;
    bool initialized = false, game_loaded = false, variables_changed = false, supports_no_game = false,
         shutdown_requested = false;
    retro_frame_time_callback frame_time{};
    retro_audio_buffer_status_callback buffer_status{};
    retro_core_options_update_display_callback option_display{};
    uint64_t previous_time = 0;
    double sample_rate = 48000;
    std::array<unsigned, input_players> applied_controllers{};
    void apply_controllers();
    static Core *active;
    static bool RETRO_CALLCONV environment(unsigned command, void *data);
    bool environment_impl(unsigned command, void *data);
    static void RETRO_CALLCONV video_callback(const void *, unsigned, unsigned, size_t);
    static size_t RETRO_CALLCONV audio_batch(const int16_t *, size_t);
    static void RETRO_CALLCONV audio_sample(int16_t, int16_t);
    std::vector<int16_t> pending_samples;
    static void RETRO_CALLCONV core_log(retro_log_level, const char *, ...);
    void define(const retro_core_option_definition *definitions);
    void define(const retro_core_option_v2_definition *definitions);
    void restore_options();
    void save_options();
    void save_ram();
    void apply_av(const retro_system_av_info &info);
};
