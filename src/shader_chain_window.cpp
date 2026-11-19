#include "shader_chain_window.h"
#include "shader_chain.h"
#include "shader_library.h"
#include "shader_source_editor.h"
#include "imgui.h"
#include "rom_browser.h"
#include <algorithm>
#include <cmath>
#include <cstring>

bool shader_window_visible = false;
namespace
{
    struct preset_draft
    {
        TextEditor editor;
        char name[256] = "New preset";
        std::string error;
        bool visible = false, initialized = false, focus = false;

        void open()
        {
            if (!initialized)
            {
                editor.SetLanguage(TextEditor::LookupLanguage("GLSL"));
                editor.SetTabSize(4);
                editor.SetInsertSpacesOnTabs(false);
                editor.SetShowWhitespacesEnabled(false);
                editor.SetText(R"(#version 450
// Edit the fragment stage to create your effect.
layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;                    // Vertex transform.
    // Size vectors contain (width, height, 1/width, 1/height).
    vec4 OriginalSize;           // Input to this preset's first pass.
    vec4 SourceSize;             // Input to the current pass.
    vec4 OutputSize;             // Current pass render target.
    vec4 FinalViewportSize;      // Final output viewport.
    uint FrameCount;             // Frame index, subject to frame_count_mod.
    int FrameDirection;          // 1 forward, -1 when rewinding.
    uint Rotation;               // Quarter turns: 0, 1, 2, or 3.
    uint TotalSubFrames;         // Subframes per frame (normally 1).
    uint CurrentSubFrame;        // Current subframe, starting at 1.
    float OriginalAspect;        // Intended source width / height.
    float OriginalAspectRotated; // Intended aspect after rotation.
    float OriginalFPS;           // Source frames per second.
    uint FrameTimeDelta;         // Milliseconds since the last frame in librashader.
} global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() {
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}
#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() {
    FragColor = texture(Source, vTexCoord);
}
)");
                std::strcpy(name, "New preset");
                error.clear();
                initialized = true;
            }
            visible = focus = true;
        }

        void draw(shader_controller &control, shader_source_editor &sources, std::string &status)
        {
            if (!visible)
                return;
            ImGui::SetNextWindowSize(ImVec2(950, 750), ImGuiCond_FirstUseEver);
            if (focus)
                ImGui::SetNextWindowFocus();
            if (ImGui::Begin("New shader preset", &visible))
            {
                ImGui::TextWrapped("Name and edit your shader, then save it to add it to the chain list. Closing this window keeps your draft; New preset reopens it.");
                if (focus)
                    ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Preset name", name, sizeof(name));
                auto directory = control.directory / "shaders";
                ImGui::TextWrapped("Save folder: %s", directory.u8string().c_str());
                bool shortcut = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
                ImGui::BeginDisabled(control.settings.chains.size() >= 16);
                bool save = ImGui::Button("Save preset and add to chain") || shortcut;
                ImGui::EndDisabled();
                if (save && control.settings.chains.size() < 16)
                {
                    shader_chain_config chain;
                    if (shader_create_preset(directory, name,
                                             editor.GetSectionText({}, {editor.GetLineCount(), 0}), chain, error))
                    {
                        // Keep even an invalid draft editable; runtime compilation retains
                        // the previous working stack and supplies source diagnostics.
                        control.changed();
                        control.inspect(chain);
                        control.settings.chains.push_back(chain);
                        sources.open_preset(chain, status);
                        status = "Shader preset saved and added. Save default to restore this stack on startup.";
                        visible = initialized = false;
                    }
                }
                if (control.settings.chains.size() >= 16)
                    ImGui::TextWrapped("The stack is full (16 chains). Remove a chain before adding this preset.");
                if (!error.empty())
                    ImGui::TextWrapped("%s", error.c_str());
                ImGui::TextUnformatted("GLSL / Slang    Ctrl+S: save and add    Ctrl+F: find    Ctrl+H: replace");
                ImGui::SetNextWindowBgAlpha(0.0f);
                editor.Render("##new-preset-source", ImVec2(-1, -1));
                focus = false;
            }
            ImGui::End();
        }
    };

    bool input(const char *label, std::string &value)
    {
        char buffer[4096]{};
        std::memcpy(buffer, value.data(), std::min(value.size(), sizeof(buffer) - 1));
        if (!ImGui::InputText(label, buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue))
            return false;
        value = buffer;
        return true;
    }
    bool choice(const char *label, std::string &value, const std::vector<const char *> &choices)
    {
        bool changed = false;
        if (ImGui::BeginCombo(label, value.empty() ? "Preset default" : value.c_str()))
        {
            if (ImGui::Selectable("Preset default", value.empty()))
            {
                value.clear();
                changed = true;
            }
            for (auto text : choices)
                if (ImGui::Selectable(text, value == text))
                {
                    value = text;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }
    template <class T>
    bool order(std::vector<T> &items, int i, int &remove, int &duplicate, int &from, int &to, size_t limit)
    {
        bool changed = false;
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Up"))
        {
            from = i;
            to = i - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == static_cast<int>(items.size()));
        if (ImGui::SmallButton("Down"))
        {
            from = i;
            to = i + 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(items.size() >= limit);
        if (ImGui::SmallButton("Duplicate"))
            duplicate = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            remove = i;
        return changed;
    }
    template <class T>
    bool reorder(std::vector<T> &items, int remove, int duplicate, int from, int to)
    {
        if (remove >= 0)
            items.erase(items.begin() + remove);
        else if (duplicate >= 0)
        {
            auto copy = items[duplicate];
            items.insert(items.begin() + duplicate + 1, std::move(copy));
        }
        else if (from >= 0)
            std::swap(items[from], items[to]);
        else
            return false;
        return true;
    }
}
void draw_shader_chain_window(bool vulkan)
{
    auto &control = video_shaders();
    control.init();
    auto &api = librashader();
    static int picker = 0, picker_chain = -1, picker_pass = -1;
    static std::string status;
    static std::map<std::filesystem::path, std::vector<std::string>> parameter_ids;
    static shader_source_editor source_editor;
    static preset_draft draft;
    draft.draw(control, source_editor, status);
    auto reload_sources = [&]
    {
        parameter_ids.clear();
        // Keep the first failure and its diagnostics; a later successful
        // inspection would otherwise overwrite the compiler error message.
        for (auto &chain : control.settings.chains)
            if (!chain.passes.empty() && !control.inspect(chain))
                break;
    };
    if (!shader_window_visible)
    {
        if (picker)
        {
            RomBrowser::Instance()->Close();
            picker = 0;
        }
        if (source_editor.draw(control.error, control.diagnostics))
        {
            control.changed();
            reload_sources();
        }
        return;
    }
    auto open = [&](int mode, const char *title, const char *filters, int chain = -1, int pass = -1)
    {
        picker = mode;
        picker_chain = chain;
        picker_pass = pass;
        BrowserConfig config;
        config.path = control.directory.u8string();
        config.flags = mode == 2 || mode == 4 ? BrowserFlags_ConfirmOverwrite : BrowserFlags_None;
        RomBrowser::Instance()->OpenDialog("ShaderPicker", title, filters, config);
    };
    bool dirty = false, source_saved = false;
    ImGui::SetNextWindowSize(ImVec2(760, 720), ImGuiCond_FirstUseEver);
    bool expanded = ImGui::Begin("Shader Chains", &shader_window_visible);
    if (expanded)
    {
        ImGui::TextWrapped("%s", api.status.c_str());
        if (!(vulkan ? api.vk_available : api.gl_available))
            ImGui::TextWrapped("Shader processing is unavailable for the current renderer.");
        ImGui::TextWrapped("Chains and passes run from top to bottom. Parameters update immediately; pass edits compile a replacement. Failed compilation keeps the previous working stack. Save default to restore on startup.");
        ImGui::BeginDisabled(picker != 0 || (!api.gl_available && !api.vk_available));
        ImGui::Checkbox("Enable shaders", &control.settings.enabled);
        if (ImGui::Button("Import preset / shader..."))
            open(1, "Import shader preset", ".slangp,.slang");
        ImGui::SameLine();
        ImGui::BeginDisabled(control.settings.chains.size() >= 16);
        if (ImGui::Button("New preset"))
            draft.open();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("New chain") && control.settings.chains.size() < 16)
        {
            shader_chain_config chain;
            chain.name = "New chain";
            control.settings.chains.push_back(chain);
            dirty = true;
        }
        if (ImGui::Button("Load stack..."))
            open(3, "Load shader stack", ".shaders");
        ImGui::SameLine();
        if (ImGui::Button("Save stack as..."))
            open(4, "Save shader stack", ".shaders");
        ImGui::SameLine();
        if (ImGui::Button("Save default"))
        {
            if (shader_save(control.directory / "einweggerat.shaders", control.settings, status))
                status = "Default shader stack saved.";
        }
        if (ImGui::Button("Reload shaders"))
        {
            dirty = true;
            source_saved = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear stack"))
        {
            control.settings.chains.clear();
            dirty = true;
        }
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        if (!control.error.empty())
            ImGui::TextWrapped("%s", control.error.c_str());
        int remove = -1, duplicate = -1, from = -1, to = -1;
        for (int c = 0; c < static_cast<int>(control.settings.chains.size()); ++c)
        {
            auto &chain = control.settings.chains[c];
            ImGui::PushID(c);
            ImGui::Separator();
            dirty |= ImGui::Checkbox("Enabled", &chain.enabled);
            ImGui::SameLine();
            ImGui::Text("%d. %s", c + 1, chain.name.c_str());
            order(control.settings.chains, c, remove, duplicate, from, to, 16);
            input("Chain name (Enter)", chain.name);
            if (ImGui::Button("Add pass..."))
                open(5, "Add shader pass", ".slang", c);
            ImGui::SameLine();
            if (ImGui::Button("Export preset..."))
                open(2, "Export enabled passes", ".slangp", c);
            ImGui::SameLine();
            ImGui::BeginDisabled(chain.passes.empty());
            if (ImGui::Button("Edit preset shaders..."))
                source_editor.open_preset(chain, status);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open every shader source in this chain in an editor tab, including disabled passes.");
            int pr = -1, pd = -1, pf = -1, pt = -1;
            for (int p = 0; p < static_cast<int>(chain.passes.size()); ++p)
            {
                auto &pass = chain.passes[p];
                ImGui::PushID(p);
                const std::string title = std::to_string(p + 1) + ". " + pass.source.filename().u8string();
                if (ImGui::TreeNodeEx("pass", ImGuiTreeNodeFlags_DefaultOpen, "%s", title.c_str()))
                {
                    dirty |= ImGui::Checkbox("Pass enabled", &pass.enabled);
                    order(chain.passes, p, pr, pd, pf, pt, 64);
                    ImGui::TextWrapped("%s", pass.source.u8string().c_str());
                    if (ImGui::SmallButton("Replace shader..."))
                        open(6, "Replace shader pass", ".slang", c, p);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Edit source..."))
                    {
                        source_editor.open(pass.source, status);
                    }
                    if (ImGui::TreeNode("Pass options"))
                    {
                        dirty |= choice("Sampling", pass.options["filter_linear"], {"true", "false"});
                        dirty |= choice("Wrap", pass.options["wrap_mode"], {"clamp_to_border", "clamp_to_edge", "repeat", "mirrored_repeat"});
                        dirty |= choice("Mipmaps", pass.options["mipmap_input"], {"true", "false"});
                        dirty |= choice("Float framebuffer", pass.options["float_framebuffer"], {"true", "false"});
                        dirty |= choice("sRGB framebuffer", pass.options["srgb_framebuffer"], {"true", "false"});
                        for (auto key : {"scale_type", "scale_type_x", "scale_type_y"})
                            dirty |= choice(key, pass.options[key], {"source", "viewport", "absolute"});
                        for (auto key : {"scale", "scale_x", "scale_y", "alias", "frame_count_mod"})
                            dirty |= input(key, pass.options[key]);
                        ImGui::TextWrapped("Press Enter to commit text. Empty options use the preset default.");
                        ImGui::TreePop();
                    }
                    auto found = parameter_ids.find(pass.source);
                    if (found == parameter_ids.end())
                        found = parameter_ids.emplace(pass.source, shader_pass_parameters(pass.source)).first;
                    for (const auto &param : chain.parameters)
                    {
                        if (std::find(found->second.begin(), found->second.end(), param.id) == found->second.end())
                            continue;
                        ImGui::PushID(param.id.c_str());
                        float &value = chain.values[param.id];
                        ImGui::SetNextItemWidth(330);
                        if (ImGui::SliderFloat(param.label.c_str(), &value, param.minimum, param.maximum, "%.4f", ImGuiSliderFlags_AlwaysClamp))
                        {
                            if (param.step > 0)
                                value = std::clamp(param.minimum + std::round((value - param.minimum) / param.step) * param.step, param.minimum, param.maximum);
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s; step %g. Matching parameter IDs are shared within this chain.", param.id.c_str(), param.step);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset"))
                            value = param.initial;
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            dirty |= reorder(chain.passes, pr, pd, pf, pt);
            // The runtime inventory includes parameters from preprocessed includes as well.
            if (ImGui::TreeNode("All chain parameters"))
            {
                for (const auto &param : chain.parameters)
                {
                    ImGui::PushID(param.id.c_str());
                    ImGui::SliderFloat(param.label.c_str(), &chain.values[param.id], param.minimum, param.maximum, "%.4f", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset"))
                        chain.values[param.id] = param.initial;
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Preset textures and other options"))
            {
                ImGui::TextWrapped("Texture paths and other preset keys are preserved here. Press Enter to apply an edit; paths should be absolute.");
                std::string erase;
                for (auto &entry : chain.globals)
                {
                    ImGui::PushID(entry.first.c_str());
                    dirty |= input(entry.first.c_str(), entry.second);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete"))
                        erase = entry.first;
                    ImGui::PopID();
                }
                if (!erase.empty())
                {
                    chain.globals.erase(erase);
                    dirty = true;
                }
                static std::string new_key, new_value;
                input("New key (Enter)", new_key);
                input("New value (Enter)", new_value);
                if (ImGui::Button("Add preset option") && !new_key.empty())
                {
                    chain.globals[new_key] = new_value;
                    new_key.clear();
                    new_value.clear();
                    dirty = true;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        dirty |= reorder(control.settings.chains, remove, duplicate, from, to);
        ImGui::EndDisabled();
    }
    ImGui::End();
    if (source_editor.draw(control.error, control.diagnostics))
    {
        dirty = true;
        source_saved = true;
    }
    const auto available = ImGui::GetMainViewport()->WorkSize;
    const ImVec2 picker_max(std::max(1.f, available.x * 0.9f), std::max(1.f, available.y * 0.9f));
    const ImVec2 picker_min(std::min(720.f, picker_max.x), std::min(480.f, picker_max.y));
    if (picker && RomBrowser::Instance()->Display("ShaderPicker", ImGuiWindowFlags_NoCollapse,
                                                       picker_min, picker_max))
    {
        if (RomBrowser::Instance()->IsOk())
        {
            auto path = std::filesystem::u8path(RomBrowser::Instance()->GetFilePathName());
            if (picker == 1 && control.settings.chains.size() < 16)
            {
                shader_chain_config chain;
                if (shader_import(path, chain, status) && control.inspect(chain))
                {
                    control.settings.chains.push_back(std::move(chain));
                    dirty = true;
                }
            }
            else if (picker == 3)
            {
                shader_settings next;
                if (shader_load(path, next, status))
                {
                    bool ok = true;
                    for (auto &chain : next.chains)
                        if (!chain.passes.empty())
                            ok = control.inspect(chain) && ok;
                    if (ok)
                    {
                        control.settings = std::move(next);
                        dirty = true;
                    }
                }
            }
            else if (picker == 4)
            {
                if (path.extension().empty())
                    path += ".shaders";
                if (shader_save(path, control.settings, status))
                    status = "Shader stack saved.";
            }
            else if (picker_chain >= 0 && picker_chain < static_cast<int>(control.settings.chains.size()))
            {
                auto &chain = control.settings.chains[picker_chain];
                if (picker == 2)
                {
                    if (path.extension().empty())
                        path += ".slangp";
                    if (shader_export(path, chain, status))
                        status = "Shader preset exported.";
                }
                else
                {
                    auto next = chain;
                    if (picker == 5 && next.passes.size() < 64)
                        next.passes.push_back({path, true, {}});
                    else if (picker == 6 && picker_pass >= 0 && picker_pass < static_cast<int>(next.passes.size()))
                        next.passes[picker_pass].source = path;
                    if (control.inspect(next))
                    {
                        chain = std::move(next);
                        dirty = true;
                    }
                }
            }
        }
        RomBrowser::Instance()->Close();
        picker = 0;
    }
    if (!shader_window_visible && picker)
    {
        RomBrowser::Instance()->Close();
        picker = 0;
    }
    if (dirty)
        control.changed();
    if (source_saved)
        reload_sources();
}
