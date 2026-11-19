#include "ui_fonts.h"
#include "imgui.h"
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

int main() {
    try {
        for (float dpi : {1.0f, 1.5f, 2.0f}) {
            ImGui::CreateContext();
            init_ui_fonts(dpi);
            auto &io = ImGui::GetIO();
            if (!io.FontDefault || io.Fonts->Fonts.Size != 1 || !io.Fonts->Build())
                throw std::runtime_error("Merged font atlas failed to build");
            auto *baked = io.FontDefault->GetFontBaked(16.0f * dpi);
            for (ImWchar codepoint : {ImWchar('A'), ImWchar('0'), ImWchar(0x00e9), ImWchar(0xf013),
                                     ImWchar(0xf04b), ImWchar(0xf07c)}) {
                auto *glyph = baked->FindGlyphNoFallback(codepoint);
                if (!glyph || !glyph->Visible)
                    throw std::runtime_error("Roboto text or Fork Awesome icon is missing");
            }
            if (ImGui::GetStyle().FontSizeBase != 16.0f * dpi)
                throw std::runtime_error("Font DPI size differs from einweggerat");
            ImGui::DestroyContext();
        }
        std::puts("PASS embedded Roboto/Fork Awesome glyphs, atlas ownership and 100/150/200% DPI");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
