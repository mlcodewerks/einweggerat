#include "shader_source_editor.h"
#include "shader_chain.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace
{
    ImFont *editor_font = nullptr;
    std::string editor_text(const TextEditor &editor)
    {
        // GetText() appends a final newline, even when the document has none.
        return editor.GetSectionText({}, {editor.GetLineCount(), 0});
    }
}
void shader_source_editor_init_font()
{
    ImFontConfig config;
    config.SizePixels = 16;
    editor_font = ImGui::GetIO().Fonts->AddFontDefault(&config);
}

size_t shader_editor_diagnostics(TextEditor &editor, const std::filesystem::path &path,
                                 const shader_diagnostic_report &report)
{
    editor.ClearMarkers();
    editor.ClearSquiggles();
    std::error_code ec;
    auto key = std::filesystem::weakly_canonical(path, ec);
    auto source = report.sources.find(key);
    if (ec || source == report.sources.end() || editor_text(editor) != source->second)
        return 0;
    std::map<size_t, std::pair<std::string, bool>> lines;
    size_t count = 0;
    for (const auto &entry : report.entries)
    {
        if (entry.source != key || !entry.line || entry.line > editor.GetLineCount())
            continue;
        auto &line = lines[entry.line - 1];
        if (!line.first.empty())
            line.first += '\n';
        line.first += entry.message;
        line.second |= !entry.warning;
        ++count;
    }
    for (const auto &[line, diagnostic] : lines)
    {
        const auto color = diagnostic.second ? IM_COL32(255, 85, 85, 255) : IM_COL32(255, 195, 70, 255);
        const auto background = diagnostic.second ? IM_COL32(255, 60, 60, 35) : IM_COL32(255, 180, 40, 35);
        editor.AddMarker(line, color, background, diagnostic.first, diagnostic.first);
        editor.AddSquiggle({line, 0}, {line, SIZE_MAX}, 0, color, diagnostic.first);
    }
    return count;
}

bool shader_source_editor::open(const std::filesystem::path &path, std::string &error)
{
    std::filesystem::path key;
    if (!load(path, key, error))
        return false;
    show({key});
    open_error.clear();
    return true;
}

void shader_source_editor::show(std::vector<std::filesystem::path> paths)
{
    if (tab_order != paths)
    {
        tabs_to_close.insert(tabs_to_close.end(), tab_order.begin(), tab_order.end());
        tab_order = std::move(paths);
    }
    selected = tab_order.empty() ? std::filesystem::path{} : tab_order.front();
    visible = true;
    focus = true;
}

