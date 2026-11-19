#include "core.h"
#include "core_options_window.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"
#include <cstdio>
static void require(bool ok, const char *text) {
    if (!ok)
        throw std::runtime_error(text);
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    try {
        WNDCLASSW wc{};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LifecycleTest";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"Lifecycle", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                                    nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "Window creation failed");
        ImGui::CreateContext();
        require(ImGui_ImplWin32_Init(window), "ImGui Win32 failed");
        auto library = fs::absolute(fs::u8path(argv[1]));
        auto directory = fs::absolute(fs::u8path(argv[2]));
        HMODULE fixture = LoadLibraryW(library.c_str());
        require(fixture != nullptr, "Fixture load failed");
        auto memory = reinterpret_cast<decltype(&retro_get_memory_data)>(
            GetProcAddress(fixture, "retro_get_memory_data"));
        require(memory != nullptr, "Fixture missing memory export");
        auto controller_device =
            reinterpret_cast<unsigned (*)(unsigned)>(GetProcAddress(fixture, "fixture_controller_device"));
        require(controller_device != nullptr, "Fixture missing controller export");
        auto define_options =
            reinterpret_cast<bool (*)(unsigned)>(GetProcAddress(fixture, "fixture_define_options"));
        require(define_options != nullptr, "Fixture missing option metadata export");
        Option boolean;
        size_t yes = 0, no = 0;
        boolean.values = {"OFF", "Enabled"};
        require(core_option_boolean(boolean, yes, no) && yes == 1 && no == 0,
                "Reversed case-insensitive boolean pair was not recognized");
        boolean.values = {"0", "1"};
        require(!core_option_boolean(boolean, yes, no),
                "Numeric option was incorrectly rendered as a checkbox");
        boolean.values = {"disabled", "enabled", "auto"};
        require(!core_option_boolean(boolean, yes, no),
                "Three-state option was incorrectly rendered as a checkbox");
        {
            Video video;
            video.init(window, Context::OpenGL);
            video.verify_pixels = true;
            Input input;
            input.init(window);
            Core core(video, input, directory);
            core.audio_enabled = false;
            for (auto context : {Context::OpenGL, Context::Vulkan, Context::OpenGL}) {
                video.select(context);
                core.load(library);
                for (const auto &entry : std::vector<LogEntry>{
                         {"fixture debug 1\n", LogSource::Core, RETRO_LOG_DEBUG},
                         {"fixture info\n", LogSource::Core, RETRO_LOG_INFO},
                         {"fixture warning\n", LogSource::Core, RETRO_LOG_WARN},
                         {"fixture error\n", LogSource::Core, RETRO_LOG_ERROR},
                         {"fixture core notification\n", LogSource::Core, RETRO_LOG_INFO}}) {
                    const auto messages = core.log_entries();
                    require(std::any_of(messages.begin(), messages.end(),
                                        [&](const LogEntry &actual) {
                                            return actual.text == entry.text &&
                                                   actual.source == entry.source &&
                                                   actual.level == entry.level;
                                        }),
                            "Core log source, severity or formatting was lost");
                }
                for (int i = 0; i < 10; ++i)
                    core.run();
                require(static_cast<uint8_t *>(memory(RETRO_MEMORY_SAVE_RAM))[0] == 10,
                        "Fixture did not run");
                core.save_state(0);
                core.reset();
                core.run();
                require(static_cast<uint8_t *>(memory(RETRO_MEMORY_SAVE_RAM))[0] == 1, "Reset failed");
                core.load_state(0);
                core.run();
                require(static_cast<uint8_t *>(memory(RETRO_MEMORY_SAVE_RAM))[0] == 11,
                        "State round trip failed");
                require(core.options.size() == 1, "Core options missing");
                core.set_option(core.options.front(), "blue");
                core.unload();
                core.load(library);
                require(core.options.front().value == "blue", "Core option persistence failed");
                require(static_cast<uint8_t *>(memory(RETRO_MEMORY_SAVE_RAM))[0] == 11,
                        "SRAM persistence failed");
                require(input.binding(0, {RETRO_DEVICE_JOYPAD, 0, 0}).label == "Fixture fire button",
                        "Core descriptor did not reach native input settings");
                input.set_controller(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1));
                core.paused = true;
                const auto paused_frames = core.frames;
                core.run();
                require(core.frames == paused_frames, "Controller change advanced a paused core");
                core.paused = false;
                require(controller_device(0) == RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1),
                        "Controller-type change did not reach libretro core");
                for (unsigned version : {0u, 1u, 2u, 3u}) {
                    require(define_options(version), "Core options interface rejected");
                    require(core.options.front().value == "blue" &&
                                core.options.front().value_labels[1] == "Cool blue",
                            "Core option value labels or persisted raw values were lost");
                    if (version < 2) {
                        require(core.option_categories.empty(), "Legacy options retained stale categories");
                        continue;
                    }
                    require(core.option_categories.size() == 2 &&
                                core.option_categories[0].label == "Video" &&
                                core.options[0].category == "video" &&
                                core.options[0].categorized_label == "Palette" &&
                                core.options[0].categorized_info == "Choose the video palette.",
                            "V2 option categories and help text were discarded");
                    core.set_option(core.options[1], "disabled");
                    require(!core.options[2].visible, "Display callback did not hide conditional option");
                    core.set_option(core.options[1], "enabled");
                    require(core.options[2].visible, "Display callback did not reveal conditional option");
                    core.set_option(core.options[1], "invalid");
                    require(core.options[1].value == "enabled", "Invalid option value was accepted");
                    // Draw collapsed and expanded categories, including the uncategorized fallback.
                    for (bool expanded : {false, true}) {
                        ImGui_ImplWin32_NewFrame();
                        video.new_frame();
                        ImGui::NewFrame();
                        if (auto *dialog = ImGui::FindWindowByName("Core Settings"))
                            dialog->StateStorage.SetAllInt(expanded ? 1 : 0);
                        video.draw(640, 480);
                        bool open = true;
                        draw_core_options(core, open);
                        ImGui::Render();
                        video.present();
                    }
                }
                core.unload();
                require(core.options.empty() && core.option_categories.empty(),
                        "Core metadata leaked on unload");
            }
        }
        FreeLibrary(fixture);
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(window);
        std::puts("PASS GL/Vulkan/GL context switching, repeated DLL lifecycle, save states, reset, SRAM and "
                  "core option persistence");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
