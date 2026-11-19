#include "core_catalog.h"
#include <cstdio>
#include <fstream>
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    try {
        auto root = fs::absolute(fs::u8path(argv[2]));
        fs::create_directories(root / L"cores");
        auto fixture = fs::absolute(fs::u8path(argv[1]));
        fs::copy_file(fixture, root / L"fixture.DLL", fs::copy_options::overwrite_existing);
        fs::copy_file(fixture, root / L"cores/another.dll", fs::copy_options::overwrite_existing);
        std::ofstream(root / L"not-a-core.dll") << "not a PE DLL";
        CoreCatalog catalog;
        catalog.scan(root);
        require(catalog.cores.size() == 2, "Scanner failed to find cores in main/cores directories");
        require(catalog.cores[0].name == "Native fixture" && catalog.cores[0].contentless,
                "Scanner lost core metadata/contentless support");
        require(catalog.matching(L"GAME.BIN").size() == 2 && catalog.matching(L"game.binx").empty(),
                "Auto core matching is not exact/case insensitive");
        require(catalog.filters() == "Supported content{.bin}", "Content browser filter is incorrect");
        require(catalog.add(root / L"fixture.DLL") && catalog.cores.size() == 2,
                "Catalog duplicated a core path");
        auto active = catalog.cores.front().path;
        HMODULE loaded = LoadLibraryW(active.c_str());
        require(loaded != nullptr, "Active-core fixture load failed");
        catalog.scan(root, active);
        require(catalog.cores.size() == 2, "Rescan dropped the active core");
        FreeLibrary(loaded);
        require(!catalog.errors.empty(), "Invalid DLL diagnostics missing");
        std::puts("PASS startup core scan, both directories, metadata, contentless support, exact matching, "
                  "duplicate paths, invalid DLLs and active-core rescan");
        return 0;
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "FAIL: %s\n", failure.what());
        return 1;
    }
}
