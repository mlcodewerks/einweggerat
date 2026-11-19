#include "input.h"
#include <cmath>
#include <sstream>

namespace {
bool hat_pressed(int angle, int direction) {
    if (angle < 0)
        return false;
    switch (direction) {
    case 0:
        return angle >= 31500 || angle <= 4500;
    case 1:
        return angle >= 4500 && angle <= 13500;
    case 2:
        return angle >= 13500 && angle <= 22500;
    case 3:
        return angle >= 22500 && angle <= 31500;
    default:
        return false;
    }
}
int16_t clamp_axis(float value) { return int16_t(std::clamp(value, -32768.0f, 32767.0f)); }
std::vector<InputDeviceChoice> default_choices() {
    return {{RETRO_DEVICE_NONE, "Disconnected"},
            {RETRO_DEVICE_JOYPAD, "Joypad"},
            {RETRO_DEVICE_ANALOG, "Analog controller"},
            {RETRO_DEVICE_KEYBOARD, "Keyboard"},
            {RETRO_DEVICE_MOUSE, "Mouse"},
            {RETRO_DEVICE_POINTER, "Pointer / touch"},
            {RETRO_DEVICE_LIGHTGUN, "Light gun (mouse)"}};
}
bool descriptor_matches(unsigned selected, unsigned device) {
    if (selected == RETRO_DEVICE_NONE)
        return false;
    const unsigned base = selected & RETRO_DEVICE_MASK;
    const unsigned descriptor_base = device & RETRO_DEVICE_MASK;
    if (device != descriptor_base)
        return device == selected;
    // Libretro pads share digital buttons and analog stick descriptors.
    const bool pad = base == RETRO_DEVICE_JOYPAD || base == RETRO_DEVICE_ANALOG;
    const bool pad_descriptor = descriptor_base == RETRO_DEVICE_JOYPAD || descriptor_base == RETRO_DEVICE_ANALOG;
    return base == descriptor_base || (pad && pad_descriptor);
}
bool same_source(const InputSource &a, const InputSource &b) {
    return a.kind == b.kind && a.device == b.device && a.code == b.code && a.direction == b.direction;
}
bool raw_source_held(const InputSource &s, const InputSnapshot &snapshot) {
    if (s.kind == InputKind::Key)
        return s.code > 0 && s.code < 256 && snapshot.keys[s.code];
    if (s.kind == InputKind::MouseButton)
        return s.code >= 0 && s.code < 5 && (snapshot.mouse_buttons & (1u << s.code));
    if (s.kind == InputKind::MouseWheel)
        return (s.code ? snapshot.horizontal_wheel : snapshot.wheel) != 0;
    for (const auto &c : snapshot.controllers)
        if (c.connected && c.id == s.device) {
            if (s.kind == InputKind::Button && s.code >= 0 && s.code < 128)
                return c.buttons[s.code];
            if (s.kind == InputKind::Axis && s.code >= 0 && s.code < 8)
                return std::abs(int(c.axes[s.code])) >= 16384;
            if (s.kind == InputKind::Hat && s.code >= 0 && s.code < 4)
                return hat_pressed(c.hats[s.code], s.direction);
        }
    return false;
}
} // namespace
bool Input::assigned(const std::string &device, unsigned player) const {
    auto found = profile.assignments.find(device);
    return player < input_players && found != profile.assignments.end() && (found->second & (1u << player));
}
void Input::assign(const std::string &device, unsigned player, bool enabled) {
    if (player >= input_players)
        return;
    auto &mask = profile.assignments[device];
    if (enabled)
        mask |= 1u << player;
    else
        mask &= ~(1u << player);
    cancel_capture();
    stop_rumble();
}
InputBinding &Input::binding(unsigned player, InputOutput output) {
    auto &bindings = profile.players.at(player).bindings;
    for (auto &entry : bindings)
        if (entry.output == output)
            return entry;
    InputBinding entry;
    entry.output = output;
    entry.label = input_output_name(output);
    bindings.push_back(std::move(entry));
    return bindings.back();
}
float Input::source_value(const InputSource &source, unsigned player, const InputBinding &b) const {
    switch (source.kind) {
    case InputKind::Key:
        return !block_keyboard && assigned("keyboard", player) && source.code > 0 && source.code < 256 &&
                       latest.keys[source.code]
                   ? 32767.0f
                   : 0;
    case InputKind::MouseButton:
        return !block_mouse && assigned("mouse", player) && source.code >= 0 && source.code < 5 &&
                       (latest.mouse_buttons & (1u << source.code))
                   ? 32767.0f
                   : 0;
    case InputKind::MouseAxis: {
        if (block_mouse || !assigned("mouse", player))
            return 0;
        float value = float(source.code ? latest.mouse_y : latest.mouse_x) * b.mouse_sensitivity;
        return source.direction ? std::max(0.0f, value * source.direction) : value;
    }
    case InputKind::MouseWheel: {
        if (block_mouse || !assigned("mouse", player))
            return 0;
        int value = source.code ? latest.horizontal_wheel : latest.wheel;
        return value * (source.direction ? source.direction : 1) > 0 ? 32767.0f : 0;
    }
    default:
        break;
    }
    int16_t combined = 0;
    for (const auto &controller : latest.controllers) {
        if (!controller.connected || !assigned(controller.id, player))
            continue;
        if (source.device == "@xinput" && !controller.xinput)
            continue;
        if (source.device == "@dinput" && controller.xinput)
            continue;
        if (source.device != "@xinput" && source.device != "@dinput" && source.device != controller.id)
            continue;
        float value = 0;
        if (source.kind == InputKind::Button && source.code >= 0 && source.code < 128)
            value = controller.buttons[source.code] ? 32767.0f : 0;
        else if (source.kind == InputKind::Axis && source.code >= 0 && source.code < 8) {
            value = controller.axes[source.code];
            if (source.direction)
                value = std::max(0.0f, value * source.direction);
        } else if (source.kind == InputKind::Hat && source.code >= 0 && source.code < 4)
            value = hat_pressed(controller.hats[source.code], source.direction) ? 32767.0f : 0;
        combined = combine_input_axes(combined, clamp_axis(value));
    }
    return combined;
}
void Input::begin_capture(unsigned player, InputOutput output, bool negative, bool append) {
    if (player >= input_players)
        return;
    capture = {true, negative, append, player, output};
    previous = latest;
    status = "Press a key, mouse button, controller button, stick or D-pad. Escape cancels.";
}
void Input::cancel_capture() { capture.active = false; }
bool Input::capturing(unsigned player, InputOutput output, bool negative) const {
    return capture.active && capture.player == player && capture.output == output &&
           capture.negative == negative;
}
void Input::capture_input(const InputSnapshot &snapshot) {
    if (!capture.active)
        return;
    if (!snapshot.focused) {
        cancel_capture();
        return;
    }
    if (snapshot.keys[VK_ESCAPE] && !previous.keys[VK_ESCAPE]) {
        cancel_capture();
        status = "Input capture cancelled.";
        return;
    }
    auto finish = [&](InputSource source) {
        auto &b = binding(capture.player, capture.output);
        auto &sources = capture.negative ? b.negative : b.positive;
        if (!capture.append)
            sources.clear();
        if (sources.size() < 32 && std::none_of(sources.begin(), sources.end(),
                                                [&](const InputSource &s) { return same_source(s, source); }))
            sources.push_back(source);
        b.custom = true;
        profile.assignments[source.device] |= 1u << capture.player;
        status = "Bound " + source_name(source) + ".";
        captured_input = source;
        release_capture_input = true;
        cancel_capture();
    };
    for (unsigned key = 1; key < 256; ++key)
        if (snapshot.keys[key] && !previous.keys[key]) {
            finish({InputKind::Key, "keyboard", int(key), 0});
            return;
        }
    for (int button = 0; button < 5; ++button)
        if ((snapshot.mouse_buttons & (1u << button)) && !(previous.mouse_buttons & (1u << button))) {
            finish({InputKind::MouseButton, "mouse", button, 0});
            return;
        }
    if (snapshot.wheel || snapshot.horizontal_wheel) {
        int value = snapshot.wheel ? snapshot.wheel : snapshot.horizontal_wheel;
        finish({InputKind::MouseWheel, "mouse", snapshot.wheel ? 0 : 1, value < 0 ? -1 : 1});
        return;
    }
    const bool full_axis =
        input_axis(capture.output) && binding(capture.player, capture.output).mode == AnalogMode::Mapped;
    for (const auto &controller : snapshot.controllers) {
        if (!controller.connected)
            continue;
        const InputController *before = nullptr;
        for (const auto &old : previous.controllers)
            if (old.id == controller.id)
                before = &old;
        // Do not bind already-held controls on a newly connected device.
        if (!before)
            continue;
        for (int axis = 0; axis < 8; ++axis)
            if (std::abs(int(controller.axes[axis])) >= 16384 && std::abs(int(before->axes[axis])) < 16384) {
                finish({InputKind::Axis, controller.id, axis,
                        full_axis ? 0 : (controller.axes[axis] < 0 ? -1 : 1)});
                return;
            }
        for (int hat = 0; hat < 4; ++hat)
            for (int direction = 0; direction < 4; ++direction)
                if (hat_pressed(controller.hats[hat], direction) &&
                    !hat_pressed(before->hats[hat], direction)) {
                    finish({InputKind::Hat, controller.id, hat, direction});
                    return;
                }
        for (int button = 0; button < 128; ++button)
            if (controller.buttons[button] && !before->buttons[button]) {
                finish({InputKind::Button, controller.id, button, 0});
                return;
            }
    }
}
void Input::apply_snapshot(const InputSnapshot &snapshot, bool capture_keyboard, bool capture_mouse) {
    const bool was_capturing = capture.active;
    latest = snapshot;
    for (const auto &controller : snapshot.controllers)
        if (controller.connected)
            profile.device_names[controller.id] = controller.name;
    capture_input(snapshot);
    if (release_capture_input && !raw_source_held(captured_input, snapshot))
        release_capture_input = false;
    suppress_frame = was_capturing || release_capture_input || !snapshot.focused;
    block_keyboard = capture_keyboard || suppress_frame;
    block_mouse = capture_mouse || suppress_frame;
    retro_keyboard.fill(0);
    if (!block_keyboard) {
        const auto &keyboard = snapshot.keys;
        for (unsigned i = 0; i < 26; ++i)
            retro_keyboard[RETROK_a + i] = keyboard['A' + i];
        for (unsigned i = 0; i < 10; ++i)
            retro_keyboard[RETROK_0 + i] = keyboard['0' + i];
        const std::pair<unsigned, unsigned> special[] = {
            {RETROK_RETURN, VK_RETURN},  {RETROK_ESCAPE, VK_ESCAPE}, {RETROK_SPACE, VK_SPACE},
            {RETROK_BACKSPACE, VK_BACK}, {RETROK_TAB, VK_TAB},       {RETROK_UP, VK_UP},
            {RETROK_DOWN, VK_DOWN},      {RETROK_LEFT, VK_LEFT},     {RETROK_RIGHT, VK_RIGHT},
            {RETROK_LSHIFT, VK_LSHIFT},  {RETROK_RSHIFT, VK_RSHIFT}, {RETROK_LCTRL, VK_LCONTROL},
            {RETROK_RCTRL, VK_RCONTROL}, {RETROK_LALT, VK_LMENU},    {RETROK_RALT, VK_RMENU}};
        for (const auto &pair : special)
            retro_keyboard[pair.first] = keyboard[pair.second];
        for (unsigned i = 0; i < 12; ++i)
            retro_keyboard[RETROK_F1 + i] = keyboard[VK_F1 + i];
        const std::pair<unsigned, unsigned> extra[] = {{RETROK_INSERT, VK_INSERT},
                                                       {RETROK_DELETE, VK_DELETE},
                                                       {RETROK_HOME, VK_HOME},
                                                       {RETROK_END, VK_END},
                                                       {RETROK_PAGEUP, VK_PRIOR},
                                                       {RETROK_PAGEDOWN, VK_NEXT},
                                                       {RETROK_CAPSLOCK, VK_CAPITAL},
                                                       {RETROK_NUMLOCK, VK_NUMLOCK},
                                                       {RETROK_SCROLLOCK, VK_SCROLL},
                                                       {RETROK_PAUSE, VK_PAUSE},
                                                       {RETROK_PRINT, VK_SNAPSHOT},
                                                       {RETROK_MINUS, VK_OEM_MINUS},
                                                       {RETROK_EQUALS, VK_OEM_PLUS},
                                                       {RETROK_LEFTBRACKET, VK_OEM_4},
                                                       {RETROK_RIGHTBRACKET, VK_OEM_6},
                                                       {RETROK_BACKSLASH, VK_OEM_5},
                                                       {RETROK_SEMICOLON, VK_OEM_1},
                                                       {RETROK_QUOTE, VK_OEM_7},
                                                       {RETROK_BACKQUOTE, VK_OEM_3},
                                                       {RETROK_COMMA, VK_OEM_COMMA},
                                                       {RETROK_PERIOD, VK_OEM_PERIOD},
                                                       {RETROK_SLASH, VK_OEM_2},
                                                       {RETROK_KP_PERIOD, VK_DECIMAL},
                                                       {RETROK_KP_DIVIDE, VK_DIVIDE},
                                                       {RETROK_KP_MULTIPLY, VK_MULTIPLY},
                                                       {RETROK_KP_MINUS, VK_SUBTRACT},
                                                       {RETROK_KP_PLUS, VK_ADD}};
        for (const auto &pair : extra)
            retro_keyboard[pair.first] = keyboard[pair.second];
        for (unsigned i = 0; i < 10; ++i)
            retro_keyboard[RETROK_KP0 + i] = keyboard[VK_NUMPAD0 + i];
    }
    for (unsigned player = 0; player < input_players; ++player)
        for (auto &b : profile.players[player].bindings) {
            b.value = 0;
            if (suppress_frame || profile.players[player].controller == RETRO_DEVICE_NONE)
                continue;
            auto digital = [&](const std::vector<InputSource> &sources) {
                for (const auto &source : sources)
                    if (source_value(source, player, b) >=
                        ((source.kind == InputKind::Axis || source.kind == InputKind::MouseAxis)
                             ? 32767 * b.threshold / 100
                             : 1))
                        return true;
                return false;
            };
            if (!input_axis(b.output)) {
                b.value = digital(b.positive) ? 1 : 0;
                continue;
            }
            float value = 0;
            if (b.mode == AnalogMode::Digital)
                value = float(int(digital(b.positive)) - int(digital(b.negative))) * 32767 * b.strength / 100;
            else if (b.mode == AnalogMode::Mouse) {
                if (!block_mouse && assigned("mouse", player))
                    value = float(input_vertical_axis(b.output) ? latest.mouse_y : latest.mouse_x) *
                            b.mouse_sensitivity;
            } else {
                int16_t combined = 0;
                for (const auto &source : b.positive)
                    combined = combine_input_axes(combined, clamp_axis(source_value(source, player, b)));
                value = combined;
                if (std::abs(value) < 32767 * b.deadzone / 100)
                    value = 0;
                value *= b.sensitivity / 100;
            }
            if (b.invert)
                value = -value;
            b.value = clamp_axis(value);
        }
    if (keyboard_callback) {
        uint16_t modifiers = 0;
        if (snapshot.keys[VK_LSHIFT] || snapshot.keys[VK_RSHIFT])
            modifiers |= RETROKMOD_SHIFT;
        if (snapshot.keys[VK_LCONTROL] || snapshot.keys[VK_RCONTROL])
            modifiers |= RETROKMOD_CTRL;
        if (snapshot.keys[VK_LMENU] || snapshot.keys[VK_RMENU])
            modifiers |= RETROKMOD_ALT;
        for (unsigned key = 1; key < RETROK_LAST; ++key) {
            bool pressed = state(0, RETRO_DEVICE_KEYBOARD, 0, key) != 0;
            if (pressed != (previous_keyboard[key] != 0))
                keyboard_callback(pressed, key, 0, modifiers);
            previous_keyboard[key] = pressed;
        }
        if (!block_keyboard && assigned("keyboard", 0))
            for (auto code : latest.text)
                keyboard_callback(true, RETROK_UNKNOWN, code, modifiers);
    }
    previous = snapshot;
}
int16_t Input::state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port >= player_count() || suppress_frame || profile.players[port].controller == RETRO_DEVICE_NONE)
        return 0;
    device &= RETRO_DEVICE_MASK;
    if (device == RETRO_DEVICE_JOYPAD && index == 0 && id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        uint16_t mask = 0;
        for (const auto &b : profile.players[port].bindings)
            if (b.output.device == device && b.output.index == 0 && b.output.id < 16 && b.value)
                mask |= uint16_t(1u << b.output.id);
        return int16_t(mask);
    }
    for (const auto &b : profile.players[port].bindings)
        if (b.output == InputOutput{device, index, id} &&
            (device == RETRO_DEVICE_JOYPAD || device == RETRO_DEVICE_ANALOG || b.custom ||
             !b.positive.empty() || b.mode != AnalogMode::Mapped))
            return b.value;
    if (device == RETRO_DEVICE_KEYBOARD)
        return !block_keyboard && assigned("keyboard", port) && id < RETROK_LAST ? retro_keyboard[id] : 0;
    if (block_mouse || !assigned("mouse", port))
        return 0;
    if (device == RETRO_DEVICE_MOUSE) {
        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X:
            return int16_t(std::clamp(latest.mouse_x, -32768, 32767));
        case RETRO_DEVICE_ID_MOUSE_Y:
            return int16_t(std::clamp(latest.mouse_y, -32768, 32767));
        case RETRO_DEVICE_ID_MOUSE_LEFT:
            return (latest.mouse_buttons & 1) != 0;
        case RETRO_DEVICE_ID_MOUSE_RIGHT:
            return (latest.mouse_buttons & 2) != 0;
        case RETRO_DEVICE_ID_MOUSE_MIDDLE:
            return (latest.mouse_buttons & 4) != 0;
        case RETRO_DEVICE_ID_MOUSE_BUTTON_4:
            return (latest.mouse_buttons & 8) != 0;
        case RETRO_DEVICE_ID_MOUSE_BUTTON_5:
            return (latest.mouse_buttons & 16) != 0;
        case RETRO_DEVICE_ID_MOUSE_WHEELUP:
            return latest.wheel > 0;
        case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
            return latest.wheel < 0;
        case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP:
            return latest.horizontal_wheel > 0;
        case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN:
            return latest.horizontal_wheel < 0;
        default:
            return 0;
        }
    }
    if (device == RETRO_DEVICE_POINTER) {
        const auto &points = latest.touch;
        if (id == RETRO_DEVICE_ID_POINTER_COUNT)
            return int16_t(points.empty() ? 1 : points.size());
        if (index >= (points.empty() ? 1 : points.size()))
            return 0;
        const auto &point = points.empty() ? latest.pointer : points[index];
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X:
            return point.x;
        case RETRO_DEVICE_ID_POINTER_Y:
            return point.y;
        case RETRO_DEVICE_ID_POINTER_PRESSED:
            return point.pressed;
        default:
            return 0;
        }
    }
    if (device == RETRO_DEVICE_LIGHTGUN && index == 0) {
        const auto &point = latest.pointer;
        switch (id) {
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
            return point.x;
        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
            return point.y;
        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
            return !point.inside;
        case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
            return (latest.mouse_buttons & 1) != 0;
        case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
        case RETRO_DEVICE_ID_LIGHTGUN_SELECT:
            return (latest.mouse_buttons & 2) != 0;
        case RETRO_DEVICE_ID_LIGHTGUN_START:
        case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
            return (latest.mouse_buttons & 4) != 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_A:
            return (latest.mouse_buttons & 8) != 0;
        case RETRO_DEVICE_ID_LIGHTGUN_AUX_B:
            return (latest.mouse_buttons & 16) != 0;
        default:
            return 0;
        }
    }
    return 0;
}
std::vector<InputController> Input::device_list() const {
    auto result = latest.controllers;
    result.insert(result.begin(), {{"keyboard", "Keyboard"}, {"mouse", "Mouse / touch"}});
    for (const auto &assignment : profile.assignments) {
        if (assignment.first.empty() || assignment.first.front() == '@')
            continue;
        if (std::any_of(result.begin(), result.end(),
                        [&](const InputController &c) { return c.id == assignment.first; }))
            continue;
        InputController device;
        device.id = assignment.first;
        device.connected = false;
        if (device.id.size() == 8 && device.id.rfind("xinput:", 0) == 0 && device.id.back() >= '0' &&
            device.id.back() <= '3') {
            device.xinput = true;
            device.name = "Xbox controller " + std::to_string(device.id.back() - '0' + 1);
        } else {
            auto name = profile.device_names.find(device.id);
            device.name =
                name == profile.device_names.end() ? "Unavailable DirectInput controller" : name->second;
        }
        result.push_back(std::move(device));
    }
    return result;
}
std::vector<std::string> Input::devices() const {
    std::vector<std::string> result;
    for (const auto &device : device_list())
        if (device.connected)
            result.push_back(device.name);
    return result;
}
std::string Input::source_name(const InputSource &source) const {
    if (source.kind == InputKind::Key)
        return input_key_name(unsigned(source.code));
    if (source.kind == InputKind::MouseButton) {
        static const char *names[] = {"Mouse left button", "Mouse right button", "Mouse middle button",
                                      "Mouse side button 1", "Mouse side button 2"};
        return source.code >= 0 && source.code < 5 ? names[source.code] : "Mouse button";
    }
    if (source.kind == InputKind::MouseAxis)
        return source.code ? "Mouse vertical motion" : "Mouse horizontal motion";
    if (source.kind == InputKind::MouseWheel)
        return std::string(source.code ? "Mouse horizontal wheel " : "Mouse wheel ") +
               (source.direction < 0 ? "down" : "up");
    InputController controller;
    controller.id = source.device;
    if (source.device == "@xinput") {
        controller.name = "Assigned Xbox controllers";
        controller.xinput = true;
    } else if (source.device == "@dinput")
        controller.name = "Assigned DirectInput controllers";
    else {
        controller.name = "Unavailable controller";
        for (const auto &known : device_list())
            if (known.id == source.device)
                controller = known;
    }
    std::string control;
    if (source.kind == InputKind::Button) {
        static const char *xbox[] = {"A",
                                     "X",
                                     "Back",
                                     "Start",
                                     "D-pad up",
                                     "D-pad down",
                                     "D-pad left",
                                     "D-pad right",
                                     "B",
                                     "Y",
                                     "Left shoulder",
                                     "Right shoulder",
                                     "Left trigger",
                                     "Right trigger",
                                     "Left stick button",
                                     "Right stick button"};
        if (controller.xinput && source.code >= 0 && source.code < 16)
            control = xbox[source.code];
        else {
            auto name = controller.button_names.find(source.code);
            control = name == controller.button_names.end() ? "Button " + std::to_string(source.code + 1)
                                                            : name->second;
        }
    } else if (source.kind == InputKind::Hat) {
        static const char *directions[] = {"up", "right", "down", "left"};
        control =
            "D-pad " + std::to_string(source.code + 1) + " " + directions[std::clamp(source.direction, 0, 3)];
    } else {
        static const char *xbox[] = {"Left stick X", "Left stick Y",  "Right stick X", "Right stick Y",
                                     "Left trigger", "Right trigger", "Axis",          "Axis"};
        static const char *generic[] = {"X axis",     "Y axis",     "Z axis",   "X rotation",
                                        "Y rotation", "Z rotation", "Slider 1", "Slider 2"};
        auto name = controller.axis_names.find(source.code);
        control = name == controller.axis_names.end()
                      ? (controller.xinput ? xbox[std::clamp(source.code, 0, 7)]
                                           : generic[std::clamp(source.code, 0, 7)])
                      : name->second;
        if (source.direction)
            control += source.direction < 0 ? " (negative direction)" : " (positive direction)";
    }
    return controller.name + " - " + control;
}
std::string Input::binding_name(const std::vector<InputSource> &sources) const {
    if (sources.empty())
        return "Unassigned";
    std::string result;
    for (const auto &source : sources) {
        if (!result.empty())
            result += " / ";
        result += source_name(source);
    }
    return result;
}
const std::vector<InputDeviceChoice> &Input::controller_choices(unsigned player) const {
    static const auto defaults = default_choices();
    return player < input_players && !choices[player].empty() ? choices[player] : defaults;
}
void Input::set_controller(unsigned player, unsigned device) {
    if (player >= input_players)
        return;
    cancel_capture();
    profile.players[player].controller = device;
    apply_labels();
    vibrations[player] = {};
    update_rumble();
}
void Input::set_controller_info(const retro_controller_info *info) {
    controller_ports = 0;
    for (auto &list : choices)
        list.clear();
    if (!info)
        return;
    unsigned declared = 0;
    for (unsigned player = 0; player < input_players && info[player].types; ++player) {
        declared = player + 1;
        auto &list = choices[player];
        list = {{RETRO_DEVICE_NONE, "Disconnected"}};
        for (unsigned i = 0; i < std::min(512u, info[player].num_types); ++i)
            list.push_back({info[player].types[i].id,
                            info[player].types[i].desc ? info[player].types[i].desc : "Controller"});
        auto selected = profile.players[player].controller;
        if (std::none_of(list.begin(), list.end(),
                         [&](const InputDeviceChoice &c) { return c.id == selected; }))
            profile.players[player].controller = list.size() > 1 ? list[1].id : RETRO_DEVICE_NONE;
    }
    controller_ports = declared;
    cancel_capture();
    apply_labels();
}
void Input::apply_labels() {
    for (auto &player : profile.players)
        for (auto &b : player.bindings)
            b.label = input_output_name(b.output);
    for (const auto &label : labels)
        if (descriptor_matches(profile.players[label.port].controller, label.device))
            binding(label.port, label.output).label = label.label;
}
std::vector<InputOutput> Input::visible_bindings(unsigned player) const {
    std::vector<InputOutput> outputs;
    if (player >= player_count())
        return outputs;
    if (!has_core()) {
        for (const auto &b : profile.players[player].bindings)
            outputs.push_back(b.output);
    } else if (profile.players[player].controller != RETRO_DEVICE_NONE) {
        for (const auto &label : labels)
            if (label.port == player && !label.label.empty() &&
                descriptor_matches(profile.players[player].controller, label.device) &&
                std::find(outputs.begin(), outputs.end(), label.output) == outputs.end())
                outputs.push_back(label.output);
    }
    return outputs;
}
void Input::set_descriptors(const retro_input_descriptor *descriptors) {
    descriptor_ports = 0;
    labels.clear();
    cancel_capture();
    if (descriptors)
        for (unsigned count = 0; count < 4096 && descriptors[count].description; ++count) {
            const auto &d = descriptors[count];
            if (d.port >= input_players)
                continue;
            labels.push_back({d.port, {d.device & RETRO_DEVICE_MASK, d.index, d.id}, d.description, d.device});
            descriptor_ports = std::max(descriptor_ports, d.port + 1);
        }
    apply_labels();
}
fs::path Input::profile_path(InputScope scope) const {
    if (scope == InputScope::Global)
        return config_directory / L"input.cfg";
    if (scope == InputScope::Core)
        return config_directory / L"input" / (core_path.stem().wstring() + L".input");
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : game_path.u8string()) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream key;
    key << std::hex << hash;
    return config_directory / L"input" / core_path.stem() /
           (game_path.stem().wstring() + L"-" + widen(key.str()) + L".input");
}
void Input::load_configuration(const fs::path &directory) {
    config_directory = directory / L"config";
    auto previous = profile.assignments;
    auto names = profile.device_names;
    profile = default_input_profile();
    std::error_code ec;
    if (fs::exists(profile_path(InputScope::Global), ec))
        load_input_profile(profile_path(InputScope::Global), profile, status);
    for (const auto &assignment : previous)
        if (!profile.assignments.count(assignment.first))
            profile.assignments.insert(assignment);
    for (const auto &name : names)
        if (!profile.device_names.count(name.first))
            profile.device_names.insert(name);
}
void Input::begin_core(const fs::path &library, const fs::path &content) {
    cancel_capture();
    stop_rumble();
    keyboard_callback = nullptr;
    previous_keyboard.fill(0);
    labels.clear();
    for (auto &list : choices)
        list.clear();
    core_path = library;
    game_path = content;
    controller_ports = descriptor_ports = 0;
    auto devices = profile.assignments;
    auto names = profile.device_names;
    profile = default_input_profile();
    current_scope = InputScope::Global;
    for (auto scope : {InputScope::Global, InputScope::Core, InputScope::Game}) {
        if (scope == InputScope::Game && content.empty())
            continue;
        auto path = profile_path(scope);
        std::error_code ec;
        if (fs::exists(path, ec) && load_input_profile(path, profile, status))
            current_scope = scope;
    }
    for (const auto &device : devices)
        if (!profile.assignments.count(device.first))
            profile.assignments.insert(device);
    for (const auto &name : names)
        if (!profile.device_names.count(name.first))
            profile.device_names.insert(name);
}
void Input::end_core() {
    keyboard_callback = nullptr;
    cancel_capture();
    stop_rumble();
    core_path.clear();
    game_path.clear();
    labels.clear();
    for (auto &list : choices)
        list.clear();
    current_scope = InputScope::Global;
    if (!config_directory.empty())
        load_configuration(config_directory.parent_path());
}
void Input::select_scope(InputScope scope) {
    if ((scope == InputScope::Core && !has_core()) || (scope == InputScope::Game && !has_game()))
        return;
    cancel_capture();
    stop_rumble();
    auto path = profile_path(scope);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        auto next = profile;
        if (!load_input_profile(path, next, status))
            return;
        profile = std::move(next);
    }
    current_scope = scope;
    for (unsigned player = 0; player < input_players; ++player)
        if (!choices[player].empty()) {
            auto selected = profile.players[player].controller;
            const auto &list = choices[player];
            if (std::none_of(list.begin(), list.end(),
                             [&](const InputDeviceChoice &c) { return c.id == selected; }))
                profile.players[player].controller = list.size() > 1 ? list[1].id : RETRO_DEVICE_NONE;
        }
    apply_labels();
    status = "Editing input profile. Use Save input profile to keep changes.";
}
bool Input::save_configuration(std::string &error) {
    return save_input_profile(profile_path(current_scope), profile, error);
}
void Input::reset_player(unsigned player) {
    if (player < input_players) {
        cancel_capture();
        profile.players[player].bindings = default_input_profile().players[player].bindings;
        apply_labels();
    }
}
