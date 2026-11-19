#include "render_backends.h"
#include "volk.h"
#include "shader_runtime.h"
#include "native.h"
#include "libretro_vulkan.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Confine the Windows COM macro collision to this backend translation unit.
#ifdef interface
#undef interface
#endif

namespace
{
    void check(VkResult result)
    {
        if (result < 0)
            throw std::runtime_error("Vulkan error " + std::to_string(result));
    }

    void add_extension(std::vector<const char *> &extensions, const char *name)
    {
        for (auto existing : extensions)
            if (std::strcmp(existing, name) == 0)
                return;
        extensions.push_back(name);
    }
}

struct VulkanRenderer::Impl
{
    static constexpr uint32_t frames_in_flight = 2;
    static constexpr uint32_t software_buffer_count = 3;
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    uint32_t api_version = VK_API_VERSION_1_1;
    ImGui_ImplVulkanH_Window swapchain;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool capture_pending = false;
    bool imgui = false;
    bool rebuild = false;
    bool negotiated = false;
    std::string last_error;
    std::recursive_mutex queue_mutex;
    retro_hw_render_context_negotiation_interface_vulkan negotiation{};
    retro_hw_render_interface_vulkan interface{};

    // Capture is submitted in refresh(), but waited only on resource reuse or
    // the core's wait_sync_index callback. Presentation can run ahead separately.
    const retro_vulkan_image *core_image = nullptr;
    retro_vulkan_image last_core_image{};
    std::vector<VkSemaphore> waits;
    std::vector<VkCommandBuffer> core_commands;
    std::vector<VkCommandBuffer> submit_commands;
    std::vector<VkPipelineStageFlags> wait_stages;
    uint32_t source_family = VK_QUEUE_FAMILY_IGNORED;
    VkSemaphore signal = VK_NULL_HANDLE;
    VkImage texture = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory = VK_NULL_HANDLE;
    VkImageView texture_view = VK_NULL_HANDLE;
    VkDescriptorSet texture_set = VK_NULL_HANDLE;
    VkFormat texture_format = VK_FORMAT_UNDEFINED;
    VkComponentMapping texture_components{};
    uint32_t texture_width = 0, texture_height = 0;
    VkImageLayout texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    struct software_slot
    {
        VkDeviceSize offset = 0;
        bool loaned = false;
        unsigned width = 0;
        unsigned height = 0;
        size_t pitch = 0;
        retro_pixel_format format = RETRO_PIXEL_FORMAT_0RGB1555;
    };
    VkBuffer software_buffer = VK_NULL_HANDLE;
    VkDeviceMemory software_memory = VK_NULL_HANDLE;
    VkDeviceSize software_buffer_size = 0;
    VkDeviceSize software_slot_stride = 0;
    uint8_t *software_mapped = nullptr;
    VkMemoryPropertyFlags software_memory_properties = 0;
    software_slot software_slots[software_buffer_count]{};
    int32_t submitted_software_slot = -1;
    int32_t active_software_slot = -1;
    uint32_t software_buffer_cursor = 0;
    retro_pixel_format software_pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    bool has_frame = false;
    shader_runtime shaders;
    struct shader_target
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        unsigned width = 0, height = 0;
    };
    std::vector<shader_target> shader_outputs;
    unsigned shader_width = 0, shader_height = 0;
    bool shader_ready = false;

    void clear_shader_outputs()
    {
        for (auto &output : shader_outputs)
        {
            if (output.set && imgui)
                ImGui_ImplVulkan_RemoveTexture(output.set);
            if (output.view)
                vkDestroyImageView(device, output.view, nullptr);
            if (output.image)
                vkDestroyImage(device, output.image, nullptr);
            if (output.memory)
                vkFreeMemory(device, output.memory, nullptr);
        }
        shader_outputs.clear();
        shader_ready = false;
    }

    VkDescriptorSet prepare_shaders(unsigned width, unsigned height)
    {
        shader_ready = false;
        auto &control = video_shaders();
        control.init();
        if (!librashader().vk_available || !control.settings.enabled)
            return texture_set;
        std::lock_guard<std::recursive_mutex> lock(queue_mutex);
        if (shaders.attempted != control.revision)
            idle();
        if (!shaders.prepare(true, {gpu, instance, device, queue, vkGetInstanceProcAddr}, texture_components))
            return texture_set;
        if (width != shader_width || height != shader_height || shader_outputs.size() != shaders.chains.size() ||
            (!shader_outputs.empty() && (shader_outputs[0].width != texture_width || shader_outputs[0].height != texture_height)))
        {
            idle();
            clear_shader_outputs();
            shader_width = width;
            shader_height = height;
            try
            {
                shader_outputs.resize(shaders.chains.size());
                for (size_t i = 0; i < shader_outputs.size(); ++i)
                {
                    auto &output = shader_outputs[i];
                    output.width = shaders.chains[i]->normalizes_input ? texture_width : width;
                    output.height = shaders.chains[i]->normalizes_input ? texture_height : height;
                    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                    info.imageType = VK_IMAGE_TYPE_2D;
                    info.format = VK_FORMAT_R8G8B8A8_UNORM;
                    info.extent = {output.width, output.height, 1};
                    info.mipLevels = info.arrayLayers = 1;
                    info.samples = VK_SAMPLE_COUNT_1_BIT;
                    info.tiling = VK_IMAGE_TILING_OPTIMAL;
                    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                    check(vkCreateImage(device, &info, nullptr, &output.image));
                    VkMemoryRequirements requirements;
                    vkGetImageMemoryRequirements(device, output.image, &requirements);
                    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    allocation.allocationSize = requirements.size;
                    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    check(vkAllocateMemory(device, &allocation, nullptr, &output.memory));
                    check(vkBindImageMemory(device, output.image, output.memory, 0));
                    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    view.image = output.image;
                    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    view.format = info.format;
                    view.components.a = VK_COMPONENT_SWIZZLE_ONE;
                    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    check(vkCreateImageView(device, &view, nullptr, &output.view));
                    output.set = ImGui_ImplVulkan_AddTexture(output.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    if (!output.set)
                        throw std::runtime_error("Cannot allocate shader texture descriptor.");
                }
            }
            catch (const std::exception &e)
            {
                clear_shader_outputs();
                control.error = e.what();
                return texture_set;
            }
        }
        shader_ready = true;
        return shader_outputs.back().set;
    }

    bool record_shaders(VkCommandBuffer buffer)
    {
        if (!shader_ready)
            return true;
        shaders.update_parameters();
        libra_image_vk_t input{texture, texture_format, texture_width, texture_height};
        const auto options = shaders.frame_options<frame_vk_opt_t>();
        auto transition = [&](shader_target &output, VkImageLayout layout, VkAccessFlags src, VkAccessFlags dst)
        {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = output.layout;
            barrier.newLayout = layout;
            barrier.srcAccessMask = src;
            barrier.dstAccessMask = dst;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = output.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            output.layout = layout;
        };
        for (size_t i = 0; i < shader_outputs.size(); ++i)
        {
            auto &output = shader_outputs[i];
            transition(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       output.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            libra_image_vk_t destination{output.image, VK_FORMAT_R8G8B8A8_UNORM, output.width, output.height};
            bool ok = librashader().check(librashader().vk_filter_chain_frame(&shaders.chains[i]->vk, buffer,
                                                                              shaders.frame, input, destination, nullptr, nullptr, &options),
                                          video_shaders().error);
            transition(output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            if (!ok)
                return false;
            input = destination;
        }
        return true;
    }

    void idle()
    {
        std::lock_guard<std::recursive_mutex> lock(queue_mutex);
        if (device)
            check(vkDeviceWaitIdle(device));
        capture_pending = false;
        submitted_software_slot = -1;
    }

    void wait_capture()
    {
        if (capture_pending)
        {
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            capture_pending = false;
            submitted_software_slot = -1;
        }
    }

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags,
                         VkMemoryPropertyFlags preferred = 0,
                         VkMemoryPropertyFlags *selected = nullptr)
    {
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
        uint32_t fallback = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        {
            if (!(bits & (1u << i)))
                continue;
            const auto available = properties.memoryTypes[i].propertyFlags;
            if ((available & flags) != flags)
                continue;
            if (fallback == UINT32_MAX)
                fallback = i;
            if (!preferred || (available & preferred) == preferred)
            {
                if (selected)
                    *selected = available;
                return i;
            }
        }
        if (fallback != UINT32_MAX)
        {
            if (selected)
                *selected = properties.memoryTypes[fallback].propertyFlags;
            return fallback;
        }
        throw std::runtime_error("No compatible Vulkan memory type");
    }

    static size_t software_bpp(retro_pixel_format format)
    {
        return format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4u : 2u;
    }

    static VkFormat software_vk_format(retro_pixel_format format)
    {
        switch (format)
        {
        case RETRO_PIXEL_FORMAT_XRGB8888:
            // Native little-endian XRGB8888 bytes are B,G,R,X.
            return VK_FORMAT_B8G8R8A8_UNORM;
        case RETRO_PIXEL_FORMAT_RGB565:
            return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case RETRO_PIXEL_FORMAT_0RGB1555:
            // Libretro 0RRRRRGGGGGBBBBB matches A1R5G5B5 with alpha ignored.
            return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        default:
            return VK_FORMAT_UNDEFINED;
        }
    }

    bool software_format_supported(retro_pixel_format format) const
    {
        if (!gpu)
            return false;
        const VkFormat vk_format = software_vk_format(format);
        if (vk_format == VK_FORMAT_UNDEFINED)
            return false;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(gpu, vk_format, &properties);
        const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        return (properties.optimalTilingFeatures & required) == required;
    }

    static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment)
    {
        if (!alignment)
            return value;
        const VkDeviceSize remainder = value % alignment;
        if (!remainder)
            return value;
        const VkDeviceSize add = alignment - remainder;
        if (value > UINT64_MAX - add)
            throw std::runtime_error("Vulkan software buffer size overflow");
        return value + add;
    }

    void destroy_software_storage()
    {
        if (software_mapped)
            vkUnmapMemory(device, software_memory);
        software_mapped = nullptr;
        if (software_buffer)
            vkDestroyBuffer(device, software_buffer, nullptr);
        if (software_memory)
            vkFreeMemory(device, software_memory, nullptr);
        software_buffer = VK_NULL_HANDLE;
        software_memory = VK_NULL_HANDLE;
        software_buffer_size = 0;
        software_slot_stride = 0;
        software_memory_properties = 0;
        for (auto &slot : software_slots)
            slot = {};
        submitted_software_slot = -1;
        active_software_slot = -1;
        software_buffer_cursor = 0;
    }

    void clear_software_buffers()
    {
        wait_capture();
        destroy_software_storage();
    }

    void ensure_software_storage(VkDeviceSize size)
    {
        if (software_buffer && software_slot_stride >= size)
            return;

        // Growing the shared allocation invalidates every mapped region. Callers
        // must not do this while a framebuffer pointer is still loaned to a core.
        for (const auto &slot : software_slots)
            if (slot.loaned)
                throw std::runtime_error("Cannot resize Vulkan software buffer while framebuffer is loaned");

        wait_capture();
        destroy_software_storage();

        VkPhysicalDeviceProperties physical{};
        vkGetPhysicalDeviceProperties(gpu, &physical);
        const VkDeviceSize copy_alignment =
            std::max<VkDeviceSize>(4, physical.limits.optimalBufferCopyOffsetAlignment);
        const VkDeviceSize stride = align_up(size, copy_alignment);
        if (stride > UINT64_MAX / software_buffer_count)
            throw std::runtime_error("Vulkan software buffer size overflow");
        const VkDeviceSize total_size = stride * software_buffer_count;

        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = total_size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device, &info, nullptr, &software_buffer));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, software_buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &software_memory_properties);
        check(vkAllocateMemory(device, &allocation, nullptr, &software_memory));
        check(vkBindBufferMemory(device, software_buffer, software_memory, 0));

        void *mapped = nullptr;
        check(vkMapMemory(device, software_memory, 0, allocation.allocationSize, 0, &mapped));
        software_mapped = static_cast<uint8_t *>(mapped);
        software_buffer_size = total_size;
        software_slot_stride = stride;

        for (uint32_t i = 0; i < software_buffer_count; ++i)
            software_slots[i].offset = stride * i;
    }

    int32_t next_software_slot(VkDeviceSize size, int32_t avoid = -1)
    {
        ensure_software_storage(size);

        for (uint32_t n = 0; n < software_buffer_count; ++n)
        {
            const uint32_t index = (software_buffer_cursor + n) % software_buffer_count;
            auto &slot = software_slots[index];
            if ((int32_t)index == avoid || slot.loaned ||
                ((int32_t)index == submitted_software_slot && capture_pending))
                continue;
            if (slot.offset > software_buffer_size || size > software_buffer_size - slot.offset)
                throw std::runtime_error("Vulkan software buffer region exceeds allocation");
            software_buffer_cursor = (index + 1) % software_buffer_count;
            return (int32_t)index;
        }

        // Only one capture submission can be in flight in this renderer. If all
        // other regions are unavailable, completing it frees the submitted region.
        wait_capture();
        for (uint32_t n = 0; n < software_buffer_count; ++n)
        {
            const uint32_t index = (software_buffer_cursor + n) % software_buffer_count;
            auto &slot = software_slots[index];
            if ((int32_t)index == avoid || slot.loaned)
                continue;
            if (slot.offset > software_buffer_size || size > software_buffer_size - slot.offset)
                throw std::runtime_error("Vulkan software buffer region exceeds allocation");
            software_buffer_cursor = (index + 1) % software_buffer_count;
            return (int32_t)index;
        }
        return -1;
    }

    void release_software_loans()
    {
        for (auto &slot : software_slots)
            slot.loaned = false;
        active_software_slot = -1;
    }

    bool get_software_framebuffer(retro_framebuffer *fb)
    {
        if (!device || !fb || !fb->width || !fb->height ||
            !(fb->access_flags & RETRO_MEMORY_ACCESS_WRITE))
            return false;

        // Prefer the format selected through SET_PIXEL_FORMAT. If the device
        // cannot sample/copy that packed format directly, XRGB8888 is a legal
        // alternative for GET_CURRENT_SOFTWARE_FRAMEBUFFER and needs no CPU
        // conversion in this renderer either.
        retro_pixel_format chosen = software_pixel_format;
        if (!software_format_supported(chosen))
            chosen = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!software_format_supported(chosen))
            return false;

        const size_t bpp = software_bpp(chosen);
        if (size_t(fb->width) > SIZE_MAX / fb->height / bpp)
            return false;
        const size_t pitch = size_t(fb->width) * bpp;
        const VkDeviceSize size = VkDeviceSize(pitch) * fb->height;

        // Never resize the one shared mapping while a pointer from an earlier
        // query in this retro_run() may still be held by the core.
        if (active_software_slot >= 0 && software_slot_stride < size)
            return false;

        int32_t index = active_software_slot;
        if (index < 0 || index == submitted_software_slot)
            index = next_software_slot(size, active_software_slot);
        else if (!software_buffer || software_slot_stride < size)
            index = next_software_slot(size, active_software_slot);
        if (index < 0)
            return false;

        auto &slot = software_slots[index];
        slot.loaned = true;
        slot.width = fb->width;
        slot.height = fb->height;
        slot.pitch = pitch;
        slot.format = chosen;
        active_software_slot = index;

        fb->data = software_mapped + slot.offset;
        fb->pitch = pitch;
        fb->format = chosen;
        fb->memory_flags = (software_memory_properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                               ? RETRO_MEMORY_TYPE_CACHED
                               : 0;
        return true;
    }

    int32_t direct_software_slot(const void *data, unsigned width,
                                 unsigned height, size_t pitch)
    {
        if (!software_mapped || !data)
            return -1;
        for (uint32_t i = 0; i < software_buffer_count; ++i)
        {
            auto &slot = software_slots[i];
            if (slot.loaned && static_cast<const void *>(software_mapped + slot.offset) == data &&
                slot.width == width && slot.height == height && slot.pitch == pitch)
                return (int32_t)i;
        }
        return -1;
    }

    void clear_texture()
    {
        if (texture_set && imgui)
            ImGui_ImplVulkan_RemoveTexture(texture_set);
        texture_set = VK_NULL_HANDLE;
        if (texture_view)
            vkDestroyImageView(device, texture_view, nullptr);
        if (texture)
            vkDestroyImage(device, texture, nullptr);
        if (texture_memory)
            vkFreeMemory(device, texture_memory, nullptr);
        texture_view = VK_NULL_HANDLE;
        texture = VK_NULL_HANDLE;
        texture_memory = VK_NULL_HANDLE;
        texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        has_frame = false;
    }

    void stop_imgui()
    {
        idle();
        shaders.clear();
        clear_shader_outputs();
        if (imgui)
        {
            if (texture_set)
                ImGui_ImplVulkan_RemoveTexture(texture_set);
            texture_set = VK_NULL_HANDLE;
            ImGui_ImplVulkan_Shutdown();
            imgui = false;
        }
    }

    void destroy_device()
    {
        stop_imgui();
        clear_texture();
        clear_software_buffers();
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(device, pool, nullptr);
        fence = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        command = VK_NULL_HANDLE;
        if (device)
            ImGui_ImplVulkanH_DestroyWindow(instance, device, &swapchain, nullptr);
        if (negotiated && negotiation.destroy_device)
            negotiation.destroy_device();
        negotiated = false;
        if (device)
            vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
        if (swapchain.Surface)
            vkDestroySurfaceKHR(instance, swapchain.Surface, nullptr);
        swapchain = ImGui_ImplVulkanH_Window();
        if (instance)
            vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        interface = {};
    }

    static VkInstance create_instance(void *opaque, const VkInstanceCreateInfo *input)
    {
        auto &self = *static_cast<Impl *>(opaque);
        auto info = *input;
        std::vector<const char *> extensions;
        for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
            extensions.push_back(info.ppEnabledExtensionNames[i]);
        add_extension(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
        add_extension(extensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        info.enabledExtensionCount = (uint32_t)extensions.size();
        info.ppEnabledExtensionNames = extensions.data();
        VkApplicationInfo app = info.pApplicationInfo ? *info.pApplicationInfo : VkApplicationInfo{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = std::max(app.apiVersion, self.api_version);
        self.api_version = app.apiVersion;
        info.pApplicationInfo = &app;
        VkInstance result = VK_NULL_HANDLE;
        if (vkCreateInstance(&info, nullptr, &result) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return result;
    }

    static VkDevice create_device(VkPhysicalDevice gpu, void *, const VkDeviceCreateInfo *input)
    {
        auto info = *input;
        std::vector<const char *> extensions;
        for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
            extensions.push_back(info.ppEnabledExtensionNames[i]);
        add_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        info.enabledExtensionCount = (uint32_t)extensions.size();
        info.ppEnabledExtensionNames = extensions.data();
        VkDevice result = VK_NULL_HANDLE;
        if (vkCreateDevice(gpu, &info, nullptr, &result) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return result;
    }

    void resize(int width, int height)
    {
        idle();
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance, gpu, device, &swapchain,
                                               family, nullptr, width, height, 2, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        swapchain.FrameIndex = swapchain.SemaphoreIndex = 0;
        rebuild = false;
    }

    void setup()
    {
        check(volkInitialize());
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "einweggerat";
        app.apiVersion = VK_API_VERSION_1_1;
        if (negotiation.get_application_info)
            if (auto requested = negotiation.get_application_info())
                app = *requested;
        api_version = std::max(app.apiVersion, uint32_t(VK_API_VERSION_1_1));
        app.apiVersion = api_version;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (negotiation.interface_version >= 2 && negotiation.create_instance)
        {
            instance = negotiation.create_instance(vkGetInstanceProcAddr, &app, create_instance, this);
            negotiated = instance != VK_NULL_HANDLE;
        }
        else
            instance = create_instance(this, &info);
        if (!instance)
            throw std::runtime_error("Unable to create Vulkan instance");
        volkLoadInstance(instance);
        VkWin32SurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface.hinstance = GetModuleHandleW(nullptr); surface.hwnd = window;
        check(vkCreateWin32SurfaceKHR(instance, &surface, nullptr, &swapchain.Surface));

        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        gpu = VK_NULL_HANDLE;
        for (auto candidate : devices)
        {
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> properties(n);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, properties.data());
            for (uint32_t i = 0; i < n; ++i)
            {
                VkBool32 present = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, swapchain.Surface, &present));
                const auto flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if (present && (properties[i].queueFlags & flags) == flags)
                {
                    gpu = candidate;
                    family = i;
                    break;
                }
            }
            if (gpu)
                break;
        }
        if (!gpu)
            throw std::runtime_error("No Vulkan graphics/compute queue can present to this window");

        retro_vulkan_context context{};
        bool created = false;
        const char *required = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (negotiation.interface_version >= 2 && negotiation.create_device2)
        {
            created = negotiation.create_device2(&context, instance, gpu, swapchain.Surface,
                                                 vkGetInstanceProcAddr, create_device, this);
            if (!created)
                created = negotiation.create_device2(&context, instance, VK_NULL_HANDLE, swapchain.Surface,
                                                     vkGetInstanceProcAddr, create_device, this);
        }
        else if (negotiation.create_device)
        {
            VkPhysicalDeviceFeatures features{};
            created = negotiation.create_device(&context, instance, gpu, swapchain.Surface,
                                                vkGetInstanceProcAddr, &required, 1, nullptr, 0, &features);
        }
        if (created)
        {
            negotiated = true;
            gpu = context.gpu;
            device = context.device;
            queue = context.queue;
            family = context.queue_family_index;
            present_queue = context.presentation_queue;
            // The swapchain helper creates exclusive images for the graphics queue.
            if (context.presentation_queue_family_index != family)
                throw std::runtime_error("Vulkan requires a shared graphics/presentation queue family");
        }
        else
        {
            float priority = 1.f;
            VkDeviceQueueCreateInfo qinfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qinfo.queueFamilyIndex = family;
            qinfo.queueCount = 1;
            qinfo.pQueuePriorities = &priority;
            VkDeviceCreateInfo dinfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            dinfo.queueCreateInfoCount = 1;
            dinfo.pQueueCreateInfos = &qinfo;
            device = create_device(gpu, this, &dinfo);
            if (!device)
                throw std::runtime_error("Unable to create Vulkan device");
            vkGetDeviceQueue(device, family, 0, &queue);
            present_queue = queue;
        }
        volkLoadDevice(device);
        if (!present_queue)
            present_queue = queue;
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, swapchain.Surface, &caps));
        if (caps.maxImageCount != 0 && caps.maxImageCount < 2)
            throw std::runtime_error("Vulkan presentation requires at least two swapchain images");
        const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        swapchain.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(gpu, swapchain.Surface,
                                                                        formats, 2, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
        swapchain.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(gpu, swapchain.Surface, modes, 3);
        swapchain.ClearValue.color.float32[3] = 1.f;
        int width, height;
        native_client_size(window, width, height);
        resize(std::max(width, 1), std::max(height, 1));
        VkCommandPoolCreateInfo pinfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pinfo.queueFamilyIndex = family;
        pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pinfo, nullptr, &pool));
        VkCommandBufferAllocateInfo ainfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ainfo.commandPool = pool;
        ainfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ainfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ainfo, &command));
        VkFenceCreateInfo finfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &finfo, nullptr, &fence));
        setup_interface();
    }

    void start_imgui()
    {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = api_version;
        info.Instance = instance;
        info.PhysicalDevice = gpu;
        info.Device = device;
        info.QueueFamily = family;
        info.Queue = queue;
        info.DescriptorPoolSize = 32;
        info.MinImageCount = 2;
        info.ImageCount = swapchain.ImageCount;
        info.PipelineInfoMain.RenderPass = swapchain.RenderPass;
        info.CheckVkResultFn = check;
        if (!ImGui_ImplVulkan_Init(&info))
            throw std::runtime_error("Unable to initialize ImGui Vulkan renderer");
        imgui = true;
    }

    void setup_interface()
    {
        interface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
        interface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
        interface.handle = this;
        interface.instance = instance;
        interface.gpu = gpu;
        interface.device = device;
        interface.get_device_proc_addr = vkGetDeviceProcAddr;
        interface.get_instance_proc_addr = vkGetInstanceProcAddr;
        interface.queue = queue;
        interface.queue_index = family;
        interface.set_image = [](void *h, const retro_vulkan_image *image, uint32_t count,
                                 const VkSemaphore *semaphores, uint32_t src_family)
        {
            auto &s = *static_cast<Impl *>(h);
            s.core_image = image;
            s.waits.clear();
            if (count)
                s.waits.assign(semaphores, semaphores + count);
            s.source_family = src_family;
        };
        interface.get_sync_index = [](void *) -> uint32_t
        { return 0; };
        interface.get_sync_index_mask = [](void *) -> uint32_t
        { return 1; };
        interface.set_command_buffers = [](void *h, uint32_t count, const VkCommandBuffer *commands)
        {
            auto &s = *static_cast<Impl *>(h);
            if (count)
                s.core_commands.insert(s.core_commands.end(), commands, commands + count);
        };
        interface.wait_sync_index = [](void *h)
        { static_cast<Impl *>(h)->wait_capture(); };
        interface.lock_queue = [](void *h)
        { static_cast<Impl *>(h)->queue_mutex.lock(); };
        interface.unlock_queue = [](void *h)
        { static_cast<Impl *>(h)->queue_mutex.unlock(); };
        interface.set_signal_semaphore = [](void *h, VkSemaphore semaphore)
        { static_cast<Impl *>(h)->signal = semaphore; };
    }

    void ensure_texture(unsigned width, unsigned height, VkFormat format, VkComponentMapping components)
    {
        if (texture && width == texture_width && height == texture_height && format == texture_format &&
            std::memcmp(&components, &texture_components, sizeof(components)) == 0)
            return;
        idle();
        clear_texture();
        if (std::memcmp(&components, &texture_components, sizeof(components)) != 0)
            shaders.clear();
        texture_width = width;
        texture_height = height;
        texture_format = format;
        texture_components = components;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(device, &info, nullptr, &texture));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, texture, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &texture_memory));
        check(vkBindImageMemory(device, texture, texture_memory, 0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = texture;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.components = components;
        // Libretro's X channel / hardware alpha must not make the game translucent.
        view.components.a = VK_COMPONENT_SWIZZLE_ONE;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &view, nullptr, &texture_view));
        texture_set = ImGui_ImplVulkan_AddTexture(texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    int32_t upload(const void *data, unsigned width, unsigned height,
                   size_t pitch, retro_pixel_format format)
    {
        const size_t bpp = format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
        if (!height || pitch < size_t(width) * bpp || size_t(width) > SIZE_MAX / height / 4)
            throw std::runtime_error("Invalid software video frame dimensions or pitch");

        const VkDeviceSize size = VkDeviceSize(width) * height * 4;
        const int32_t index = next_software_slot(size, active_software_slot);
        if (index < 0)
            throw std::runtime_error("No free Vulkan software upload region");

        auto *output = software_mapped + software_slots[index].offset;
        for (unsigned y = 0; y < height; ++y)
        {
            auto row = static_cast<const uint8_t *>(data) + y * pitch;
            if (format == RETRO_PIXEL_FORMAT_XRGB8888)
            {
                std::memcpy(output, row, size_t(width) * 4);
                output += size_t(width) * 4;
                continue;
            }
            for (unsigned x = 0; x < width; ++x, output += 4)
            {
                uint16_t color;
                std::memcpy(&color, row + x * 2, 2);
                unsigned r = (color >> (format == RETRO_PIXEL_FORMAT_RGB565 ? 11 : 10)) & 31;
                unsigned g = (color >> 5) & (format == RETRO_PIXEL_FORMAT_RGB565 ? 63 : 31);
                unsigned b = color & 31;
                output[0] = uint8_t((r << 3) | (r >> 2));
                output[1] = format == RETRO_PIXEL_FORMAT_RGB565
                                ? uint8_t((g << 2) | (g >> 4))
                                : uint8_t((g << 3) | (g >> 2));
                output[2] = uint8_t((b << 3) | (b >> 2));
                output[3] = 255;
            }
        }
        return index;
    }

    void barrier(VkImage image, VkImageSubresourceRange range, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags src, VkAccessFlags dst, uint32_t src_family = VK_QUEUE_FAMILY_IGNORED,
                 uint32_t dst_family = VK_QUEUE_FAMILY_IGNORED,
                 VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = src_family;
        b.dstQueueFamilyIndex = dst_family;
        b.image = image;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void capture(const void *data, unsigned width, unsigned height, size_t pitch, retro_pixel_format format)
    {
        const bool hardware = data == RETRO_HW_FRAME_BUFFER_VALID;
        int32_t software_source = -1;
        const int32_t direct_index = !hardware && data
                                         ? direct_software_slot(data, width, height, pitch)
                                         : -1;
        const bool direct_software = direct_index >= 0;
        if (direct_software)
            software_source = direct_index;

        if (data && (!width || !height))
            throw std::runtime_error("Empty Vulkan video frame");
        if (hardware && !core_image)
            throw std::runtime_error("Vulkan core did not supply an image");
        if (hardware && core_image == &last_core_image && source_family != family &&
            source_family != VK_QUEUE_FAMILY_IGNORED)
            data = nullptr; // Ownership was returned to the core; duplicate our private copy.
        if (!data && core_commands.empty() && !signal)
        {
            // Duplication reuses the retained texture. Libretro image semaphores
            // are not waited on for duplicates; no GPU submission is needed.
            waits.clear();
            release_software_loans();
            return;
        }

        // If the core requested a framebuffer but ignored it, video_refresh()
        // marks the end of its lifetime. Release that loan before a fallback
        // upload potentially grows the one shared Vulkan buffer allocation.
        if (!direct_software)
            release_software_loans();

        // The capture command pool/fence is single-buffered. Completing the
        // preceding capture frees its region, while the core was able to render
        // this frame into another region of the same persistently mapped buffer.
        wait_capture();
        if (data)
        {
            VkFormat image_format;
            if (hardware)
                image_format = core_image->create_info.format;
            else if (direct_software)
                image_format = software_vk_format(software_slots[software_source].format);
            else if (format == RETRO_PIXEL_FORMAT_XRGB8888)
                image_format = VK_FORMAT_B8G8R8A8_UNORM;
            else
                image_format = VK_FORMAT_R8G8B8A8_UNORM;

            // Present encoded 8-bit colors to our UNORM swapchain without an
            // unintended sRGB decode. Copying these compatible formats preserves bits.
            if (image_format == VK_FORMAT_R8G8B8A8_SRGB)
                image_format = VK_FORMAT_R8G8B8A8_UNORM;
            if (image_format == VK_FORMAT_B8G8R8A8_SRGB)
                image_format = VK_FORMAT_B8G8R8A8_UNORM;
            VkComponentMapping components = hardware ? core_image->create_info.components : VkComponentMapping{};
            ensure_texture(width, height, image_format, components);
            if (!hardware && !direct_software)
                software_source = upload(data, width, height, pitch, format);
        }

        check(vkResetCommandPool(device, pool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin));
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (data)
        {
            barrier(texture, range, texture_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    texture_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            if (hardware)
            {
                const auto &image = *core_image;
                const auto &view = image.create_info;
                const bool ownership = core_commands.empty() && !waits.empty() &&
                                       source_family != VK_QUEUE_FAMILY_IGNORED && source_family != family;
                const uint32_t src = ownership ? source_family : VK_QUEUE_FAMILY_IGNORED;
                const uint32_t dst = ownership ? family : VK_QUEUE_FAMILY_IGNORED;
                // GENERAL permits concurrent core reads and must retain its layout.
                VkImageLayout copy_layout = image.image_layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                if (ownership)
                    barrier(view.image, view.subresourceRange, image.image_layout, image.image_layout,
                            0, VK_ACCESS_TRANSFER_READ_BIT, src, dst);
                barrier(view.image, view.subresourceRange, image.image_layout, copy_layout,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, view.subresourceRange.baseMipLevel,
                                       view.subresourceRange.baseArrayLayer, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {width, height, 1};
                vkCmdCopyImage(command, view.image, copy_layout, texture,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier(view.image, view.subresourceRange, copy_layout, image.image_layout,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
                if (ownership)
                    barrier(view.image, view.subresourceRange, image.image_layout, image.image_layout,
                            VK_ACCESS_TRANSFER_READ_BIT, 0, dst, src);
            }
            else
            {
                if (software_source < 0 || !software_buffer)
                    throw std::runtime_error("Missing Vulkan software upload region");
                VkBufferImageCopy copy{};
                copy.bufferOffset = software_slots[software_source].offset;
                copy.bufferRowLength = 0;
                copy.bufferImageHeight = 0;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(command, software_buffer, texture,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
            barrier(texture, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        check(vkEndCommandBuffer(command));
        submit_commands.assign(core_commands.begin(), core_commands.end());
        submit_commands.push_back(command);
        wait_stages.assign(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (hardware && core_commands.empty())
        {
            submit.waitSemaphoreCount = (uint32_t)waits.size();
            submit.pWaitSemaphores = waits.data();
            submit.pWaitDstStageMask = wait_stages.data();
        }
        submit.commandBufferCount = (uint32_t)submit_commands.size();
        submit.pCommandBuffers = submit_commands.data();
        submit.signalSemaphoreCount = signal ? 1 : 0;
        submit.pSignalSemaphores = &signal;
        check(vkResetFences(device, 1, &fence));
        {
            std::lock_guard<std::recursive_mutex> lock(queue_mutex);
            check(vkQueueSubmit(queue, 1, &submit, fence));
        }
        capture_pending = true;
        submitted_software_slot = !hardware && data ? software_source : -1;
        waits.clear();
        core_commands.clear();
        if (hardware)
        {
            // Keep only the metadata we use, never a core-owned pNext chain.
            // A core may refresh the same image without another set_image call.
            last_core_image = *core_image;
            last_core_image.create_info.pNext = nullptr;
            core_image = &last_core_image;
        }
        signal = VK_NULL_HANDLE;
        if (data)
        {
            texture_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            has_frame = true;
        }
        release_software_loans();
    }

    void present(ImDrawData *draw_data, void (*pace)(void *), void *userdata)
    {
        int width, height;
        native_client_size(window, width, height);
        if (width <= 0 || height <= 0 || IsIconic(window))
        {
            if (pace) pace(userdata);
            return;
        }
        // Keep ImGui's vertex-buffer rotation paired with successful submissions.
        if (draw_data->DisplaySize.x * draw_data->FramebufferScale.x <= 0 ||
            draw_data->DisplaySize.y * draw_data->FramebufferScale.y <= 0)
        {
            if (pace) pace(userdata);
            return;
        }
        if (rebuild || width != swapchain.Width || height != swapchain.Height)
            resize(width, height);
        // Command pools, CPU-updated ImGui buffers and acquire semaphores use a
        // bounded frame ring, independently of the acquired swapchain image.
        auto &frame = swapchain.Frames[swapchain.SemaphoreIndex];
        check(vkWaitForFences(device, 1, &frame.Fence, VK_TRUE, UINT64_MAX));
        auto &semaphores = swapchain.FrameSemaphores[swapchain.SemaphoreIndex];
        VkResult result = vkAcquireNextImageKHR(device, swapchain.Swapchain, UINT64_MAX,
                                                semaphores.ImageAcquiredSemaphore, VK_NULL_HANDLE, &swapchain.FrameIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            rebuild = true;
            if (pace) pace(userdata);
            return;
        }
        check(result);
        rebuild = result == VK_SUBOPTIMAL_KHR;
        // Reacquiring this image guarantees its previous presentation wait has
        // consumed the semaphore. A submission fence alone cannot guarantee that.
        auto &render_complete = swapchain.FrameSemaphores[swapchain.FrameIndex].RenderCompleteSemaphore;
        check(vkResetCommandPool(device, frame.CommandPool, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(frame.CommandBuffer, &begin));
        // Shader work shares the frame fence and graphics queue with ImGui. Each
        // output transition also waits for sampling by the preceding presentation.
        {
            std::lock_guard<std::recursive_mutex> lock(queue_mutex);
            if (!record_shaders(frame.CommandBuffer))
            {
                // Replace the background texture in this frame with the unfiltered
                // image; never present an incomplete shader output after an error.
                const ImTextureID failed = (ImTextureID)shader_outputs.back().set;
                for (int i = 0; i < draw_data->CmdListsCount; ++i)
                    for (auto &draw : draw_data->CmdLists[i]->CmdBuffer)
                        if (draw.GetTexID() == failed)
                            draw.TexRef = ImTextureRef((ImTextureID)texture_set);
                video_shaders().settings.enabled = false;
            }
            VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            pass.renderPass = swapchain.RenderPass;
            pass.framebuffer = swapchain.Frames[swapchain.FrameIndex].Framebuffer;
            pass.renderArea.extent = {uint32_t(swapchain.Width), uint32_t(swapchain.Height)};
            pass.clearValueCount = 1;
            pass.pClearValues = &swapchain.ClearValue;
            vkCmdBeginRenderPass(frame.CommandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
            // ImGui may submit font uploads here. Serialize them with core queue use.
            ImGui_ImplVulkan_RenderDrawData(draw_data, frame.CommandBuffer);
            vkCmdEndRenderPass(frame.CommandBuffer);
            check(vkEndCommandBuffer(frame.CommandBuffer));
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &semaphores.ImageAcquiredSemaphore;
            submit.pWaitDstStageMask = &stage;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &frame.CommandBuffer;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &render_complete;
            check(vkResetFences(device, 1, &frame.Fence));
            check(vkQueueSubmit(queue, 1, &submit, frame.Fence));
            if (shader_ready)
                ++shaders.frame;
        }
        // GPU work can complete while the CPU waits for the presentation time.
        // Release the shared queue lock before sleeping so core workers remain
        // able to submit. Acquiring/recording a frame must not delay this wait.
        if (pace) pace(userdata);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_complete;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain.Swapchain;
        present.pImageIndices = &swapchain.FrameIndex;
        {
            std::lock_guard<std::recursive_mutex> lock(queue_mutex);
            result = vkQueuePresentKHR(present_queue, &present);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            rebuild = true;
        else
            check(result);
        swapchain.SemaphoreIndex = (swapchain.SemaphoreIndex + 1) % frames_in_flight;
    }
};

VulkanRenderer::VulkanRenderer() : impl(std::make_unique<Impl>()) {}
VulkanRenderer::~VulkanRenderer() { shutdown(); }
const char *VulkanRenderer::error() const { return impl->last_error.c_str(); }

bool VulkanRenderer::init(HWND window)
{
    impl->window = window;
    try
    {
        impl->setup();
        return true;
    }
    catch (const std::exception &e)
    {
        impl->last_error = e.what();
        shutdown();
        return false;
    }
}

bool VulkanRenderer::init_imgui()
{
    try
    {
        impl->start_imgui();
        return true;
    }
    catch (const std::exception &e)
    {
        impl->last_error = e.what();
        return false;
    }
}

void VulkanRenderer::shutdown_imgui() { impl->stop_imgui(); }
void VulkanRenderer::shutdown()
{
    try
    {
        impl->destroy_device();
    }
    catch (const std::exception &e)
    {
        native_log("Vulkan shutdown: %s", e.what());
    }
    // The next Vulkan activation may belong to a different, newly loaded DLL.
    impl->negotiation = {};
    impl->core_image = nullptr;
    impl->last_core_image = {};
    impl->waits.clear();
    impl->core_commands.clear();
    impl->signal = VK_NULL_HANDLE;
}
void VulkanRenderer::wait_idle() { impl->idle(); }

bool VulkanRenderer::readback(std::vector<uint8_t> &pixels, unsigned &width, unsigned &height)
{
    auto &s = *impl;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    try {
        s.idle();
        if (!s.texture || (s.texture_format != VK_FORMAT_R8G8B8A8_UNORM &&
            s.texture_format != VK_FORMAT_B8G8R8A8_UNORM))
            throw std::runtime_error("Readback requires an RGBA8 or BGRA8 frame");
        width = s.texture_width; height = s.texture_height;
        pixels.resize(size_t(width) * height * 4);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = pixels.size(); info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(s.device, &info, nullptr, &buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(s.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = s.memory_type(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(s.device, &allocation, nullptr, &memory));
        check(vkBindBufferMemory(s.device, buffer, memory, 0));
        check(vkResetCommandBuffer(s.command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(s.command, &begin));
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = s.texture_layout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = s.texture; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(s.command, s.texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        std::swap(barrier.oldLayout, barrier.newLayout);
        vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(s.command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &s.command;
        {
            std::lock_guard<std::recursive_mutex> lock(s.queue_mutex);
            check(vkQueueSubmit(s.queue, 1, &submit, VK_NULL_HANDLE));
            check(vkQueueWaitIdle(s.queue));
        }
        check(vkMapMemory(s.device, memory, 0, pixels.size(), 0, &mapped));
        std::memcpy(pixels.data(), mapped, pixels.size());
        vkUnmapMemory(s.device, memory); mapped = nullptr;
        vkDestroyBuffer(s.device, buffer, nullptr); buffer = VK_NULL_HANDLE;
        vkFreeMemory(s.device, memory, nullptr); memory = VK_NULL_HANDLE;
        if (s.texture_format == VK_FORMAT_B8G8R8A8_UNORM)
            for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
        return true;
    } catch (const std::exception &e) {
        if (mapped) vkUnmapMemory(s.device, memory);
        if (buffer) vkDestroyBuffer(s.device, buffer, nullptr);
        if (memory) vkFreeMemory(s.device, memory, nullptr);
        s.last_error = e.what(); return false;
    }
}

bool VulkanRenderer::init_core()
{
    try
    {
        if (impl->negotiation.interface_version)
        {
            impl->destroy_device();
            impl->setup();
            impl->start_imgui();
        }
        return true;
    }
    catch (const std::exception &e)
    {
        impl->last_error = e.what();
        native_log("Vulkan core initialization: %s", e.what());
        // Keep the menu usable after a core requests an unsupported device.
        try
        {
            impl->destroy_device();
            impl->negotiation = {};
            impl->setup();
            impl->start_imgui();
        }
        catch (const std::exception &recovery)
        {
            impl->last_error += std::string("; recovery failed: ") + recovery.what();
        }
        return false;
    }
}

void VulkanRenderer::destroy_core()
{
    impl->idle();
    impl->release_software_loans();
    impl->shaders.clear();
    impl->clear_shader_outputs();
    impl->clear_texture();
    impl->core_image = nullptr;
    impl->last_core_image = {};
    impl->waits.clear();
    impl->core_commands.clear();
    impl->signal = VK_NULL_HANDLE;
    if (impl->negotiated && impl->negotiation.destroy_device)
        impl->negotiation.destroy_device();
    impl->negotiated = false;
    impl->negotiation = {};
}

bool VulkanRenderer::environment(unsigned command, void *data)
{
    if (!data)
        return false;
    if (command == RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER)
    {
        try
        {
            return impl->get_software_framebuffer(static_cast<retro_framebuffer *>(data));
        }
        catch (const std::exception &e)
        {
            impl->last_error = e.what();
            native_log("Vulkan software framebuffer: %s", e.what());
            return false;
        }
    }
    if (command == RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE)
    {
        if (!impl->device)
            return false;
        *static_cast<const retro_hw_render_interface **>(data) =
            reinterpret_cast<const retro_hw_render_interface *>(&impl->interface);
        return true;
    }
    if (command == RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT)
    {
        auto info = static_cast<retro_hw_render_context_negotiation_interface *>(data);
        info->interface_version = info->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN ? 2 : 0;
        return true;
    }
    if (command == RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE)
    {
        auto info = static_cast<const retro_hw_render_context_negotiation_interface_vulkan *>(data);
        if (info->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN || !info->interface_version)
            return false;
        impl->negotiation = {};
        // Version 1 callers do not have the trailing version 2 fields.
        std::memcpy(&impl->negotiation, info, info->interface_version < 2 ? offsetof(retro_hw_render_context_negotiation_interface_vulkan, create_instance) : sizeof(*info));
        return true;
    }
    return false;
}

void VulkanRenderer::set_software_pixel_format(retro_pixel_format format)
{
    switch (format)
    {
    case RETRO_PIXEL_FORMAT_0RGB1555:
    case RETRO_PIXEL_FORMAT_XRGB8888:
    case RETRO_PIXEL_FORMAT_RGB565:
        impl->software_pixel_format = format;
        break;
    default:
        break;
    }
}

bool VulkanRenderer::refresh(const void *data, unsigned width, unsigned height, size_t pitch, retro_pixel_format format)
{
    try
    {
        impl->capture(data, width, height, pitch, format);
        return true;
    }
    catch (const std::exception &e)
    {
        impl->last_error = e.what();
        native_log("Vulkan frame: %s", e.what());
        return false;
    }
}

void VulkanRenderer::draw(const vp &viewport, unsigned width, unsigned height)
{
    impl->shader_ready = false;
    if (!impl->has_frame || !impl->texture_set || !width || !height)
        return;
    auto &io = ImGui::GetIO();
    const float sx = io.DisplaySize.x / width, sy = io.DisplaySize.y / height;
    auto list = ImGui::GetBackgroundDrawList();
    auto &platform = ImGui::GetPlatformIO();
    list->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
    // Vulkan images use libretro's top-left origin. bottom_left_origin belongs
    // to the GL interface; some cores leave it set when requesting Vulkan.
    VkDescriptorSet displayed = impl->texture_set;
    try
    {
        displayed = impl->prepare_shaders(viewport.width, viewport.height);
    }
    catch (const std::exception &e)
    {
        video_shaders().error = e.what();
    }
    list->AddImage((ImTextureID)displayed,
                   ImVec2(viewport.x * sx, viewport.y * sy),
                   ImVec2((viewport.x + viewport.width) * sx, (viewport.y + viewport.height) * sy),
                   ImVec2(0, 0), ImVec2(1, 1));
    list->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
}

bool VulkanRenderer::new_frame()
{
    if (!impl->imgui)
        return false;
    ImGui_ImplVulkan_NewFrame();
    return true;
}
bool VulkanRenderer::present(ImDrawData *data, void (*pace)(void *), void *userdata)
{
    try
    {
        impl->present(data, pace, userdata);
        return true;
    }
    catch (const std::exception &e)
    {
        impl->last_error = e.what();
        return false;
    }
}
