#include "common.h"
#include <array>
static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio;
static retro_audio_sample_t sample;
static retro_input_poll_t poll;
static retro_input_state_t input;
static unsigned frame;
static std::array<uint8_t, 128> ram{};
static std::array<unsigned, 16> controller_devices{};
static bool RETRO_CALLCONV update_options_display() {
    retro_variable toggle{"fixture_toggle", nullptr};
    environment(RETRO_ENVIRONMENT_GET_VARIABLE, &toggle);
    retro_core_option_display display{"fixture_detail",
                                      toggle.value && std::strcmp(toggle.value, "enabled") == 0};
    return environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
}
#ifdef FIXTURE_HARDWARE
bool fixture_hardware_load(retro_environment_t, retro_video_refresh_t);
void fixture_hardware_run();
#endif
extern "C" {
RETRO_API void retro_set_environment(retro_environment_t cb) {
    environment = cb;
    bool support = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &support);
}
RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { sample = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input = cb; }
RETRO_API void retro_init() {
    retro_log_callback logger{};
    if (!environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logger) || !logger.log)
        throw std::runtime_error("Log interface rejected");
    logger.log(RETRO_LOG_DEBUG, "fixture debug %d\n", 1);
    logger.log(RETRO_LOG_INFO, "fixture info\n");
    logger.log(RETRO_LOG_WARN, "fixture warning\n");
    logger.log(RETRO_LOG_ERROR, "fixture error\n");
    retro_message message{"fixture core notification", 60};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
    retro_vfs_interface_info vfs{4, nullptr};
    if (!environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs))
        throw std::runtime_error("VFS negotiation failed");
    retro_midi_interface midi{};
    if (!environment(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE, &midi) || midi.input_enabled())
        throw std::runtime_error("MIDI negotiation failed");
    static retro_variable variables[] = {{"fixture_color", "Color; red|blue"}, {nullptr, nullptr}};
    environment(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
    static const retro_controller_description types[] = {
        {"Fixture digital controller", RETRO_DEVICE_JOYPAD},
        {"Fixture analog controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)}};
    static const retro_controller_info controllers[] = {{types, 2}, {nullptr, 0}};
    if (!environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info *>(controllers)))
        throw std::runtime_error("Controller metadata rejected");
    static const retro_input_descriptor descriptions[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, 0, "Fixture fire button"}, {0, 0, 0, 0, nullptr}};
    if (!environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
                     const_cast<retro_input_descriptor *>(descriptions)))
        throw std::runtime_error("Input descriptors rejected");
}
RETRO_API void retro_deinit() {}
RETRO_API unsigned retro_api_version() { return RETRO_API_VERSION; }
RETRO_API void retro_get_system_info(retro_system_info *info) {
    *info = {"Native fixture", "1", "bin", false, false};
}
RETRO_API void retro_get_system_av_info(retro_system_av_info *info) {
    *info = {{320, 240, 640, 480, 4.0f / 3}, {60, 48000}};
}
RETRO_API bool retro_load_game(const retro_game_info *) {
    frame = 0;
#ifdef FIXTURE_HARDWARE
    return fixture_hardware_load(environment, video);
#else
    return true;
#endif
}
RETRO_API bool retro_load_game_special(unsigned, const retro_game_info *, size_t) { return false; }
RETRO_API void retro_unload_game() {}
RETRO_API void retro_reset() { frame = 0; }
RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port < controller_devices.size())
        controller_devices[port] = device;
}
__declspec(dllexport) unsigned fixture_controller_device(unsigned port) {
    return port < controller_devices.size() ? controller_devices[port] : 0;
}
__declspec(dllexport) bool fixture_define_options(unsigned version) {
    static retro_core_option_definition v1[] = {
        {"fixture_color", "Color", "Choose a color.", {{"red", "Warm red"}, {"blue", "Cool blue"}}, "red"},
        {}};
    static retro_core_option_v2_category categories[] = {
        {"video", "Video", "Video settings."}, {"empty", "Empty category", "Should not be shown."}, {}};
    static retro_core_option_v2_definition v2[] = {{"fixture_color",
                                                    "Color",
                                                    "Palette",
                                                    "Choose a color.",
                                                    "Choose the video palette.",
                                                    "video",
                                                    {{"red", "Warm red"}, {"blue", "Cool blue"}},
                                                    "red"},
                                                   {"fixture_toggle",
                                                    "Enable details",
                                                    nullptr,
                                                    "Show advanced settings.",
                                                    nullptr,
                                                    "video",
                                                    {{"disabled", "Disabled"}, {"enabled", "Enabled"}},
                                                    "disabled"},
                                                   {"fixture_detail",
                                                    "Detail level",
                                                    nullptr,
                                                    "Advanced detail.",
                                                    nullptr,
                                                    "unknown-category",
                                                    {{"low", "Low"}, {"high", "High"}},
                                                    "low"},
                                                   {}};
    if (version < 2) {
        if (version == 0) {
            retro_core_options_intl intl{v1, nullptr};
            return environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &intl);
        }
        return environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, v1);
    }
    retro_core_options_v2 options{categories, v2};
    bool result;
    if (version == 3) {
        retro_core_options_v2_intl intl{&options, nullptr};
        result = environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &intl);
    } else {
        result = environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
    }
    retro_core_options_update_display_callback callback{update_options_display};
    return result && environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &callback) &&
           update_options_display();
}
RETRO_API unsigned retro_get_region() { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM ? ram.data() : nullptr;
}
RETRO_API size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? ram.size() : 0; }
RETRO_API size_t retro_serialize_size() { return sizeof(frame); }
RETRO_API bool retro_serialize(void *data, size_t size) {
    if (size != sizeof(frame))
        return false;
    std::memcpy(data, &frame, size);
    return true;
}
RETRO_API bool retro_unserialize(const void *data, size_t size) {
    if (size != sizeof(frame))
        return false;
    std::memcpy(&frame, data, size);
    return true;
}
RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}
RETRO_API void retro_run() {
    poll();
    input(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
#ifdef FIXTURE_HARDWARE
    fixture_hardware_run();
#else
    unsigned widths[] = {320, 256, 640, 368};
    unsigned width = widths[(frame / 15) % 4], height = 240;
    auto format = static_cast<retro_pixel_format>((frame / 60) % 3);
    environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
    unsigned bpp = format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    size_t pitch = 1024 * bpp + ((frame % 2) ? 1 : 0);
    std::vector<uint8_t> pixels(pitch * height, 0xcc);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            bool red = (x < width / 2) != (y < height / 2);
            if (bpp == 4) {
                uint32_t color = red ? 0x00ff0000 : 0x000000ff;
                std::memcpy(pixels.data() + pitch * y + x * 4, &color, 4);
            } else {
                uint16_t color = red ? (format == RETRO_PIXEL_FORMAT_RGB565 ? 0xf800 : 0x7c00) : 0x001f;
                std::memcpy(pixels.data() + pitch * y + x * 2, &color, 2);
            }
        }
    video(pixels.data(), width, height, pitch);
    if (frame % 10 == 0)
        video(nullptr, 0, 0, 0);
#endif
    std::array<int16_t, 1600> pcm{};
    if (frame % 2)
        audio(pcm.data(), 800);
    else
        for (size_t i = 0; i < 800; ++i)
            sample(0, 0);
    ++frame;
    ram[0] = uint8_t(frame);
}
}
