#include "log_window.h"

ImU32 log_message_color(const LogEntry &entry) {
    if (entry.source == LogSource::Frontend)
        return IM_COL32(96, 192, 255, 255);
    // Match einweggerat's ExampleAppLog palette exactly.
    switch (entry.level) {
    case RETRO_LOG_WARN:
        return IM_COL32(255, 255, 0, 255);
    case RETRO_LOG_ERROR:
        return IM_COL32(255, 0, 0, 255);
    default:
        return IM_COL32(0, 255, 0, 255);
    }
}
void draw_log_window(bool &open) {
    if (!open)
        return;
    ImGui::SetNextWindowSize(ImVec2(760, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Log", &open)) {
        static bool auto_scroll = true;
        if (ImGui::Button("Clear"))
            clear_log();
        ImGui::SameLine();
        const bool auto_changed = ImGui::Checkbox("Auto-scroll", &auto_scroll);
        ImGui::SameLine();
        const bool scroll_down = ImGui::Button("Scroll to bottom");
        ImGui::TextUnformatted("Core: green info/debug, yellow warnings, red errors. einweggerat: blue.");
        ImGui::Separator();
        if (ImGui::BeginChild("Messages", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
            const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            for (const auto &entry : snapshot_log()) {
                auto text = entry.source == LogSource::Frontend ? "[einweggerat] " + entry.text : entry.text;
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                ImGui::PushStyleColor(ImGuiCol_Text, log_message_color(entry));
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar();
            if (scroll_down || (auto_scroll && (follow || auto_changed)))
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}
