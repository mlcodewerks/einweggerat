#include "shader_gl.h"
#include "shader_runtime.h"
#ifndef USE_RPI
#include "glad.h"
#include <array>
#include <vector>

namespace
{
    struct gl_state
    {
        GLint program, vao, array, uniform, read, draw, active, viewport[4], scissor[4], polygon[2];
        GLboolean color[4];
        const GLenum flags[14] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_FRAMEBUFFER_SRGB, GL_RASTERIZER_DISCARD,
                                  GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE, GL_SAMPLE_MASK, GL_PRIMITIVE_RESTART, GL_COLOR_LOGIC_OP, GL_DITHER, GL_PRIMITIVE_RESTART_FIXED_INDEX};
        GLboolean enabled[14];
        GLint unpack_buffer, unpack[6], blend[6];
        const GLenum unpack_keys[6] = {GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_IMAGES};
        const GLenum blend_keys[6] = {GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA, GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA};
        std::vector<std::array<GLint, 2>> textures;
        struct binding
        {
            GLint buffer;
            GLint64 start, size;
        };
        std::vector<binding> uniforms;
        gl_state()
        {
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
            glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &uniform);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetIntegerv(GL_SCISSOR_BOX, scissor);
            glGetIntegerv(GL_POLYGON_MODE, polygon);
            glGetBooleanv(GL_COLOR_WRITEMASK, color);
            for (int i = 0; i < 14; ++i)
            {
                enabled[i] = glIsEnabled(flags[i]);
                glDisable(flags[i]);
            }
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            for (int i = 0; i < 6; ++i)
            {
                glGetIntegerv(unpack_keys[i], &unpack[i]);
                glPixelStorei(unpack_keys[i], i == 0 ? 1 : 0);
                glGetIntegerv(blend_keys[i], &blend[i]);
            }
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            GLint n;
            glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &n);
            textures.resize(n);
            for (int i = 0; i < n; ++i)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i][0]);
                glGetIntegeri_v(GL_SAMPLER_BINDING, i, &textures[i][1]);
            }
            glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &n);
            uniforms.resize(n);
            for (int i = 0; i < n; ++i)
            {
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, i, &uniforms[i].buffer);
                glGetInteger64i_v(GL_UNIFORM_BUFFER_START, i, &uniforms[i].start);
                glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, i, &uniforms[i].size);
            }
        }
        ~gl_state()
        {
            for (size_t i = 0; i < textures.size(); ++i)
            {
                glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
                glBindTexture(GL_TEXTURE_2D, textures[i][0]);
                glBindSampler(static_cast<GLuint>(i), textures[i][1]);
            }
            for (size_t i = 0; i < uniforms.size(); ++i)
            {
                auto &b = uniforms[i];
                if (b.buffer && b.size)
                    glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(i), b.buffer, b.start, b.size);
                else
                    glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(i), b.buffer);
            }
            glActiveTexture(active);
            glUseProgram(program);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, array);
            glBindBuffer(GL_UNIFORM_BUFFER, uniform);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            glPolygonMode(GL_FRONT_AND_BACK, polygon[0]);
            glColorMask(color[0], color[1], color[2], color[3]);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
            for (int i = 0; i < 6; ++i)
                glPixelStorei(unpack_keys[i], unpack[i]);
            glBlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
            glBlendEquationSeparate(blend[4], blend[5]);
            for (int i = 0; i < 14; ++i)
                if (enabled[i])
                    glEnable(flags[i]);
                else
                    glDisable(flags[i]);
        }
    };
    struct target
    {
        GLuint texture = 0, fbo = 0;
        unsigned width = 0, height = 0;
        void clear()
        {
            if (fbo)
                glDeleteFramebuffers(1, &fbo);
            if (texture)
                glDeleteTextures(1, &texture);
            *this = {};
        }
        bool resize(unsigned w, unsigned h)
        {
            if (w == width && h == height && texture)
                return true;
            clear();
            width = w;
            height = h;
            glCreateTextures(GL_TEXTURE_2D, 1, &texture);
            glTextureStorage2D(texture, 1, GL_RGBA8, w, h);
            glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTextureParameteri(texture, GL_TEXTURE_SWIZZLE_A, GL_ONE);
            glCreateFramebuffers(1, &fbo);
            glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, texture, 0);
            return glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
    };
    // Explicit teardown while a context is current; no GL calls during static destruction.
    shader_runtime runtime;
    target input;
    std::vector<target> outputs;
}
unsigned shader_gl_render(unsigned source_fbo, unsigned iw, unsigned ih, unsigned ow, unsigned oh)
{
    auto &control = video_shaders();
    control.init();
    if (!librashader().gl_available || !control.settings.enabled || !iw || !ih || !ow || !oh)
        return 0;
    gl_state saved;
    if (!runtime.prepare(false))
        return 0;
    runtime.update_parameters();
    if (!input.resize(iw, ih))
    {
        control.error = "Cannot allocate shader input texture.";
        return 0;
    }
    glBlitNamedFramebuffer(source_fbo, input.fbo, 0, 0, iw, ih, 0, 0, iw, ih, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    while (outputs.size() > runtime.chains.size())
    {
        outputs.back().clear();
        outputs.pop_back();
    }
    outputs.resize(runtime.chains.size());
    libra_image_gl_t source{input.texture, GL_RGBA8, iw, ih};
    const auto options = runtime.frame_options<frame_gl_opt_t>();
    for (size_t i = 0; i < runtime.chains.size(); ++i)
    {
        auto &output = outputs[i];
        if (!output.resize(ow, oh))
        {
            control.error = "Cannot allocate shader output texture.";
            return 0;
        }
        libra_image_gl_t destination{output.texture, GL_RGBA8, ow, oh};
        if (!librashader().check(librashader().gl_filter_chain_frame(&runtime.chains[i]->gl, runtime.frame,
                                                                     source, destination, nullptr, nullptr, &options),
                                 control.error))
        {
            control.settings.enabled = false;
            return 0;
        }
        source = destination;
    }
    ++runtime.frame;
    return outputs.back().fbo;
}
void shader_gl_destroy()
{
    runtime.clear();
    input.clear();
    for (auto &output : outputs)
        output.clear();
    outputs.clear();
}
#else
unsigned shader_gl_render(unsigned, unsigned, unsigned, unsigned, unsigned) { return 0; }
void shader_gl_destroy() {}
#endif
