#include "rom_browser.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
    std::filesystem::create_directories(root / "folder");
    std::ofstream(root / "GAME.BIN") << "content";
    std::ofstream(root / "other.binx") << "hidden";
    std::ofstream(root / "existing.state") << "keep this data";
    std::ofstream(root / std::filesystem::u8path(u8"\u65e5\u672c\u8a9e.bin"))
        << "unicode";
    auto entries = RomBrowser::scan(root, "Supported{.bin,.chd}");
    require(entries.size() == 4 && entries[0].name == ".." && entries[1].isDir,
            "Browser lost directories or extension filtering");
    require(RomBrowser::matches("GAME.BIN", ".bin") &&
                !RomBrowser::matches("game.binx", ".bin"),
            "Browser extension comparison is not exact/case insensitive");
    BrowserConfig config;
    config.path = root.u8string();
    config.flags = BrowserFlags_ConfirmOverwrite;
    RomBrowser browser;
    browser.OpenDialog("save", "Save state", ".state", config);
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60;
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (int i = 0; i < 12; ++i) {
      ImGui::NewFrame();
      browser.Display("save");
      ImGui::Render();
      std::this_thread::yield();
    }
    browser.ChooseFile("new-save");
    require(browser.IsOk() &&
                std::filesystem::u8path(browser.GetFilePathName()).filename() ==
                    "new-save.state",
            "Save mode did not accept a new filename/add its extension");
    require(!std::filesystem::exists(root / "new-save.state"),
            "Browser wrote a file itself");
    browser.OpenDialog("save", "Save state", ".state", config);
    browser.ChooseFile("existing.state");
    require(!browser.IsOk() && browser.NeedsOverwriteConfirmation(),
            "Save overwrote without confirmation");
    ImGui::NewFrame();
    browser.Display("save");
    ImGui::Render();
    browser.ConfirmOverwrite(false);
    require(!browser.IsOk(), "Declining overwrite accepted a file");
    browser.ChooseFile("existing.state");
    browser.ConfirmOverwrite(true);
    require(browser.IsOk(), "Confirmed overwrite was not accepted");
    std::string existing;
    std::ifstream(root / "existing.state") >> existing;
    require(existing == "keep", "File picker modified existing data");
    config.flags = BrowserFlags_None;
    browser.OpenDialog("open", "Open", ".bin", config);
    browser.ChooseFile("missing.bin");
    require(!browser.IsOk(), "Open mode accepted a missing file");
    browser.ChooseFile("GAME.BIN");
    require(browser.IsOk(), "Open mode rejected an existing file");
    browser.OpenDialog(
        "save", "Save state", ".state",
        BrowserConfig{
            root.u8string(), {}, nullptr, BrowserFlags_ConfirmOverwrite});
    browser.ChooseFile("missing-directory/file.state");
    require(!browser.IsOk(),
            "Save mode accepted a missing destination directory");
    browser.Close();
    require(!browser.IsOpened() && !browser.IsOk(),
            "Closing the browser retained a result");
    ImGui::DestroyContext();
    std::puts(
        "PASS shared ROM browser: Unicode, folders, exact filters, open/save, "
        "extension completion, overwrite confirmation and cancellation");
    return 0;
  } catch (const std::exception &failure) {
    std::fprintf(stderr, "FAIL: %s\n", failure.what());
    return 1;
  }
}
