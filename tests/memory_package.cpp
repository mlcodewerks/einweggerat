#include "common.h"
extern "C" __declspec(dllimport) int fixture_dependency();
extern "C" {
RETRO_API unsigned retro_api_version() { return RETRO_API_VERSION; }
RETRO_API void retro_get_system_info(retro_system_info *info) {
    *info = {"ZIP dependency fixture", "1", "bin", false, false};
}
__declspec(dllexport) int fixture_zip_dependency() { return fixture_dependency(); }
__declspec(dllexport) int fixture_native_dependency_loaded() { return GetModuleHandleW(L"memory-dependency.dll") != nullptr; }
}
