#include "imgui.h"
#include "imgui_internal.h"
#include "input.h"
#include "input_window.h"
#include <cmath>
#include <cstdio>
#include <fstream>
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static InputOutput button(unsigned id) { return {RETRO_DEVICE_JOYPAD, 0, id}; }
static InputOutput axis(unsigned stick, unsigned id) { return {RETRO_DEVICE_ANALOG, stick, id}; }
static std::vector<std::array<unsigned, 3>> key_events;
static void RETRO_CALLCONV key_event(bool down, unsigned key, uint32_t character, uint16_t) {
    key_events.push_back({unsigned(down), key, character});
}
static InputSnapshot neutral() {
    InputSnapshot snapshot;
    for (unsigned i = 0; i < 2; ++i) {
        InputController c;
        c.id = "xinput:" + std::to_string(i);
        c.name = "Xbox controller " + std::to_string(i + 1);
        c.xinput = true;
        snapshot.controllers.push_back(c);
    }
    InputController c;
    c.id = "dinput:test-guid";
    c.name = "Test flight stick";
    snapshot.controllers.push_back(c);
    return snapshot;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    try {
        auto root = fs::absolute(fs::u8path(argv[1]));
        fs::create_directories(root);
        {
            Input input;
            for (const auto &player : input.profile.players)
                for (const auto &binding : player.bindings)
                    require(binding.positive.empty() && binding.negative.empty(),
                            "Fresh input configuration contains preset bindings");
            auto snapshot = neutral();
            snapshot.keys['Z'] = snapshot.keys[VK_UP] = true;
            snapshot.controllers[0].buttons[0] = true;
            snapshot.controllers[0].axes[0] = 30000;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) == 0 &&
                        input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 0,
                    "Fresh configuration responds to unbound keyboard/controller inputs");
            input.binding(0, button(0)).positive = {{InputKind::Key, "keyboard", 'Z', 0}};
            input.binding(0, axis(0, 0)).negative = {{InputKind::Key, "keyboard", 'A', 0}};
            std::string error;
            const auto path = root / L"fresh-bindings.input";
            require(save_input_profile(path, input.profile, error), error.c_str());
            InputProfile restored;
            require(load_input_profile(path, restored, error) &&
                        restored.players[0].bindings[0].positive.size() == 1 &&
                        restored.players[0].bindings[0].positive[0].code == 'Z' &&
                        restored.players[0].bindings[16].negative.size() == 1 &&
                        restored.players[0].bindings[16].negative[0].code == 'A',
                    "Saved input bindings were cleared when loading an existing configuration");
            input.reset_player(0);
            for (const auto &binding : input.profile.players[0].bindings)
                require(binding.positive.empty() && binding.negative.empty(),
                        "Reset player restored preset bindings");
            input.begin_core(root / L"fresh-core.dll", {});
            const retro_input_descriptor descriptors[] = {
                {0, RETRO_DEVICE_JOYPAD, 0, 0, "Fire"},
                {0, RETRO_DEVICE_ANALOG, 0, 0, "Steering"}, {0, 0, 0, 0, nullptr}};
            input.set_descriptors(descriptors);
            require(input.binding(0, button(0)).positive.empty() &&
                        input.binding(0, axis(0, 0)).positive.empty(),
                    "Core descriptors assigned fresh bindings automatically");
        }
        {
            Input input;
            // Configure sources explicitly: fresh profiles no longer supply presets.
            for (unsigned player = 0; player < 2; ++player) {
                input.binding(player, button(0)).positive = {{InputKind::Button, "@xinput", 0, 0}};
                input.binding(player, button(8)).positive = {{InputKind::Button, "@xinput", 8, 0}};
            }
            input.binding(0, button(10)).positive = {{InputKind::Button, "@dinput", 4, 0}};
            input.binding(0, axis(0, 0)).positive = {{InputKind::Axis, "@xinput", 0, 0}};
            auto snapshot = neutral();
            input.assign("xinput:1", 0, true);
            input.assign("dinput:test-guid", 0, true);
            snapshot.controllers[0].buttons[0] = true;
            snapshot.controllers[1].buttons[8] = true;
            snapshot.controllers[2].buttons[4] = true;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) == 1 &&
                        input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1 &&
                        input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) == 1,
                    "Three controllers did not combine on one player");
            require(input.state(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1,
                    "One physical device could not feed two players");
            snapshot.controllers[1].buttons[0] = true;
            snapshot.controllers[0].buttons[0] = false;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) == 1,
                    "Releasing one controller released another controller's held button");
            snapshot.controllers[0].axes[0] = -24000;
            snapshot.controllers[1].axes[0] = 30000;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 30000,
                    "Strongest analog movement did not win");
            auto &b = input.binding(0, axis(0, 0));
            b.sensitivity = 200;
            b.invert = true;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == -32768,
                    "Analog sensitivity/inversion did not clamp");
            input.assign("xinput:1", 0, false);
            snapshot.controllers[0].axes[0] = 1000;
            snapshot.controllers[2].axes[0] = 0;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 0, "Stick deadzone failed");
            snapshot.controllers.clear();
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) == 0,
                    "Disconnect retained controller buttons");
        }
        {
            Input input;
            auto snapshot = neutral();
            auto &b = input.binding(0, axis(0, 0));
            b.mode = AnalogMode::Digital;
            b.strength = 50;
            b.positive = {{InputKind::Key, "keyboard", 'D', 0}};
            b.negative = {{InputKind::Key, "keyboard", 'A', 0}};
            snapshot.keys['D'] = true;
            input.apply_snapshot(snapshot);
            require(std::abs(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) - 16384) <= 1,
                    "Keyboard-to-stick strength failed");
            snapshot.keys['A'] = true;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 0,
                    "Opposite digital stick directions did not cancel");
            b.mode = AnalogMode::Mapped;
            b.positive = {{InputKind::Button, "xinput:0", 0, 0}};
            snapshot.controllers[0].buttons[0] = true;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 32767,
                    "Controller button could not drive analog stick");
            b.mode = AnalogMode::Mouse;
            b.mouse_sensitivity = 500;
            snapshot.mouse_x = 3;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 1500, "Mouse-to-stick sensitivity failed");
            snapshot.mouse_x = 0;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 0, "Mouse stick did not return to center");
            const InputOutput gun_x{RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X};
            const InputOutput gun_y{RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y};
            input.binding(0, gun_x).mode = AnalogMode::Mouse;
            input.binding(0, gun_y).mode = AnalogMode::Mouse;
            input.binding(0, gun_x).mouse_sensitivity = 500;
            input.binding(0, gun_y).mouse_sensitivity = 500;
            snapshot.mouse_x = 3;
            snapshot.mouse_y = -2;
            input.apply_snapshot(snapshot);
            require(input.state(0, gun_x.device, 0, gun_x.id) == 1500 &&
                        input.state(0, gun_y.device, 0, gun_y.id) == -1000,
                    "Mouse axis mappings confused lightgun X and Y");
            snapshot.mouse_x = snapshot.mouse_y = 0;
            input.binding(0, button(3)).positive = {{InputKind::Axis, "xinput:0", 0, -1}};
            snapshot.controllers[0].axes[0] = -20000;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 3) == 1,
                    "Negative stick direction could not drive digital button");
            snapshot.controllers[0].axes[0] = 20000;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 3) == 0,
                    "Stick digital direction ignored its sign");
            input.binding(0, button(10)).positive = {{InputKind::MouseButton, "mouse", 2, 0}};
            snapshot.mouse_buttons = 4;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 10) == 1, "Mouse button mapping failed");
            input.apply_snapshot(snapshot, true, true);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 10) == 0 &&
                        input.state(0, RETRO_DEVICE_ANALOG, 0, 0) == 0,
                    "ImGui capture leaked keyboard/mouse into the game");
        }
        {
            Input input;
            auto snapshot = neutral();
            input.apply_snapshot(snapshot);
            input.begin_capture(0, button(0));
            snapshot.keys['Q'] = true;
            input.apply_snapshot(snapshot, true, true);
            require(!input.capturing() && input.binding(0, button(0)).positive[0].code == 'Q',
                    "Human-readable key capture failed");
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 0) == 0, "Captured key leaked into emulation");
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 0) == 0,
                    "Captured key was not suppressed until release");
            snapshot.keys['Q'] = false;
            input.apply_snapshot(snapshot);
            snapshot.keys['Q'] = true;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 0) == 1,
                    "Captured key did not work after release");
            snapshot.keys['Q'] = false;
            input.apply_snapshot(snapshot);
            input.begin_capture(0, button(0), false, true);
            snapshot.controllers[1].buttons[8] = true;
            input.apply_snapshot(snapshot);
            require(input.binding(0, button(0)).positive.size() == 2 && input.assigned("xinput:1", 0),
                    "Alternate binding capture did not retain keyboard/assign controller");
            snapshot.controllers[1].buttons[8] = false;
            input.apply_snapshot(snapshot);
            input.begin_capture(0, button(0));
            snapshot.keys[VK_ESCAPE] = true;
            input.apply_snapshot(snapshot);
            require(!input.capturing() && input.binding(0, button(0)).positive.size() == 2,
                    "Escape did not cancel capture transactionally");
            snapshot.keys[VK_ESCAPE] = false;
            snapshot.controllers[0].axes[1] = 22000;
            input.apply_snapshot(snapshot);
            input.begin_capture(0, axis(0, 1));
            input.apply_snapshot(snapshot);
            require(input.capturing(), "Held axis was captured without a fresh movement");
            snapshot.controllers[0].axes[1] = 0;
            input.apply_snapshot(snapshot);
            snapshot.controllers[0].axes[1] = -23000;
            input.apply_snapshot(snapshot);
            require(!input.capturing() && input.binding(0, axis(0, 1)).positive[0].kind == InputKind::Axis &&
                        input.binding(0, axis(0, 1)).positive[0].direction == 0,
                    "Full analog-axis capture failed");
        }
        {
            Input input;
            auto snapshot = neutral();
            input.assign("dinput:test-guid", 0, true);
            input.binding(0, button(4)).positive = {{InputKind::Hat, "@dinput", 0, 0}};
            input.binding(0, button(7)).positive = {{InputKind::Hat, "@dinput", 0, 1}};
            snapshot.controllers[2].hats[0] = 4500;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 4) && input.state(0, RETRO_DEVICE_JOYPAD, 0, 7),
                    "Diagonal DirectInput POV failed");
            snapshot.pointer = {12000, -9000, true, true};
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_POINTER, 0, 0) == 12000 &&
                        input.state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) == 1,
                    "Pointer coordinates/press failed");
            snapshot.mouse_buttons = 1;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) == 1,
                    "Mouse lightgun trigger failed");
            snapshot.pointer.inside = false;
            snapshot.touch = {{1, 2, true, true}, {3, 4, true, true}};
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) &&
                        input.state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT) == 2 &&
                        input.state(0, RETRO_DEVICE_POINTER, 1, 0) == 3,
                    "Lightgun offscreen/multitouch indexing failed");
            key_events.clear();
            input.set_keyboard_callback(key_event);
            snapshot.keys['A'] = true;
            snapshot.text = {'a'};
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_a) == 1 && key_events.size() >= 2,
                    "Native keyboard state/text callbacks failed");
            snapshot.focused = false;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) == 0 &&
                        input.state(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_a) == 0,
                    "Focus loss retained input");
            require(input_key_name(VK_RSHIFT) == "Right Shift" && input_key_name('Z') == "Z" &&
                        input.binding_name({}) == "Unassigned",
                    "Input labels expose raw numeric codes");
        }
        {
            Input input;
            input.load_configuration(root);
            input.begin_core(root / L"core.dll", root / L"first.rom");
            input.select_scope(InputScope::Core);
            input.binding(2, axis(0, 0)).mode = AnalogMode::Mouse;
            input.binding(2, axis(0, 0)).mouse_sensitivity = 750;
            input.assign("mouse", 2, true);
            std::string error;
            require(input.save_configuration(error), error.c_str());
            input.select_scope(InputScope::Game);
            input.binding(2, axis(0, 0)).mouse_sensitivity = 1234;
            require(input.save_configuration(error), error.c_str());
            input.end_core();
            input.begin_core(root / L"core.dll", root / L"second.rom");
            require(input.scope() == InputScope::Core &&
                        input.binding(2, axis(0, 0)).mouse_sensitivity == 750,
                    "Game input profile leaked into another game");
            input.end_core();
            input.begin_core(root / L"core.dll", root / L"first.rom");
            require(input.scope() == InputScope::Game &&
                        input.binding(2, axis(0, 0)).mouse_sensitivity == 1234 && input.assigned("mouse", 2),
                    "Per-game profile did not restore bindings/device assignments");
            const retro_input_descriptor descriptions[] = {{0, RETRO_DEVICE_JOYPAD, 0, 0, "Cross"},
                                                           {0, RETRO_DEVICE_ANALOG, 0, 0, "Steering"},
                                                           {0, 0, 0, 0, nullptr}};
            input.set_descriptors(descriptions);
            require(input.binding(0, button(0)).label == "Cross" &&
                        input.binding(0, axis(0, 0)).label == "Steering",
                    "Core control labels were not copied");
            const retro_controller_description types[] = {
                {"Digital controller", RETRO_DEVICE_JOYPAD},
                {"Dual analog controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)}};
            const retro_controller_info ports[] = {{types, 2}, {nullptr, 0}};
            input.set_controller_info(ports);
            require(input.player_count() == 1, "One-player core retained extra player tabs");
            require(input.controller_choices(0).size() == 3 &&
                        input.controller_choices(0)[2].name == "Dual analog controller",
                    "Core controller types were not exposed");
            auto before = input.profile;
            std::ofstream(root / L"invalid.input") << "EINWEGGERAT_INPUT 2\nBIND 0 1 0 0 0 nan 0 1 100 50 0 0\n";
            require(!load_input_profile(root / L"invalid.input", input.profile, error) &&
                        input.binding(0, button(0)).positive.size() ==
                            before.players[0].bindings[0].positive.size(),
                    "Invalid profile changed active inputs");
        }
        {
            Input input;
            require(input.visible_bindings(0).size() == 20,
                    "Global defaults must remain editable without a core");
            input.begin_core(root / L"visibility-core.dll", {});
            require(input.visible_bindings(0).empty(), "Undeclared generic controls were shown for a core");
            const retro_input_descriptor descriptors[] = {
                {0, RETRO_DEVICE_JOYPAD, 0, 0, "Fire"},  {0, RETRO_DEVICE_ANALOG, 0, 0, "Steering"},
                {0, RETRO_DEVICE_JOYPAD, 0, 0, "Fire"},  {0, RETRO_DEVICE_JOYPAD, 0, 8, ""},
                {1, RETRO_DEVICE_JOYPAD, 0, 3, "Start"}, {0, 0, 0, 0, nullptr}};
            input.set_descriptors(descriptors);
            require(input.player_count() == 2, "Descriptor-only core did not expose exactly two players");
            auto visible = input.visible_bindings(0);
            require(visible.size() == 2 && visible[0] == button(0) && visible[1] == axis(0, 0) &&
                        input.visible_bindings(1).size() == 1 && input.visible_bindings(2).empty(),
                    "Bindings were not filtered by core descriptors, player and nonempty labels");
            input.reset_player(0);
            require(input.visible_bindings(0).size() == 2, "Reset revealed generic bindings");
            auto snapshot = neutral();
            input.binding(0, button(8)).positive = {{InputKind::Button, "@xinput", 8, 0}};
            snapshot.controllers[0].buttons[8] = true;
            input.apply_snapshot(snapshot);
            require(input.state(0, RETRO_DEVICE_JOYPAD, 0, 8) == 1,
                    "Hiding a binding changed emulation input state");
            input.set_controller(0, RETRO_DEVICE_NONE);
            require(input.visible_bindings(0).empty(), "Disconnected player still showed controls");
            input.set_controller(0, RETRO_DEVICE_JOYPAD);
            const retro_input_descriptor replacement[] = {{0, RETRO_DEVICE_JOYPAD, 0, 3, "New start"},
                                                          {0, 0, 0, 0, nullptr}};
            input.set_descriptors(replacement);
            require(input.player_count() == 1, "Replaced descriptors retained unsupported players");
            require(input.visible_bindings(0).size() == 1 && input.visible_bindings(0)[0] == button(3) &&
                        input.visible_bindings(1).empty(),
                    "Old core descriptors remained visible after replacement");
            input.set_descriptors(nullptr);
            require(input.visible_bindings(0).empty(), "Cleared descriptor list retained visible bindings");
            input.end_core();
            require(input.visible_bindings(0).size() == 20, "Closing a core did not restore global editing");
        }
        {
            Input input;
            input.begin_core(root / L"type-core.dll", {});
            const unsigned analog = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1);
            const retro_controller_description pad_types[] = {
                {"Digital pad", RETRO_DEVICE_JOYPAD}, {"Analog pad", analog}};
            const retro_controller_description mouse_types[] = {{"Mouse", RETRO_DEVICE_MOUSE}};
            const retro_controller_info ports[] = {{pad_types, 2}, {mouse_types, 1}, {nullptr, 0}};
            const retro_input_descriptor descriptors[] = {
                {0, RETRO_DEVICE_JOYPAD, 0, 0, "Fire"},
                {0, analog, 0, 0, "Analog steering"},
                {0, RETRO_DEVICE_MOUSE, 0, 0, "Mouse X"},
                {1, RETRO_DEVICE_MOUSE, 0, 1, "Aim Y"},
                {0, 0, 0, 0, nullptr}};
            input.set_descriptors(descriptors);
            input.set_controller_info(ports);
            require(input.player_count() == 2 && input.controller_choices(0).size() == 3 &&
                        input.controller_choices(1).size() == 2,
                    "Core types were not offered independently per player");
            require(input.visible_bindings(0).size() == 1 && input.visible_bindings(1).size() == 1,
                    "Bindings from unrelated input types were shown");
            input.begin_capture(0, button(0));
            input.set_controller(0, analog);
            require(!input.capturing() && input.visible_bindings(0).size() == 2 &&
                        input.binding(0, axis(0, 0)).label == "Analog steering" &&
                        input.profile.players[1].controller == RETRO_DEVICE_MOUSE,
                    "Per-player type selection did not refresh its definitions independently");
            input.set_controller(0, RETRO_DEVICE_JOYPAD);
            require(input.visible_bindings(0).size() == 1,
                    "Subtype-specific definitions leaked into another input type");
            const retro_controller_info one_port[] = {{pad_types, 2}, {nullptr, 0}};
            input.set_controller_info(one_port);
            input.set_descriptors(nullptr);
            require(input.player_count() == 1, "Metadata replacement retained stale player ports");
            input.begin_core(root / L"next-core.dll", {});
            require(input.player_count() == 1, "New core inherited the previous player count");
        }
        {
            std::ofstream legacy(root / L"legacy.cfg");
            for (unsigned i = 0; i < 16; ++i)
                legacy << int('A' + i) << ' ';
            for (int i = 0; i < 16; ++i)
                legacy << i << ' ';
            legacy.close();
            InputProfile profile;
            std::string error;
            require(load_input_profile(root / L"legacy.cfg", profile, error), error.c_str());
            bool migrated = false;
            for (const auto &source : profile.players[0].bindings[0].positive)
                if (source.kind == InputKind::Key && source.code == 'A')
                    migrated = true;
            require(migrated, "Original numeric input.cfg keyboard bindings were not migrated");
            profile.device_names["dinput:persistent-guid"] = "My custom controller";
            profile.assignments["dinput:persistent-guid"] = 5;
            require(save_input_profile(root / L"roundtrip.cfg", profile, error), error.c_str());
            InputProfile restored;
            require(load_input_profile(root / L"roundtrip.cfg", restored, error) &&
                        restored.device_names["dinput:persistent-guid"] == "My custom controller" &&
                        restored.assignments["dinput:persistent-guid"] == 5,
                    "Device names or multi-player assignments were not persisted");
            Input input;
            input.profile = restored;
            require(input.source_name({InputKind::Button, "dinput:persistent-guid", 3, 0}) ==
                        "My custom controller - Button 4",
                    "Disconnected controller binding is not human readable");
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1280, 900);
            io.DeltaTime = 1.0f / 60;
            unsigned char *pixels;
            int width, height;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            for (unsigned i = 0; i < 3; ++i) {
                if (i == 1) {
                    input.begin_core(root / L"ui-core.dll", {});
                    const retro_input_descriptor descriptors[] = {
                        {0, RETRO_DEVICE_JOYPAD, 0, 0, "Fire"}, {0, 0, 0, 0, nullptr}};
                    input.set_descriptors(descriptors);
                } else if (i == 2)
                    input.set_descriptors(nullptr);
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(800, 800));
                ImGui::Begin("Native input settings");
                draw_input_settings(input);
                ImGui::End();
                ImGui::Render();
            }
            input.end_core();
            bool open = true;
            ImGui::NewFrame();
            draw_input_window(input, open);
            require(!open && !ImGui::IsPopupOpen("Input Settings"),
                    "Input settings opened without a core");
            ImGui::Render();
            input.begin_core(root / L"ui-core.dll", {});
            open = true;
            for (unsigned i = 0; i < 3; ++i) {
                ImGui::NewFrame();
                draw_input_window(input, open);
                ImGui::Render();
            }
            const auto *dialog = ImGui::FindWindowByName("Input Settings");
            require(open && dialog && dialog->Active && (dialog->Flags & ImGuiWindowFlags_Modal),
                    "Input settings did not open as a modal");
            require(std::abs(dialog->Size.x - io.DisplaySize.x * 0.7f) < 2 &&
                        dialog->Size.y <= io.DisplaySize.y * 0.5f + 1 &&
                        std::abs(dialog->Pos.x + dialog->Size.x * 0.5f - io.DisplaySize.x * 0.5f) < 2 &&
                        std::abs(dialog->Pos.y + dialog->Size.y * 0.5f - io.DisplaySize.y * 0.5f) < 2,
                    "Input settings did not match the centered reference dialog bounds");
            auto frame = [&] {
                ImGui::NewFrame();
                draw_input_window(input, open);
                ImGui::Render();
            };
            auto click_ok = [&] {
                // Auto-sized dialogs settle over multiple frames after status text changes.
                for (unsigned i = 0; i < 3; ++i)
                    frame();
                const auto *content = dialog->DC.ChildWindows[0];
                io.AddMousePosEvent(content->Pos.x + 12,
                                    content->Pos.y + content->Size.y + ImGui::GetStyle().ItemSpacing.y +
                                        ImGui::GetFrameHeight() * 0.5f);
                frame();
                io.AddMouseButtonEvent(0, true);
                frame();
                io.AddMouseButtonEvent(0, false);
                frame();
            };
            // A file in place of the configuration directory makes saving fail reliably.
            std::ofstream(root / L"ui-config-blocker") << "blocked";
            input.load_configuration(root / L"ui-config-blocker");
            input.select_scope(InputScope::Core);
            frame();
            click_ok();
            require(open && input.status.find("Could not save input settings:") == 0,
                    "Save failure closed input settings or did not explain the failure");
            input.load_configuration(root / L"ui-save");
            input.select_scope(InputScope::Core);
            frame();
            click_ok();
            require(!open && fs::exists(input.profile_path(InputScope::Core)),
                    "OK did not save the profile and close input settings on retry");
            open = true;
            for (unsigned i = 0; i < 3; ++i)
                frame();
            input.begin_capture(0, button(0));
            // Closing the title-bar button must release an unfinished binding capture.
            io.AddMousePosEvent(dialog->Pos.x + dialog->Size.x - ImGui::GetFrameHeight() * 0.5f,
                                dialog->Pos.y + ImGui::GetFrameHeight() * 0.5f);
            for (unsigned i = 0; i < 3; ++i) {
                if (i == 1)
                    io.AddMouseButtonEvent(0, true);
                else if (i == 2)
                    io.AddMouseButtonEvent(0, false);
                ImGui::NewFrame();
                draw_input_window(input, open);
                ImGui::Render();
            }
            require(!open && !input.capturing(), "Closing input settings left capture active");
            open = true;
            frame();
            input.end_core();
            frame();
            require(!open, "Unloading the core left input settings open");
            ImGui::DestroyContext();
        }
        std::puts("PASS native input: shared controllers/players, alternate bindings, digital/mouse sticks, "
                  "analog directions, sensitivity/inversion/deadzone, capture/release, profiles, metadata, "
                  "keyboard, pointer, multitouch and lightgun");
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "FAIL: %s\n", failure.what());
        return 1;
    }
}
