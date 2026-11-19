#pragma once
#include "imgui.h"
#include <filesystem>
#include <future>
#include <string>
#include <vector>

enum BrowserFlags {
  BrowserFlags_None = 0,
  BrowserFlags_ConfirmOverwrite = 1,
  BrowserFlags_Modal = 2,
  BrowserFlags_CaseInsensitiveExtentionFiltering = 4
};
struct BrowserConfig {
  std::string path = ".", filePathName;
  void *userDatas = nullptr;
  int flags = BrowserFlags_None;
};
struct BrowserEntry {
  bool isDir = false;
  std::filesystem::path name;
  std::string showName;
};

// einweggerat's asynchronous ROM browser, shared by content and open/save pickers.
// No platform windowing or external file-dialog library is required.
class RomBrowser {
public:
  static RomBrowser *Instance();
  void OpenDialog(const std::string &key, const std::string &title,
                  const char *filters, const BrowserConfig &config = {});
  bool Display(const std::string &key, ImGuiWindowFlags flags = 0,
               ImVec2 minimum = {}, ImVec2 maximum = {});
  bool IsOpened(const std::string &key = {}) const;
  bool IsOk() const { return accepted; }
  std::string GetFilePathName() const { return result.u8string(); }
  std::string GetCurrentPath() const { return directory.u8string(); }
  void Close();
  void ChooseFile(const std::string &name);
  bool NeedsOverwriteConfirmation() const { return overwrite_pending; }
  void ConfirmOverwrite(bool replace);
  // Also used by the file-picker tests and supported-content filtering.
  static bool matches(const std::filesystem::path &path,
                      const std::string &filters);
  static std::vector<BrowserEntry> scan(const std::filesystem::path &directory,
                                        const std::string &filters);
  // Resolve an entered name without writing anything. Returns an error on
  // invalid targets.
  static std::filesystem::path target(const std::filesystem::path &directory,
                                      const std::string &name,
                                      const std::string &filters, bool save);

private:
  std::string key, title, filters, error;
  std::filesystem::path directory, result, selected;
  std::vector<BrowserEntry> records;
  std::future<std::vector<BrowserEntry>> future;
  bool opened = false, accepted = false, finished = false, save = false,
       refresh_pending = false;
  bool overwrite_pending = false, first_frame = false;
  char filename[4096] = {}, path_text[4096] = {};
  void refresh();
  void poll();
  void change_directory(const std::filesystem::path &path);
  void submit();
};
