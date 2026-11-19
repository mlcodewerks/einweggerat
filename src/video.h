#pragma once
#include "common.h"
#include "render_backends.h"
#include <memory>
enum class Context { OpenGL, Vulkan };
class Video {
  public:
    Video();
    ~Video();
    void init(HWND window, Context context);
    void select(Context context);
    void shutdown();
    bool hardware(retro_hw_render_callback *callback);
    bool environment(unsigned command, void *data);
    void start_core(const retro_game_geometry &geometry);
    void stop_core();
    void geometry(const retro_game_geometry &geometry, bool allocate = false);
    void refresh(const void *data, unsigned width, unsigned height, size_t pitch);
    void new_frame();
    void draw(unsigned width, unsigned height);
    void present();
    void capture(const fs::path &path);
    void verify_software(const void *data, unsigned width, unsigned height, size_t pitch);
    bool set_format(retro_pixel_format value);
    Context context() const;
    unsigned width() const;
    unsigned height() const;
    float aspect() const;
    vp viewport(unsigned width, unsigned height) const;
    bool hardware_active() const;
    bool integer_scale = false;
    bool verify_pixels = false;
    uint64_t verified_frames = 0;
    retro_hw_context_type preferred_hardware = RETRO_HW_CONTEXT_OPENGL_CORE;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    static uintptr_t RETRO_CALLCONV framebuffer();
};
