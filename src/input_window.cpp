#include "input_window.h"
#include "imgui.h"

namespace {
void source_editor(Input &input, unsigned player, InputOutput output, bool negative, bool field = true) {
    auto &binding = input.binding(player, output);
    auto &sources = negative ? binding.negative : binding.positive;
    ImGui::PushID(negative ? 1 : 0);
    const bool capturing = input.capturing(player, output, negative);
    const auto description = input.binding_name(sources);
    const char *prompt = "Press a key, mouse button or controller input...";
    bool activate = false;
    if (field) {
        const std::string text = capturing ? prompt : description;
        std::vector<char> buffer(text.begin(), text.end());
        buffer.push_back(0);
        ImGui::InputText("##joykey", buffer.data(), buffer.size(), ImGuiInputTextFlags_ReadOnly);
        activate = ImGui::IsItemActivated() || ImGui::IsItemClicked();
    } else {
        activate = ImGui::Button(capturing ? prompt : description.c_str());
    }
    if (activate)
        input.begin_capture(player, output, negative, false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\nClick to bind an input. Right-click for more options.", description.c_str());
    if (ImGui::BeginPopupContextItem("Binding options")) {
        if (ImGui::Selectable("Add input"))
            input.begin_capture(player, output, negative, true);
        if (ImGui::Selectable("Clear")) {
            input.cancel_capture();
            sources.clear();
            binding.custom = true;
        }
        if (ImGui::BeginMenu("Choose key")) {
            ImGui::BeginChild("Key list", ImVec2(230, 260));
            for (unsigned key = 1; key < 256; ++key) {
                const auto name = input_key_name(key);
                if (name == "Unknown key")
                    continue;
                ImGui::PushID(int(key));
                if (ImGui::Selectable(name.c_str())) {
                    input.cancel_capture();
                    sources.push_back({InputKind::Key, "keyboard", int(key), 0});
                    binding.custom = true;
                    input.assign("keyboard", player, true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndMenu();
        }
        if (input_axis(output)) {
            ImGui::Separator();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            ImGui::SliderFloat("Deadzone", &binding.deadzone, 0, 95, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::Checkbox(input_vertical_axis(output) ? "Invert Y axis" : "Invert X axis", &binding.invert);
        } else if (std::any_of(sources.begin(), sources.end(),
                               [](const InputSource &source) { return source.kind == InputKind::Axis; })) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            ImGui::SliderFloat("Axis activation", &binding.threshold, 1, 100, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::EndPopup();
    }
    if (!field) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            input.cancel_capture();
            sources.clear();
            binding.custom = true;
        }
    }
    ImGui::PopID();
}
void binding_row(Input &input, unsigned player, InputOutput output) {
    // Reserve the rightmost fifth for the binding field, as in wtweg.
    const float start = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = available * 0.2f;
    ImGui::PushTextWrapPos(start + available - width - ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextWrapped("%s", input.binding(player, output).label.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine(start + available - width);
    ImGui::SetNextItemWidth(width);
    source_editor(input, player, output, false);
}
} // namespace
void draw_input_settings(Input &input) {
    if (!input.has_core())
        return;
    static int active_player = -1;
    if (!ImGui::BeginTabBar("Input players", ImGuiTabBarFlags_FittingPolicyScroll))
        return;
    for (unsigned player = 0; player < input.player_count(); ++player) {
        const auto name = "Player " + std::to_string(player + 1);
        if (!ImGui::BeginTabItem(name.c_str()))
            continue;
        if (active_player != int(player)) {
            input.cancel_capture();
            active_player = int(player);
        }
        ImGui::PushID(int(player));
        const float control_width = ImGui::GetContentRegionAvail().x * 0.3f;
        ImGui::PushItemWidth(control_width);
        const char *scope_names[] = {"Global defaults", "Core", "This game"};
        if (ImGui::BeginCombo("Bindings", scope_names[unsigned(input.scope())])) {
            for (unsigned i = 1; i < 3; ++i) {
                bool available = i == 0 || (i == 1 ? input.has_core() : input.has_game());
                if (ImGui::Selectable(scope_names[i], unsigned(input.scope()) == i,
                                      available ? 0 : ImGuiSelectableFlags_Disabled))
                    input.select_scope(InputScope(i));
            }
            ImGui::EndCombo();
        }
        if (input.scope() == InputScope::Game)
            ImGui::TextWrapped("These bindings apply only to this game.");
        else if (input.scope() == InputScope::Core)
            ImGui::TextWrapped("These core bindings are shared by games using this core. Selecting \"This game\" "
                               "starts with a copy when no game profile exists.");
        else
            ImGui::TextWrapped("These defaults apply when no core or game input profile is saved.");
        if (!input.has_game())
            ImGui::TextDisabled("Load a game to use game bindings.");
        auto &selected = input.profile.players[player].controller;
        const auto &choices = input.controller_choices(player);
        std::string selected_name = "Disconnected";
        for (const auto &choice : choices)
            if (choice.id == selected)
                selected_name = choice.name;
        if (ImGui::BeginCombo("Input type", selected_name.c_str())) {
            for (const auto &choice : choices)
                if (ImGui::Selectable(choice.name.c_str(), choice.id == selected))
                    input.set_controller(player, choice.id);
            ImGui::EndCombo();
        }
        // Copy keys: editing stick modes can add backing bindings to a sparse profile.
        const auto outputs = input.visible_bindings(player);
        if (outputs.empty())
            ImGui::TextWrapped("The core exposes no bindings for this player and input type.");
        for (const auto &output : outputs) {
            auto &binding = input.binding(player, output);
            ImGui::PushID(int(output.device));
            ImGui::PushID(int(output.index));
            ImGui::PushID(int(output.id));
            const bool stick = output.device == RETRO_DEVICE_ANALOG && output.index < 2 && output.id < 2;
            if (stick) {
                const float row_start = ImGui::GetCursorPosX();
                const float row_width = ImGui::GetContentRegionAvail().x;
                const float bind_width = row_width * 0.2f;
                ImGui::TextUnformatted(binding.label.c_str());
                int mode = int(binding.mode);
                if (ImGui::Combo("##Source", &mode,
                                 "Controller / key / mouse button\0Mouse\0Digital directions\0")) {
                    input.cancel_capture();
                    binding.mode = AnalogMode(mode);
                    binding.custom = true;
                    if (binding.mode == AnalogMode::Digital) {
                        const auto negative = input.binding(player, {RETRO_DEVICE_JOYPAD, 0,
                            input_vertical_axis(output) ? 4u : 6u}).positive;
                        const auto positive = input.binding(player, {RETRO_DEVICE_JOYPAD, 0,
                            input_vertical_axis(output) ? 5u : 7u}).positive;
                        auto &updated = input.binding(player, output);
                        updated.negative = negative;
                        updated.positive = positive;
                    }
                }
                auto &axis = input.binding(player, output);
                if (axis.mode == AnalogMode::Mapped) {
                    ImGui::SameLine(row_start + row_width - bind_width);
                    ImGui::SetNextItemWidth(bind_width);
                    source_editor(input, player, output, false);
                }
                if (input_vertical_axis(output))
                    ImGui::Checkbox("Invert Y axis", &axis.invert);
                if (axis.mode == AnalogMode::Mouse) {
                    ImGui::SliderFloat("Mouse sensitivity", &axis.mouse_sensitivity, 1, 32767, "%.0f",
                                       ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
                    ImGui::TextWrapped("Mouse %s motion drives this axis and returns to center when stopped. "
                                       "Sensitivity is analog units per mouse pixel.",
                                       input_vertical_axis(output) ? "Y" : "X");
                } else if (axis.mode == AnalogMode::Digital) {
                    ImGui::SliderFloat("Emulated strength", &axis.strength, 0, 100, "%.0f%%",
                                       ImGuiSliderFlags_AlwaysClamp);
                    for (int direction = 0; direction < 2; ++direction) {
                        const bool negative = direction == 0;
                        ImGui::TextUnformatted(input_vertical_axis(output) ? (negative ? "Up" : "Down")
                                                                          : (negative ? "Left" : "Right"));
                        ImGui::SameLine();
                        source_editor(input, player, output, negative, false);
                    }
                } else {
                    ImGui::SliderFloat("Analog sensitivity", &axis.sensitivity, 0, 400, "%.0f%%",
                                       ImGuiSliderFlags_AlwaysClamp);
                    ImGui::TextWrapped("100%% uses the original input strength. Lower sensitivity reduces "
                                       "the input strength.");
                }
            } else {
                binding_row(input, player, output);
                if (input_axis(output)) {
                    ImGui::SliderFloat("Analog sensitivity", &input.binding(player, output).sensitivity,
                                       0, 400, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::TextWrapped("100%% uses the original input strength. Lower sensitivity reduces "
                                       "the input strength.");
                }
            }
            ImGui::PopID();
            ImGui::PopID();
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Advanced")) {
            if (ImGui::CollapsingHeader("Devices assigned to this player", ImGuiTreeNodeFlags_None)) {
                ImGui::TextWrapped("Select any number of controllers. Their buttons combine; the strongest stick "
                                   "movement wins. A device may also feed more than one player.");
                for (const auto &device : input.device_list()) {
                    ImGui::PushID(device.id.c_str());
                    bool enabled = input.assigned(device.id, player);
                    auto label = device.name + (device.connected ? "" : " (disconnected)");
                    if (ImGui::Checkbox(label.c_str(), &enabled))
                        input.assign(device.id, player, enabled);
                    ImGui::PopID();
                }
                if (ImGui::Button("Rescan controllers"))
                    input.rescan();
            }
            if (ImGui::Button("Reset this player's bindings"))
                input.reset_player(player);
        }
        ImGui::PopItemWidth();
        ImGui::PopID();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void draw_input_window(Input &input, bool &open) {
    if (!input.has_core()) {
        if (ImGui::BeginPopupModal("Input Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (open)
            input.cancel_capture();
        open = false;
        return;
    }
    if (!open)
        return;
    if (!ImGui::IsPopupOpen("Input Settings") && input.scope() == InputScope::Global)
        input.select_scope(InputScope::Core);
    ImGui::OpenPopup("Input Settings");
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(display.x * 0.7f, display.y * 0.3f),
                                        ImVec2(display.x * 0.7f, display.y * 0.5f));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                           ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Input Settings", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        const float content_height = std::max(ImGui::GetFrameHeight(),
            display.y * 0.5f - ImGui::GetFrameHeightWithSpacing() * 4);
        ImGui::BeginChild("Input content", ImVec2(0, content_height));
        draw_input_settings(input);
        ImGui::EndChild();
        if (ImGui::Button("OK")) {
            input.cancel_capture();
            std::string error;
            if (input.save_configuration(error)) {
                input.status = "Input profile saved.";
                open = false;
                ImGui::CloseCurrentPopup();
            } else
                input.status = "Could not save input settings: " + error + ". Press OK to retry.";
        }
        if (input.capturing()) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel capture"))
                input.cancel_capture();
        }
        if (input.status.find("Could not save input settings:") == 0)
            ImGui::TextWrapped("%s", input.status.c_str());
        ImGui::EndPopup();
    }
    if (!open)
        input.cancel_capture();
}
