#pragma once

// Call once after creating the ImGui context and before initializing its renderer.
// DPI is relative to Windows' standard 96 DPI.
void init_ui_fonts(float dpi_scale);
