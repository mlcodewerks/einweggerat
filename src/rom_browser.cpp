#include "rom_browser.h"
#include "IconsForkAwesome.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace {
// Continue the toolbar row only when the next control fits inside the window.
void toolbar_next(float width) {
  const float right = ImGui::GetCursorScreenPos().x +
                      ImGui::GetContentRegionAvail().x;
  if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
    ImGui::SameLine();
}

// Preserve the original ROM browser's underlined, keyboard-focusable links.
bool breadcrumb_link(const char *label) {
  const auto &style = ImGui::GetStyle();
  auto *window = ImGui::GetCurrentWindow();
  auto *draw = ImGui::GetWindowDrawList();
  const ImVec2 pos(window->DC.CursorPos.x,
                   window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
  const auto text_size = ImGui::CalcTextSize(label);
  const ImVec2 size(std::min(text_size.x,
                            std::max(1.f, ImGui::GetContentRegionAvail().x)),
                    text_size.y);
  const ImRect bounds(pos, ImVec2(pos.x + size.x, pos.y + size.y));
  const auto id = ImGui::GetID(label);
  ImGui::ItemSize(bounds, 0.0f);
  if (!ImGui::ItemAdd(bounds, id))
    return false;
  bool hovered = false;
  const bool clicked = ImGui::ButtonBehavior(bounds, id, &hovered, nullptr);
  const bool focused = ImGui::IsItemFocused();
  const auto color = ImGui::ColorConvertFloat4ToU32(
      style.Colors[hovered || focused ? ImGuiCol_Text : ImGuiCol_TextDisabled]);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::RenderTextEllipsis(draw, bounds.Min, bounds.Max, bounds.Max.x, label,
                            nullptr, &text_size);
  ImGui::PopStyleColor();
  if (hovered && size.x < text_size.x)
    ImGui::SetTooltip("%s", label);
  if (focused)
    draw->AddRect(bounds.Min, bounds.Max, color);
  else
    draw->AddLine(ImVec2(bounds.Min.x, bounds.Max.y), bounds.Max, color);
  return clicked;
}

std::string lower(std::string text) {
  for (char &c : text)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return text;
}
std::set<std::string> extensions(const std::string &filters) {
  std::set<std::string> values;
  for (size_t start = 0;
       (start = filters.find('.', start)) != std::string::npos;) {
    size_t end = start + 1;
    while (end < filters.size() &&
           (std::isalnum(static_cast<unsigned char>(filters[end])) ||
            filters[end] == '_' || filters[end] == '*' || filters[end] == '-'))
      ++end;
    if (end > start + 1)
      values.insert(lower(filters.substr(start, end - start)));
    start = end;
  }
  return values;
}
} // namespace
RomBrowser *RomBrowser::Instance() {
  static RomBrowser browser;
  return &browser;
}
bool RomBrowser::matches(const std::filesystem::path &path,
                         const std::string &filters) {
  const auto values = extensions(filters);
  return values.empty() || values.count(".*") ||
         values.count(lower(path.extension().u8string()));
}
std::vector<BrowserEntry>
RomBrowser::scan(const std::filesystem::path &directory,
                 const std::string &filters) {
  std::vector<BrowserEntry> entries{{true, "..", ICON_FK_FOLDER " .."}};
  std::error_code ec;
  for (std::filesystem::directory_iterator it(directory, ec), end;
       !ec && it != end; it.increment(ec)) {
    const auto &entry = *it;
    bool is_dir = entry.is_directory(ec);
    if (ec)
      break;
    if (!is_dir &&
        (!entry.is_regular_file(ec) || !matches(entry.path(), filters)))
      continue;
    auto name = entry.path().filename();
    const auto text = name.u8string();
    if (text.empty() || text.front() == '$')
      continue;
    entries.push_back(
        {is_dir, name,
         std::string(is_dir ? ICON_FK_FOLDER " " : ICON_FK_GAMEPAD " ") +
             text});
  }
  if (ec)
    throw std::filesystem::filesystem_error("Unable to browse directory",
                                            directory, ec);
  std::sort(entries.begin() + 1, entries.end(),
            [](const BrowserEntry &a, const BrowserEntry &b) {
              if (a.isDir != b.isDir)
                return a.isDir;
              return lower(a.name.u8string()) < lower(b.name.u8string());
            });
  return entries;
}
std::filesystem::path RomBrowser::target(const std::filesystem::path &directory,
                                         const std::string &name,
                                         const std::string &filters,
                                         bool save) {
  if (name.empty())
    throw std::runtime_error("Enter a filename.");
  auto path = std::filesystem::u8path(name);
  if (!path.is_absolute())
    path = directory / path;
  const auto values = extensions(filters);
  if (save && path.extension().empty() && values.size() == 1 &&
      !values.count(".*"))
    path += std::filesystem::u8path(*values.begin());
  path = std::filesystem::absolute(path).lexically_normal();
  if (std::filesystem::is_directory(path))
    throw std::runtime_error("Choose a file, not a directory.");
  if (!std::filesystem::is_directory(path.parent_path()))
    throw std::runtime_error("The destination directory does not exist.");
  if (!save && !std::filesystem::is_regular_file(path))
    throw std::runtime_error("The selected file does not exist.");
  if (!matches(path, filters))
    throw std::runtime_error(
        "The filename does not match the selected file types.");
  return path;
}
void RomBrowser::refresh() {
  records.clear();
  selected.clear();
  error.clear();
  refresh_pending = true;
  std::snprintf(path_text, sizeof(path_text), "%s",
                directory.u8string().c_str());
}
void RomBrowser::change_directory(const std::filesystem::path &path) {
  std::error_code ec;
  auto next = std::filesystem::absolute(path, ec).lexically_normal();
  if (ec || !std::filesystem::is_directory(next, ec)) {
    error = "Cannot open that directory.";
    std::snprintf(path_text, sizeof(path_text), "%s",
                  directory.u8string().c_str());
    return;
  }
  directory = next;
  refresh();
}
void RomBrowser::poll() {
  if (future.valid() &&
      future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      auto next = future.get();
      if (!refresh_pending)
        records = std::move(next);
    } catch (const std::exception &failure) {
      if (!refresh_pending)
        error = failure.what();
    }
  }
  // Coalesce navigation requests; never replace a running async future.
  if (!refresh_pending || future.valid())
    return;
  refresh_pending = false;
  future = std::async(std::launch::async, [path = directory, types = filters] {
    return scan(path, types);
  });
}
void RomBrowser::OpenDialog(const std::string &new_key,
                            const std::string &new_title, const char *types,
                            const BrowserConfig &config) {
  Close();
  key = new_key;
  title = new_title;
  filters = types ? types : ".*";
  save = (config.flags & BrowserFlags_ConfirmOverwrite) != 0;
  opened = true;
  first_frame = true;
  filename[0] = 0;
  directory = std::filesystem::current_path();
  if (!config.path.empty())
    change_directory(std::filesystem::u8path(config.path));
  if (!config.filePathName.empty()) {
    auto initial = std::filesystem::u8path(config.filePathName);
    if (initial.has_parent_path())
      change_directory(initial.parent_path());
    std::snprintf(filename, sizeof(filename), "%s",
                  initial.filename().u8string().c_str());
  }
  refresh();
}
bool RomBrowser::IsOpened(const std::string &wanted) const {
  return opened && (wanted.empty() || wanted == key);
}
void RomBrowser::Close() {
  opened = accepted = finished = overwrite_pending = false;
  result.clear();
  records.clear();
  refresh_pending = true;
}
void RomBrowser::submit() {
  try {
    auto candidate = target(directory, filename, filters, save);
    result = candidate;
    error.clear();
    if (save && std::filesystem::exists(candidate))
      overwrite_pending = true;
    else {
      accepted = finished = true;
    }
  } catch (const std::exception &failure) {
    error = failure.what();
  }
}
void RomBrowser::ChooseFile(const std::string &name) {
  if (!opened || finished || overwrite_pending)
    return;
  std::snprintf(filename, sizeof(filename), "%s", name.c_str());
  submit();
}
void RomBrowser::ConfirmOverwrite(bool replace) {
  if (!overwrite_pending)
    return;
  overwrite_pending = false;
  if (replace)
    accepted = finished = true;
}
bool RomBrowser::Display(const std::string &wanted, ImGuiWindowFlags flags,
                         ImVec2 minimum, ImVec2 maximum) {
  if (!IsOpened(wanted))
    return false;
  if (finished)
    return true;
  poll();
  const auto available = ImGui::GetIO().DisplaySize;
  if (maximum.x <= 0 || maximum.y <= 0)
    maximum = ImVec2(std::max(1.f, available.x * .95f),
                     std::max(1.f, available.y * .95f));
  minimum.x = std::min(minimum.x > 0 ? minimum.x : 320.f, maximum.x);
  minimum.y = std::min(minimum.y > 0 ? minimum.y : 240.f, maximum.y);
  ImGui::SetNextWindowSizeConstraints(minimum, maximum);
  if (first_frame) {
    ImGui::SetNextWindowSize(ImVec2(std::max(minimum.x, maximum.x * .85f),
                                    std::max(minimum.y, maximum.y * .85f)));
    ImGui::SetNextWindowPos(ImVec2(available.x * .5f, available.y * .5f),
                            ImGuiCond_Always, ImVec2(.5f, .5f));
    first_frame = false;
  }
  bool visible = true;
  const auto window_name = title + "##" + key;
  if (ImGui::Begin(window_name.c_str(), &visible,
                   flags | ImGuiWindowFlags_NoCollapse)) {
    ImGui::BeginDisabled(overwrite_pending);
    const auto &style = ImGui::GetStyle();
    const auto button_width = [&](const char *label) {
      return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.f;
    };
#ifdef _WIN32
    const std::string drive = directory.root_name().u8string();
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x,
                                    ImGui::GetFrameHeight() +
                                        button_width("C:")));
    if (ImGui::BeginCombo("##drive", drive.c_str())) {
      DWORD drives = GetLogicalDrives();
      for (int i = 0; i < 26; ++i)
        if (drives & (1u << i)) {
          char root[] = {char('A' + i), ':', '\\', 0};
          if (ImGui::Selectable(root))
            change_directory(std::filesystem::u8path(root));
        }
      ImGui::EndCombo();
    }
    toolbar_next(button_width(ICON_FK_LEVEL_UP " Up"));
