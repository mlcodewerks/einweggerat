#pragma once
#include "common.h"
#include "log.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
inline uint64_t native_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void native_client_size(HWND window, int &width, int &height) {
    RECT rect{};
    GetClientRect(window, &rect);
    width = rect.right;
    height = rect.bottom;
}
inline void native_log(const char *format, ...) {
    char text[8192];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    append_log(std::string(text) + "\n", LogSource::Frontend, RETRO_LOG_ERROR, true);
}
void *native_gl_proc(const char *name);
void init_command_line_output();
