#include "common.h"
#include <stdexcept>
extern "C" const IMAGE_TLS_DIRECTORY _tls_used;
#ifdef _WIN64
static constexpr unsigned tls_vector_offset = 0x58;
#else
static constexpr unsigned tls_vector_offset = 0x2C;
#endif
__attribute__((section(".tls$AAC"), used)) static int initial = 37;
#ifdef FIXTURE_LARGE_TLS
__attribute__((section(".tls$AAD"), used)) static BYTE large_tls[5 * 1024 * 1024];
#endif
static volatile LONG attached, detached;
static DWORD *detach_events;
static void record(DWORD kind, DWORD reason) {
    if (reason == DLL_PROCESS_DETACH && detach_events && detach_events[0] < 2)
        detach_events[++detach_events[0]] = kind;
}
static int &value() {
    auto **vector = *reinterpret_cast<void ***>(reinterpret_cast<BYTE *>(NtCurrentTeb()) + tls_vector_offset);
    DWORD index = *reinterpret_cast<DWORD *>(_tls_used.AddressOfIndex);
    auto offset = reinterpret_cast<uintptr_t>(&initial) - _tls_used.StartAddressOfRawData;
    return *reinterpret_cast<int *>(static_cast<BYTE *>(vector[index]) + offset);
}
static void NTAPI callback(PVOID, DWORD reason, PVOID) {
    record(1, reason);
    if (reason == DLL_THREAD_ATTACH) InterlockedIncrement(&attached);
    if (reason == DLL_THREAD_DETACH) InterlockedIncrement(&detached);
}
__attribute__((section(".CRT$XLB"), used)) static PIMAGE_TLS_CALLBACK tls_callback = callback;
extern "C" {
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, PVOID) {
    record(2, reason);
#ifdef FIXTURE_FAIL_ATTACH
    if (reason == DLL_PROCESS_ATTACH) return FALSE;
#endif
    return TRUE;
}
__declspec(dllexport) void fixture_observe(DWORD *events) { detach_events = events; }
__declspec(dllexport) int fixture_tls(int next) { int previous = value(); value() = next; return previous; }
__declspec(dllexport) int fixture_zero_fill(int next) {
    auto **vector = *reinterpret_cast<void ***>(reinterpret_cast<BYTE *>(NtCurrentTeb()) + tls_vector_offset);
    DWORD index = *reinterpret_cast<DWORD *>(_tls_used.AddressOfIndex);
    auto offset = _tls_used.EndAddressOfRawData - _tls_used.StartAddressOfRawData + 2048;
    auto &slot = *reinterpret_cast<int *>(static_cast<BYTE *>(vector[index]) + offset);
    int previous = slot; slot = next; return previous;
}
__declspec(dllexport) LONG fixture_attached() { return attached; }
__declspec(dllexport) LONG fixture_detached() { return detached; }
__declspec(dllexport) void fixture_throw() { throw std::runtime_error("mapped C++ exception"); }
__declspec(dllexport) int fixture_catch() {
    int destroyed = 0;
    struct Guard { int &n; ~Guard() { ++n; } };
    try { Guard guard{destroyed}; fixture_throw(); }
    catch (const std::exception &e) { return destroyed == 1 && std::string(e.what()) == "mapped C++ exception"; }
    return 0;
}
}
