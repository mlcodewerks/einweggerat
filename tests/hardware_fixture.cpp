#include "common.h"
#include "libretro_vulkan.h"
#ifdef interface
#undef interface
#endif
static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_hw_render_callback hw{};
static bool initialized = false;
#if FIXTURE_HARDWARE == 1
using BindFramebuffer = void(APIENTRY *)(unsigned, unsigned);
using ClearColor = void(APIENTRY *)(float, float, float, float);
using Clear = void(APIENTRY *)(unsigned);
using GetError = unsigned(APIENTRY *)();
static void reset() { initialized = true; }
static void destroy() { initialized = false; }
void fixture_hardware_run() {
    if (!initialized)
        throw std::runtime_error("GL context_reset was not called");
    auto bind = reinterpret_cast<BindFramebuffer>(hw.get_proc_address("glBindFramebuffer"));
    auto color = reinterpret_cast<ClearColor>(hw.get_proc_address("glClearColor"));
    auto clear = reinterpret_cast<Clear>(hw.get_proc_address("glClear"));
    auto error = reinterpret_cast<GetError>(hw.get_proc_address("glGetError"));
    if (!bind || !color || !clear || !error || !hw.get_current_framebuffer())
        throw std::runtime_error("WGL hardware interface incomplete");
    bind(0x8d40, unsigned(hw.get_current_framebuffer()));
    color(0.2f, 0.6f, 0.9f, 1);
    clear(0x00004000 | 0x00000100 | 0x00000400);
    if (error())
        throw std::runtime_error("GL hardware rendering error");
    video(RETRO_HW_FRAME_BUFFER_VALID, 320, 240, 0);
    video(nullptr, 0, 0, 0);
}
#else
static const retro_hw_render_interface_vulkan *api = nullptr;
static VkImage image = VK_NULL_HANDLE;
static VkDeviceMemory memory = VK_NULL_HANDLE;
static VkImageView view = VK_NULL_HANDLE;
static VkCommandPool pool = VK_NULL_HANDLE;
static VkCommandBuffer command = VK_NULL_HANDLE;
static VkFence fence = VK_NULL_HANDLE;
static bool first = true;
static retro_vulkan_image frame_image{};
#define DEVICE_CALLS(X)                                                                                      \
    X(CreateImage)                                                                                           \
    X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)         \
        X(CreateImageView) X(DestroyImageView) X(CreateCommandPool) X(DestroyCommandPool)                    \
            X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer)        \
                X(CmdPipelineBarrier) X(CmdClearColorImage) X(QueueSubmit) X(QueueWaitIdle) X(CreateFence)   \
                    X(DestroyFence) X(WaitForFences) X(ResetFences)
#define DECLARE(name) static PFN_vk##name call##name;
DEVICE_CALLS(DECLARE)
#undef DECLARE
static void vk_check(VkResult result) {
    if (result < 0)
        throw std::runtime_error("Vulkan hardware fixture error " + std::to_string(result));
}
static void reset() {
    const retro_hw_render_interface *base = nullptr;
    if (!environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &base) || !base ||
        base->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN)
        throw std::runtime_error("Vulkan interface missing");
    api = reinterpret_cast<const retro_hw_render_interface_vulkan *>(base);
#define LOAD(name)                                                                                           \
    call##name = reinterpret_cast<PFN_vk##name>(api->get_device_proc_addr(api->device, "vk" #name));         \
    if (!call##name)                                                                                         \
        throw std::runtime_error("Missing Vulkan function");
    DEVICE_CALLS(LOAD)
#undef LOAD
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {320, 240, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vk_check(callCreateImage(api->device, &info, nullptr, &image));
    VkMemoryRequirements requirements{};
    callGetImageMemoryRequirements(api->device, image, &requirements);
    auto properties_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        api->get_instance_proc_addr(api->instance, "vkGetPhysicalDeviceMemoryProperties"));
    VkPhysicalDeviceMemoryProperties properties{};
    properties_fn(api->gpu, &properties);
    unsigned type = 0;
    while (type < properties.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type)))
        ++type;
    if (type == properties.memoryTypeCount)
        throw std::runtime_error("No image memory type");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    vk_check(callAllocateMemory(api->device, &allocation, nullptr, &memory));
    vk_check(callBindImageMemory(api->device, image, memory, 0));
    frame_image = {};
    frame_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    frame_image.create_info.image = image;
    frame_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    frame_image.create_info.format = info.format;
    frame_image.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(callCreateImageView(api->device, &frame_image.create_info, nullptr, &view));
    frame_image.image_view = view;
    frame_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    p.queueFamilyIndex = api->queue_index;
    p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_check(callCreateCommandPool(api->device, &p, nullptr, &pool));
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = pool;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    vk_check(callAllocateCommandBuffers(api->device, &a, &command));
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    f.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vk_check(callCreateFence(api->device, &f, nullptr, &fence));
    first = true;
    initialized = true;
}
static void destroy() {
    if (!api)
        return;
    api->lock_queue(api->handle);
    callQueueWaitIdle(api->queue);
    api->unlock_queue(api->handle);
    if (fence)
        callDestroyFence(api->device, fence, nullptr);
    if (pool)
        callDestroyCommandPool(api->device, pool, nullptr);
    if (view)
        callDestroyImageView(api->device, view, nullptr);
    if (image)
        callDestroyImage(api->device, image, nullptr);
    if (memory)
        callFreeMemory(api->device, memory, nullptr);
    fence = VK_NULL_HANDLE;
    pool = VK_NULL_HANDLE;
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    api = nullptr;
    initialized = false;
}
void fixture_hardware_run() {
    if (!initialized)
        throw std::runtime_error("Vulkan context_reset was not called");
    api->wait_sync_index(api->handle);
    vk_check(callWaitForFences(api->device, 1, &fence, VK_TRUE, UINT64_MAX));
    vk_check(callResetFences(api->device, 1, &fence));
    vk_check(callResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(callBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = first ? 0 : VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    callCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue color{{0.2f, 0.6f, 0.9f, 1}};
    callCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                           &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    callCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
    vk_check(callEndCommandBuffer(command));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    api->lock_queue(api->handle);
    auto result = callQueueSubmit(api->queue, 1, &submit, fence);
    api->unlock_queue(api->handle);
    vk_check(result);
    api->set_image(api->handle, &frame_image, 0, nullptr, api->queue_index);
    video(RETRO_HW_FRAME_BUFFER_VALID, 320, 240, 0);
    video(nullptr, 0, 0, 0);
    first = false;
}
#endif
bool fixture_hardware_load(retro_environment_t env, retro_video_refresh_t cb) {
    environment = env;
    video = cb;
    hw = {};
    hw.context_reset = reset;
    hw.context_destroy = destroy;
#if FIXTURE_HARDWARE == 1
    hw.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    hw.version_major = 4;
    hw.version_minor = 5;
    hw.depth = hw.stencil = true;
    hw.bottom_left_origin = true;
#else
    hw.context_type = RETRO_HW_CONTEXT_VULKAN;
#endif
    return environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw);
}
