#include "shader_runtime.h"
#include "native.h"
#include <algorithm>
#include <cmath>
#include <fstream>

shader_runtime::chain::~chain()
{
    auto &api = librashader();
    std::string ignored;
    if (gl)
        api.check(api.gl_filter_chain_free(&gl), ignored);
    if (vk)
        api.check(api.vk_filter_chain_free(&vk), ignored);
}
void shader_runtime::clear()
{
    chains.clear();
    attempted = applied = 0;
    frame = 0;
    timestamp = 0;
}
bool shader_runtime::prepare(bool vulkan, libra_device_vk_t device, VkComponentMapping components)
try
{
    auto &control = video_shaders();
    control.init();
    auto &api = librashader();
    bool available = vulkan ? api.vk_available : api.gl_available;
    if (!available || !control.settings.enabled) return false;
    if (attempted == control.revision)
        return !chains.empty();
    attempted = control.revision;
    std::vector<std::unique_ptr<chain>> next;
    shader_chain_config normalization;
    auto source_path = shader_temporary_preset();
    source_path.replace_extension(".slang");
    struct remove_temporary
    {
        std::filesystem::path path;
        ~remove_temporary()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{source_path};
    const bool has_passes = std::any_of(control.settings.chains.begin(), control.settings.chains.end(), [](const shader_chain_config &c)
                                        { return c.enabled && std::any_of(c.passes.begin(), c.passes.end(), [](const shader_pass &p)
                                                                          { return p.enabled; }); });
    if (vulkan && has_passes)
    {
        // The C API accepts a VkImage, not the core's VkImageView. Apply its RGB
        // swizzle and force opaque alpha before the user's Original/History input.
        auto component = [](VkComponentSwizzle swizzle, const char *identity) -> std::string
        {
            switch (swizzle)
            {
            case VK_COMPONENT_SWIZZLE_ZERO:
                return "0.0";
            case VK_COMPONENT_SWIZZLE_ONE:
                return "1.0";
            case VK_COMPONENT_SWIZZLE_R:
                return "color.r";
            case VK_COMPONENT_SWIZZLE_G:
                return "color.g";
            case VK_COMPONENT_SWIZZLE_B:
                return "color.b";
            case VK_COMPONENT_SWIZZLE_A:
                return "color.a";
            default:
                return identity;
            }
        };
        std::ofstream source(source_path);
        source << "#version 450\nlayout(std140,set=0,binding=0) uniform UBO { mat4 MVP; } global;\n"
                  "#pragma stage vertex\nlayout(location=0) in vec4 Position; layout(location=1) in vec2 TexCoord;\n"
                  "layout(location=0) out vec2 uv; void main(){gl_Position=global.MVP*Position;uv=TexCoord;}\n"
                  "#pragma stage fragment\nlayout(location=0) in vec2 uv; layout(location=0) out vec4 FragColor;\n"
                  "layout(set=0,binding=2) uniform sampler2D Source; void main(){vec4 color=texture(Source,uv);FragColor=vec4("
               << component(components.r, "color.r") << ',' << component(components.g, "color.g") << ','
               << component(components.b, "color.b") << ",1.0);}\n";
        source.close();
        if (!source)
        {
            control.error = "Cannot prepare the shader input conversion.";
            return !chains.empty();
        }
        normalization.passes.push_back({source_path, true, {{"filter_linear", "false"}}});
    }
    for (size_t entry = 0; entry < control.settings.chains.size() + 1; ++entry)
    {
        if (entry == 0 && normalization.passes.empty())
            continue;
        const size_t i = entry - 1;
        auto &config = entry == 0 ? normalization : control.settings.chains[i];
        if (!config.enabled || std::none_of(config.passes.begin(), config.passes.end(), [](const shader_pass &p)
                                            { return p.enabled; }))
            continue;
        auto path = shader_temporary_preset();
        if (!shader_export(path, config, control.error))
            return !chains.empty();
        libra_shader_preset_t preset = nullptr;
        bool ok = api.check(api.preset_create(path.u8string().c_str(), &preset), control.error);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        if (!ok)
        {
            control.compilation_failed(config);
            return !chains.empty();
        }
        auto instance = std::make_unique<chain>();
        instance->index = i;
        instance->vulkan = vulkan;
        instance->normalizes_input = entry == 0;
        libra_preset_param_list_t params{};
        ok = api.check(api.preset_get_runtime_params(&preset, &params), control.error);
        if (ok)
        {
            for (uint64_t p = 0; p < params.length; ++p)
            {
                float value = params.parameters[p].initial;
                api.check(api.preset_get_param(&preset, params.parameters[p].name, &value), control.error);
                instance->parameters[params.parameters[p].name] = value;
            }
            api.check(api.preset_free_runtime_params(params), control.error);
            if (vulkan)
            {
                filter_chain_vk_opt_t options{};
                options.version = LIBRASHADER_CURRENT_VERSION;
                options.frames_in_flight = 2;
                options.use_dynamic_rendering = false;
                ok = api.check(api.vk_filter_chain_create(&preset, device, &options, &instance->vk), control.error);
            }
            else
            {
                filter_chain_gl_opt_t options{};
                options.version = LIBRASHADER_CURRENT_VERSION;
                // The desktop frontend already requires OpenGL 4.5 DSA for its
                // framebuffers. Use the matching librashader runtime and cache.
                options.glsl_version = 450;
                options.use_dsa = true;
                auto loader = +[](const char *name) -> const void *
                { return reinterpret_cast<const void *>(native_gl_proc(name)); };
                ok = api.check(api.gl_filter_chain_create(&preset, loader, &options, &instance->gl), control.error);
            }
        }
        if (preset)
        {
            std::string cleanup;
            api.check(api.preset_free(&preset), cleanup);
        }
        if (!ok)
        {
            control.compilation_failed(config);
            return !chains.empty();
        }
        next.push_back(std::move(instance));
    }
    chains.swap(next);
    applied = attempted;
    frame = 0;
    timestamp = 0;
    control.error.clear();
    control.diagnostics.clear();
    return !chains.empty();
}
catch (const std::exception &e)
{
    video_shaders().error = e.what();
    return !chains.empty();
}
void shader_runtime::update_parameters()
{
    auto &control = video_shaders();
    auto &api = librashader();
    if (applied != control.revision)
        return; // Failed structural edits retain the complete previous state.
    for (auto &chain : chains)
        for (auto &parameter : chain->parameters)
        {
            if (chain->normalizes_input)
                continue;
            const auto &values = control.settings.chains[chain->index].values;
            auto found = values.find(parameter.first);
            if (found == values.end() || found->second == parameter.second || !std::isfinite(found->second))
                continue;
            libra_error_t error;
            error = chain->vulkan ? api.vk_filter_chain_set_param(&chain->vk, parameter.first.c_str(), found->second) : api.gl_filter_chain_set_param(&chain->gl, parameter.first.c_str(), found->second);
            if (api.check(error, control.error))
                parameter.second = found->second;
        }
}
