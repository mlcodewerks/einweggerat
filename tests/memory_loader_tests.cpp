#include "common.h"
#include "core_library.h"
#include "../third_party/MemoryModulePP.h"
#include <atomic>
#include <cstdio>
#include <thread>
static void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
int wmain(int argc, wchar_t **argv) {
    bool fixtureMode = argc > 2 && std::wstring(argv[1]) == L"--fixture";
    bool rejectMode = argc > 2 && std::wstring(argv[1]) == L"--reject";
    int pathIndex = fixtureMode || rejectMode ? 2 : 1;
    if (argc <= pathIndex) return 2;
    try {
        if (argc > pathIndex + 1) {
            auto directory = fs::absolute(fs::path(argv[pathIndex + 1]));
            AddDllDirectory(directory.c_str());
        }
        auto path = fs::absolute(fs::path(argv[pathIndex]));
        if (rejectMode) {
            auto data = read_file(path);
            for (unsigned i = 0; i < 24; ++i) {
                auto *module = MemoryLoadLibrary(data.data(), data.size());
                require(!module && GetLastError() == ERROR_DLL_INIT_FAILED, "Failed process attach was not rolled back");
            }
            auto good = read_file(executable_directory() / L"memory-fixture.dll");
            auto *module = MemoryLoadLibrary(good.data(), good.size());
            require(module != nullptr, "Failed attach poisoned the native TLS registry");
            MemoryFreeLibrary(module);
            std::puts("PASS failed attach rollback and subsequent native TLS registration");
            return 0;
        }
        if (!fixtureMode) {
            CoreLibrary library(path);
            auto api = library.symbol("retro_api_version");
            require(api != nullptr, "Missing core API version");
            require(reinterpret_cast<unsigned (*)()>(api)() == RETRO_API_VERSION, "Invalid core API version");
            auto info = reinterpret_cast<void (*)(retro_system_info *)>(library.symbol("retro_get_system_info"));
            require(info != nullptr, "Missing core metadata");
            retro_system_info system{}; info(&system);
            if (auto dependency = library.symbol("fixture_zip_dependency")) {
                require(reinterpret_cast<int (*)()>(dependency)() == 73, "Bundled ZIP dependency did not resolve");
                auto native = reinterpret_cast<int (*)()>(library.symbol("fixture_native_dependency_loaded"));
                require(native && !native(), "Bundled dependency was loaded from disk instead of ZIP memory");
            }
            std::printf("PASS %s: %s %s\n", path.filename().u8string().c_str(), system.library_name, system.library_version);
            return 0;
        }
        auto bytes = read_file(path);
        DWORD nativeEvents[3]{};
        auto baseline = LoadLibraryW((executable_directory() / L"memory-fixture.dll").c_str());
        require(baseline != nullptr, "Cannot load native notification-order baseline");
        auto observeNative = reinterpret_cast<void (*)(DWORD *)>(GetProcAddress(baseline, "fixture_observe"));
        require(observeNative != nullptr, "Missing native observation export");
        observeNative(nativeEvents);
        FreeLibrary(baseline);
        require(nativeEvents[0] == 2, "Native baseline did not deliver both process-detach notifications");
        for (unsigned iteration = 0; iteration < 6; ++iteration) {
            std::atomic<bool> ready{false}, go{false};
            int previous = 0;
            int (*tls)(int) = nullptr;
            std::thread existing([&] { ready = true; while (!go) std::this_thread::yield(); if (tls) previous = tls(82); });
            while (!ready) std::this_thread::yield();
            auto *module = MemoryLoadLibrary(bytes.data(), bytes.size());
            if (!module) { DWORD error = GetLastError(); go = true; existing.join(); throw std::runtime_error("MemoryLoadLibrary error " + std::to_string(error)); }
            tls = reinterpret_cast<int (*)(int)>(MemoryGetProcAddress(module, "fixture_tls"));
            auto caught = reinterpret_cast<int (*)()>(MemoryGetProcAddress(module, "fixture_catch"));
            auto seh = reinterpret_cast<int (*)()>(MemoryGetProcAddress(module, "fixture_seh"));
            auto throwing = reinterpret_cast<void (*)()>(MemoryGetProcAddress(module, "fixture_throw"));
            auto attached = reinterpret_cast<LONG (*)()>(MemoryGetProcAddress(module, "fixture_attached"));
            auto detached = reinterpret_cast<LONG (*)()>(MemoryGetProcAddress(module, "fixture_detached"));
            require(tls && caught && throwing && attached && detached && seh, "Missing fixture exports");
            require(tls(51) == 37 && tls(52) == 51, "Native static TLS template/index/current-thread storage incorrect");
            go = true; existing.join();
            require(previous == 37, "Existing thread did not receive initialized native TLS");
            std::thread fresh([&] { previous = tls(99); }); fresh.join();
            require(previous == 37 && tls(53) == 52, "New-thread native TLS initialization/isolation failed");
            // Native DLL loads may resize every TLS vector. Mapped slots must
            // remain valid, and their indices must not collide with new DLLs.
            auto native = LoadLibraryW((executable_directory() / L"memory-fixture.dll").c_str());
            require(native != nullptr, "Native DLL load failed while mapped TLS was active");
            auto nativeTls = reinterpret_cast<int (*)(int)>(GetProcAddress(native, "fixture_tls"));
            require(nativeTls && nativeTls(91) == 37 && tls(54) == 53, "Native/mapped TLS slot collision");
            FreeLibrary(native);
            auto *tlsDir = reinterpret_cast<IMAGE_TLS_DIRECTORY *>(module->codeBase +
                module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
            if (tlsDir->SizeOfZeroFill >= 4096) {
                auto zero = reinterpret_cast<int (*)(int)>(MemoryGetProcAddress(module, "fixture_zero_fill"));
                require(zero && zero(41) == 0 && zero(42) == 41, "TLS zero fill is missing");
                std::thread zeroThread([&] { previous = zero(73); }); zeroThread.join();
                require(previous == 0 && zero(43) == 42, "TLS zero fill is not initialized/isolated on new threads");
            }
            require(attached() >= 1 && detached() >= 1, "Core TLS thread callbacks missing");
            require(caught() == 1, "C++ exception catch/destructor failed inside mapped DLL");
            auto forwarded = reinterpret_cast<DWORD (WINAPI *)()>(MemoryGetProcAddress(module, "fixture_forward"));
            auto ordinal = reinterpret_cast<int (*)()>(MemoryGetProcAddress(module, MAKEINTRESOURCEA(102)));
            require(forwarded && forwarded() == GetCurrentProcessId(), "Forwarded export failed");
            require(ordinal && ordinal() == 1, "Ordinal-only export failed");
            require(!MemoryGetProcAddress(module, "FIXTURE_CATCH"), "Export lookup is not case sensitive");
            auto *exportDir = reinterpret_cast<IMAGE_EXPORT_DIRECTORY *>(module->codeBase +
                module->headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
            require(!MemoryGetProcAddress(module, MAKEINTRESOURCEA(exportDir->Base + exportDir->NumberOfFunctions)),
                    "Export ordinal boundary accepted an index past the export table");
            require(seh() == 1, "Windows SEH handler did not catch the fixture exception");
            bool escaped = false;
            try { throwing(); } catch (const std::runtime_error &e) { escaped = std::string(e.what()) == "mapped C++ exception"; }
            require(escaped, "C++ exception did not unwind from DLL into host");
#ifdef _WIN64
            DWORD64 imageBase;
            auto pc = reinterpret_cast<DWORD64>(caught);
            require(RtlLookupFunctionEntry(pc, &imageBase, nullptr) != nullptr, "Unwind table not registered");
#else
            require(module->functionTable != nullptr, "Native x86 exception cache not registered");
            auto *exceptionCache = static_cast<const DWORD *>(module->functionTable);
            DWORD exceptionCount = exceptionCache[0];
#endif
            DWORD mappedEvents[3]{};
            auto observe = reinterpret_cast<void (*)(DWORD *)>(MemoryGetProcAddress(module, "fixture_observe"));
            require(observe != nullptr, "Missing mapped observation export");
            observe(mappedEvents);
            MemoryFreeLibrary(module);
            require(std::equal(std::begin(nativeEvents), std::end(nativeEvents), std::begin(mappedEvents)),
                    "TLS/DllMain process-detach order differs from native Windows loading");
#ifdef _WIN64
            require(!RtlLookupFunctionEntry(pc, &imageBase, nullptr), "Unwind table survived unload");
#else
            require(exceptionCache[0] + 1 == exceptionCount, "Native x86 exception cache entry survived unload");
#endif
        }
        auto smallBytes = read_file(executable_directory() / L"memory-fixture.dll");
        std::vector<PMEMORYMODULE> concurrent;
        for (unsigned i = 0; i < 24; ++i) {
            auto *module = MemoryLoadLibrary(smallBytes.data(), smallBytes.size());
            require(module != nullptr, "In-memory TLS retained the old 16-module limit");
            concurrent.push_back(module);
            auto tls = reinterpret_cast<int (*)(int)>(MemoryGetProcAddress(module, "fixture_tls"));
            require(tls && tls(100 + i) == 37, "Concurrent mapped TLS template/index is incorrect");
        }
        for (unsigned i = 0; i < concurrent.size(); ++i) {
            auto tls = reinterpret_cast<int (*)(int)>(MemoryGetProcAddress(concurrent[i], "fixture_tls"));
            require(tls(0) == int(100 + i), "Concurrent mapped modules share static TLS storage");
        }
        for (auto i = concurrent.rbegin(); i != concurrent.rend(); ++i) MemoryFreeLibrary(*i);
        for (size_t size : {size_t(0), size_t(1), size_t(63), size_t(128), size_t(512)})
            require(!MemoryLoadLibrary(bytes.data(), size), "Truncated PE accepted");
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(bytes.data());
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(bytes.data() + dos->e_lfanew);
#ifdef _WIN64
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
#else
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
#endif
        require(!MemoryLoadLibrary(bytes.data(), bytes.size()) && GetLastError() == ERROR_BAD_EXE_FORMAT, "Opposite-architecture DLL was not rejected");
        std::puts("PASS native TLS, callbacks, C++ exceptions/destructors, SEH, native coexistence, reload and architecture checks");
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
