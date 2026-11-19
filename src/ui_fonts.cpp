#include "ui_fonts.h"
#include "imgui.h"
#include "IconsForkAwesome.h"
#include "imgui_font.h"
#include "forkawesome.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

void init_ui_fonts(float dpi_scale) {
    if (!std::isfinite(dpi_scale) || dpi_scale <= 0)
        dpi_scale = 1.0f;

    auto &io = ImGui::GetIO();
    // einweggerat's 12-point text is 16 pixels at Windows' standard 96 DPI.
    const float size = 16.0f * dpi_scale;
    ImFontConfig text;
    text.FontDataOwnedByAtlas = false; // The embedded Roboto data has static lifetime.
    std::snprintf(text.Name, sizeof(text.Name), "Roboto Regular");
    auto *font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(Roboto_Regular),
                                             Roboto_Regular_size, size, &text);
    if (!font)
        throw std::runtime_error("Cannot load embedded Roboto font");
    io.FontDefault = font;

    static const ImWchar icon_ranges[] = {ICON_MIN_FK, ICON_MAX_FK, 0};
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.GlyphMinAdvanceX = 13.0f * dpi_scale;
    std::snprintf(icons.Name, sizeof(icons.Name), "Fork Awesome");
    // The atlas owns the decompressed font allocation, unlike the static Roboto data.
    if (!io.Fonts->AddFontFromMemoryCompressedTTF(forkawesome_compressed_data, forkawesome_compressed_size,
                                                size, &icons, icon_ranges))
        throw std::runtime_error("Cannot load embedded Fork Awesome font");
    ImGui::GetStyle().FontSizeBase = size;
}
