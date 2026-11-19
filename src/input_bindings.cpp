#include "input_bindings.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

bool input_axis(const InputOutput &o) {
    return (o.device == RETRO_DEVICE_ANALOG && o.index < 2 && o.id < 2) ||
           (o.device == RETRO_DEVICE_MOUSE && o.id < 2) || (o.device == RETRO_DEVICE_POINTER && o.id < 2) ||
           (o.device == RETRO_DEVICE_LIGHTGUN &&
            (o.id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X || o.id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y));
}
bool input_vertical_axis(const InputOutput &o) {
    return o.device == RETRO_DEVICE_LIGHTGUN ? o.id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y : o.id == 1;
}
std::string input_output_name(const InputOutput &o) {
    static const char *buttons[] = {"B / bottom face button",
                                    "Y / left face button",
                                    "Select / Back",
                                    "Start",
                                    "D-pad up",
                                    "D-pad down",
                                    "D-pad left",
                                    "D-pad right",
                                    "A / right face button",
                                    "X / top face button",
                                    "Left shoulder",
                                    "Right shoulder",
                                    "Left trigger",
                                    "Right trigger",
                                    "Left stick button",
                                    "Right stick button"};
    if (o.device == RETRO_DEVICE_JOYPAD && o.id < 16)
        return buttons[o.id];
    if (o.device == RETRO_DEVICE_ANALOG && o.index < 2 && o.id < 2)
        return std::string(o.index ? "Right stick " : "Left stick ") +
               (o.id ? "Y (up/down)" : "X (left/right)");
    return "Control " + std::to_string(o.id + 1);
}
std::string input_key_name(unsigned key) {
    if (!key)
        return "Unassigned";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return std::string(1, char(key));
    switch (key) {
    case VK_RETURN:
        return "Enter";
    case VK_SPACE:
        return "Space";
    case VK_ESCAPE:
        return "Escape";
    case VK_LSHIFT:
        return "Left Shift";
    case VK_RSHIFT:
        return "Right Shift";
    case VK_LCONTROL:
        return "Left Ctrl";
    case VK_RCONTROL:
        return "Right Ctrl";
    case VK_LMENU:
        return "Left Alt";
    case VK_RMENU:
        return "Right Alt";
    case VK_UP:
        return "Up arrow";
    case VK_DOWN:
        return "Down arrow";
    case VK_LEFT:
        return "Left arrow";
    case VK_RIGHT:
        return "Right arrow";
    case VK_TAB:
        return "Tab";
    case VK_BACK:
        return "Backspace";
    case VK_INSERT:
        return "Insert";
    case VK_DELETE:
        return "Delete";
    case VK_HOME:
        return "Home";
    case VK_END:
        return "End";
    case VK_PRIOR:
        return "Page Up";
    case VK_NEXT:
        return "Page Down";
    case VK_LWIN:
        return "Left Windows";
    case VK_RWIN:
        return "Right Windows";
    case VK_APPS:
        return "Menu";
    default:
        break;
    }
    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return "Numpad " + std::to_string(key - VK_NUMPAD0);
    UINT scan = MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, GetKeyboardLayout(0));
    LONG bits = LONG((scan & 255) << 16);
    if (scan & 0xff00)
        bits |= 1 << 24;
    wchar_t name[128]{};
    if (GetKeyNameTextW(bits, name, 128))
        return narrow(name);
    return "Unknown key";
}
int16_t combine_input_axes(int16_t a, int16_t b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; }
InputProfile default_input_profile() {
    InputProfile profile;
    profile.assignments = {{"keyboard", 1}, {"mouse", 1},    {"xinput:0", 1},
                           {"xinput:1", 2}, {"xinput:2", 4}, {"xinput:3", 8}};
    for (auto &player : profile.players) {
        for (unsigned i = 0; i < 16; ++i) {
            InputBinding binding;
            binding.output = {RETRO_DEVICE_JOYPAD, 0, i};
            binding.label = input_output_name(binding.output);
            player.bindings.push_back(std::move(binding));
        }
        for (unsigned axis = 0; axis < 4; ++axis) {
            InputBinding binding;
            binding.output = {RETRO_DEVICE_ANALOG, axis / 2, axis % 2};
            binding.label = input_output_name(binding.output);
            player.bindings.push_back(std::move(binding));
        }
    }
    return profile;
}
namespace {
InputBinding &find_binding(InputProfile &profile, unsigned player, InputOutput output) {
    auto &bindings = profile.players.at(player).bindings;
    for (auto &binding : bindings)
        if (binding.output == output)
            return binding;
    if (bindings.size() >= 512)
        throw std::runtime_error("Too many input bindings");
    InputBinding binding;
    binding.output = output;
    binding.label = input_output_name(output);
    bindings.push_back(binding);
    return bindings.back();
}
void validate(const InputBinding &b) {
    auto valid = [](float value, float low, float high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    if (unsigned(b.mode) > 2 || !valid(b.sensitivity, 0, 400) || !valid(b.deadzone, 0, 95) ||
        !valid(b.mouse_sensitivity, 1, 32767) || !valid(b.strength, 0, 100) || !valid(b.threshold, 1, 100))
        throw std::runtime_error("Invalid analog input settings");
}
} // namespace
bool load_input_profile(const fs::path &path, InputProfile &profile, std::string &error) {
    try {
        auto bytes = read_file(path);
        if (bytes.size() > 4 * 1024 * 1024)
            throw std::runtime_error("Input profile is too large");
        std::istringstream file(std::string(bytes.begin(), bytes.end()));
        std::string header;
        file >> header;
        auto next = default_input_profile();
        if (header != "EINWEGGERAT_INPUT") {
            // Migrate the original Win64 keyboard/DirectInput numeric configuration.
            file.clear();
            file.seekg(0);
            std::array<int, 16> keys{}, buttons{};
            for (auto &key : keys)
                if (!(file >> key) || key < 0 || key > 255)
                    throw std::runtime_error("Invalid legacy keyboard mapping");
            for (auto &button : buttons)
                if (!(file >> button) || button < -1 || button > 127)
                    throw std::runtime_error("Invalid legacy controller mapping");
            for (auto &player : next.players)
                for (unsigned i = 0; i < 16; ++i) {
                    auto &sources = player.bindings[i].positive;
                    sources.erase(std::remove_if(sources.begin(), sources.end(),
                                                 [](const InputSource &s) {
                                                     return s.kind == InputKind::Key ||
                                                            (s.device == "@dinput" &&
                                                             s.kind == InputKind::Button);
                                                 }),
                                  sources.end());
                    if (keys[i])
                        sources.push_back({InputKind::Key, "keyboard", keys[i], 0});
                    if (buttons[i] >= 0)
                        sources.push_back({InputKind::Button, "@dinput", buttons[i], 0});
                }
        } else {
            unsigned version;
            if (!(file >> version) || version != 2)
                throw std::runtime_error("Unsupported input profile version");
            next.assignments.clear();
            for (auto &player : next.players)
                player.bindings.clear();
            std::string tag;
            while (file >> tag) {
                if (tag == "NAME") {
                    std::string device, name;
                    if (!(file >> std::quoted(device) >> std::quoted(name)) || device.size() > 1024 ||
                        name.size() > 1024)
                        throw std::runtime_error("Invalid device name");
                    next.device_names[device] = name;
                } else if (tag == "ASSIGN") {
                    std::string device;
                    uint32_t mask;
                    if (!(file >> std::quoted(device) >> mask) || device.size() > 1024 || mask > 65535)
                        throw std::runtime_error("Invalid controller assignment");
                    next.assignments[device] = mask;
                } else if (tag == "PORT") {
                    unsigned port, device;
                    if (!(file >> port >> device) || port >= input_players)
                        throw std::runtime_error("Invalid player");
                    next.players[port].controller = device;
                } else if (tag == "BIND") {
                    unsigned port, mode;
                    InputBinding b;
                    if (!(file >> port >> b.output.device >> b.output.index >> b.output.id >> mode >>
                          b.sensitivity >> b.deadzone >> b.mouse_sensitivity >> b.strength >> b.threshold >>
                          b.invert >> b.custom) ||
                        port >= input_players || b.output.index > 255 || b.output.id > 65535 ||
                        b.output.device > RETRO_DEVICE_POINTER)
                        throw std::runtime_error("Invalid input binding");
                    b.mode = AnalogMode(mode);
                    validate(b);
                    auto &entry = find_binding(next, port, b.output);
                    b.label = entry.label;
                    entry = b;
                } else if (tag == "SOURCE") {
                    unsigned port, side, kind;
                    InputOutput output;
                    InputSource source;
                    if (!(file >> port >> output.device >> output.index >> output.id >> side >> kind >>
                          std::quoted(source.device) >> source.code >> source.direction) ||
                        port >= input_players || side > 1 || kind > unsigned(InputKind::Hat) ||
                        source.device.size() > 1024 || source.code < 0 || source.code > 65535 ||
                        source.direction < -1 || source.direction > 3)
                        throw std::runtime_error("Invalid input source");
                    source.kind = InputKind(kind);
                    int limit = source.kind == InputKind::Key           ? 255
                                : source.kind == InputKind::Button      ? 127
                                : source.kind == InputKind::Axis        ? 7
                                : source.kind == InputKind::Hat         ? 3
                                : source.kind == InputKind::MouseButton ? 4
                                                                        : 1;
                    if (source.code > limit || (source.kind != InputKind::Hat && source.direction > 1))
                        throw std::runtime_error("Invalid input source control");
                    auto &b = find_binding(next, port, output);
                    auto &sources = side ? b.negative : b.positive;
                    if (sources.size() >= 32)
                        throw std::runtime_error("Too many alternate inputs");
                    sources.push_back(std::move(source));
                } else
                    throw std::runtime_error("Unknown input profile record");
            }
            if (!file.eof())
                throw std::runtime_error("Truncated input profile");
        }
        profile = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception &failure) {
        error = failure.what();
        return false;
    }
}
bool save_input_profile(const fs::path &path, const InputProfile &profile, std::string &error) {
    try {
        std::ostringstream out;
        out << "EINWEGGERAT_INPUT 2\n";
        for (const auto &name : profile.device_names)
            out << "NAME " << std::quoted(name.first) << ' ' << std::quoted(name.second) << '\n';
        for (const auto &assignment : profile.assignments)
            out << "ASSIGN " << std::quoted(assignment.first) << ' ' << assignment.second << '\n';
        for (unsigned port = 0; port < input_players; ++port) {
            out << "PORT " << port << ' ' << profile.players[port].controller << '\n';
            for (const auto &b : profile.players[port].bindings) {
                validate(b);
                out << "BIND " << port << ' ' << b.output.device << ' ' << b.output.index << ' '
                    << b.output.id << ' ' << unsigned(b.mode) << ' ' << b.sensitivity << ' ' << b.deadzone
                    << ' ' << b.mouse_sensitivity << ' ' << b.strength << ' ' << b.threshold << ' '
                    << b.invert << ' ' << b.custom << '\n';
                for (unsigned side = 0; side < 2; ++side)
                    for (const auto &s : side ? b.negative : b.positive)
                        out << "SOURCE " << port << ' ' << b.output.device << ' ' << b.output.index << ' '
                            << b.output.id << ' ' << side << ' ' << unsigned(s.kind) << ' '
                            << std::quoted(s.device) << ' ' << s.code << ' ' << s.direction << '\n';
            }
        }
        fs::create_directories(path.parent_path());
        auto text = out.str();
        write_file(path, text.data(), text.size());
        error.clear();
        return true;
    } catch (const std::exception &failure) {
        error = failure.what();
        return false;
    }
}
