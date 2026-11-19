#pragma once
#include "libretro.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
namespace fs = std::filesystem;
std::wstring widen(const std::string &value);
std::string narrow(const std::wstring &value);
void check(HRESULT result, const char *operation);
std::vector<uint8_t> read_file(const fs::path &path);
void write_file(const fs::path &path, const void *data, size_t size);
fs::path executable_directory();
