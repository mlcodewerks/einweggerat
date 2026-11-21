#pragma once
#include "common.h"
#include <memory>

// Owns either a Windows DLL or a DLL mapped directly from a ZIP entry.
class CoreLibrary {
  public:
    explicit CoreLibrary(const fs::path &path);
    ~CoreLibrary();
    CoreLibrary(const CoreLibrary &) = delete;
    CoreLibrary &operator=(const CoreLibrary &) = delete;
    FARPROC symbol(const char *name) const;
    static bool accepts(const fs::path &path);
    static bool archive_has_core(const fs::path &path);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