bool shader_source_editor::load(const std::filesystem::path &path, std::filesystem::path &key, std::string &error)
{
    try
    {
        key = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        if (!documents.count(key))
        {
            auto size = std::filesystem::file_size(key);
            if (size > 4 * 1024 * 1024)
                throw std::runtime_error("Cannot open shader source (maximum 4 MiB).");
            std::ifstream file(key, std::ios::binary);
            std::string text(static_cast<size_t>(size), '\0');
            if (!file.read(text.data(), static_cast<std::streamsize>(size)))
                throw std::runtime_error("Cannot read shader source.");
            if (text.find('\0') != text.npos)
                throw std::runtime_error("Shader source contains NUL bytes.");
            auto &doc = documents[key];
            doc.crlf = text.find("\r\n") != text.npos;
            doc.bom = text.compare(0, 3, "\xef\xbb\xbf") == 0;
            if (doc.bom)
                text.erase(0, 3);
            doc.editor.SetLanguage(TextEditor::LookupLanguage("GLSL"));
            doc.editor.SetTabSize(4);
            doc.editor.SetInsertSpacesOnTabs(false);
            doc.editor.SetShowWhitespacesEnabled(false);
            // Preserve shader text delivered in batches by SDL or libretro.
            // Pair completion otherwise inserts extra closing braces/parentheses.
            doc.editor.SetCompletePairedGlyphs(false);
            doc.editor.SetText(text);
            doc.saved_text = editor_text(doc.editor);
        }
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}

bool shader_source_editor::open_preset(const shader_chain_config &chain, std::string &error)
{
    std::vector<std::filesystem::path> paths;
    std::string failures;
    // Include bypassed passes and reuse any existing buffers and undo histories.
    for (const auto &pass : chain.passes)
    {
        std::string failure;
        std::filesystem::path key;
        if (load(pass.source, key, failure))
        {
            if (std::find(paths.begin(), paths.end(), key) == paths.end())
                paths.push_back(key);
        }
        else
        {
            if (!failures.empty())
                failures += '\n';
            failures += pass.source.u8string() + ": " + failure;
        }
    }
    show(std::move(paths));
    if (chain.passes.empty())
        failures = "This chain has no shader passes to edit.";
    open_error = error = std::move(failures);
    return error.empty();
}

bool shader_source_editor::save(const std::filesystem::path &path, document &doc)
{
    auto text = editor_text(doc.editor);
    std::string output;
    if (doc.bom)
        output = "\xef\xbb\xbf";
    for (char ch : text)
    {
        if (ch == '\n' && doc.crlf)
            output += '\r';
        output += ch;
    }
    if (!shader_save_source(path, output, doc.status))
        return false;
    doc.saved_text = std::move(text);
    doc.modified = false;
    doc.status = "Shader source saved. Reload requested.";
    return true;
}

bool shader_source_editor::has_unsaved_changes() const
{
    for (const auto &[path, doc] : documents)
        if (doc.modified)
            return true;
    return false;
}

bool shader_source_editor::save_all(std::string &error)
{
    for (auto &[path, doc] : documents)
        if (doc.modified && !save(path, doc))
        {
            error = path.u8string() + ": " + doc.status;
            return false;
        }
    error.clear();
    return true;
}

bool shader_source_editor::draw(const std::string &compile_error, const shader_diagnostic_report &diagnostics, ImVec2 size,
                                const char *language_label, ImGuiWindowFlags window_flags)
{
    if (!visible)
        return false;
    bool saved = false;
    if (size.x > 0 && size.y > 0)
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    else
        ImGui::SetNextWindowSize(ImVec2(950, 750), ImGuiCond_FirstUseEver);
    if (focus)
        ImGui::SetNextWindowFocus();
    // Closing the window hides it; documents remain available from Edit source.
    if (ImGui::Begin("Shader source", &visible, window_flags))
    {
        const bool compact = ImGui::GetContentRegionAvail().y < 200.f;
        if (!compact)
            ImGui::TextWrapped("Save writes the source file used by every pass referencing it and reloads the shader stack. Failed compilation keeps the previous working shaders.");
        if (!open_error.empty())
            ImGui::TextWrapped("%s", open_error.c_str());
        if (!compile_error.empty())
        {
            ImGui::TextUnformatted("Shader compilation failed. Hover highlighted lines for details.");
            if (!diagnostics.entries.empty() && ImGui::TreeNode("Compiler diagnostics"))
            {
                ImGui::BeginChild("Diagnostic list", ImVec2(-1, 120), ImGuiChildFlags_Borders);
                for (size_t i = 0; i < diagnostics.entries.size(); ++i)
                {
                    const auto &entry = diagnostics.entries[i];
                    ImGui::PushID(static_cast<int>(i));
                    if (entry.line && !entry.source.empty())
                    {
                        const auto label = entry.source.filename().u8string() + ":" + std::to_string(entry.line);
                        if (ImGui::SmallButton(label.c_str()))
                        {
                            std::filesystem::path key;
                            if (load(entry.source, key, open_error))
                            {
                                if (std::find(tab_order.begin(), tab_order.end(), key) == tab_order.end())
                                    tab_order.push_back(key);
                                selected = key;
                                focus = true;
                                documents.at(key).editor.SetCursor({entry.line - 1, 0});
                                documents.at(key).editor.ScrollToLine(entry.line - 1);
                            }
                        }
                        ImGui::SameLine();
                    }
                    ImGui::TextUnformatted(entry.message.c_str());
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Full compiler output"))
            {
                ImGui::BeginChild("Compiler log", ImVec2(-1, 150), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::TextUnformatted(compile_error.c_str());
                ImGui::EndChild();
                ImGui::TreePop();
            }
        }
        std::filesystem::path remove;
        if (ImGui::BeginTabBar("Shader documents", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_TabListPopupButton))
        {
            // Reset the visible tabs immediately when switching entry or preset.
            for (const auto &path : tabs_to_close)
            {
                const auto title = path.filename().u8string() + "###" + path.u8string();
                ImGui::SetTabItemClosed(title.c_str());
            }
            tabs_to_close.clear();
            for (const auto &path : tab_order)
            {
                auto &doc = documents.at(path);
                if (doc.diagnostic_revision != diagnostics.revision)
                {
                    doc.diagnostic_count = shader_editor_diagnostics(doc.editor, path, diagnostics);
                    doc.diagnostic_revision = diagnostics.revision;
                }
                const auto id = path.u8string();
                const auto title = path.filename().u8string() + "###" + id;
                ImGuiTabItemFlags flags = doc.modified ? ImGuiTabItemFlags_UnsavedDocument : ImGuiTabItemFlags_None;
                if (selected == path)
                    flags |= ImGuiTabItemFlags_SetSelected;
                bool keep = true;
                bool active = ImGui::BeginTabItem(title.c_str(), &keep, flags);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", id.c_str());
                if (!keep)
                {
                    if (doc.modified)
                        closing = path;
                    else
                        remove = path;
                }
                if (active)
                {
                    ImGui::PushID(id.c_str());
                    if (!compact) ImGui::TextWrapped("%s", id.c_str());
                    bool shortcut = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
                    if (compact && shortcut) saved |= save(path, doc);
                    if (!compact)
                    {
                        if (ImGui::Button("Save source and reload") || shortcut)
                            saved |= save(path, doc);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!doc.editor.CanUndo());
                        if (ImGui::Button("Undo"))
                        {
                            doc.editor.Undo();
                            doc.modified = editor_text(doc.editor) != doc.saved_text;
                            doc.diagnostic_revision = UINT64_MAX;
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!doc.editor.CanRedo());
                        if (ImGui::Button("Redo"))
                        {
                            doc.editor.Redo();
                            doc.modified = editor_text(doc.editor) != doc.saved_text;
                            doc.diagnostic_revision = UINT64_MAX;
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::TextUnformatted(doc.modified ? "Unsaved changes" : "Saved");
                        if (doc.diagnostic_count)
                        {
                            ImGui::Text("%zu compiler diagnostics", doc.diagnostic_count);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Next error"))
                            {
                                size_t next = SIZE_MAX, first = SIZE_MAX;
                                const auto cursor = doc.editor.GetMainCursorPosition().line;
                                for (const auto &entry : diagnostics.entries)
                                    if (entry.source == path && entry.line)
                                    {
                                        first = std::min(first, entry.line - 1);
                                        if (entry.line - 1 > cursor)
                                            next = std::min(next, entry.line - 1);
                                    }
                                if (next == SIZE_MAX)
                                    next = first;
                                if (next != SIZE_MAX)
                                {
                                    doc.editor.SetCursor({next, 0});
                                    doc.editor.ScrollToLine(next);
                                    doc.editor.SetFocus();
                                }
                            }
                        }
                        if (!doc.status.empty())
                            ImGui::TextWrapped("%s", doc.status.c_str());
                        ImGui::Text("%s    Ctrl+S: save    Ctrl+F: find    Ctrl+H: replace", language_label);
                    }
                    if (focus && selected == path)
                    {
                        doc.editor.SetFocus();
                        focus = false;
                        selected.clear();
                    }
                    ImGui::PushFont(editor_font, ImGui::GetFontSize());
                    // Let the dialog's translucent background show through the editor.
                    ImGui::SetNextWindowBgAlpha(0.0f);
                    if (doc.editor.Render("##source", ImVec2(-1, -1)))
                    {
                        doc.modified = editor_text(doc.editor) != doc.saved_text;
                        doc.diagnostic_count = shader_editor_diagnostics(doc.editor, path, diagnostics);
                    }
                    ImGui::PopFont();
                    ImGui::PopID();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        // A newly created tab may not become active until the next frame.
        // Keep its selection/focus request until its editor has been rendered.
        if (!closing.empty())
            ImGui::OpenPopup("Unsaved shader changes");
        if (ImGui::BeginPopupModal("Unsaved shader changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            auto &doc = documents.at(closing);
            ImGui::Text("Save changes to %s?", closing.filename().u8string().c_str());
            if (!doc.status.empty())
                ImGui::TextWrapped("%s", doc.status.c_str());
            bool done = false;
            if (ImGui::Button("Save and close") && save(closing, doc))
            {
                saved = true;
                remove = closing;
                done = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard changes"))
            {
                remove = closing;
                done = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                done = true;
            if (done)
            {
                closing.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!remove.empty())
        {
            documents.erase(remove);
            tab_order.erase(std::remove(tab_order.begin(), tab_order.end(), remove), tab_order.end());
        }
        if (tab_order.empty() && open_error.empty())
            visible = false;
    }
    ImGui::End();
    return saved;
}
