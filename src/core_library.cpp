#include "core_library.h"
#include "../third_party/MemoryModulePP.h"
#define MINIZ_HEADER_FILE_ONLY
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz.h"
#include <cctype>
#include <map>
#include <set>
#include <functional>
namespace {
std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool libretro_image(const std::vector<uint8_t> &bytes) {
    auto fits = [&](size_t offset, size_t size) { return offset <= bytes.size() && size <= bytes.size() - offset; };
    if (!fits(0, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || !fits(dos->e_lfanew, 24)) return false;
    size_t nt = static_cast<size_t>(dos->e_lfanew), optional = nt + 24;
    if (*reinterpret_cast<const DWORD *>(bytes.data() + nt) != IMAGE_NT_SIGNATURE) return false;
    const auto *header = reinterpret_cast<const IMAGE_FILE_HEADER *>(bytes.data() + nt + 4);
    if (!fits(optional, header->SizeOfOptionalHeader) || header->SizeOfOptionalHeader < 2) return false;
    WORD magic = *reinterpret_cast<const WORD *>(bytes.data() + optional);
    size_t directory = magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? 112 : magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ? 96 : 0;
    if (!directory || header->SizeOfOptionalHeader < directory + 8) return false;
    const auto *exports = reinterpret_cast<const IMAGE_DATA_DIRECTORY *>(bytes.data() + optional + directory);
    size_t sections = optional + header->SizeOfOptionalHeader;
    if (!fits(sections, size_t(header->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER))) return false;
    auto file_offset = [&](DWORD rva, size_t size) -> size_t {
        const auto *section = reinterpret_cast<const IMAGE_SECTION_HEADER *>(bytes.data() + sections);
        for (unsigned i = 0; i < header->NumberOfSections; ++i) {
            if (rva >= section[i].VirtualAddress && rva - section[i].VirtualAddress <= section[i].SizeOfRawData &&
                size <= section[i].SizeOfRawData - (rva - section[i].VirtualAddress)) {
                size_t offset = size_t(section[i].PointerToRawData) + rva - section[i].VirtualAddress;
                if (fits(offset, size)) return offset;
            }
        }
        return bytes.size();
    };
    size_t offset = file_offset(exports->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (offset == bytes.size()) return false;
    const auto *table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(bytes.data() + offset);
    offset = file_offset(table->AddressOfNames, size_t(table->NumberOfNames) * sizeof(DWORD));
    if (offset == bytes.size()) return false;
    const auto *names = reinterpret_cast<const DWORD *>(bytes.data() + offset);
    bool version = false, info = false;
    for (DWORD i = 0; i < table->NumberOfNames; ++i) {
        size_t name = file_offset(names[i], 23);
        if (name == bytes.size()) continue;
        version |= std::memcmp(bytes.data() + name, "retro_api_version", 18) == 0;
        info |= std::memcmp(bytes.data() + name, "retro_get_system_info", 22) == 0;
        if (version && info) return true;
    }
    return false;
}
struct Zip {
    std::vector<uint8_t> bytes;
    mz_zip_archive zip{};
    explicit Zip(const fs::path &path) : bytes(read_file(path)) {
        if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0))
            throw std::runtime_error("Cannot read core ZIP: " + path.u8string());
    }
    ~Zip() { mz_zip_reader_end(&zip); }
    std::vector<std::pair<unsigned, std::string>> dlls() {
        std::vector<std::pair<unsigned, std::string>> result;
        for (unsigned i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip, i, &st)) throw std::runtime_error("Invalid ZIP directory");
            if (!st.m_is_directory && lower(fs::u8path(st.m_filename).extension().u8string()) == ".dll")
                result.emplace_back(i, st.m_filename);
        }
        return result;
    }
    std::vector<uint8_t> extract(unsigned index) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, index, &st) || st.m_uncomp_size > 1024ULL * 1024 * 1024 || st.m_is_encrypted)
            throw std::runtime_error("Unsupported or oversized core ZIP entry");
        std::vector<uint8_t> data(static_cast<size_t>(st.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&zip, index, data.data(), data.size(), 0))
            throw std::runtime_error("Cannot decompress core ZIP entry (corrupt data or CRC)");
        return data;
    }
};
}
struct CoreLibrary::Impl {
    struct Module {
        HMODULE native = nullptr;
        PMEMORYMODULE memory = nullptr;
        bool loading = false;
        std::vector<Module *> dependencies;
    };
    fs::path path;
    std::unique_ptr<Zip> zip;
    std::map<std::string, unsigned> entries;
    std::map<std::string, std::unique_ptr<Module>> modules;
    std::vector<Module *> order;
    Module *root = nullptr;
    Module *current = nullptr;
    struct Context {
        Impl &owner;
        Module *previous;
        Context(Impl &owner, Module *module) : owner(owner), previous(owner.current) { owner.current = module; }
        ~Context() { owner.current = previous; }
    };
    std::string error;
    DLL_DIRECTORY_COOKIE search = nullptr;
    explicit Impl(const fs::path &p) : path(fs::absolute(p)) {}
    ~Impl() {
        // A forwarded export may add a dependency after initial mapping.
        // Compute the graph order so shared dependencies outlive every user.
        std::set<Module *> visited;
        std::vector<Module *> sorted;
        std::function<void(Module *)> visit = [&](Module *module) {
            if (!visited.insert(module).second) return;
            for (auto *dependency : module->dependencies) visit(dependency);
            sorted.push_back(module);
        };
        for (auto *module : order) visit(module);
        for (auto i = sorted.rbegin(); i != sorted.rend(); ++i) {
            if ((*i)->memory) MemoryFreeLibrary((*i)->memory);
            if ((*i)->native) FreeLibrary((*i)->native);
        }
        if (search) RemoveDllDirectory(search);
    }
    static HCUSTOMMODULE load(const char *name, void *context) {
        auto &self = *static_cast<Impl *>(context);
        try {
            auto *dependency = self.dependency(name);
            if (self.current) self.current->dependencies.push_back(dependency);
            return dependency;
        }
        catch (const std::exception &e) { self.error = e.what(); SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    }
    static FARPROC proc(HCUSTOMMODULE handle, const char *name, void *context) {
        auto &module = *static_cast<Module *>(handle);
        Context scope(*static_cast<Impl *>(context), &module);
        return module.memory ? MemoryGetProcAddress(module.memory, name) : GetProcAddress(module.native, name);
    }
    // This context owns the entire dependency graph, including shared imports.
    static void release(HCUSTOMMODULE, void *) {}
    Module *dependency(const std::string &name) {
        auto key = lower(name);
        auto found = modules.find(key);
        if (found != modules.end()) {
            if (found->second->loading) throw std::runtime_error("Circular ZIP DLL dependency: " + name);
            return found->second.get();
        }
        auto node = std::make_unique<Module>();
        auto *module = node.get();
        modules.emplace(key, std::move(node));
        auto entry = entries.find(key);
        if (entry != entries.end()) {
            Context scope(*this, module);
            module->loading = true;
            auto bytes = zip->extract(entry->second);
            module->memory = MemoryLoadLibraryEx(bytes.data(), bytes.size(), MemoryDefaultAlloc, MemoryDefaultFree,
                                                 load, proc, release, this);
            module->loading = false;
        } else {
            auto local = path.parent_path() / fs::u8path(name);
            if (fs::is_regular_file(local))
                module->native = LoadLibraryExW(local.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            else
                module->native = LoadLibraryExW(widen(name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        }
        if (!module->memory && !module->native)
            throw std::runtime_error("Cannot load DLL " + name + " (Windows error " + std::to_string(GetLastError()) + ")" +
                                     (error.empty() ? "" : ": " + error));
        order.push_back(module);
        return module;
    }
    void open() {
        if (lower(path.extension().u8string()) != ".zip") { root = dependency(path.u8string()); return; }
        search = AddDllDirectory(path.parent_path().c_str());
        zip = std::make_unique<Zip>(path);
        std::vector<std::string> candidates;
        for (const auto &entry : zip->dlls()) {
            auto base = lower(fs::u8path(entry.second).filename().u8string());
            if (!entries.emplace(base, entry.first).second)
                throw std::runtime_error("Duplicate DLL basenames in core ZIP: " + base);
            if (base.size() >= 13 && base.compare(base.size() - 13, 13, "_libretro.dll") == 0) candidates.push_back(base);
        }
        std::string selected;
        auto expected = lower(path.stem().u8string());
        if (fs::u8path(expected).extension() != ".dll") expected += ".dll";
        if (entries.count(expected)) selected = expected;
        else if (candidates.size() == 1) selected = candidates.front();
        else if (entries.size() == 1) selected = entries.begin()->first;
        else throw std::runtime_error(entries.empty() ? "Core ZIP contains no DLL" : "Core ZIP contains multiple cores; name the ZIP after the core DLL");
        root = dependency(selected);
        // Keep the directory and archive for forwarded exports resolved lazily.
    }
};
CoreLibrary::CoreLibrary(const fs::path &path) : impl(std::make_unique<Impl>(path)) { impl->open(); }
CoreLibrary::~CoreLibrary() = default;
FARPROC CoreLibrary::symbol(const char *name) const { return Impl::proc(impl->root, name, impl.get()); }
bool CoreLibrary::accepts(const fs::path &path) {
    auto ext = lower(path.extension().u8string()); return ext == ".dll" || ext == ".zip";
}
bool CoreLibrary::archive_has_core(const fs::path &path) {
    if (lower(path.extension().u8string()) != ".zip") return false;
    try {
        Zip zip(path);
        for (const auto &entry : zip.dlls()) if (libretro_image(zip.extract(entry.first))) return true;
    } catch (...) {}
    return false;
}
