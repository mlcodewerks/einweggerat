#pragma once
#include "shader_library.h"
#include "shader_chain.h"
#include <memory>
#include "native.h"

// All methods require the owning GL context or externally synchronized Vulkan device.
class shader_runtime {
public:
    struct chain {
        bool vulkan = false;
        size_t index = 0;
        bool normalizes_input = false;
        libra_gl_filter_chain_t gl = nullptr;
        libra_vk_filter_chain_t vk = nullptr;
        std::map<std::string, float> parameters;
        ~chain();
    };
    std::vector<std::unique_ptr<chain>> chains;
    uint64_t attempted = 0, applied = 0;
    size_t frame = 0;
    uint64_t timestamp = 0;
    template<class Options> Options frame_options() {
        Options options{};
        // New filter chains initialize their own history. Clearing on their
        // first GL frame can target history FBOs before lazy allocation.
        options.version = LIBRASHADER_CURRENT_VERSION;
        options.frame_direction = 1; options.total_subframes = options.current_subframe = 1;
        options.aspect_ratio = video_shaders().source_aspect; options.frames_per_second = video_shaders().source_fps;
        const auto now = native_nanoseconds();
        options.frametime_delta = timestamp ? static_cast<uint32_t>((now - timestamp) / 1000000) : 0;
        timestamp = now; return options;
    }
    bool prepare(bool vulkan, libra_device_vk_t device = {}, VkComponentMapping components = {});
    void update_parameters();
    void clear();
};
