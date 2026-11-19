#pragma once
#include "input_bindings.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

class Input {
  public:
    Input();
    ~Input();
    void init(HWND window);
    void rescan();
    void raw(LPARAM data);
    void character(wchar_t character);
    void touch(WPARAM count, LPARAM handle);
    void focus(bool focused);
    void poll(bool capture_keyboard, bool capture_mouse);
    void apply_snapshot(const InputSnapshot &snapshot, bool capture_keyboard = false,
                        bool capture_mouse = false);
    int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    bool rumble(unsigned port, retro_rumble_effect effect, uint16_t strength);
    void assign(const std::string &device, unsigned player, bool enabled);
    bool assigned(const std::string &device, unsigned player) const;
    std::vector<InputController> device_list() const;
    std::vector<std::string> devices() const;
    std::string source_name(const InputSource &source) const;
    std::string binding_name(const std::vector<InputSource> &sources) const;
    InputBinding &binding(unsigned player, InputOutput output);
    std::vector<InputOutput> visible_bindings(unsigned player) const;
    const std::vector<InputDeviceChoice> &controller_choices(unsigned player) const;
    unsigned player_count() const {
        return has_core() ? std::max(1u, std::max(controller_ports, descriptor_ports)) : input_players;
    }
    void set_controller_info(const retro_controller_info *info);
    void set_controller(unsigned player, unsigned device);
    void set_descriptors(const retro_input_descriptor *descriptors);
    void begin_core(const fs::path &library, const fs::path &content);
    void end_core();
    void load_configuration(const fs::path &directory);
    void select_scope(InputScope scope);
    bool save_configuration(std::string &error);
    InputScope scope() const { return current_scope; }
    bool has_core() const { return !core_path.empty(); }
    bool has_game() const { return !game_path.empty(); }
    fs::path profile_path(InputScope scope) const;
    void reset_player(unsigned player);
    void begin_capture(unsigned player, InputOutput output, bool negative = false, bool append = false);
    void cancel_capture();
    bool capturing() const { return capture.active; }
    bool capturing(unsigned player, InputOutput output, bool negative) const;
    void set_pointer_viewport(RECT viewport) { pointer_viewport = viewport; }
    void set_keyboard_callback(retro_keyboard_event_t callback) { keyboard_callback = callback; }
    InputProfile profile;
    std::string status;
    static uint16_t xinput_buttons(const XINPUT_GAMEPAD &pad);

  private:
    HWND window = nullptr;
    bool focused = true;
    ComPtr<IDirectInput8W> direct;
    struct Joystick {
        ComPtr<IDirectInputDevice8W> device;
        InputController info;
    };
    std::vector<Joystick> joysticks;
    std::array<bool, 256> keyboard{};
    LONG pending_x = 0, pending_y = 0;
    unsigned mouse_buttons = 0;
    int wheel = 0, horizontal_wheel = 0;
    wchar_t high_surrogate = 0;
    std::vector<uint32_t> pending_text;
    std::map<DWORD, POINT> touch_points;
    RECT pointer_viewport{};
    InputSnapshot latest, previous;
    bool block_keyboard = false, block_mouse = false, suppress_frame = false;
    std::array<int16_t, RETROK_LAST> retro_keyboard{}, previous_keyboard{};
    std::array<XINPUT_VIBRATION, input_players> vibrations{};
    std::array<bool, 4> rumble_owned{};
    std::array<std::vector<InputDeviceChoice>, input_players> choices;
    bool release_capture_input = false;
    InputSource captured_input;
    struct Descriptor {
        unsigned port;
        InputOutput output;
        std::string label;
        unsigned device;
    };
    std::vector<Descriptor> labels;
    fs::path config_directory, core_path, game_path;
    InputScope current_scope = InputScope::Global;
    unsigned controller_ports = 0, descriptor_ports = 0;
    retro_keyboard_event_t keyboard_callback = nullptr;
    struct Capture {
        bool active = false, negative = false, append = false;
        unsigned player = 0;
        InputOutput output;
    } capture;
    float source_value(const InputSource &source, unsigned player, const InputBinding &binding) const;
    void capture_input(const InputSnapshot &snapshot);
    void apply_labels();
    void update_rumble();
    void stop_rumble();
    static BOOL CALLBACK enumerate(const DIDEVICEINSTANCEW *, void *);
    static BOOL CALLBACK range(const DIDEVICEOBJECTINSTANCEW *, void *);
};
