#pragma once
#include "TextEditor.h"
#include <filesystem>
#include <map>
#include <vector>
#include "shader_diagnostics.h"

struct shader_chain_config;

// Call while setting up the ImGui font atlas, before its first frame.
void shader_source_editor_init_font();
// Apply only diagnostics that still match the editor's source snapshot.
size_t shader_editor_diagnostics(TextEditor &, const std::filesystem::path &,
    const shader_diagnostic_report &);

// Documents follow source paths, so reordering passes never loses edits and
// passes referencing the same file share one buffer and undo history.
class shader_source_editor {
public:
    bool open(const std::filesystem::path &path, std::string &error);
    bool open_preset(const shader_chain_config &chain, std::string &error);
    bool has_unsaved_changes() const;
    bool is_visible() const { return visible; }
    bool save_all(std::string &error);
    // An optional positive size lets an embedding tool resize the editor with its workspace.
    bool draw(const std::string &compile_error, const shader_diagnostic_report & = {}, ImVec2 size = {},
        const char *language_label = "GLSL / Slang", ImGuiWindowFlags window_flags = 0); // True after a successful save.
private:
    struct document {
        TextEditor editor;
        std::string saved_text, status;
        bool modified = false, crlf = false, bom = false;
        uint64_t diagnostic_revision = UINT64_MAX;
        size_t diagnostic_count = 0;
    };
    std::map<std::filesystem::path, document> documents;
    // Only the current entry or preset is visible; other drafts remain cached.
    std::vector<std::filesystem::path> tab_order;
    std::vector<std::filesystem::path> tabs_to_close;
    std::string open_error;
    std::filesystem::path selected, closing;
    bool visible = false, focus = false;
    bool load(const std::filesystem::path &, std::filesystem::path &key, std::string &error);
    void show(std::vector<std::filesystem::path> paths);
    bool save(const std::filesystem::path &, document &);
};
