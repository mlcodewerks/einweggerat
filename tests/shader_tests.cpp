#include "shader_chain.h"
#include "shader_library.h"
#include "shader_gl.h"
#include "glad.h"
#include "video.h"
#include "imgui.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdio>

static void require(bool value, const char *text) { if (!value) throw std::runtime_error(text); }
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    try {
        auto scratch = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        auto fixture = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
        std::filesystem::create_directories(scratch / "nested");
        std::string error;
        shader_chain_config chain;
        require(shader_import(fixture, chain, error), error.c_str());
        require(chain.passes.size() == 1, "Shader import lost pass");
        chain.values = {{"GAIN", 0.5f}, {"BIAS", 0.1f}};
        chain.passes[0].options = {{"scale_type", "viewport"}, {"scale", "1.0"}};
        chain.globals = {{"textures", "LUT"}, {"LUT", (scratch / "lookup.png").generic_u8string()}, {"LUT_linear", "false"}};
        require(shader_export(scratch / "base.slangp", chain, error), error.c_str());
        std::ofstream(scratch / "nested" / "reference.slangp") << "#reference \"../base.slangp\"\nGAIN = 0.7\nLUT = \"override.png\"\n";
        shader_chain_config imported;
        require(shader_import(scratch / "nested" / "reference.slangp", imported, error), error.c_str());
        require(imported.values["GAIN"] == 0.7f, "Referenced parameter override lost");
        require(imported.passes[0].source == fixture, "Referenced source resolved incorrectly");
        require(imported.globals["LUT"] == (scratch / "nested" / "override.png").generic_u8string(), "LUT override resolved incorrectly");
        imported.passes.push_back(imported.passes[0]); imported.passes[0].enabled = false;
        shader_settings settings{false, {imported, chain}}, restored;
        settings.chains[1].enabled = false;
        require(shader_save(scratch / "stack.shaders", settings, error), error.c_str());
        require(shader_save(scratch / "stack.shaders", settings, error), "Replacing stack failed");
        require(shader_load(scratch / "stack.shaders", restored, error), error.c_str());
        require(!restored.enabled && restored.chains.size() == 2 && !restored.chains[1].enabled &&
            !restored.chains[0].passes[0].enabled && restored.chains[0].values == imported.values &&
            restored.chains[0].passes[1].options == imported.passes[1].options, "Shader stack roundtrip lost state");
        require(shader_export(scratch / "enabled.slangp", imported, error), error.c_str());
        require(shader_import(scratch / "enabled.slangp", imported, error) && imported.passes.size() == 1, "Pass bypass export failed");
        std::ofstream(scratch / "cycle.slangp") << "#reference \"cycle.slangp\"\n";
        require(!shader_import(scratch / "cycle.slangp", imported, error), "Cyclic reference accepted");
        require(imported.passes.size() == 1, "Failed load changed previous chain");
        std::ofstream(scratch / "bad.shaders") << "EINWEGGERAT_SHADERS 1\n1 99999999\n";
        require(!shader_load(scratch / "bad.shaders", restored, error), "Unbounded stack accepted");
        shader_library absent; absent.load(scratch);
        require(!absent.gl_available && !absent.vk_available, "Loader searched outside main directory");
        auto ids = shader_pass_parameters(fixture);
        require(ids.size() == 2, "Pass parameter discovery failed");
        // Compiler locations refer to librashader's #line map, including its
        // pragma resets, rather than directly to physical source line numbers.
        auto diagnostic_source = scratch / "diagnostic.slang";
        std::ofstream(diagnostic_source) << "#version 450\n#pragma stage fragment\nvoid main() {\n    broken();\n}\n";
        shader_chain_config diagnostic_chain; diagnostic_chain.passes = {{diagnostic_source, true, {}}};
        shader_diagnostic_report report;
        shader_read_diagnostics(report, "GlslangErrorLog { log: \"ERROR: diagnostic.slang:3: 'broken' : undefined\\nERROR: 1 compilation errors.\\n\", debug_log: \"\" }", diagnostic_chain, true);
        require(report.entries.size() == 2 && report.entries[0].line == 4 &&
            report.entries[0].source == std::filesystem::weakly_canonical(diagnostic_source), "Compiler source line mapping failed");
        auto include_path = scratch / "nested" / "broken.inc";
        std::ofstream(include_path) << "\n\nvoid broken() {\n    unknown();\n}\n";
        std::ofstream(diagnostic_source) << "#version 450\n#pragma stage fragment\n#include \"nested/broken.inc\"\nvoid main() {}\n";
        shader_read_diagnostics(report, "ERROR: broken.inc:2: unknown function\nWARNING: diagnostic.slang:2: after include", diagnostic_chain, true);
        require(report.entries.size() == 2 && report.entries[0].source == std::filesystem::weakly_canonical(include_path) &&
            report.entries[0].line == 4 && report.entries[1].line == 4 && report.entries[1].warning, "Include diagnostics mapping failed");
        // Do not put generated-driver locations or ambiguous basenames on an
        // unrelated editor line, even when the line happens to be in range.
        auto duplicate_source = scratch / "nested" / "diagnostic.slang";
        std::filesystem::copy_file(diagnostic_source, duplicate_source, std::filesystem::copy_options::overwrite_existing);
        diagnostic_chain.passes.push_back({duplicate_source, true, {}});
        shader_read_diagnostics(report, "ERROR: diagnostic.slang:2: ambiguous\nERROR: 0:3: generated code", diagnostic_chain, true);
        require(report.entries.size() == 2 && !report.entries[0].line && !report.entries[1].line, "Ambiguous compiler location was guessed");
        std::puts("PASS: compiler log decoding, pragma/include line maps, warning and ambiguous-source handling");
        std::puts("PASS: shader reference/LUT resolution, order/bypass/parameter persistence, export, invalid input, optional absence");
        if (argc < 4) return 0;
        WNDCLASSW wc{}; wc.style=CS_OWNDC; wc.lpfnWndProc=DefWindowProcW;
        wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"ShaderNativeTest";
        RegisterClassW(&wc);
        auto window=CreateWindowW(wc.lpszClassName,L"Shader test",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
        require(window!=nullptr,"Native window failed");
        ImGui::CreateContext();
        Video video; video.init(window,Context::OpenGL);
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(+[](GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar *message, const void *) {
            if (type == GL_DEBUG_TYPE_ERROR) std::fprintf(stderr, "OpenGL: %s\n", message);
        }, nullptr);
        auto &control = video_shaders(); control.init();
        require(librashader().gl_available, librashader().status.c_str());
        chain.globals.clear(); require(control.inspect(chain), control.error.c_str());
        require(chain.parameters.size() == 2, "Runtime parameter metadata missing");
        control.settings = {true, {chain}}; control.changed();
        GLuint texture, fbo; glCreateTextures(GL_TEXTURE_2D, 1, &texture); glTextureStorage2D(texture, 1, GL_RGBA8, 4, 4);
        unsigned char pixels[64]; for (int i = 0; i < 64; ++i) pixels[i] = i % 4 == 3 ? 255 : 100;
        glTextureSubImage2D(texture, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glCreateFramebuffers(1, &fbo); glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, texture, 0);
        auto render = [&](int expected) {
            glViewport(3, 4, 5, 6); glEnable(GL_BLEND);
            auto output = shader_gl_render(fbo, 4, 4, 8, 8);
            require(output != 0, control.error.c_str());
            GLint viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport);
            require(viewport[0] == 3 && viewport[3] == 6 && glIsEnabled(GL_BLEND), "Shader corrupted OpenGL state");
            unsigned char pixel[4]; glBindFramebuffer(GL_READ_FRAMEBUFFER, output);
            glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            require(std::abs(int(pixel[0]) - expected) <= 2, "Shader output pixel incorrect");
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            if (auto error = glGetError()) throw std::runtime_error("Shader generated OpenGL error " +
                std::to_string(error) + " while expecting pixel " + std::to_string(expected));
        };
        render(76);
        control.settings.chains[0].values["GAIN"] = 1; render(126);
        auto second = chain; second.values = {{"GAIN", 0.5f}, {"BIAS", 0}};
        control.settings.chains.push_back(second); control.changed(); render(63);
        std::swap(control.settings.chains[0], control.settings.chains[1]); control.changed(); render(76);
        control.settings.chains[0].passes[0].source = scratch / "missing.slang"; control.changed(); render(76);
        require(!control.error.empty(), "Compilation failure not reported");
        control.settings.chains[0].passes[0].source = fixture; control.changed(); render(76);
        control.settings.chains[0].enabled = false; control.changed(); render(126);
        auto multipass = chain; multipass.values = {{"GAIN", 0.5f}, {"BIAS", 0}};
        multipass.passes.push_back(multipass.passes[0]);
        control.settings.chains = {multipass}; control.changed(); render(25);
        control.settings.chains[0].passes[1].enabled = false; control.changed(); render(50);
        shader_chain_config history;
        require(shader_import(fixture.parent_path() / "shader_history.slang", history, error), error.c_str());
        require(control.inspect(history), control.error.c_str());
        control.settings.chains = {history}; control.changed(); render(50); render(100);
        // Real librashader error_write output must reach the physical source
        // line, and a successful replacement must clear the old diagnostics.
        std::ifstream fixture_in(fixture, std::ios::binary);
        std::string valid_source(std::istreambuf_iterator<char>(fixture_in), {});
        std::string invalid_source = valid_source;
        auto invalid_at = invalid_source.find("FragColor = vec4");
        require(invalid_at != invalid_source.npos, "Missing shader error injection point");
        const auto invalid_line = 1 + std::count(invalid_source.begin(), invalid_source.begin() + invalid_at, '\n');
        invalid_source.replace(invalid_at, 16, "FragColor = missing_function");
        require(shader_save_source(diagnostic_source, invalid_source, error), error.c_str());
        diagnostic_chain.passes = {{fixture, true, {}}, {diagnostic_source, true, {}}};
        control.settings.chains = {diagnostic_chain}; control.changed(); render(100);
        require(!control.error.empty(), "Shader syntax error was not reported");
        bool located = false;
        for (const auto &entry : control.diagnostics.entries)
            located |= entry.source == std::filesystem::weakly_canonical(diagnostic_source) && entry.line == static_cast<size_t>(invalid_line);
        require(located, "Real librashader syntax error did not map to its physical source line");
        // Errors inside trimmed include files must open that include, not the
        // pass containing the #include directive.
        std::ofstream(include_path) << "\n\nvec4 included_error() {\n    return unknown_include_function();\n}\n";
        auto include_source = valid_source;
        auto stage = include_source.find("#pragma stage fragment");
        auto stage_end = include_source.find('\n', stage);
        include_source.insert(stage_end + 1, "#include \"nested/broken.inc\"\n");
        require(shader_save_source(diagnostic_source, include_source, error), error.c_str());
        control.changed(); render(100);
        located = false;
        for (const auto &entry : control.diagnostics.entries)
            located |= entry.source == std::filesystem::weakly_canonical(include_path) && entry.line == 4;
        require(located, "Real librashader include error did not map to the include source line");
        require(shader_save_source(diagnostic_source, valid_source, error), error.c_str());
        control.changed(); render(100);
        require(control.error.empty() && control.diagnostics.entries.empty(), "Successful shader compilation left stale diagnostics");
        control.settings.enabled = false; require(!shader_gl_render(fbo, 4, 4, 8, 8), "Master bypass ignored");
        if (argc >= 5) {
            shader_chain_config preset;
            require(shader_import(std::filesystem::absolute(std::filesystem::u8path(argv[4])), preset, error), error.c_str());
            require(control.inspect(preset), control.error.c_str());
            control.settings = {true, {preset}}; control.changed();
            for (int i = 0; i < 3; ++i) {
                require(shader_gl_render(fbo, 4, 4, 640, 480) != 0, control.error.c_str());
                require(control.error.empty(), control.error.c_str());
                glFinish(); require(glGetError() == GL_NO_ERROR, "Preset rendering generated an OpenGL error");
            }
            std::puts("PASS: supplied preset compiled and rendered in OpenGL");
        }
        shader_gl_destroy(); glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture);
        video.shutdown(); ImGui::DestroyContext(); DestroyWindow(window);
        std::puts("PASS: real librashader OpenGL pixels, live parameters, stacked chain order, bypass, failed edit rollback, state restoration");
        std::puts("PASS: real librashader compiler API errors, pass/include source locations and successful rebuild clearing");
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
