#include "shader_library.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

shader_library &librashader()
{
    static shader_library api;
    return api;
}
void shader_library::load(const std::filesystem::path &directory)
{
    if (module)
        return;
#ifdef _WIN32
    auto path = directory / "librashader.dll";
#elif defined(__APPLE__)
    auto path = directory / "librashader.dylib";
#else
    auto path = directory / "librashader.so";
#endif
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        status = "Optional librashader library is absent from the executable directory.";
        return;
    }
#ifdef _WIN32
    // Resolve dependencies only beside the DLL or in Windows system directories.
    module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    auto symbol = [&](const char *name)
    { return module ? reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(module), name)) : nullptr; };
#else
    module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto symbol = [&](const char *name)
    { return module ? dlsym(module, name) : nullptr; };
#endif
    if (!module)
    {
        status = "Cannot load " + path.u8string() + "; check its architecture and dependencies.";
#ifdef _WIN32
        status += " Windows loader error " + std::to_string(GetLastError()) + ".";
#endif
        return;
    }
    bool common = true;
#define LOAD(N)                                               \
    N = reinterpret_cast<PFN_libra_##N>(symbol("libra_" #N)); \
    common = common && N;
    SHADER_COMMON_FUNCTIONS(LOAD)
#undef LOAD
    if (!common || instance_abi_version() != LIBRASHADER_CURRENT_ABI || instance_api_version() < LIBRASHADER_CURRENT_VERSION)
    {
        status = "Incompatible librashader: ABI 2 and API 5 or newer are required.";
        return;
    }
    bool backend = true;
#define LOAD(N)                                               \
    N = reinterpret_cast<PFN_libra_##N>(symbol("libra_" #N)); \
    backend = backend && N;
    SHADER_BACKEND_FUNCTIONS(LOAD, gl)
    gl_available = backend;
#ifdef USE_RPI
    gl_available = false;
#endif
    backend = true;
    SHADER_BACKEND_FUNCTIONS(LOAD, vk)
    vk_available = backend;
#undef LOAD
    status = "librashader: OpenGL " + std::string(gl_available ? "available" : "unavailable") +
             ", Vulkan " + (vk_available ? "available" : "unavailable");
}
bool shader_library::check(libra_error_t failure, std::string &error)
{
    if (!failure)
        return true;
    char *message = nullptr;
    error_write(failure, &message);
    error = message ? message : "librashader failed without an error message.";
    if (message)
        error_free_string(&message);
    error_free(&failure);
    return false;
}
