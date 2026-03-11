#include "cli/command_hotword.h"
#include "cli/editor_utils.h"
#include "common/i18n.h"
#include "common/core_config.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace {
static std::string FormatMsg1(const char* tmpl, const std::string& a) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), tmpl, a.c_str());
  return buf;
}
} // namespace

int RunHotwordList(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &w : config.hotwords) {
      arr.push_back(w);
    }
    fmt.PrintJson(arr);
    return 0;
  }

  if (config.hotwords.empty()) {
    fmt.PrintInfo(_("No hotwords configured."));
    return 0;
  }

  std::vector<std::string> headers = {"HOTWORD"};
  std::vector<std::vector<std::string>> rows;
  for (const auto &w : config.hotwords) {
    rows.push_back({w});
  }
  fmt.PrintTable(headers, rows);
  return 0;
}

int RunHotwordLoad(const std::string &file_path, Formatter &fmt,
                   const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  std::vector<std::string> new_hotwords;

  auto process_stream = [&new_hotwords](std::istream &is) {
    std::string line;
    while (std::getline(is, line)) {
      // Trim whitespace
      line.erase(line.begin(),
                 std::find_if(line.begin(), line.end(), [](unsigned char ch) {
                   return !std::isspace(ch);
                 }));
      line.erase(
          std::find_if(line.rbegin(), line.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          line.end());

      if (!line.empty()) {
        new_hotwords.push_back(line);
      }
    }
  };

  if (file_path == "-") {
    process_stream(std::cin);
  } else {
    std::ifstream file(file_path);
    if (!file.is_open()) {
      fmt.PrintError(FormatMsg1(_("Failed to open file: %s"), file_path));
      return 1;
    }
    process_stream(file);
  }

  config.hotwords = new_hotwords;
  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }

  char buf[256];
  std::snprintf(buf, sizeof(buf), _("Loaded %zu hotwords."),
                new_hotwords.size());
  fmt.PrintSuccess(buf);
  return 0;
}

int RunHotwordClear(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();
  config.hotwords.clear();

  if (!SaveCoreConfig(config)) {
    fmt.PrintError(_("Failed to save config."));
    return 1;
  }

  fmt.PrintSuccess(_("Cleared all hotwords."));
  return 0;
}

int RunHotwordEdit(Formatter &fmt, const CliContext &ctx) {
  (void)ctx;

  // Write current hotwords to a temp file
  char tmp_template[] = "/tmp/vinput_hotwords_XXXXXX";
  int fd = mkstemp(tmp_template);
  if (fd == -1) {
    fmt.PrintError(_("Failed to create temporary file for editing."));
    return 1;
  }
  close(fd);

  std::string tmp_path = tmp_template;
  {
    CoreConfig config = LoadCoreConfig();
    std::ofstream out(tmp_path);
    for (const auto &w : config.hotwords) {
      out << w << "\n";
    }
  }

  // Open editor
  int ret = OpenInEditor(tmp_path);
  if (ret != 0) {
    std::remove(tmp_path.c_str());
    fmt.PrintError(_("Editor exited with error."));
    return ret;
  }

  // Load back
  ret = RunHotwordLoad(tmp_path, fmt, ctx);
  std::remove(tmp_path.c_str());

  if (ret == 0) {
    fmt.PrintSuccess(_("Hotwords updated from editor."));
  }
  return ret;
}
