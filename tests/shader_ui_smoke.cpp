#include "shader_chain_window.h"
#include "shader_chain.h"
#include "shader_library.h"
#include "shader_source_editor.h"
#include "imgui_internal.h"
#include <unordered_map>
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include <cstring>
#include "native.h"

static std::unordered_map<ImGuiID, ImRect> rectangles;
static std::unordered_map<std::string, std::vector<ImRect>> items;
static std::unordered_map<std::string, ImGuiID> identifiers;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *, ImGuiID id, const ImRect &bb, const ImGuiLastItemData *) { rectangles[id] = bb; }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID id, const char *label, ImGuiItemStatusFlags) {
    identifiers[label] = id;
    if (rectangles.count(id)) items[label].push_back(rectangles.at(id));
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID) { return nullptr; }
static void require(bool value, const char *text) { if (!value) throw std::runtime_error(text); }
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 2;
    try {
        auto &control = video_shaders(); control.initialized = true;
        control.directory = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        std::filesystem::create_directories(control.directory);
        // Model a loaded backend for device-independent widget tests. Actual
        // library loading, compilation and GPU effects have separate pixel tests.
        librashader().gl_available = true;
        shader_chain_config chain;
        require(shader_import(std::filesystem::absolute(std::filesystem::u8path(argv[2])), chain, control.error), control.error.c_str());
        chain.parameters = {{"GAIN", "Gain", 1, 0, 2, 0.1f}}; chain.values["GAIN"] = 1;
        control.settings = {true, {chain}};
        ImGui::CreateContext(); auto &io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1200); io.DeltaTime = 1.f / 60;
        io.Fonts->AddFontDefault(); shader_source_editor_init_font();
        unsigned char *pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        GImGui->TestEngineHookItems = true; shader_window_visible = true;
        auto frame = [&] { rectangles.clear(); items.clear(); ImGui::NewFrame(); draw_shader_chain_window(false); ImGui::Render(); };
        // Optional reproduction mode using real preset metadata and every pass's
        // source editor, without requiring a game core or a graphics device.
        if (argc == 4 && std::strcmp(argv[3], "--preset") == 0) {
            auto &api = librashader(); api.gl_available = false;
            api.load(executable_directory());
            require(api.gl_available || api.vk_available, api.status.c_str());
            const auto preset = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
            control.settings.chains.clear(); control.directory = preset.parent_path();
            auto press = [&](const char *label) {
                frame(); require(items.count(label), label);
                auto point = items.at(label)[0].GetCenter();
                io.AddMousePosEvent(point.x, point.y); frame();
                io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
            };
            frame(); frame(); press("Import preset / shader..."); frame(); frame();
            press("##FileName");
            io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
            io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
            io.AddInputCharactersUTF8(preset.filename().u8string().c_str()); frame();
            io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
            require(control.settings.chains.size() == 1, "Preset file-picker import failed");
            frame(); frame(); frame();
            shader_window_visible = false;
            shader_source_editor editor;
            for (const auto &pass : control.settings.chains[0].passes) {
                std::fprintf(stderr, "Opening %s\n", pass.source.u8string().c_str());
                require(editor.open(pass.source, control.error), control.error.c_str());
                for (int i = 0; i < 3; ++i) { ImGui::NewFrame(); editor.draw(control.error, control.diagnostics); ImGui::Render(); }
            }
            ImGui::DestroyContext();
            std::puts("PASS: real preset import, parameter inspection, pass UI and source editors");
            return 0;
        }
        auto click = [&](const char *label, size_t index = 0, bool ctrl = false) {
            frame(); require(items.count(label) && items.at(label).size() > index, label);
            auto point = items.at(label)[index].GetCenter();
            io.AddMousePosEvent(point.x, point.y); frame();
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame();
        };
        frame(); frame(); ImGui::SetWindowSize("Shader Chains", ImVec2(1100, 1080)); frame();
        auto revision = control.revision;
        click("Gain", 0, true);
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
        io.AddInputCharactersUTF8("0.5"); frame();
        io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        require(control.settings.chains[0].values["GAIN"] == 0.5f && control.revision == revision, "Live shader parameter caused a rebuild or was lost");
        click("Pass enabled"); require(!control.settings.chains[0].passes[0].enabled && control.revision > revision, "Pass bypass failed");
        click("Pass enabled"); click("Duplicate", 1);
        require(control.settings.chains[0].passes.size() == 2, "Pass duplicate failed");
        click("Remove", 2); require(control.settings.chains[0].passes.size() == 1, "Pass remove failed");
        click("Duplicate", 0); require(control.settings.chains.size() == 2, "Chain duplicate failed");
        control.settings.chains[0].name = "First"; control.settings.chains[1].name = "Second";
        click("Down", 0); require(control.settings.chains[0].name == "Second", "Chain reordering failed");
        click("Enabled", 0); require(!control.settings.chains[0].enabled, "Chain bypass failed");
        click("Enable shaders"); require(!control.settings.enabled, "Master bypass failed");
        click("Save default"); shader_settings restored; std::string error;
        require(shader_load(control.directory / "einweggerat.shaders", restored, error), error.c_str());
        require(!restored.enabled && restored.chains.size() == 2 && restored.chains[0].values["GAIN"] == 0.5f, "Shader UI save lost values");
        click("Clear stack"); require(control.settings.chains.empty(), "Clear shader stack failed");
        click("New chain"); require(control.settings.chains.size() == 1, "New shader chain failed");
        click("Import preset / shader...");
        auto *picker = ImGui::FindWindowByName("Import shader preset##ShaderPicker"); require(picker && picker->Active, "Shader picker failed");
        shader_window_visible = false; frame();

        // Exercise the actual pass editor with disposable sources, never the fixture.
        // Reverse filename sorting to check that new tabs follow pass order.
        const auto first = control.directory / "zfirst.slang";
        const auto second = control.directory / "second.slang";
        const std::string original = "// first\r\n\tvoid main() {}\r\n";
        const std::string second_original = "\xef\xbb\xbf// second";
        std::ofstream(first, std::ios::binary) << original;
        std::ofstream(second, std::ios::binary) << second_original;
        control.settings = {true, {shader_chain_config{}}};
        control.settings.chains[0].passes = {{first, true, {}}, {second, false, {}}, {first, true, {}}};
        shader_window_visible = true; frame(); frame();
        revision = control.revision;
        click("Edit preset shaders..."); frame(); frame();
        require(control.revision == revision, "Opening preset sources unexpectedly reloaded shaders");
        auto *source_window = ImGui::FindWindowByName("Shader source");
        require(source_window && source_window->Active, "Source editor did not open");
        const auto first_tab = first.filename().u8string() + "###" + std::filesystem::weakly_canonical(first).u8string();
        const auto second_tab = second.filename().u8string() + "###" + std::filesystem::weakly_canonical(second).u8string();
        auto has_tab = [&](const std::string &label) { return identifiers.count(label) && rectangles.count(identifiers.at(label)); };
        require(has_tab(first_tab) && has_tab(second_tab), "Preset editor did not open all shaders, including disabled passes");
        auto *tab_bar = GImGui->TabBars.GetByKey(source_window->GetID("Shader documents"));
        require(tab_bar && tab_bar->Tabs.Size == 2, "Repeated shader source created duplicate tabs");
        require(tab_bar->SelectedTabId == identifiers.at(first_tab), "Preset editor did not select the first pass");
        require(rectangles.at(identifiers.at(first_tab)).Min.x < rectangles.at(identifiers.at(second_tab)).Min.x,
            "Preset shader tabs did not follow pass order");
        // Keep the source window beside the pass list while opening other passes.
        ImGui::SetWindowPos("Shader source", ImVec2(760, 20));
        auto key = [&](ImGuiKey key, bool ctrl = false) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(key, true); frame(); io.AddKeyEvent(key, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame();
        };
        auto type_at_start = [&](const char *text) {
            frame();
            ImGuiWindow *editor = nullptr;
            for (auto *window : GImGui->Windows)
                if (window->Active && std::strstr(window->Name, "##source")) editor = window;
            require(editor, "Color text editor child missing");
            ImGui::SetWindowFocus(editor->Name); frame();
            key(ImGuiKey_Home, true); io.AddInputCharactersUTF8(text); frame(); frame();
        };
        auto read = [](const std::filesystem::path &path) {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        type_at_start("// edited\n");
        require(read(first) == original, "Typing unexpectedly saved shader source");
        click("Edit source...", 1); frame(); frame();
        require(has_tab(second_tab) && !has_tab(first_tab), "Single-source editor showed another preset shader");
        tab_bar = GImGui->TabBars.GetByKey(source_window->GetID("Shader documents"));
        require(tab_bar && tab_bar->Tabs.Size == 1, "Single-source editor retained unrelated tabs");
        type_at_start("// other edit\n");
        click("Edit source...", 0); frame(); frame();
        require(has_tab(first_tab) && !has_tab(second_tab), "Switching entries retained the previous entry's tab");
        // A different chain must not inherit tabs from the previously edited preset.
        auto full_preset = control.settings.chains[0];
        control.settings.chains[0].passes = {{second, true, {}}};
        click("Edit preset shaders..."); frame(); frame();
        require(has_tab(second_tab) && !has_tab(first_tab), "Preset editor showed shaders from another chain");
        control.settings.chains[0] = std::move(full_preset);
        click("Edit preset shaders..."); frame(); frame();
        require(has_tab(first_tab) && has_tab(second_tab), "Preset editor did not restore its shader tabs");
        // inspect() deliberately has no real backend in this widget test.
        librashader().gl_available = false;
        revision = control.revision;
        click("Save source and reload");
        require(read(first) == "// edited\r\n" + original, "Switching passes lost edits or changed CRLF/tabs");
        require(read(second) == second_original, "Saving a pass overwrote another source");
        require(control.revision > revision, "Saving source did not request shader reload");
        click("Undo"); click("Save source and reload");
        require(read(first) == "// edited" + original, "Editor undo failed");
        click("Redo"); click("Save source and reload");
        require(read(first) == "// edited\r\n" + original, "Editor redo failed");
        shader_window_visible = false; frame();
        require(source_window->Active, "Hiding pass list hid the source editor");
        type_at_start("// shortcut\n");
        key(ImGuiKey_S, true);
        require(read(first) == "// shortcut\r\n// edited\r\n" + original, "Ctrl+S did not save editor contents");
        // A failed write must retain the editable buffer for a later retry.
        type_at_start("// retry\n");
        std::filesystem::remove(first); std::filesystem::create_directory(first);
        revision = control.revision; click("Save source and reload");
        require(control.revision == revision, "Failed source save requested reload");
        std::filesystem::remove(first); click("Save source and reload");
        require(read(first) == "// retry\r\n// shortcut\r\n// edited\r\n" + original, "Failed save lost the editor buffer");
        frame(); auto tab_point = rectangles.at(identifiers.at(second_tab)).GetCenter();
        io.AddMousePosEvent(tab_point.x, tab_point.y); frame();
        io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame(); frame();
        click("Save source and reload");
        require(read(second) == "\xef\xbb\xbf// other edit\n// second", "Second pass buffer, BOM or final newline changed");
        type_at_start("// discard me\n");
        frame(); tab_point = rectangles.at(identifiers.at(second_tab)).GetCenter();
        io.AddMousePosEvent(tab_point.x, tab_point.y); frame();
        io.AddMouseButtonEvent(2, true); frame(); io.AddMouseButtonEvent(2, false); frame();
        require(ImGui::IsPopupOpen("Unsaved shader changes", ImGuiPopupFlags_AnyPopupId), "Dirty tab closed without confirmation");
        click("Cancel"); click("Save source and reload");
        require(read(second) == "\xef\xbb\xbf// discard me\n// other edit\n// second", "Cancel close lost unsaved changes");
        shader_source_editor partial_editor;
        shader_chain_config partial_chain;
        const auto missing = control.directory / "missing-source.slang";
        partial_chain.passes = {{missing, true, {}}, {first, true, {}}, {second, false, {}}};
        require(!partial_editor.open_preset(partial_chain, error) && error.find(missing.u8string()) != std::string::npos,
            "Preset editor did not report the unreadable source path");
        for (int i = 0; i < 3; ++i) {
            rectangles.clear(); items.clear();
            ImGui::NewFrame(); partial_editor.draw(""); ImGui::Render();
        }
        require(has_tab(first_tab) && has_tab(second_tab), "Unreadable preset source prevented other tabs from opening");
        shader_diagnostic_report diagnostics;
        diagnostics.revision = 1;
        auto canonical_first = std::filesystem::weakly_canonical(first);
        diagnostics.sources[canonical_first] = shader_normalize_source(read(first));
        diagnostics.entries.push_back({canonical_first, 2, false, "Unknown function"});
        diagnostics.entries.push_back({canonical_first, 2, false, "Cannot assign expression"});
        TextEditor marked_editor;
        marked_editor.SetInsertSpacesOnTabs(false);
        marked_editor.SetText(diagnostics.sources.at(canonical_first));
        require(shader_editor_diagnostics(marked_editor, first, diagnostics) == 2 &&
            marked_editor.HasMarkers() && marked_editor.HasSquiggles(), "Compiler diagnostics did not create inline markers and squiggles");
        require(shader_editor_diagnostics(marked_editor, second, diagnostics) == 0 &&
            !marked_editor.HasMarkers() && !marked_editor.HasSquiggles(), "Compiler markers leaked to another pass");
        marked_editor.SetText("// changed after compilation\n");
        require(shader_editor_diagnostics(marked_editor, first, diagnostics) == 0 && !marked_editor.HasMarkers(),
            "Stale compiler markers were applied to an edited buffer");
        marked_editor.SetText(diagnostics.sources.at(canonical_first));
        require(shader_editor_diagnostics(marked_editor, first, diagnostics) == 2, "Matching source did not restore diagnostics");
        for (int i = 0; i < 3; ++i) {
            rectangles.clear(); items.clear(); ImGui::NewFrame();
            partial_editor.draw("Compilation failed", diagnostics); ImGui::Render();
        }
        require(items.count("Next error"), "Source editor did not expose diagnostic navigation");
        diagnostics.clear();
        require(shader_editor_diagnostics(marked_editor, first, diagnostics) == 0 &&
            !marked_editor.HasMarkers() && !marked_editor.HasSquiggles(), "Successful compile left inline markers behind");
        require(!partial_editor.open_preset(shader_chain_config{}, error) && !error.empty(), "Empty preset did not report an error");
        // New presets remain drafts until the named source and preset are saved.
        librashader().gl_available = true;
        control.settings.chains.clear(); shader_window_visible = true;
        ImGui::SetWindowFocus("Shader Chains"); frame(); frame();
        click("New preset"); frame(); frame();
        auto *draft_window = ImGui::FindWindowByName("New shader preset");
        require(draft_window && draft_window->Active && control.settings.chains.empty(), "New preset did not open an unsaved draft");
        const auto preset_name = "UI preset " + std::to_string(native_nanoseconds());
        auto rename_draft = [&](const std::string &name) {
            click("Preset name"); key(ImGuiKey_A, true);
            io.AddInputCharactersUTF8(name.c_str()); frame();
            key(ImGuiKey_Enter);
        };
        rename_draft(preset_name);
        ImGuiWindow *draft_source = nullptr;
        for (auto *window : GImGui->Windows)
            if (window->Active && std::strstr(window->Name, "##new-preset-source")) draft_source = window;
        require(draft_source, "New preset source editor missing");
        ImGui::SetWindowFocus(draft_source->Name); frame();
        key(ImGuiKey_Home, true); io.AddInputCharactersUTF8("// draft edit\n"); frame(); frame();
        require(!std::filesystem::exists(control.directory / "shaders" / (preset_name + ".slang")), "Draft was saved before adding");
        rename_draft("../invalid"); click("Save preset and add to chain");
        require(control.settings.chains.empty() && draft_window->Active, "Invalid preset name added a chain or lost the draft");
        rename_draft(preset_name);
        // The widget test has no backend function pointers installed.
        librashader().gl_available = false;
        revision = control.revision; click("Save preset and add to chain"); frame(); frame();
        require(control.settings.chains.size() == 1 && control.revision > revision, "Saved preset was not added and reloaded");
        const auto created = control.settings.chains.front();
        require(created.name == preset_name && created.passes.size() == 1, "New preset name or pass was lost");
        require(read(created.passes[0].source).find("// draft edit\n#version 450") == 0, "New preset lost source edits");
        shader_chain_config reimported;
        require(shader_import(control.directory / "shaders" / (preset_name + ".slangp"), reimported, error) &&
            reimported.passes[0].source == created.passes[0].source, "New preset could not be imported again");
        require(ImGui::FindWindowByName("Shader source")->Active, "Saved preset did not open for continued editing");
        require(!shader_create_preset(control.directory / "shaders", preset_name, "overwrite", reimported, error) &&
            read(created.passes[0].source).find("// draft edit\n") == 0, "Duplicate preset overwrote existing source");
        // A failed save retains the named draft for retry.
        librashader().gl_available = true;
        ImGui::SetWindowFocus("Shader Chains"); frame(); click("New preset"); frame(); frame();
        rename_draft(preset_name); click("Save preset and add to chain");
        require(control.settings.chains.size() == 1 && draft_window->Active, "Duplicate draft was added or discarded");
        rename_draft(preset_name + " retry");
        librashader().gl_available = false; key(ImGuiKey_S, true);
        require(control.settings.chains.size() == 2 && control.settings.chains.back().name == preset_name + " retry",
            "Renaming and saving a failed draft with Ctrl+S failed");
        const auto failed_name = preset_name + " disk retry";
        const auto failed_preset = control.directory / "shaders" / (failed_name + ".slangp");
        auto blocked_temp = failed_preset; blocked_temp += ".tmp";
        std::filesystem::create_directory(blocked_temp);
        reimported = created;
        require(!shader_create_preset(control.directory / "shaders", failed_name, "// keep draft", reimported, error) &&
            reimported.name == created.name && !std::filesystem::exists(failed_preset) &&
            !std::filesystem::exists(control.directory / "shaders" / (failed_name + ".slang")),
            "Failed preset save left a partial source or changed the chain");
        std::filesystem::remove(blocked_temp);
        require(shader_create_preset(control.directory / "shaders", failed_name, "// keep draft", reimported, error),
            "Could not retry after a preset write failure");
        ImGui::DestroyContext();
        std::puts("PASS: new preset drafts, source editing, naming, save/add, reimport, collision protection and retry");
        std::puts("PASS: shader UI live parameters, pass and chain editing, bypass, persistence and file picker");
        std::puts("PASS: per-pass color editor, independent buffers, source save/reload, CRLF/tabs, undo/redo, Ctrl+S and failed save retry");
        std::puts("PASS: scoped entry/preset tabs, pass order, disabled/shared sources, preserved edits and unreadable sources");
        std::puts("PASS: inline compiler markers, squiggles, source isolation, stale diagnostics and navigation");
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
