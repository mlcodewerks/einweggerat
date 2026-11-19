#include "imgui_internal.h"
#include "log_window.h"
#include "native.h"
#include <cfloat>
#include <cstdio>
#include <map>

static std::map<ImGuiID, ImRect> rectangles;
static std::map<std::string, ImRect> items;
static std::map<std::string, ImGuiItemStatusFlags> flags;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *, ImGuiID id, const ImRect &bounds,
                                 const ImGuiLastItemData *) {
    rectangles[id] = bounds;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID id, const char *label,
                                  ImGuiItemStatusFlags status) {
    if (rectangles.count(id))
        items[label] = rectangles[id];
    flags[label] = status;
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID) { return nullptr; }
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        clear_log();
        append_log("frontend before core startup\n");
        native_log("native diagnostic %d", 42);
        for (auto level : {RETRO_LOG_DEBUG, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR})
            append_log("core message\nsecond line\n", LogSource::Core, level);
        auto entries = snapshot_log();
        require(entries.size() == 6 && entries[0].source == LogSource::Frontend &&
                    entries[1].source == LogSource::Frontend && entries[1].text == "native diagnostic 42\n",
                "Frontend or native diagnostics did not reach the unified log");
        require(log_message_color(entries[0]) == IM_COL32(96, 192, 255, 255), "Wrong frontend color");
        require(log_message_color(entries[2]) == IM_COL32(0, 255, 0, 255) &&
                    log_message_color(entries[3]) == IM_COL32(0, 255, 0, 255) &&
                    log_message_color(entries[4]) == IM_COL32(255, 255, 0, 255) &&
                    log_message_color(entries[5]) == IM_COL32(255, 0, 0, 255),
                "Core palette differs from einweggerat");
        require(plain_log().find("core message\nsecond line\n") != std::string::npos,
                "Multiline message was lost");
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 1200);
        io.DeltaTime = 1.0f / 60;
        unsigned char *pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        GImGui->TestEngineHookItems = true;
        bool open = true;
        auto frame = [&] {
            rectangles.clear();
            items.clear();
            flags.clear();
            ImGui::NewFrame();
            draw_log_window(open);
            ImGui::Render();
        };
        auto click = [&](const char *label) {
            frame();
            require(items.count(label), label);
            const auto point = items.at(label).GetCenter();
            io.AddMousePosEvent(point.x, point.y);
            frame();
            io.AddMouseButtonEvent(0, true);
            frame();
            io.AddMouseButtonEvent(0, false);
            frame();
            frame();
        };
        auto scrolling = [&]() -> ImGuiWindow * {
            auto *parent = ImGui::FindWindowByName("Log");
            for (auto *window : GImGui->Windows)
                if (window->ParentWindow == parent && std::strstr(window->Name, "Messages"))
                    return window;
            throw std::runtime_error("Log scroll region missing");
        };
        frame();
        frame();
        require(flags["Auto-scroll"] & ImGuiItemStatusFlags_Checked, "Auto-scroll should default on");
        for (int i = 0; i < 200; ++i)
            append_log("line " + std::to_string(i) + "\n");
        frame();
        frame();
        frame();
        require(scrolling()->ScrollMax.y > 0 && scrolling()->Scroll.y >= scrolling()->ScrollMax.y - 1,
                "New messages did not auto-scroll to the bottom");
        click("Auto-scroll");
        require(!(flags["Auto-scroll"] & ImGuiItemStatusFlags_Checked),
                "Auto-scroll toggle did not turn off");
        scrolling()->Scroll.y = 0;
        scrolling()->ScrollTarget.y = FLT_MAX;
        append_log("last line\n");
        frame();
        frame();
        require(scrolling()->Scroll.y == 0, "Disabled auto-scroll moved the view");
        click("Scroll to bottom");
        require(scrolling()->Scroll.y >= scrolling()->ScrollMax.y - 1, "Scroll-to-bottom button failed");
        click("Clear");
        require(snapshot_log().empty() && plain_log().empty(), "Clear did not empty all log sources");
        native_log("after clear");
        frame();
        require(snapshot_log().size() == 1, "Logging stopped after Clear");
        open = false;
        frame();
        ImGui::DestroyContext();
        std::puts(
            "PASS unified colored log, native diagnostics, multiline messages, Clear and scrolling controls");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
