#pragma once
#include "common.h"

struct CoreInfo {
    fs::path path;
    std::string name, version, extensions;
    bool contentless = false;
    bool supports(const fs::path &content) const;
};
class CoreCatalog {
  public:
    void scan(const fs::path &directory, const fs::path &active_core = {});
    bool add(const fs::path &path);
    std::vector<CoreInfo> matching(const fs::path &content) const;
    std::string filters() const;
    std::vector<CoreInfo> cores;
    std::vector<std::string> errors;

  private:
    void sort();
};
