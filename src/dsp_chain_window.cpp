#include "dsp_chain_window.h"
#include "imgui.h"
#include "rom_browser.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace
{

    bool text_input(const char *label, std::string &value)
    {
        // A bounded editor matches the preset validator and accommodates long arrays/paths.
        char buffer[4097]{};
        std::memcpy(buffer, value.data(), std::min(value.size(), sizeof(buffer) - 1));
        if (!ImGui::InputText(label, buffer, sizeof(buffer)))
            return false;
        value = buffer;
        return true;
    }

    bool text_input_slider(const char *label,
                           std::string &value,
                           float min_value,
                           float max_value,
                           const char *format = "%.3f")
    {
        char *end = nullptr;
        float slider_value = std::strtof(value.c_str(), &end);

        if (end == value.c_str() || *end != '\0')
            slider_value = min_value;

        slider_value = std::clamp(slider_value, min_value, max_value);

        ImGui::PushID(label);

        const float label_width = ImGui::CalcTextSize(label).x;

        const float slider_label_spacing = 8.0f;
        const float right_padding = 8.0f;

        ImGui::SetNextItemWidth(
            std::max(1.0f,
                     ImGui::GetContentRegionAvail().x -
                         label_width -
                         slider_label_spacing -
                         right_padding));

        bool changed = ImGui::SliderFloat(
            label,
            &slider_value,
            min_value,
            max_value,
            format,
            ImGuiSliderFlags_AlwaysClamp);

        if (changed)
        {
            char buffer[128]{};
            std::snprintf(buffer, sizeof(buffer), format, slider_value);
            value = buffer;
        }

        ImGui::PopID();

        return changed;
    }

    bool echo_tap_sliders(const dsp_parameter &parameter, std::string &value)
    {
        std::istringstream stream(value);
        std::vector<std::string> taps;
        for (std::string tap; stream >> tap;)
            taps.push_back(tap);

        bool changed = false;
        ImGui::PushID(parameter.key);
        for (size_t i = 0; i < taps.size() && i < 64; ++i)
        {
            const std::string label = "Tap " + std::to_string(i + 1) +
                (std::strcmp(parameter.key, "delay") == 0 ? " delay (ms)" : " feedback");
            changed |= text_input_slider(label.c_str(), taps[i], parameter.minimum, parameter.maximum);
        }
        ImGui::PopID();
        if (changed)
        {
            value.clear();
            for (const auto &tap : taps)
            {
                if (!value.empty())
                    value += ' ';
                value += tap;
            }
        }
        return changed;
    }

    bool echo_tap_buttons(dsp_stage &stage)
    {
        auto &delays = stage.parameters["delay"];
        auto &feedback = stage.parameters["feedback"];
        std::istringstream stream(delays);
        size_t count = 0;
        for (std::string tap; stream >> tap;)
            ++count;
        bool changed = false;
        ImGui::BeginDisabled(count >= 64);
        if (ImGui::SmallButton("Add tap"))
        {
            delays += " 200";
            feedback += " 0.5";
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(count <= 1);
        if (ImGui::SmallButton("Remove last tap"))
        {
            for (auto *value : {&delays, &feedback})
            {
                value->erase(value->find_last_not_of(" \t\r\n") + 1);
                const auto separator = value->find_last_of(" \t\r\n");
                if (separator != std::string::npos)
                    value->erase(separator);
            }
            changed = true;
        }
        ImGui::EndDisabled();
        return changed;
    }
}

void dsp_chain_window::draw_editor(editor &edit, dsp_chain &chain,
                                   const std::filesystem::path &path, int source)
{
    if (!edit.initialized)
    {
        edit.draft = chain.settings();
        edit.initialized = true;
    }
    const auto &catalog = dsp_filters();
    ImGui::Text("Input: %.1f Hz. Filters run from top to bottom.", chain.sample_rate());
    ImGui::TextWrapped("Edit the chain, then Apply to hear and save changes. Applying resets effect tails. Arrays use space-separated numbers.");
    edit.dirty |= ImGui::Checkbox("Enable DSP chain", &edit.draft.enabled);
    if (ImGui::Button("Apply chain"))
    {
        if (chain.configure(edit.draft, edit.status, true))
        {
            edit.dirty = false;
            if (dsp_save(path, edit.draft, edit.status))
                edit.status = "Chain applied and saved.";
            else
                edit.status = "Chain applied, but saving failed: " + edit.status;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert edits"))
    {
        edit.draft = chain.settings();
        edit.dirty = false;
        edit.status.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear chain"))
    {
        edit.draft.stages.clear();
        edit.dirty = true;
    }
    if (edit.dirty)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted("Unapplied changes");
    }
    ImGui::BeginDisabled(picker_source != -1);
    if (ImGui::Button("Load preset..."))
    {
        picker_source = source;
        picker_save = false;
        BrowserConfig config;
        config.path = path.parent_path().u8string();
        RomBrowser::Instance()->OpenDialog("DSPPreset", "Load DSP preset", ".dsp", config);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save preset as..."))
    {
        picker_source = source;
        picker_save = true;
        BrowserConfig config;
        config.path = path.parent_path().u8string();
        config.filePathName = "chain.dsp";
        config.flags = BrowserFlags_ConfirmOverwrite;
        RomBrowser::Instance()->OpenDialog("DSPPreset", "Save DSP preset", ".dsp", config);
    }
    ImGui::EndDisabled();
    if (!edit.status.empty())
        ImGui::TextWrapped("%s", edit.status.c_str());
    const auto runtime_error = chain.error();
    if (!runtime_error.empty())
        ImGui::TextWrapped("%s", runtime_error.c_str());
    ImGui::Separator();
    ImGui::SetNextItemWidth(230);
    if (ImGui::BeginCombo("New filter", catalog[edit.add_filter].name))
    {
        for (int i = 0; i < static_cast<int>(catalog.size()); ++i)
            if (ImGui::Selectable(catalog[i].name, i == edit.add_filter))
                edit.add_filter = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(edit.draft.stages.size() >= 32);
    if (ImGui::Button("Add filter"))
    {
        edit.draft.stages.push_back(dsp_default_stage(catalog[edit.add_filter].id));
        edit.dirty = true;
    }
    ImGui::EndDisabled();
    if (edit.draft.stages.empty())
        ImGui::TextUnformatted("No filters. Audio passes through unchanged.");
    ImGui::BeginChild("Stages", ImVec2(0, 0), false);
    int remove = -1, duplicate = -1, move_from = -1, move_to = -1;
    for (size_t i = 0; i < edit.draft.stages.size(); ++i)
    {
        auto &stage = edit.draft.stages[i];
        const auto *filter = dsp_find_filter(stage.filter);
        if (!filter)
            continue;
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::Text("%u. %s", static_cast<unsigned>(i + 1), filter->name);
        edit.dirty |= ImGui::Checkbox("Enabled", &stage.enabled);
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Move up"))
        {
            move_from = static_cast<int>(i);
            move_to = move_from - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == edit.draft.stages.size());
        if (ImGui::SmallButton("Move down"))
        {
            move_from = static_cast<int>(i);
            move_to = move_from + 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(edit.draft.stages.size() >= 32);
        if (ImGui::SmallButton("Duplicate"))
            duplicate = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            remove = static_cast<int>(i);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset to defaults"))
        {
            stage.parameters = dsp_default_stage(stage.filter).parameters;
            edit.dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restore this entry's default parameters. Apply chain to hear and save the change.");
        ImGui::SetNextItemWidth(230);
        if (ImGui::BeginCombo("Effect", filter->name))
        {
            for (const auto &candidate : catalog)
                if (ImGui::Selectable(candidate.name, stage.filter == candidate.id) && stage.filter != candidate.id)
                {
                    stage = dsp_default_stage(candidate.id);
                    filter = &candidate;
                    edit.dirty = true;
                }
            ImGui::EndCombo();
        }
        if (ImGui::TreeNodeEx("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (const auto &p : filter->parameters)
            {
                auto &v = stage.parameters[p.key];
                ImGui::SetNextItemWidth(300);
                if (p.kind == dsp_parameter_kind::choice)
                {
                    if (ImGui::BeginCombo(p.label, v.c_str()))
                    {
                        for (const auto *choice : p.choices)
                            if (ImGui::Selectable(choice, v == choice))
                            {
                                v = choice;
                                edit.dirty = true;
                            }
                        ImGui::EndCombo();
                    }
                }
                else if (stage.filter == "echo" && p.kind == dsp_parameter_kind::array)
                    edit.dirty |= echo_tap_sliders(p, v);
                else if (p.kind == dsp_parameter_kind::array || p.kind == dsp_parameter_kind::text)
                    edit.dirty |= text_input(p.label, v);
                else
                    edit.dirty |= text_input_slider(p.label, v, p.minimum, p.maximum,
                                                    p.kind == dsp_parameter_kind::integer ? "%.0f" : "%.3f");
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("Preset key: %s", p.key);
                    if (p.kind != dsp_parameter_kind::text && p.kind != dsp_parameter_kind::choice)
                        ImGui::Text("Range: %g to %g%s", p.minimum, p.maximum,
                                    p.kind == dsp_parameter_kind::integer ? " (integer)" : "");
                    ImGui::EndTooltip();
                }
            }
            if (stage.filter == "eq")
                ImGui::TextWrapped("Frequency and gain arrays must have the same length. Optional impulse response output is written when you Apply.");
            if (stage.filter == "echo")
                edit.dirty |= echo_tap_buttons(stage);
            if (stage.filter == "wsola")
                ImGui::TextWrapped("Pitch supports fractional semitones.");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove >= 0)
    {
        edit.draft.stages.erase(edit.draft.stages.begin() + remove);
        edit.dirty = true;
    }
    else if (duplicate >= 0)
    {
        auto copy = edit.draft.stages[duplicate];
        edit.draft.stages.insert(edit.draft.stages.begin() + duplicate + 1, std::move(copy));
        edit.dirty = true;
    }
    else if (move_from >= 0)
    {
        std::swap(edit.draft.stages[move_from], edit.draft.stages[move_to]);
        edit.dirty = true;
    }
    ImGui::EndChild();
}

void dsp_chain_window::draw(dsp_chain &core, dsp_chain &midi, const std::filesystem::path &directory)
{
    if (!visible)
        return;
    ImGui::SetNextWindowSize(ImVec2(780, 680), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Audio DSP", &visible))
    {
        if (ImGui::BeginTabBar("Audio source"))
        {
            if (ImGui::BeginTabItem("Core audio"))
            {
                ImGui::PushID(0);
                draw_editor(editors[0], core, directory / "einweggerat.dsp", 0);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("MIDI synthesizer"))
            {
                ImGui::PushID(1);
                draw_editor(editors[1], midi, directory / "einweggerat-midi.dsp", 1);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (picker_source != -1 && RomBrowser::Instance()->Display("DSPPreset"))
    {
        if (RomBrowser::Instance()->IsOk())
        {
            auto &edit = editors[picker_source];
            auto path = std::filesystem::u8path(RomBrowser::Instance()->GetFilePathName());
            if (picker_save)
            {
                if (!path.has_extension())
                    path += ".dsp";
                if (dsp_save(path, edit.draft, edit.status))
                    edit.status = "Preset saved.";
            }
            else if (dsp_load(path, edit.draft, edit.status))
            {
                edit.dirty = true;
                edit.status = "Preset loaded. Apply to hear and save it as the default.";
            }
        }
        RomBrowser::Instance()->Close();
        picker_source = -1;
    }
    if (!visible && picker_source != -1)
    {
        RomBrowser::Instance()->Close();
        picker_source = -1;
    }
}