#endif
    if (ImGui::Button(ICON_FK_LEVEL_UP " Up"))
      change_directory(directory.parent_path());
    toolbar_next(button_width("Refresh"));
    if (ImGui::Button("Refresh"))
      refresh();
    std::filesystem::path breadcrumb;
    int part_id = 0;
    for (const auto &part : directory) {
      breadcrumb /= part;
      if (directory.has_root_name() && part == directory.root_directory())
        continue;
      const auto label = part.u8string();
      const float separator_width = part_id > 0
          ? ImGui::CalcTextSize("/").x + style.ItemSpacing.x : 0.f;
      toolbar_next(separator_width + ImGui::CalcTextSize(label.c_str()).x);
      if (part_id > 0) {
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
      }
      ImGui::PushID(part_id++);
      if (breadcrumb_link(label.c_str())) {
        change_directory(part == directory.root_name() ? directory.root_path()
                                                       : breadcrumb);
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##directory", path_text, sizeof(path_text),
                         ImGuiInputTextFlags_EnterReturnsTrue))
      change_directory(std::filesystem::u8path(path_text));
    if (!error.empty())
      ImGui::TextWrapped("%s", error.c_str());
    if (refresh_pending || future.valid())
      ImGui::TextDisabled("Loading directory...");
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    if (ImGui::BeginChild("Files", ImVec2(0, -footer))) {
      std::filesystem::path navigate;
      for (const auto &entry : records) {
        bool clicked =
            ImGui::Selectable(entry.showName.c_str(), selected == entry.name,
                              ImGuiSelectableFlags_AllowDoubleClick |
                                  ImGuiSelectableFlags_NoAutoClosePopups);
        if (ImGui::IsItemHovered() &&
            ImGui::CalcTextSize(entry.showName.c_str()).x >
                ImGui::GetContentRegionAvail().x)
          ImGui::SetTooltip("%s", entry.showName.c_str());
        if (clicked) {
          selected = entry.name;
          if (!entry.isDir)
            std::snprintf(filename, sizeof(filename), "%s",
                          entry.name.u8string().c_str());
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (entry.isDir)
              navigate = entry.name == ".." ? directory.parent_path()
                                            : directory / entry.name;
            else
              submit();
          }
        }
      }
      if (!navigate.empty())
        change_directory(navigate);
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputText("##filename", filename, sizeof(filename),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button(save ? "Save" : "Open") || enter)
      submit();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      accepted = false;
      finished = true;
    }
    ImGui::SameLine();
    if (save)
      ImGui::TextDisabled("%s", filters.c_str());
    ImGui::EndDisabled();
    if (overwrite_pending)
      ImGui::OpenPopup("Replace existing file?");
    if (ImGui::BeginPopupModal("Replace existing file?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("Replace %s?", result.u8string().c_str());
      if (ImGui::Button("Replace")) {
        ConfirmOverwrite(true);
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Keep existing")) {
        ConfirmOverwrite(false);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }
  ImGui::End();
  if (!visible) {
    accepted = false;
    finished = true;
  }
  return finished;
}
