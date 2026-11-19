#include "common.h"
#include <cstdio>
#include <fstream>
std::wstring widen(const std::string &value) {
    if (value.empty())
        return {};
    int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("Invalid UTF-8 path");
    std::wstring out(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), out.data(), count);
    return out;
}
std::string narrow(const std::wstring &value) {
    if (value.empty())
        return {};
    int count =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(count, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), count, nullptr, nullptr);
    return out;
}
void check(HRESULT result, const char *operation) {
    if (FAILED(result)) {
        char code[32];
        std::snprintf(code, sizeof(code), " (0x%08lx)", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(operation) + code);
    }
}
std::vector<uint8_t> read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open " + path.u8string());
    auto length = file.tellg();
    if (length < 0 || uint64_t(length) > SIZE_MAX)
        throw std::runtime_error("Invalid file size");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error("File read failed");
    return bytes;
}
void write_file(const fs::path &path, const void *data, size_t size) {
    // Replace only after the complete save has reached disk.
    auto temp = path;
    temp += L".tmp";
    HANDLE file =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create " + temp.u8string());
    auto *bytes = static_cast<const uint8_t *>(data);
    bool ok = true;
    while (size && ok) {
        DWORD wrote = 0, chunk = DWORD(std::min<size_t>(size, 1u << 24));
        ok = WriteFile(file, bytes, chunk, &wrote, nullptr) && wrote == chunk;
        bytes += wrote;
        size -= wrote;
    }
    ok = FlushFileBuffers(file) && ok;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Save write failed: " + path.u8string());
}
fs::path executable_directory() {
    std::wstring path(32768, 0);
    DWORD count = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!count || count >= path.size())
        throw std::runtime_error("Cannot locate executable");
    path.resize(count);
    return fs::path(path).parent_path();
}
