#include <windows.h>
#include <stdint.h>
extern const DWORD fixture_seh_table[];
/* Supply real SafeSEH metadata; do not rely on debugger-relaxed dispatch. */
__attribute__((used)) const IMAGE_LOAD_CONFIG_DIRECTORY32 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY32),
    .SEHandlerTable = (DWORD)(uintptr_t)fixture_seh_table,
    .SEHandlerCount = 1
};
