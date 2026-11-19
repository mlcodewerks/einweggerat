#pragma once
#include "common.h"
#include <array>
#include <map>

constexpr unsigned input_players = 16;
enum class InputKind { Key, MouseButton, MouseAxis, MouseWheel, Button, Axis, Hat };
enum class AnalogMode { Mapped, Mouse, Digital };
enum class InputScope { Global, Core, Game };
struct InputSource {
    InputKind kind = InputKind::Key;
    std::string device = "keyboard";
    int code = 0, direction = 0;
};
struct InputOutput {
    unsigned device = RETRO_DEVICE_JOYPAD, index = 0, id = 0;
    bool operator==(const InputOutput &other) const {
        return device == other.device && index == other.index && id == other.id;
    }
};
struct InputBinding {
    InputOutput output;
    std::string label;
    std::vector<InputSource> positive, negative;
    AnalogMode mode = AnalogMode::Mapped;
    float sensitivity = 100, deadzone = 24, mouse_sensitivity = 1000, strength = 100, threshold = 50;
    bool invert = false, custom = false;
    int16_t value = 0;
};
struct InputPlayer {
    unsigned controller = RETRO_DEVICE_JOYPAD;
    std::vector<InputBinding> bindings;
};
struct InputProfile {
    std::array<InputPlayer, input_players> players;
    // A physical device may feed several players; several devices may feed one player.
    std::map<std::string, uint32_t> assignments;
    std::map<std::string, std::string> device_names;
};
struct InputController {
    std::string id, name;
    bool xinput = false, connected = true;
    std::array<bool, 128> buttons{};
    std::array<int16_t, 8> axes{};
    std::array<int, 4> hats{{-1, -1, -1, -1}};
    std::map<int, std::string> button_names, axis_names;
};
struct InputPointer {
    int16_t x = 0, y = 0;
    bool pressed = false, inside = false;
};
struct InputSnapshot {
    bool focused = true;
    std::array<bool, 256> keys{};
    unsigned mouse_buttons = 0;
    int mouse_x = 0, mouse_y = 0, wheel = 0, horizontal_wheel = 0;
    InputPointer pointer;
    std::vector<InputPointer> touch;
    std::vector<InputController> controllers;
    std::vector<uint32_t> text;
};
struct InputDeviceChoice {
    unsigned id;
    std::string name;
};

InputProfile default_input_profile();
bool input_axis(const InputOutput &output);
bool input_vertical_axis(const InputOutput &output);
std::string input_output_name(const InputOutput &output);
std::string input_key_name(unsigned key);
int16_t combine_input_axes(int16_t first, int16_t second);
bool load_input_profile(const fs::path &path, InputProfile &profile, std::string &error);
bool save_input_profile(const fs::path &path, const InputProfile &profile, std::string &error);
