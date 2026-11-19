#include "shader_chain.h"
#include "shader_library.h"
#include "native.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    using entries = std::map<std::string, std::string>;
    std::string trim(std::string s)
    {
        auto a = s.find_first_not_of(" \t\r\n");
        return a == s.npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }
    std::string unquote(std::string s)
    {
        s = trim(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return s.substr(1, s.size() - 2);
        return s;
    }
    std::vector<std::string> split(const std::string &s)
    {
        std::vector<std::string> v;
        std::istringstream in(s);
        std::string part;
        while (std::getline(in, part, ';'))
            if (!trim(part).empty())
                v.push_back(trim(part));
        return v;
    }
    unsigned count(const std::string &s, unsigned limit)
    {
        size_t used;
        auto n = std::stoul(s, &used);
        if (used != s.size() || n > limit)
            throw std::runtime_error("Invalid shader count.");
        return static_cast<unsigned>(n);
    }
    void read_preset(const std::filesystem::path &path, entries &out, std::set<std::filesystem::path> &seen)
    {
        auto absolute = std::filesystem::weakly_canonical(path);
        if (seen.size() >= 32 || !seen.insert(absolute).second)
            throw std::runtime_error("Cyclic or excessive preset references.");
        std::ifstream in(absolute);
        if (!in)
            throw std::runtime_error("Cannot open " + absolute.u8string());
        entries local;
        std::vector<std::filesystem::path> references;
        std::string line;
        size_t bytes = 0;
        while (std::getline(in, line))
        {
            if ((bytes += line.size()) > 1024 * 1024)
                throw std::runtime_error("Preset too large.");
            line = trim(line);
            if (line.rfind("#reference", 0) == 0)
            {
                references.push_back(absolute.parent_path() / std::filesystem::u8path(unquote(line.substr(10))));
                continue;
            }
            bool quoted = false;
            for (size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"')
                    quoted = !quoted;
                if (line[i] == '#' && !quoted)
                {
                    line.resize(i);
                    break;
                }
            }
            if (trim(line).empty())
                continue;
            auto equals = line.find('=');
            if (equals == line.npos || quoted)
                throw std::runtime_error("Malformed shader preset line.");
            local[trim(line.substr(0, equals))] = unquote(line.substr(equals + 1));
        }
        for (const auto &reference : references)
            read_preset(reference, out, seen);
        // A reference can override only a LUT path, inheriting the texture inventory.
        const auto textures = split(local.count("textures") ? local["textures"] : out["textures"]);
        static const std::regex shader_key("shader[0-9]+");
        for (auto &entry : local)
        {
            if (std::regex_match(entry.first, shader_key) || std::find(textures.begin(), textures.end(), entry.first) != textures.end())
                entry.second = (absolute.parent_path() / std::filesystem::u8path(entry.second)).lexically_normal().generic_u8string();
            out[entry.first] = entry.second;
        }
        seen.erase(absolute);
    }
    void replace_file(const std::filesystem::path &tmp, const std::filesystem::path &path)
    {
#ifdef _WIN32
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace " + path.u8string());
#else
        std::filesystem::rename(tmp, path);
#endif
    }
    void safe_text(const std::string &s)
    {
        if (s.size() > 16384 || s.find_first_of("\r\n\"") != s.npos)
            throw std::runtime_error("Invalid shader preset text.");
    }
    void write_preset(std::ostream &out, const shader_chain_config &chain)
    {
        if (chain.passes.size() > 64)
            throw std::runtime_error("A shader chain supports up to 64 passes.");
        auto write = [&](const std::string &key, const std::string &v)
        {
            safe_text(key);
            safe_text(v);
            out << key << " = \"" << v << "\"\n";
        };
        for (const auto &e : chain.globals)
            write(e.first, e.second);
        unsigned index = 0;
        for (const auto &pass : chain.passes)
            if (pass.enabled)
            {
                write("shader" + std::to_string(index), pass.source.generic_u8string());
                for (const auto &e : pass.options)
                    if (!e.second.empty())
                        write(e.first + std::to_string(index), e.second);
                ++index;
            }
        out << "shaders = " << index << '\n';
        std::string names;
        for (const auto &p : chain.values)
        {
            if (!names.empty())
                names += ';';
            names += p.first;
        }
        write("parameters", names);
        for (const auto &p : chain.values)
        {
            if (!std::isfinite(p.second))
                throw std::runtime_error("Shader parameter must be finite.");
            safe_text(p.first);
            out << p.first << " = " << std::setprecision(9) << p.second << '\n';
        }
    }
    void read_parameter_ids(const std::filesystem::path &path, std::set<std::filesystem::path> &seen, std::vector<std::string> &ids)
    {
        if (seen.size() >= 128 || !seen.insert(path.lexically_normal()).second)
            return;
        std::ifstream in(path);
        std::string line;
        static const std::regex param(R"(^\s*#\s*pragma\s+parameter\s+([A-Za-z_][A-Za-z_0-9]*))");
        static const std::regex include("^\\s*#\\s*include\\s+\"([^\"]+)\"");
        size_t bytes = 0;
        while (std::getline(in, line) && (bytes += line.size()) < 4 * 1024 * 1024)
        {
            std::smatch m;
            if (std::regex_search(line, m, param) && std::find(ids.begin(), ids.end(), m[1]) == ids.end())
                ids.push_back(m[1]);
            if (std::regex_search(line, m, include))
                read_parameter_ids(path.parent_path() / m[1].str(), seen, ids);
        }
    }
}

