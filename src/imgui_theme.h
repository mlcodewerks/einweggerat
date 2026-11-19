#pragma once

#include "imgui.h"
#include <algorithm>

inline void apply_imgui_theme(float scale = 1.0f)
{
    auto &style = ImGui::GetStyle();
    style = ImGuiStyle{};
    ImGui::StyleColorsDark(&style);

    style.TabRounding = 4.0f;
    style.ScrollbarRounding = 9.0f;
    style.WindowRounding = 7.0f;
    style.GrabRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.ScrollbarSize = 10.0f;

    // ImGui defaults to borderless frames, including checkboxes and radio buttons.
    style.FrameBorderSize = 1.0f;
    style.Colors[ImGuiCol_Border] = ImVec4(0.48f, 0.50f, 0.55f, 1.00f);
    style.Colors[ImGuiCol_CheckboxSelectedBg] = ImVec4(0.16f, 0.24f, 0.34f, 1.00f);

    style.ScaleAllSizes(scale);
    // ScaleAllSizes truncates border widths; keep outlines visible below 100%.
    style.FrameBorderSize = std::max(1.0f, style.FrameBorderSize);
    style.WindowBorderSize = std::max(1.0f, style.WindowBorderSize);
    style.ChildBorderSize = std::max(1.0f, style.ChildBorderSize);
    style.PopupBorderSize = std::max(1.0f, style.PopupBorderSize);
}
