#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#define LIBRA_RUNTIME_OPENGL
#define LIBRA_RUNTIME_VULKAN
#include "librashader.h"
#include <filesystem>
#include <string>

#define SHADER_COMMON_FUNCTIONS(X) \
 X(instance_abi_version) X(instance_api_version) X(error_write) X(error_free) X(error_free_string) \
 X(preset_create) X(preset_free) X(preset_get_runtime_params) X(preset_free_runtime_params) X(preset_get_param)
#define SHADER_BACKEND_FUNCTIONS(X, B) \
 X(B##_filter_chain_create) X(B##_filter_chain_free) X(B##_filter_chain_frame) X(B##_filter_chain_set_param)
struct shader_library {
#define DECLARE(N) PFN_libra_##N N = nullptr;
    SHADER_COMMON_FUNCTIONS(DECLARE)
    SHADER_BACKEND_FUNCTIONS(DECLARE, gl)
    SHADER_BACKEND_FUNCTIONS(DECLARE, vk)
#undef DECLARE
    bool gl_available = false, vk_available = false;
    std::string status;
    void load(const std::filesystem::path &directory);
    bool check(libra_error_t, std::string &);
private:
    void *module = nullptr; // Kept loaded until process exit; backend destructors use its API.
};
shader_library &librashader();