std::vector<std::string> shader_pass_parameters(const std::filesystem::path &path)
{
    std::set<std::filesystem::path> seen;
    std::vector<std::string> ids;
    read_parameter_ids(path, seen, ids);
    return ids;
}
std::filesystem::path shader_temporary_preset()
{
#ifdef _WIN32
    auto process = GetCurrentProcessId();
#else
    auto process = getpid();
#endif
    static unsigned sequence = 0;
    return std::filesystem::temp_directory_path() / ("einweggerat-shader-" + std::to_string(process) + "-" +
                                                     std::to_string(native_nanoseconds()) + "-" + std::to_string(++sequence) + ".slangp");
}
bool shader_import(const std::filesystem::path &path, shader_chain_config &result, std::string &error)
{
    try
    {
        shader_chain_config next;
        next.name = path.stem().u8string();
        if (path.extension() == ".slang")
            next.passes.push_back({std::filesystem::absolute(path), true, {}});
        else
        {
            entries values;
            std::set<std::filesystem::path> seen;
            read_preset(path, values, seen);
            auto n = count(values.at("shaders"), 64);
            values.erase("shaders");
            next.passes.resize(n);
            static const std::regex pass_key("^(shader|filter_linear|wrap_mode|frame_count_mod|srgb_framebuffer|float_framebuffer|mipmap_input|alias|scale_type|scale_type_x|scale_type_y|scale|scale_x|scale_y)([0-9]+)$");
            for (auto it = values.begin(); it != values.end();)
            {
                std::smatch m;
                if (std::regex_match(it->first, m, pass_key))
                {
                    auto i = count(m[2], 63);
                    if (i >= n)
                        throw std::runtime_error("Preset option refers to a nonexistent pass.");
                    if (m[1] == "shader")
                        next.passes[i].source = std::filesystem::u8path(it->second);
                    else
                        next.passes[i].options[m[1]] = it->second;
                    it = values.erase(it);
                }
                else
                    ++it;
            }
            for (const auto &pass : next.passes)
                if (pass.source.empty())
                    throw std::runtime_error("Missing shader source.");
            for (const auto &id : split(values["parameters"]))
            {
                auto found = values.find(id);
                if (found == values.end())
                    continue;
                std::istringstream number(found->second);
                number.imbue(std::locale::classic());
                float v;
                if (!(number >> v) || !number.eof() || !std::isfinite(v))
                    throw std::runtime_error("Invalid shader parameter.");
                next.values[id] = v;
                values.erase(found);
            }
            values.erase("parameters");
            next.globals = std::move(values);
        }
        result = std::move(next);
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
bool shader_export(const std::filesystem::path &path, const shader_chain_config &chain, std::string &error)
{
    try
    {
        auto tmp = path;
        tmp += ".tmp";
        std::ofstream out(tmp);
        out.imbue(std::locale::classic());
        if (!out)
            throw std::runtime_error("Cannot write " + path.u8string());
        write_preset(out, chain);
        out.close();
        if (!out)
            throw std::runtime_error("Cannot finish shader preset.");
        replace_file(tmp, path);
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
bool shader_create_preset(const std::filesystem::path &directory, const std::string &name,
                          const std::string &source, shader_chain_config &result, std::string &error)
{
    try
    {
        if (name.empty() || name == "." || name == ".." || name.size() > 200 ||
            name.front() == ' ' || name.back() == ' ' || name.back() == '.' ||
            name.find_first_of("<>:\"/\\|?*") != name.npos ||
            std::any_of(name.begin(), name.end(), [](unsigned char c)
                        { return c < 32; }))
            throw std::runtime_error("Enter a preset name without path separators or filename punctuation.");
        auto root = std::filesystem::absolute(directory);
        auto path = root / std::filesystem::u8path(name + ".slang");
        auto preset = root / std::filesystem::u8path(name + ".slangp");
        if (std::filesystem::exists(path) || std::filesystem::exists(preset))
            throw std::runtime_error("A shader or preset with this name already exists. Choose another name.");
        std::filesystem::create_directories(root);
        shader_chain_config next;
        next.name = name;
        next.passes.push_back({path, true, {}});
        if (!shader_save_source(path, source, error))
            return false;
        if (!shader_export(preset, next, error))
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return false;
        }
        result = std::move(next);
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
bool shader_save(const std::filesystem::path &path, const shader_settings &settings, std::string &error)
{
    try
    {
        auto tmp = path;
        tmp += ".tmp";
        std::ofstream out(tmp);
        out.imbue(std::locale::classic());
        if (!out || settings.chains.size() > 16)
            throw std::runtime_error("Cannot save shader stack.");
        out << "EINWEGGERAT_SHADERS 1\n"
            << settings.enabled << ' ' << settings.chains.size() << '\n';
        for (const auto &chain : settings.chains)
        {
            out << std::quoted(chain.name) << ' ' << chain.enabled << ' ' << chain.passes.size() << '\n';
            for (const auto &pass : chain.passes)
            {
                out << std::quoted(pass.source.generic_u8string()) << ' ' << pass.enabled << ' ' << pass.options.size() << '\n';
                for (const auto &e : pass.options)
                    out << std::quoted(e.first) << ' ' << std::quoted(e.second) << '\n';
            }
            out << chain.globals.size() << '\n';
            for (const auto &e : chain.globals)
                out << std::quoted(e.first) << ' ' << std::quoted(e.second) << '\n';
            out << chain.values.size() << '\n';
            for (const auto &e : chain.values)
                out << std::quoted(e.first) << ' ' << std::setprecision(9) << e.second << '\n';
        }
        out.close();
        if (!out)
            throw std::runtime_error("Cannot finish shader stack.");
        replace_file(tmp, path);
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
bool shader_load(const std::filesystem::path &path, shader_settings &settings, std::string &error)
{
    try
    {
        if (std::filesystem::file_size(path) > 4 * 1024 * 1024)
            throw std::runtime_error("Shader stack too large.");
        std::ifstream in(path);
        in.imbue(std::locale::classic());
        std::string magic;
        int version;
        in >> magic >> version;
        if (magic != "EINWEGGERAT_SHADERS" || version != 1)
            throw std::runtime_error("Unsupported shader stack.");
        auto size = [&](size_t max)
        { size_t n; if (!(in >> n) || n > max) throw std::runtime_error("Invalid shader stack count."); return n; };
        auto string = [&]()
        { std::string s; if (!(in >> std::quoted(s)) || s.size() > 16384) throw std::runtime_error("Invalid shader stack text."); return s; };
        auto map = [&](entries &entries)
        { auto n = size(4096); for (size_t i = 0; i < n; ++i) { auto key = string(); entries[key] = string(); } };
        shader_settings next;
        in >> next.enabled;
        next.chains.resize(size(16));
        for (auto &chain : next.chains)
        {
            chain.name = string();
            in >> chain.enabled;
            chain.passes.resize(size(64));
            for (auto &pass : chain.passes)
            {
                pass.source = std::filesystem::u8path(string());
                if (pass.source.is_relative())
                    pass.source = path.parent_path() / pass.source;
                in >> pass.enabled;
                map(pass.options);
            }
            map(chain.globals);
            auto n = size(4096);
            for (size_t i = 0; i < n; ++i)
            {
                auto key = string();
                float v;
                if (!(in >> v) || !std::isfinite(v))
                    throw std::runtime_error("Invalid shader value.");
                chain.values[key] = v;
            }
        }
        in >> std::ws;
        if (!in.eof())
            throw std::runtime_error("Trailing shader stack data.");
        settings = std::move(next);
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
shader_controller &video_shaders()
{
    static shader_controller controller;
    return controller;
}
void shader_controller::init()
{
    if (initialized)
        return;
    initialized = true;
    directory = executable_directory();
    auto &api = librashader();
    api.load(directory);
    auto path = directory / "einweggerat.shaders";
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && shader_load(path, settings, error))
        for (auto &chain : settings.chains)
            if ((api.gl_available || api.vk_available) && !chain.passes.empty())
                inspect(chain);
}
bool shader_controller::inspect(shader_chain_config &chain)
try
{
    auto &api = librashader();
    if (!api.gl_available && !api.vk_available)
        return false;
    // Windows gives each process its own temporary filename, outside shader source directories.
    auto temporary = shader_temporary_preset();
    auto all = chain;
    for (auto &pass : all.passes)
        pass.enabled = true;
    if (!shader_export(temporary, all, error))
        return false;
    libra_shader_preset_t preset = nullptr;
    const bool loaded = api.check(api.preset_create(temporary.u8string().c_str(), &preset), error);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (!loaded)
    {
        compilation_failed(chain, false);
        return false;
    }
    libra_preset_param_list_t params{};
    bool ok = api.check(api.preset_get_runtime_params(&preset, &params), error);
    if (ok)
    {
        chain.parameters.clear();
        for (uint64_t i = 0; i < params.length; ++i)
        {
            const auto &p = params.parameters[i];
            chain.parameters.push_back({p.name, p.description, p.initial, p.minimum, p.maximum, p.step});
            if (!chain.values.count(p.name))
            {
                float value = p.initial;
                api.check(api.preset_get_param(&preset, p.name, &value), error);
                chain.values[p.name] = value;
            }
        }
        api.check(api.preset_free_runtime_params(params), error);
    }
    api.check(api.preset_free(&preset), error);
    if (!ok)
        compilation_failed(chain, false);
    return ok;
}
catch (const std::exception &e)
{
    error = e.what();
    return false;
}
