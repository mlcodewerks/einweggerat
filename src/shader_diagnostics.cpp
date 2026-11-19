#include "shader_diagnostics.h"
#include "shader_chain.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

std::string shader_normalize_source(std::string text)
{
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
        text.erase(0, 3);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}
namespace
{
    using path = std::filesystem::path;
    struct source_map
    {
        std::string text;
        std::map<size_t, std::set<size_t>> lines;
    };
    using source_maps = std::map<path, source_map>;

    // Match librashader 0.12's include/stage preprocessing and its emitted #line
    // directives. In particular, pragma/include resets are not physical line
    // numbers. See librashader-preprocess/src/{include,stage}.rs at v0.12.0.
    void map_source(const path &file, source_maps &maps, bool root, std::set<path> &active)
    {
        std::error_code ec;
        auto key = std::filesystem::weakly_canonical(file, ec);
        if (ec || maps.size() >= 128 || active.count(key))
            return;
        auto size = std::filesystem::file_size(key, ec);
        if (ec || size > 4 * 1024 * 1024)
            return;
        std::ifstream in(key, std::ios::binary);
        std::string text(static_cast<size_t>(size), '\0');
        if (!in.read(text.data(), static_cast<std::streamsize>(size)))
            return;
        text = shader_normalize_source(std::move(text));
        auto &source = maps[key];
        source.text = text;
        active.insert(key);
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
            lines.push_back(line);
        size_t begin = 0;
        while (begin < lines.size() && lines[begin].find_first_not_of(" \t") == line.npos)
            ++begin;
        if (root)
            ++begin; // The trimmed #version line is emitted separately.
        size_t logical = root ? 2 : 1;
        for (size_t i = begin; i < lines.size(); ++i)
        {
            const auto &s = lines[i];
            const auto index = i - begin;
            bool optional = s.rfind("#pragma include_optional ", 0) == 0;
            if (s.rfind("#include ", 0) == 0 || optional)
            {
                auto a = s.find('"'), b = a == s.npos ? s.npos : s.find('"', a + 1);
                if (b == s.npos)
                    continue;
                auto include = key.parent_path() / std::filesystem::u8path(s.substr(a + 1, b - a - 1));
                bool exists = std::filesystem::exists(include, ec) && !ec;
                if (exists)
                    map_source(include, maps, false, active);
                logical = optional && !exists ? index : index + 1;
            }
            else if (s.rfind("#line ", 0) == 0)
            {
                // Custom source maps may name virtual files; do not guess locations.
                break;
            }
            else
            {
                if (s.rfind("#pragma", 0) != 0 && s.rfind("#endif", 0) != 0)
                    source.lines[logical].insert(i + 1);
                ++logical;
                if (s.rfind("#pragma", 0) == 0 || s.rfind("#endif", 0) == 0)
                    logical = index + 2;
            }
        }
        active.erase(key);
    }

    // Rust's Debug formatting embeds a quoted compiler log in the C API message.
    // Unescape only that string, never raw Windows paths in an ordinary diagnostic.
    std::string compiler_log(const std::string &error)
    {
        auto start = error.find("log: \"");
        if (start == error.npos)
            return error;
        start += 6;
        std::string log;
        for (size_t i = start; i < error.size(); ++i)
        {
            char c = error[i];
            if (c == '"')
                break;
            if (c == '\\' && i + 1 < error.size())
            {
                c = error[++i];
                if (c == 'n')
                    c = '\n';
                else if (c == 'r')
                    c = '\r';
                else if (c == 't')
                    c = '\t';
                else if (c != '\\' && c != '"')
                    log += '\\';
            }
            log += c;
        }
        return log;
    }
    bool location(const std::string &line, std::string &file, size_t &number, size_t &message_start)
    {
        for (size_t colon = line.find(':'); colon != line.npos; colon = line.find(':', colon + 1))
        {
            size_t end = colon + 1;
            while (end < line.size() && line[end] >= '0' && line[end] <= '9')
                ++end;
            if (end == colon + 1 || end >= line.size() || line[end] != ':' || end - colon > 9)
                continue;
            file = line.substr(0, colon);
            number = std::stoul(line.substr(colon + 1, end - colon - 1));
            message_start = end + 1;
            while (message_start < line.size() && line[message_start] == ' ')
                ++message_start;
            return true;
        }
        return false;
    }
}

void shader_read_diagnostics(shader_diagnostic_report &report, const std::string &error,
                             const shader_chain_config &chain, bool enabled_only)
{
    report.clear();
    source_maps maps;
    std::set<path> active;
    for (const auto &pass : chain.passes)
        if (!enabled_only || pass.enabled)
            map_source(pass.source, maps, true, active);
    for (const auto &[file, source] : maps)
        report.sources[file] = source.text;
    std::istringstream log(compiler_log(error));
    std::string line;
    while (std::getline(log, line) && report.entries.size() < 256)
    {
        auto start = line.find("ERROR: ");
        bool warning = false;
        if (start == line.npos)
        {
            start = line.find("WARNING: ");
            warning = true;
        }
        if (start == line.npos)
            continue;
        line = line.substr(start + (warning ? 9 : 7));
        shader_diagnostic diagnostic;
        diagnostic.warning = warning;
        diagnostic.message = line;
        std::string filename;
        size_t number = 0, message_start = 0;
        if (location(line, filename, number, message_start))
        {
            auto named = std::filesystem::u8path(filename);
            std::vector<path> matches;
            for (const auto &[file, source] : maps)
            {
                if (named.is_absolute() ? file.lexically_normal() == named.lexically_normal() : file.filename() == named.filename())
                    matches.push_back(file);
            }
            // A basename shared by multiple passes is ambiguous, as are numeric
            // source IDs in generated driver GLSL. Keep those in the full log.
            if (matches.size() == 1)
            {
                const auto &source = maps.at(matches[0]);
                auto found = source.lines.find(number);
                if (found != source.lines.end() && found->second.size() == 1)
                {
                    diagnostic.source = matches[0];
                    diagnostic.line = *found->second.begin();
                    diagnostic.message = line.substr(message_start);
                }
            }
        }
        report.entries.push_back(std::move(diagnostic));
    }
}
