#include "core_catalog.h"
#include "core_library.h"
#include <cctype>
#include <set>
#include <sstream>
namespace {
std::string lower(std::string value) {
    for (auto &c : value)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::set<std::string> extensions(const std::string &value) {
    std::set<std::string> result;
    std::istringstream stream(value);
    std::string ext;
    while (std::getline(stream, ext, '|')) {
        auto first = ext.find_first_not_of(" .\t");
        if (first == std::string::npos)
            continue;
        ext = lower(ext.substr(first, ext.find_last_not_of(" \t") - first + 1));
        result.insert(ext);
    }
    return result;
}
bool same(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return !a.empty() && !b.empty() && fs::equivalent(a, b, ec) && !ec;
}
thread_local CoreInfo *probing = nullptr;
bool RETRO_CALLCONV metadata_environment(unsigned command, void *data) {
    if (command == RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME && data && probing) {
        probing->contentless = *static_cast<const bool *>(data);
        return true;
    }
    return false;
}
} // namespace
bool CoreInfo::supports(const fs::path &content) const {
    auto ext = lower(content.extension().u8string());
    if (ext.empty())
        return false;
    return ::extensions(extensions).count(ext.substr(1)) != 0;
}
void CoreCatalog::sort() {
    std::sort(cores.begin(), cores.end(), [](const CoreInfo &a, const CoreInfo &b) {
        auto left = lower(a.name), right = lower(b.name);
        return left == right ? lower(a.path.u8string()) < lower(b.path.u8string()) : left < right;
    });
}
bool CoreCatalog::add(const fs::path &path) {
    for (const auto &known : cores)
        if (same(known.path, path))
            return true;
    if (!CoreLibrary::accepts(path))
        return false;
    const auto absolute = fs::absolute(path);
    // Probing must not replace callbacks in an executing core's loaded module.
    if (GetModuleHandleW(absolute.c_str()))
        return false;
    std::unique_ptr<CoreLibrary> module;
    try { module = std::make_unique<CoreLibrary>(absolute); }
    catch (const std::exception &e) {
        errors.push_back(path.u8string() + ": " + e.what());
        return false;
    }
    struct Unload {
        ~Unload() {
            probing = nullptr;
        }
    } unload;
    CoreInfo core;
    core.path = absolute;
    try {
        auto version =
            reinterpret_cast<decltype(&retro_api_version)>(module->symbol("retro_api_version"));
        auto environment =
            reinterpret_cast<decltype(&retro_set_environment)>(module->symbol("retro_set_environment"));
        auto info =
            reinterpret_cast<decltype(&retro_get_system_info)>(module->symbol("retro_get_system_info"));
        const char *required[] = {"retro_init",
                                  "retro_deinit",
                                  "retro_run",
                                  "retro_load_game",
                                  "retro_unload_game",
                                  "retro_set_video_refresh",
                                  "retro_set_audio_sample",
                                  "retro_set_audio_sample_batch",
                                  "retro_set_input_poll",
                                  "retro_set_input_state"};
        if (!version || !environment || !info)
            return false;
        for (const auto *symbol : required)
            if (!module->symbol(symbol))
                return false;
        if (version() != RETRO_API_VERSION)
            return false;
        probing = &core;
        environment(metadata_environment);
        retro_system_info system{};
        info(&system);
        probing = nullptr;
        core.name = system.library_name ? system.library_name : absolute.stem().u8string();
        core.version = system.library_version ? system.library_version : "";
        core.extensions = system.valid_extensions ? system.valid_extensions : "";
    } catch (const std::exception &failure) {
        errors.push_back(path.u8string() + ": " + failure.what());
        return false;
    }
    cores.push_back(std::move(core));
    sort();
    return true;
}
void CoreCatalog::scan(const fs::path &directory, const fs::path &active_core) {
    const auto previous = cores;
    cores.clear();
    errors.clear();
    for (const auto &location : {directory, directory / L"cores"}) {
        std::error_code ec;
        if (!fs::exists(location, ec))
            continue;
        for (fs::directory_iterator it(location, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || !CoreLibrary::accepts(it->path()))
                continue;
            if (same(it->path(), active_core)) {
                for (const auto &known : previous)
                    if (same(known.path, active_core)) {
                        cores.push_back(known);
                        break;
                    }
            } else
                add(it->path());
        }
        if (ec)
            errors.push_back(location.u8string() + ": " + ec.message());
    }
    sort();
}
std::vector<CoreInfo> CoreCatalog::matching(const fs::path &content) const {
    std::vector<CoreInfo> result;
    for (const auto &core : cores)
        if (core.supports(content))
            result.push_back(core);
    return result;
}
std::string CoreCatalog::filters() const {
    std::set<std::string> all;
    for (const auto &core : cores)
        for (const auto &ext : extensions(core.extensions))
            all.insert(ext);
    if (all.empty())
        return ".*";
    std::string result = "Supported content{";
    for (const auto &ext : all) {
        if (result.back() != '{')
            result += ',';
        result += '.' + ext;
    }
    return result + '}';
}
