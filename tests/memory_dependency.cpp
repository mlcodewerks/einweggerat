#include <windows.h>
extern "C" __declspec(dllexport) int fixture_dependency() { return GetCurrentProcessId() ? 73 : 0; }
