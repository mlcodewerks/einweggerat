#pragma once
#include "imgui.h"
#include "log.h"
ImU32 log_message_color(const LogEntry &entry);
void draw_log_window(bool &open);
