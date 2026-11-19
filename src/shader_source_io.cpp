#include "shader_chain.h"
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

bool shader_save_source(const std::filesystem::path &path, const std::string &source, std::string &error)
{
    try
    {
        if (source.size() > 4 * 1024 * 1024 || source.find('\0') != source.npos)
            throw std::runtime_error("Shader source is too large or contains NUL bytes.");
        auto tmp = path;
        tmp += ".tmp";
        std::ofstream out(tmp, std::ios::binary);
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write shader source.");
#ifdef _WIN32
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace " + path.u8string());
#else
        std::filesystem::rename(tmp, path);
#endif
        error.clear();
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}
