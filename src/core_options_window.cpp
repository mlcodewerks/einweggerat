#include "core_options_window.h"
#include "imgui.h"
#include <cctype>
#include <optional>

namespace {
int boolean_token(std::string value) {
    for (auto &c : value)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (value == "true" || value == "on" || value == "yes" || value == "enabled")
        return 1;
    if (value == "false" || value == "off" || value == "no" || value == "disabled")
        return 0;
    return -1;
}
void help(const std::string &text) {
    if (text.empty())
        return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
} // namespace

bool core_option_boolean(const Option &option, size_t &enabled, size_t &disabled) {
    if (option.values.size() != 2)
        return false;
    const int first = boolean_token(option.values[0]), second = boolean_token(option.values[1]);
    if (first < 0 || second < 0 || first == second)
        return false;
    enabled = first ? 0 : 1;
    disabled = first ? 1 : 0;
    return true;
}

void draw_core_options(Core &core, bool &open) {
    if (!open || !core.loaded())
        return;
    ImGui::OpenPopup("Core Settings");
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(display.x * 0.7f, display.y * 0.1f),
                                        ImVec2(display.x * 0.7f, display.y * 0.5f));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // A display-update callback may redefine options, so apply edits after drawing all rows.
    std::optional<std::pair<std::string, std::string>> change;
    if (ImGui::BeginPopupModal("Core Settings", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto row = [&](const Option &option, bool categorized) {
            if (!option.visible || option.values.empty())
                return;
            ImGui::PushID(option.key.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto &label =
                categorized && !option.categorized_label.empty() ? option.categorized_label : option.label;
            const auto &info =
                categorized && !option.categorized_info.empty() ? option.categorized_info : option.info;
            ImGui::TextWrapped("%s", label.c_str());
            help(info);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            size_t yes = 0, no = 0;
            if (core_option_boolean(option, yes, no)) {
                bool value = option.value == option.values[yes];
                if (ImGui::Checkbox("##value", &value))
                    change = {{option.key, option.values[value ? yes : no]}};
            } else {
                auto value_label = [&](size_t index) -> const std::string & {
                    return index < option.value_labels.size() && !option.value_labels[index].empty()
                               ? option.value_labels[index]
                               : option.values[index];
                };
                auto current = std::find(option.values.begin(), option.values.end(), option.value);
                const size_t selected =
                    current == option.values.end() ? 0 : size_t(current - option.values.begin());
                if (ImGui::BeginCombo("##value", value_label(selected).c_str())) {
                    for (size_t i = 0; i < option.values.size(); ++i) {
                        ImGui::PushID(int(i));
                        if (ImGui::Selectable(value_label(i).c_str(), selected == i))
                            change = {{option.key, option.values[i]}};
                        if (selected == i)
                            ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::PopID();
        };
        auto begin_rows = [] {
            const bool begun = ImGui::BeginTable("Options", 2, ImGuiTableFlags_SizingStretchProp);
            if (begun) {
                ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.65f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            }
            return begun;
        };
        bool any = false;
        for (const auto &category : core.option_categories) {
            const bool visible = std::any_of(core.options.begin(), core.options.end(), [&](const Option &o) {
                return o.category == category.key && o.visible && !o.values.empty();
            });
            if (!visible)
                continue;
            any = true;
            ImGui::PushID(category.key.c_str());
            const bool expanded = ImGui::TreeNode(category.label.c_str());
            help(category.info);
            if (expanded) {
                if (begin_rows()) {
                    for (const auto &option : core.options)
                        if (option.category == category.key)
                            row(option, true);
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (begin_rows()) {
            for (const auto &option : core.options) {
                const bool known =
                    std::any_of(core.option_categories.begin(), core.option_categories.end(),
                                [&](const OptionCategory &c) { return c.key == option.category; });
                if (!known && option.visible && !option.values.empty()) {
                    any = true;
                    row(option, false);
                }
            }
            ImGui::EndTable();
        }
        if (!any)
            ImGui::TextUnformatted("There are no core settings for this core.");
        if (ImGui::Button("OK")) {
            open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (change)
        for (auto &option : core.options)
            if (option.key == change->first) {
                core.set_option(option, change->second);
                break;
            }
}
