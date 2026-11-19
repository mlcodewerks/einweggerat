#include "dsp_chain_window.h"
#include "imgui_internal.h"
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

static std::unordered_map<ImGuiID, ImRect> rectangles;
static std::unordered_map<std::string, std::vector<ImRect>> items;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *context, ImGuiID id, const ImRect &bb, const ImGuiLastItemData *) {
    rectangles[id] = bb;
    // Tabs in the bundled ImGui do not emit ItemInfo; use their own inventory.
    if (auto *bar = context->CurrentTabBar)
        for (auto &tab : bar->Tabs) if (tab.ID == id && tab.NameOffset >= 0)
            items[ImGui::TabBarGetTabName(bar, &tab)].push_back(bb);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID id, const char *label, ImGuiItemStatusFlags) {
    if (rectangles.count(id)) items[label].push_back(rectangles.at(id));
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID) { return nullptr; }
static void require(bool v, const char *s) { if (!v) throw std::runtime_error(s); }

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    try {
        auto directory = std::filesystem::u8path(argv[1]);
        std::filesystem::create_directories(directory);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = ImVec2(1600, 1200); io.DeltaTime = 1.f / 60;
        unsigned char *pixels; int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        GImGui->TestEngineHookItems = true;
        dsp_chain core, midi;
        dsp_chain_window window;
        window.visible = true;
        std::string error;
        auto frame = [&] {
            rectangles.clear(); items.clear();
            ImGui::NewFrame(); window.draw(core, midi, directory); ImGui::Render();
        };
        auto click = [&](const char *name, size_t index = 0) {
            frame(); require(items.count(name) && items.at(name).size() > index, name);
            const auto pos = items.at(name)[index].GetCenter();
            io.AddMousePosEvent(pos.x, pos.y); frame();
            io.AddMouseButtonEvent(0, true); frame();
            io.AddMouseButtonEvent(0, false); frame(); frame();
        };
        auto input = [&](const char *label, const char *text) {
            click(label);
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
            io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
            io.AddInputCharactersUTF8(text); frame();
            io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        };
        frame(); frame();
        ImGui::SetWindowSize("Audio DSP", ImVec2(1100, 1080));
        // Every upstream parameter must be reachable in the real window.
        for (const auto &filter : dsp_filters()) {
            require(core.configure({true, {dsp_default_stage(filter.id)}}, error), error.c_str());
            window = dsp_chain_window{}; window.visible = true;
            frame(); frame();
            for (const auto &p : filter.parameters) {
                if (std::string(filter.id) == "echo" && p.kind == dsp_parameter_kind::array) {
                    require(items.count(p.label) == 0, "Echo still has array text fields");
                    const char *label = std::string(p.key) == "delay" ? "Tap 1 delay (ms)" : "Tap 1 feedback";
                    require(items.count(label) != 0, label);
                    continue;
                }
                // This ImGui revision does not emit ItemInfo for BeginCombo.
                if (p.kind != dsp_parameter_kind::choice) require(items.count(p.label) != 0, p.label);
            }
        }
        auto echo = dsp_default_stage("echo");
        echo.parameters["delay"] = "200 400";
        echo.parameters["feedback"] = "0.5 0.25";
        require(core.configure({true, {echo}}, error), error.c_str());
        window = dsp_chain_window{}; window.visible = true; frame(); frame();
        click("Tap 2 delay (ms)");
        click("Tap 2 feedback");
        click("Apply chain");
        const auto echo_parameters = core.settings().stages[0].parameters;
        require(echo_parameters.at("delay") != echo.parameters.at("delay") &&
                echo_parameters.at("delay").find("200 ") == 0,
                "Echo delay slider did not independently update the second tap");
        require(echo_parameters.at("feedback") != echo.parameters.at("feedback") &&
                echo_parameters.at("feedback").find("0.5 ") == 0,
                "Echo feedback slider did not independently update the second tap");
        dsp_settings echo_saved;
        require(dsp_load(directory / "einweggerat.dsp", echo_saved, error) &&
                echo_saved.stages[0].parameters == echo_parameters,
                "Echo slider edits were not persisted");
        click("Add tap"); click("Apply chain");
        require(core.settings().stages[0].parameters.at("delay") == echo_parameters.at("delay") + " 200" &&
                core.settings().stages[0].parameters.at("feedback") == echo_parameters.at("feedback") + " 0.5",
                "Adding an echo tap did not add a matching delay and feedback");
        click("Remove last tap"); click("Apply chain");
        require(core.settings().stages[0].parameters == echo_parameters, "Removing an echo tap failed");
        require(core.configure({true, {dsp_default_stage("panning")}}, error), error.c_str());
        window = dsp_chain_window{}; window.visible = true; frame(); frame();
        input("Left output: L / R gain", "0.25 0");
        click("Apply chain");
        int16_t samples[]{12000, -4000}; std::vector<int16_t> output;
        core.process(samples, 1, output);
        require(output[0] == 3000 && output[1] == -4000, "Parameter edit did not affect actual audio");
        dsp_settings saved;
        require(dsp_load(directory / "einweggerat.dsp", saved, error) && saved.stages[0].parameters["left_mix"] == "0.25 0",
            "Applied UI edit was not persisted");
        input("Left output: L / R gain", "nan"); click("Apply chain");
        core.process(samples, 1, output); require(output[0] == 3000, "Invalid UI edit damaged active audio");
        click("Revert edits");
        click("Duplicate"); click("Apply chain");
        require(core.settings().stages.size() == 2, "Duplicate failed");
        click("Remove", 1); click("Apply chain");
        require(core.settings().stages.size() == 1, "Remove failed");
        click("Add filter"); click("Apply chain"); // Default new filter is Chorus.
        require(core.settings().stages[1].filter == "chorus", "Add failed");
        click("Move up", 1); click("Apply chain");
        require(core.settings().stages[0].filter == "chorus", "Move up failed");
        click("Enabled", 0); click("Apply chain");
        require(!core.settings().stages[0].enabled, "Stage bypass UI failed");
        click("Enable DSP chain"); click("Apply chain");
        core.process(samples, 1, output); require(output[0] == 12000, "Master bypass UI failed");
        click("Clear chain"); click("Apply chain");
        require(core.settings().stages.empty(), "Clear failed");
        click("MIDI synthesizer"); click("Add filter"); click("Apply chain");
        require(midi.settings().stages.size() == 1 && core.settings().stages.empty(), "Audio source chains were not independent");
        click("Load preset...");
        auto *picker = ImGui::FindWindowByName("Load DSP preset##DSPPreset");
        require(picker && picker->Active, "Preset picker did not open");
        window.visible = false; frame();
        ImGui::DestroyContext();
        std::puts("DSP UI: all parameters visible; edits, validation, persistence, duplicate/remove/reorder, bypass, MIDI and picker passed");
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
