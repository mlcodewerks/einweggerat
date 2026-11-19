#pragma once

#include "common.h"
#include <memory>
#include "libretro.h"

struct ImDrawData;
struct vp { unsigned x=0, y=0, width=0, height=0; };

class VulkanRenderer
{
public:
    VulkanRenderer();
    ~VulkanRenderer();
    bool init(HWND window);
    bool init_imgui();
    void shutdown_imgui();
    void shutdown();
    bool init_core();
    void destroy_core();
    bool environment(unsigned command, void *data);
    void set_software_pixel_format(retro_pixel_format format);
    bool refresh(const void *data, unsigned width, unsigned height, size_t pitch,
                 retro_pixel_format format);
    void draw(const vp &viewport, unsigned width, unsigned height);
    bool new_frame();
    // Pace after GPU submission and before presentation, without the queue lock.
    // Minimized/empty/outdated frames also pace before returning.
    bool present(ImDrawData *data, void (*pace)(void *) = nullptr, void *userdata = nullptr);
    void wait_idle();
    bool readback(std::vector<uint8_t> &pixels, unsigned &width, unsigned &height);
    const char *error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
