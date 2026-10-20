#include "cli/utils/editor_utils.h"

#include <CLI/StringTools.hpp>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <subprocess.hpp>
#include <vector>

int OpenInEditor(const std::filesystem::path& file_path) {
  const char* editor = std::getenv("VISUAL");
  if (editor == nullptr || editor[0] == '\0') {
    editor = std::getenv("EDITOR");
  }
  if (editor == nullptr || editor[0] == '\0') {
    editor = "vi";
  }

  auto args = CLI::detail::split_up(editor);
  CLI::detail::remove_quotes(args);
  if (args.empty()) {
    return -1;
  }
  args.push_back(file_path.string());

  try {
    return subprocess::call(args);
  } catch (const std::exception&) {
    return -1;
  }
}
