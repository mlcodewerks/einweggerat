#include "video.h"
#include "glad.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "native.h"
#include "shader_chain.h"
#include "shader_gl.h"
#include <cmath>
#include <fstream>
namespace {
using CreateContext = HGLRC(WINAPI *)(HDC);
using DeleteContext = BOOL(WINAPI *)(HGLRC);
using MakeCurrent = BOOL(WINAPI *)(HDC, HGLRC);
using GetProc = PROC(WINAPI *)(LPCSTR);
HMODULE gl_module = nullptr;
GetProc wgl_proc = nullptr;
CreateContext wgl_create = nullptr;
DeleteContext wgl_delete = nullptr;
MakeCurrent wgl_current = nullptr;
Video *active_video = nullptr;
void load_gl() {
    if (gl_module)
        return;
    gl_module = LoadLibraryExW(L"opengl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gl_module)
        throw std::runtime_error("OpenGL runtime is unavailable");
    wgl_proc = reinterpret_cast<GetProc>(GetProcAddress(gl_module, "wglGetProcAddress"));
    wgl_create = reinterpret_cast<CreateContext>(GetProcAddress(gl_module, "wglCreateContext"));
    wgl_delete = reinterpret_cast<DeleteContext>(GetProcAddress(gl_module, "wglDeleteContext"));
    wgl_current = reinterpret_cast<MakeCurrent>(GetProcAddress(gl_module, "wglMakeCurrent"));
    if (!wgl_proc || !wgl_create || !wgl_delete || !wgl_current)
        throw std::runtime_error("Incomplete WGL runtime");
}
} // namespace
void *native_gl_proc(const char *name) {
    if (!gl_module)
        return nullptr;
    PROC value = wgl_proc ? wgl_proc(name) : nullptr;
    auto integer = reinterpret_cast<intptr_t>(value);
    if (!value || integer == 1 || integer == 2 || integer == 3 || integer == -1)
        value = GetProcAddress(gl_module, name);
    return reinterpret_cast<void *>(value);
}
struct Video::Impl {
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC gl = nullptr;
    Context api = Context::OpenGL;
    bool imgui = false, has_frame = false, core_started = false;
    retro_hw_render_callback hw{};
    retro_pixel_format format = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_game_geometry geom{320, 240, 320, 240, 4.0f / 3};
    GLuint texture = 0, fbo = 0, depth = 0;
    unsigned tex_w = 0, tex_h = 0, frame_w = 0, frame_h = 0;
    VulkanRenderer vk;
    void allocate(unsigned width, unsigned height) {
        if (!width || !height || width > 16384 || height > 16384)
            throw std::runtime_error("Invalid framebuffer size");
        if (texture)
            glDeleteTextures(1, &texture);
        glCreateTextures(GL_TEXTURE_2D, 1, &texture);
        glTextureStorage2D(texture, 1, hw.context_reset ? GL_RGBA8 : GL_RGB8, width, height);
        glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (!fbo)
            glCreateFramebuffers(1, &fbo);
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, texture, 0);
        if (hw.depth) {
            if (!depth)
                glCreateRenderbuffers(1, &depth);
            glNamedRenderbufferStorage(depth, hw.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24, width,
                                       height);
            glNamedFramebufferRenderbuffer(
                fbo, hw.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        }
        if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Incomplete OpenGL framebuffer");
        tex_w = width;
        tex_h = height;
    }
    void start_gl() {
        load_gl();
        dc = GetDC(window);
        if (!GetPixelFormat(dc)) {
            PIXELFORMATDESCRIPTOR p{};
            p.nSize = sizeof(p);
            p.nVersion = 1;
            p.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            p.iPixelType = PFD_TYPE_RGBA;
            p.cColorBits = 32;
            p.cAlphaBits = 8;
            p.cDepthBits = 24;
            p.cStencilBits = 8;
            int pixel = ChoosePixelFormat(dc, &p);
            if (!pixel || !SetPixelFormat(dc, pixel, &p))
                throw std::runtime_error("Cannot set WGL pixel format");
        }
        HGLRC bootstrap = wgl_create(dc);
        if (!bootstrap || !wgl_current(dc, bootstrap))
            throw std::runtime_error("Cannot create WGL bootstrap context");
        using Attribs = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
        auto create = reinterpret_cast<Attribs>(native_gl_proc("wglCreateContextAttribsARB"));
        bool core = hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
        unsigned major = std::max(4u, hw.version_major),
                 minor = hw.version_major > 4 ? hw.version_minor : std::max(5u, hw.version_minor);
        const int attributes[] = {0x2091, int(major), 0x2092, int(minor), 0x9126, core ? 1 : 2, 0};
        gl = create ? create(dc, nullptr, attributes) : nullptr;
        wgl_current(nullptr, nullptr);
        wgl_delete(bootstrap);
        if (!gl || !wgl_current(dc, gl))
            throw std::runtime_error("OpenGL 4.5 desktop context unavailable");
        if (!gladLoadGL([](const char *name) { return reinterpret_cast<GLADapiproc>(native_gl_proc(name)); }))
            throw std::runtime_error("OpenGL entry point loading failed");
        using Interval = BOOL(WINAPI *)(int);
        if (auto interval = reinterpret_cast<Interval>(native_gl_proc("wglSwapIntervalEXT")))
            interval(0);
        if (!ImGui_ImplOpenGL3_Init("#version 450"))
            throw std::runtime_error("ImGui OpenGL initialization failed");
        imgui = true;
    }
    void release_gl() {
        if (gl) {
            wgl_current(dc, gl);
            shader_gl_destroy();
            if (imgui)
                ImGui_ImplOpenGL3_Shutdown();
            if (texture)
                glDeleteTextures(1, &texture);
            if (depth)
                glDeleteRenderbuffers(1, &depth);
            if (fbo)
                glDeleteFramebuffers(1, &fbo);
            wgl_current(nullptr, nullptr);
            wgl_delete(gl);
        }
        texture = fbo = depth = 0;
        gl = nullptr;
        imgui = false;
        tex_w = tex_h = 0;
        if (dc)
            ReleaseDC(window, dc);
        dc = nullptr;
    }
};
Video::Video() : impl(std::make_unique<Impl>()) {}
Video::~Video() { shutdown(); }
Context Video::context() const { return impl->api; }
unsigned Video::width() const { return impl->frame_w ? impl->frame_w : impl->geom.base_width; }
unsigned Video::height() const { return impl->frame_h ? impl->frame_h : impl->geom.base_height; }
float Video::aspect() const {
    return impl->geom.aspect_ratio > 0 ? impl->geom.aspect_ratio : float(width()) / std::max(1u, height());
}
bool Video::hardware_active() const { return impl->hw.context_reset != nullptr; }
void Video::init(HWND window, Context api) {
    impl->window = window;
    active_video = this;
    select(api);
}
void Video::select(Context api) {
    if (impl->core_started)
        throw std::runtime_error("Close the core before changing rendering context");
    impl->release_gl();
    impl->vk.shutdown();
    impl->api = api;
    impl->has_frame = false;
    if (api == Context::OpenGL)
        impl->start_gl();
    else if (!impl->vk.init(impl->window) || !impl->vk.init_imgui())
        throw std::runtime_error(impl->vk.error());
}
void Video::shutdown() {
    if (!impl)
        return;
    stop_core();
    impl->release_gl();
    impl->vk.shutdown();
    if (active_video == this)
        active_video = nullptr;
}
uintptr_t RETRO_CALLCONV Video::framebuffer() { return active_video ? active_video->impl->fbo : 0; }
bool Video::hardware(retro_hw_render_callback *callback) {
    if (!callback || impl->core_started || !callback->context_reset)
        return false;
    auto type = callback->context_type;
    if (type != RETRO_HW_CONTEXT_OPENGL && type != RETRO_HW_CONTEXT_OPENGL_CORE &&
        type != RETRO_HW_CONTEXT_VULKAN)
        return false;
    callback->get_current_framebuffer = framebuffer;
    callback->get_proc_address = +[](const char *name) -> retro_proc_address_t {
        return reinterpret_cast<retro_proc_address_t>(native_gl_proc(name));
    };
    impl->hw = *callback;
    try {
        select(type == RETRO_HW_CONTEXT_VULKAN ? Context::Vulkan : Context::OpenGL);
        return true;
    } catch (...) {
        impl->hw = {};
        throw;
    }
}
bool Video::environment(unsigned command, void *data) {
    return impl->api == Context::Vulkan && impl->vk.environment(command, data);
}
void Video::start_core(const retro_game_geometry &geom) {
    geometry(geom, true);
    impl->has_frame = false;
    if (impl->api == Context::Vulkan && !impl->vk.init_core())
        throw std::runtime_error(impl->vk.error());
    impl->core_started = true;
    if (impl->hw.context_reset)
        impl->hw.context_reset();
}
void Video::stop_core() {
    if (impl->core_started && impl->hw.context_destroy)
        impl->hw.context_destroy();
    impl->core_started = false;
    if (impl->api == Context::Vulkan)
        impl->vk.destroy_core();
    else if (impl->gl) {
        shader_gl_destroy();
        if (impl->fbo)
            glDeleteFramebuffers(1, &impl->fbo);
        if (impl->texture)
            glDeleteTextures(1, &impl->texture);
        if (impl->depth)
            glDeleteRenderbuffers(1, &impl->depth);
        impl->fbo = impl->texture = impl->depth = 0;
    }
    impl->hw = {};
    impl->frame_w = impl->frame_h = 0;
    impl->has_frame = false;
}
void Video::geometry(const retro_game_geometry &geom, bool allocate) {
    if (!geom.base_width || !geom.base_height || geom.base_width > 16384 || geom.base_height > 16384 ||
        !std::isfinite(geom.aspect_ratio))
        throw std::runtime_error("Invalid core geometry");
    impl->geom.base_width = geom.base_width;
    impl->geom.base_height = geom.base_height;
    impl->geom.aspect_ratio = geom.aspect_ratio;
    if (allocate) {
        impl->geom.max_width = std::max(geom.max_width, geom.base_width);
        impl->geom.max_height = std::max(geom.max_height, geom.base_height);
        if (impl->api == Context::OpenGL)
            impl->allocate(std::max(impl->tex_w, impl->geom.max_width),
                           std::max(impl->tex_h, impl->geom.max_height));
    }
}
bool Video::set_format(retro_pixel_format value) {
    if (value != RETRO_PIXEL_FORMAT_XRGB8888 && value != RETRO_PIXEL_FORMAT_RGB565 &&
        value != RETRO_PIXEL_FORMAT_0RGB1555)
        return false;
    impl->format = value;
    impl->vk.set_software_pixel_format(value);
    return true;
}
void Video::refresh(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data)
        return;
    if (!w || !h || w > 16384 || h > 16384)
        throw std::runtime_error("Invalid video frame dimensions");
    if (impl->api == Context::Vulkan) {
        if (!impl->vk.refresh(data, w, h, pitch, impl->format))
            throw std::runtime_error(impl->vk.error());
    } else if (data != RETRO_HW_FRAME_BUFFER_VALID) {
        const unsigned bpp = impl->format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
        if (pitch < size_t(w) * bpp || pitch > SIZE_MAX / h)
            throw std::runtime_error("Invalid software frame pitch");
        if (w > impl->tex_w || h > impl->tex_h)
            impl->allocate(std::max(w, impl->tex_w), std::max(h, impl->tex_h));
        GLint old_pbo, alignment, row_length, skip_pixels, skip_rows, swap_bytes;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &old_pbo);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SWAP_BYTES, &swap_bytes);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
        GLenum format = impl->format == RETRO_PIXEL_FORMAT_RGB565 ? GL_RGB : GL_BGRA;
        GLenum type = impl->format == RETRO_PIXEL_FORMAT_XRGB8888 ? GL_UNSIGNED_BYTE
                      : impl->format == RETRO_PIXEL_FORMAT_RGB565 ? GL_UNSIGNED_SHORT_5_6_5
                                                                  : GL_UNSIGNED_SHORT_1_5_5_5_REV;
        if (pitch % bpp == 0 && pitch / bpp <= INT_MAX) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / bpp));
            glTextureSubImage2D(impl->texture, 0, 0, 0, w, h, format, type, data);
        } else {
            // Odd byte pitches cannot be expressed as GL pixel row lengths.
            for (unsigned y = 0; y < h; ++y)
                glTextureSubImage2D(impl->texture, 0, 0, y, w, 1, format, type,
                                    static_cast<const uint8_t *>(data) + y * pitch);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, swap_bytes);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, old_pbo);
    } else if (!impl->hw.context_reset)
        throw std::runtime_error("Unexpected hardware frame");
    impl->frame_w = w;
    impl->frame_h = h;
    impl->has_frame = true;
    if (verify_pixels && data != RETRO_HW_FRAME_BUFFER_VALID)
        verify_software(data, w, h, pitch);
}
void Video::new_frame() {
    if (impl->api == Context::OpenGL)
        ImGui_ImplOpenGL3_NewFrame();
    else if (!impl->vk.new_frame())
        throw std::runtime_error(impl->vk.error());
}
vp Video::viewport(unsigned w, unsigned h) const {
    float ratio = aspect();
    unsigned dw = w, dh = unsigned(w / ratio);
    if (dh > h) {
        dh = h;
        dw = unsigned(h * ratio);
    }
    if (integer_scale && width() && height()) {
        unsigned scale = std::min(w / width(), h / height());
        if (scale) {
            dw = width() * scale;
            dh = height() * scale;
        }
    }
    return {(w - dw) / 2, (h - dh) / 2, dw, dh};
}
void Video::draw(unsigned w, unsigned h) {
    const auto viewport = this->viewport(w, h);
    const auto dw = viewport.width, dh = viewport.height;
    const float ratio = aspect();
    video_shaders().source_aspect = ratio;
    if (impl->api == Context::Vulkan) {
        impl->vk.draw(viewport, w, h);
        return;
    }
    GLint old_draw;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    const float black[] = {0, 0, 0, 1};
    glClearBufferfv(GL_COLOR, 0, black);
    if (impl->has_frame && dw && dh) {
        GLuint source = shader_gl_render(impl->fbo, width(), height(), dw, dh);
        unsigned sw = source ? dw : width(), sh = source ? dh : height();
        if (!source)
            source = impl->fbo;
        bool flip = !impl->hw.context_reset || !impl->hw.bottom_left_origin;
        GLint y0 = viewport.y, y1 = viewport.y + dh;
        if (flip)
            std::swap(y0, y1);
        glBlitNamedFramebuffer(source, 0, 0, 0, sw, sh, viewport.x, y0, viewport.x + dw, y1,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
}
void Video::present() {
    if (impl->api == Context::Vulkan) {
        if (!impl->vk.present(ImGui::GetDrawData()))
            throw std::runtime_error(impl->vk.error());
        return;
    }
    GLint old;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old);
    if (!SwapBuffers(impl->dc))
        throw std::runtime_error("OpenGL SwapBuffers failed");
}
void Video::capture(const fs::path &path) {
    if (impl->api != Context::OpenGL)
        throw std::runtime_error("Capture currently requires OpenGL");
    int w, h;
    native_client_size(impl->window, w, h);
    if (w <= 0 || h <= 0)
        return;
    GLint old;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = DWORD(file.bfOffBits + pixels.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = w;
    info.biHeight = h;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<char *>(&file), sizeof(file));
    out.write(reinterpret_cast<char *>(&info), sizeof(info));
    out.write(reinterpret_cast<char *>(pixels.data()), pixels.size());
    if (!out)
        throw std::runtime_error("Screenshot write failed");
}
void Video::verify_software(const void *data, unsigned w, unsigned h, size_t pitch) {
    std::vector<uint8_t> pixels(size_t(w) * h * 4);
    if (impl->api == Context::Vulkan) {
        unsigned actual_w = 0, actual_h = 0;
        if (!impl->vk.readback(pixels, actual_w, actual_h))
            throw std::runtime_error(impl->vk.error());
        if (actual_w != w || actual_h != h)
            throw std::runtime_error("Vulkan readback dimensions differ");
    } else {
        GLint old;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, impl->fbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, old);
    }
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            auto *row = static_cast<const uint8_t *>(data) + y * pitch;
            uint32_t value = 0;
            unsigned r, g, b;
            if (impl->format == RETRO_PIXEL_FORMAT_XRGB8888) {
                std::memcpy(&value, row + x * 4, 4);
                r = (value >> 16) & 255;
                g = (value >> 8) & 255;
                b = value & 255;
            } else {
                uint16_t color;
                std::memcpy(&color, row + x * 2, 2);
                r = (color >> (impl->format == RETRO_PIXEL_FORMAT_RGB565 ? 11 : 10)) & 31;
                g = (color >> 5) & (impl->format == RETRO_PIXEL_FORMAT_RGB565 ? 63 : 31);
                b = color & 31;
                r = (r * 255 + 15) / 31;
                g = (g * 255 + (impl->format == RETRO_PIXEL_FORMAT_RGB565 ? 31 : 15)) /
                    (impl->format == RETRO_PIXEL_FORMAT_RGB565 ? 63 : 31);
                b = (b * 255 + 15) / 31;
            }
            auto *p = pixels.data() + (size_t(y) * w + x) * 4;
            if (std::abs(int(p[0]) - int(r)) > 1 || std::abs(int(p[1]) - int(g)) > 1 ||
                std::abs(int(p[2]) - int(b)) > 1)
                throw std::runtime_error("Software frame verification failed");
        }
    if (impl->api == Context::OpenGL && glGetError() != GL_NO_ERROR)
        throw std::runtime_error("OpenGL error during software verification");
    ++verified_frames;
}
