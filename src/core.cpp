#include "core.h"
#include "native.h"
#include "shader_chain.h"
#include "vfs.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iomanip>
struct Core::Api {
    HMODULE module = nullptr;
    decltype(&retro_init) init = nullptr;
    decltype(&retro_deinit) deinit = nullptr;
    decltype(&retro_api_version) version = nullptr;
    decltype(&retro_set_environment) set_environment = nullptr;
    decltype(&retro_set_video_refresh) set_video = nullptr;
    decltype(&retro_set_audio_sample) set_sample = nullptr;
    decltype(&retro_set_audio_sample_batch) set_batch = nullptr;
    decltype(&retro_set_input_poll) set_poll = nullptr;
    decltype(&retro_set_input_state) set_input = nullptr;
    decltype(&retro_get_system_info) system_info = nullptr;
    decltype(&retro_get_system_av_info) av_info = nullptr;
    decltype(&retro_set_controller_port_device) controller = nullptr;
    decltype(&retro_load_game) load_game = nullptr;
    decltype(&retro_unload_game) unload_game = nullptr;
    decltype(&retro_run) run = nullptr;
    decltype(&retro_reset) reset = nullptr;
    decltype(&retro_serialize_size) state_size = nullptr;
    decltype(&retro_serialize) serialize = nullptr;
    decltype(&retro_unserialize) unserialize = nullptr;
    decltype(&retro_get_memory_data) memory = nullptr;
    decltype(&retro_get_memory_size) memory_size = nullptr;
    ~Api() {
        if (module)
            FreeLibrary(module);
    }
    template <class T> void bind(T &target, const char *name) {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!target)
            throw std::runtime_error(std::string("Missing core export: ") + name);
    }
};
Core *Core::active = nullptr;
Core::Core(Video &v, Input &i, const fs::path &dir) : video(v), input(i), directory(dir) {
    if (active)
        throw std::runtime_error("Only one libretro core can run per process");
    active = this;
    fs::create_directories(dir / L"system");
    fs::create_directories(dir / L"saves");
    fs::create_directories(dir / L"config");
    fs::create_directories(dir / L"cores");
    system_directory = (dir / L"system").u8string();
    assets_directory = dir.u8string();
    input.load_configuration(dir);
}
Core::~Core() {
    try {
        unload();
    } catch (...) {
    }
    active = nullptr;
}
void Core::log(const std::string &text, LogSource source, retro_log_level level) {
    append_log(text, source, level);
}
std::string Core::log_text() { return plain_log(); }
std::vector<LogEntry> Core::log_entries() { return snapshot_log(); }
void RETRO_CALLCONV Core::core_log(retro_log_level level, const char *format, ...) {
    if (!active)
        return;
    char text[8192];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    active->log(text, LogSource::Core, level);
}
void Core::load(const fs::path &library, const fs::path &content) {
    unload();
    core_path = fs::absolute(library);
    content_path = content.empty() ? fs::path{} : fs::absolute(content);
    core_path_utf8 = core_path.u8string();
    content_path_utf8 = content_path.u8string();
    save_path = directory / L"saves" / core_path.stem();
    fs::create_directories(save_path);
    save_directory = save_path.u8string();
    save_path /= content.empty() ? L"no-content" : content_path.stem();
    restore_options();
    input.begin_core(core_path, content_path);
    applied_controllers.fill(UINT_MAX);
    video.set_format(RETRO_PIXEL_FORMAT_0RGB1555);
    supports_no_game = false;
    try {
        api = std::make_unique<Api>();
        api->module = LoadLibraryExW(core_path.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!api->module)
            throw std::runtime_error("Cannot load Windows x64 core: " + core_path_utf8 + " (" +
                                     std::to_string(GetLastError()) + ")");
#define BIND(field, name) api->bind(api->field, "retro_" name)
        BIND(init, "init");
        BIND(deinit, "deinit");
        BIND(version, "api_version");
        BIND(set_environment, "set_environment");
        BIND(set_video, "set_video_refresh");
        BIND(set_sample, "set_audio_sample");
        BIND(set_batch, "set_audio_sample_batch");
        BIND(set_poll, "set_input_poll");
        BIND(set_input, "set_input_state");
        BIND(system_info, "get_system_info");
        BIND(av_info, "get_system_av_info");
        BIND(controller, "set_controller_port_device");
        BIND(load_game, "load_game");
        BIND(unload_game, "unload_game");
        BIND(run, "run");
        BIND(reset, "reset");
        BIND(state_size, "serialize_size");
        BIND(serialize, "serialize");
        BIND(unserialize, "unserialize");
        BIND(memory, "get_memory_data");
        BIND(memory_size, "get_memory_size");
#undef BIND
        if (api->version() != RETRO_API_VERSION)
            throw std::runtime_error("Unsupported libretro API version");
        api->set_environment(environment);
        api->set_video(video_callback);
        api->set_sample(audio_sample);
        api->set_batch(audio_batch);
        api->set_poll(+[]() {});
        api->set_input(+[](unsigned port, unsigned device, unsigned index, unsigned id) -> int16_t {
            return active ? active->input.state(port, device, index, id) : 0;
        });
        midi.init(directory);
        api->init();
        initialized = true;
        if (!callback_error.empty())
            throw std::runtime_error(callback_error);
        retro_system_info info{};
        api->system_info(&info);
        title = info.library_name ? info.library_name : core_path.stem().u8string();
        retro_game_info game{};
        game.path = content_path_utf8.c_str();
        if (content.empty() && !supports_no_game)
            throw std::runtime_error("This core requires content");
        if (!content.empty() && !info.need_fullpath) {
            content_bytes = read_file(content_path);
            game.data = content_bytes.data();
            game.size = content_bytes.size();
        }
        if (!api->load_game(content.empty() ? nullptr : &game))
            throw std::runtime_error("Core rejected the content");
        game_loaded = true;
        if (!callback_error.empty())
            throw std::runtime_error(callback_error);
        retro_system_av_info av{};
        api->av_info(&av);
        apply_av(av);
        video.start_core(av.geometry);
        apply_controllers();
        auto ram = save_path;
        ram += L".srm";
        if (fs::exists(ram)) {
            auto bytes = read_file(ram);
            auto size = api->memory_size(RETRO_MEMORY_SAVE_RAM);
            auto *target = api->memory(RETRO_MEMORY_SAVE_RAM);
            if (target && size == bytes.size()) {
                std::memcpy(target, bytes.data(), size);
                log("Loaded SRAM: " + ram.u8string() + " (" + std::to_string(size) + " bytes)\n");
            }
        }
        frames = 0;
        previous_time = 0;
        paused = false;
        shutdown_requested = false;
        log("Loaded " + title + " with " + (video.context() == Context::OpenGL ? "OpenGL" : "Vulkan") + "\n");
    } catch (...) {
        unload();
        throw;
    }
}
void Core::save_ram() {
    if (!game_loaded || !api)
        return;
    auto size = api->memory_size(RETRO_MEMORY_SAVE_RAM);
    auto *data = api->memory(RETRO_MEMORY_SAVE_RAM);
    if (data && size) {
        auto path = save_path;
        path += L".srm";
        write_file(path, data, size);
        log("Saved SRAM: " + path.u8string() + " (" + std::to_string(size) + " bytes)\n");
    }
}
void Core::unload() {
    if (!api)
        return;
    std::string failure;
    try {
        save_ram();
        save_options();
    } catch (const std::exception &e) {
        failure = e.what();
    }
    audio.stop();
    midi.stop();
    // GPU resources and callbacks must be retired while the core DLL is loaded.
    video.stop_core();
    if (game_loaded)
        api->unload_game();
    game_loaded = false;
    if (initialized)
        api->deinit();
    input.end_core();
    initialized = false;
    api.reset();
    options.clear();
    option_categories.clear();
    content_bytes.clear();
    pending_samples.clear();
    callback_error.clear();
    disks = {};
    frame_time = {};
    buffer_status = {};
    option_display = {};
    title.clear();
    if (!failure.empty())
        log("Save error: " + failure + "\n");
}
void Core::apply_av(const retro_system_av_info &info) {
    if (!std::isfinite(info.timing.fps) || info.timing.fps < 1 || info.timing.fps > 1000 ||
        !std::isfinite(info.timing.sample_rate) || info.timing.sample_rate < 8000 ||
        info.timing.sample_rate > 384000)
        throw std::runtime_error("Invalid core AV timing");
    if (audio_enabled)
        audio.start(info.timing.sample_rate);
    sample_rate = info.timing.sample_rate;
    fps = info.timing.fps;
    video_shaders().source_fps = float(fps);
    previous_time = 0;
    video.geometry(info.geometry, true);
}
void Core::run() {
    if (!game_loaded)
        return;
    apply_controllers();
    if (paused)
        return;
    const auto now = native_nanoseconds();
    if (frame_time.callback)
        frame_time.callback(previous_time ? retro_usec_t((now - previous_time) / 1000)
                                          : frame_time.reference);
    previous_time = now;
    if (buffer_status.callback) {
        auto percent = unsigned(std::clamp(audio.queued_seconds() / 0.1 * 100, 0.0, 100.0));
        buffer_status.callback(audio.running(), percent, percent < 20);
    }
    pending_samples.clear();
    api->run();
    if (!pending_samples.empty())
        audio.push(pending_samples.data(), pending_samples.size() / 2);
    if (!callback_error.empty())
        throw std::runtime_error(callback_error);
    if (audio_enabled)
        midi.frame(fps);
    ++frames;
    if (frames % 600 == 0)
        save_ram();
    if (shutdown_requested)
        paused = true;
}
void Core::apply_controllers() {
    for (unsigned player = 0; player < input.player_count(); ++player) {
        unsigned device = input.profile.players[player].controller;
        if (applied_controllers[player] != device) {
            api->controller(player, device);
            applied_controllers[player] = device;
        }
    }
}
void Core::reset() {
    if (game_loaded) {
        api->reset();
        audio.clear();
        audio.dsp.reset(float(sample_rate));
        previous_time = 0;
    }
}
void Core::save_state(unsigned slot) {
    auto path = save_path;
    path += L".state" + std::to_wstring(slot);
    save_state(path);
}
void Core::save_state(const fs::path &path) {
    if (!game_loaded)
        return;
    size_t size = api->state_size();
    if (!size || size > size_t(1) << 30)
        throw std::runtime_error("Core does not provide a valid save state");
    std::vector<uint8_t> state(size);
    if (!api->serialize(state.data(), size))
        throw std::runtime_error("State serialization failed");
    write_file(path, state.data(), size);
}
void Core::load_state(unsigned slot) {
    auto path = save_path;
    path += L".state" + std::to_wstring(slot);
    load_state(path);
}
void Core::load_state(const fs::path &path) {
    if (!game_loaded)
        return;
    auto bytes = read_file(path);
    if (!api->unserialize(bytes.data(), bytes.size()))
        throw std::runtime_error("State load failed");
    audio.clear();
    audio.dsp.reset(float(sample_rate));
    previous_time = 0;
}
void Core::restore_options() {
    saved_options.clear();
    std::ifstream file(directory / L"config" / (core_path.stem().wstring() + L".options"));
    std::string key, value;
    while (file >> std::quoted(key) >> std::quoted(value))
        saved_options[key] = value;
}
void Core::save_options() {
    if (core_path.empty())
        return;
    std::string text;
    for (const auto &option : options) {
        std::ostringstream line;
        line << std::quoted(option.key) << ' ' << std::quoted(option.value) << '\n';
        text += line.str();
    }
    write_file(directory / L"config" / (core_path.stem().wstring() + L".options"), text.data(), text.size());
}
void Core::set_option(Option &option, const std::string &value) {
    if (std::find(option.values.begin(), option.values.end(), value) == option.values.end())
        return;
    option.value = value;
    saved_options[option.key] = value;
    variables_changed = true;
    save_options();
    if (option_display.callback)
        option_display.callback();
}
void Core::define(const retro_core_option_definition *definitions) {
    options.clear();
    option_categories.clear();
    if (!definitions)
        return;
    for (auto *d = definitions; d->key; ++d) {
        Option option;
        option.key = d->key;
        option.label = d->desc ? d->desc : d->key;
        option.info = d->info ? d->info : "";
        for (const auto &value : d->values) {
            if (!value.value)
                break;
            option.values.emplace_back(value.value);
            option.value_labels.emplace_back(value.label ? value.label : value.value);
        }
        option.value = d->default_value        ? d->default_value
                       : option.values.empty() ? ""
                                               : option.values.front();
        auto saved = saved_options.find(option.key);
        if (saved != saved_options.end() &&
            std::find(option.values.begin(), option.values.end(), saved->second) != option.values.end())
            option.value = saved->second;
        options.push_back(std::move(option));
    }
}
void Core::define(const retro_core_option_v2_definition *definitions) {
    std::vector<retro_core_option_definition> legacy;
    if (definitions)
        for (auto *d = definitions; d->key; ++d) {
            retro_core_option_definition value{};
            value.key = d->key;
            value.desc = d->desc;
            value.info = d->info;
            value.default_value = d->default_value;
            std::copy(std::begin(d->values), std::end(d->values), std::begin(value.values));
            legacy.push_back(value);
        }
    legacy.push_back({});
    define(legacy.data());
    if (definitions)
        for (size_t i = 0; definitions[i].key; ++i) {
            const auto &d = definitions[i];
            options[i].category = d.category_key ? d.category_key : "";
            options[i].categorized_label = d.desc_categorized ? d.desc_categorized : "";
            options[i].categorized_info = d.info_categorized ? d.info_categorized : "";
        }
}
void RETRO_CALLCONV Core::video_callback(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!active)
        return;
    try {
        active->video.refresh(data, w, h, pitch);
    } catch (const std::exception &e) {
        active->callback_error = e.what();
    }
}
size_t RETRO_CALLCONV Core::audio_batch(const int16_t *data, size_t count) {
    if (!active)
        return count;
    try {
        return active->audio.push(data, count);
    } catch (const std::exception &e) {
        active->callback_error = e.what();
        return count;
    }
}
void RETRO_CALLCONV Core::audio_sample(int16_t left, int16_t right) {
    if (active) {
        active->pending_samples.push_back(left);
        active->pending_samples.push_back(right);
    }
}
bool RETRO_CALLCONV Core::environment(unsigned command, void *data) {
    if (!active)
        return false;
    try {
        return active->environment_impl(command, data);
    } catch (const std::exception &e) {
        active->callback_error = e.what();
        return false;
    }
}
bool Core::environment_impl(unsigned command, void *data) {
    if (!data && command != RETRO_ENVIRONMENT_SHUTDOWN && command != RETRO_ENVIRONMENT_GET_INPUT_BITMASKS)
        return false;
    switch (command) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *static_cast<bool *>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char **>(data) = system_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char **>(data) = save_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *static_cast<const char **>(data) = assets_directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
        *static_cast<const char **>(data) = core_path_utf8.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned *>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        supports_no_game = *static_cast<bool *>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return video.set_format(*static_cast<retro_pixel_format *>(data));
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        return video.hardware(static_cast<retro_hw_render_callback *>(data));
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        *static_cast<unsigned *>(data) = video.preferred_hardware;
        return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
        return video.environment(command, data);
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        return vfs_environment(data);
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback *>(data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        auto *message = static_cast<retro_message *>(data);
        if (message->msg)
            log(std::string(message->msg) + "\n", LogSource::Core);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *static_cast<unsigned *>(data) = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        auto *message = static_cast<retro_message_ext *>(data);
        if (message->msg)
            log(std::string(message->msg) + "\n", LogSource::Core, message->level);
        return true;
    }
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdown_requested = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        static_cast<retro_rumble_interface *>(data)->set_rumble_state =
            +[](unsigned port, retro_rumble_effect effect, uint16_t strength) -> bool {
            return active && active->input.rumble(port, effect, strength);
        };
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
        *static_cast<uint64_t *>(data) = (1ull << RETRO_DEVICE_JOYPAD) | (1ull << RETRO_DEVICE_ANALOG) |
                                         (1ull << RETRO_DEVICE_KEYBOARD) | (1ull << RETRO_DEVICE_MOUSE) |
                                         (1ull << RETRO_DEVICE_POINTER) | (1ull << RETRO_DEVICE_LIGHTGUN);
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        input.set_descriptors(static_cast<const retro_input_descriptor *>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        input.set_controller_info(static_cast<const retro_controller_info *>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        input.set_keyboard_callback(static_cast<const retro_keyboard_callback *>(data)->callback);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        video.geometry(*static_cast<retro_game_geometry *>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        apply_av(*static_cast<retro_system_av_info *>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        frame_time = *static_cast<retro_frame_time_callback *>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
        buffer_status = *static_cast<retro_audio_buffer_status_callback *>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *static_cast<int *>(data) = 3;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned *>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        define(static_cast<retro_core_option_definition *>(data));
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
        auto *intl = static_cast<retro_core_options_intl *>(data);
        define(intl->us);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
        auto *opts = static_cast<retro_core_options_v2 *>(data);
        define(opts->definitions);
        if (opts->categories)
            for (auto *c = opts->categories; c->key; ++c)
                option_categories.push_back({c->key, c->desc ? c->desc : c->key, c->info ? c->info : ""});
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        auto *intl = static_cast<retro_core_options_v2_intl *>(data);
        if (!intl->us)
            return false;
        define(intl->us->definitions);
        if (intl->us->categories)
            for (auto *c = intl->us->categories; c->key; ++c)
                option_categories.push_back({c->key, c->desc ? c->desc : c->key, c->info ? c->info : ""});
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        option_display = *static_cast<retro_core_options_update_display_callback *>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
        auto *display = static_cast<retro_core_option_display *>(data);
        for (auto &option : options)
            if (display->key && option.key == display->key) {
                option.visible = display->visible;
                return true;
            }
        return false;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        options.clear();
        option_categories.clear();
        auto *vars = static_cast<retro_variable *>(data);
        for (auto *v = vars; v->key; ++v) {
            if (!v->value)
                continue;
            std::string text = v->value;
            auto separator = text.find(';');
            if (separator == text.npos)
                continue;
            Option option;
            option.key = v->key;
            option.label = text.substr(0, separator);
            auto start = text.find_first_not_of(' ', separator + 1);
            while (start != text.npos) {
                auto end = text.find('|', start);
                option.values.push_back(text.substr(start, end == text.npos ? end : end - start));
                start = end == text.npos ? end : end + 1;
            }
            if (option.values.empty())
                continue;
            option.value = option.values.front();
            auto saved = saved_options.find(option.key);
            if (saved != saved_options.end() &&
                std::find(option.values.begin(), option.values.end(), saved->second) != option.values.end())
                option.value = saved->second;
            options.push_back(std::move(option));
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *variable = static_cast<retro_variable *>(data);
        variable->value = nullptr;
        if (!variable->key)
            return false;
        for (auto &option : options)
            if (option.key == variable->key) {
                variable->value = option.value.c_str();
                return true;
            }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool *>(data) = variables_changed;
        variables_changed = false;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *static_cast<unsigned *>(data) = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        disks = *static_cast<retro_disk_control_ext_callback *>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
        auto *d = static_cast<retro_disk_control_callback *>(data);
        disks = {};
        disks.set_eject_state = d->set_eject_state;
        disks.get_eject_state = d->get_eject_state;
        disks.get_image_index = d->get_image_index;
        disks.set_image_index = d->set_image_index;
        disks.get_num_images = d->get_num_images;
        disks.replace_image_index = d->replace_image_index;
        disks.add_image_index = d->add_image_index;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_MIDI_INTERFACE: {
        auto *m = static_cast<retro_midi_interface *>(data);
        m->input_enabled = +[]() -> bool { return false; };
        m->output_enabled = +[]() -> bool { return active && active->midi.enabled(); };
        m->read = +[](uint8_t *) -> bool { return false; };
        m->write =
            +[](uint8_t byte, uint32_t delta) -> bool { return active && active->midi.write(byte, delta); };
        m->flush = +[]() -> bool { return active && active->midi.flush(); };
        return true;
    }
    default:
        return false;
    }
}
