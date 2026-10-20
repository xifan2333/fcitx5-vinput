#include "cli/utils/editor_utils.h"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <subprocess.hpp>
#include <utility>
#include <vector>

namespace {

enum class QuoteMode {
  None,
  Single,
  Double,
};

} // namespace

std::optional<std::vector<std::string>> SplitEditorCommand(std::string_view command) {
  std::vector<std::string> args;
  std::string current;
  auto mode = QuoteMode::None;
  bool escape = false;

  for (const char ch : command) {
    if (escape) {
      current.push_back(ch);
      escape = false;
    } else if (ch == '\\' && mode != QuoteMode::Single) {
      escape = true;
    } else if (ch == '\'' && mode != QuoteMode::Double) {
      mode = (mode == QuoteMode::Single) ? QuoteMode::None : QuoteMode::Single;
    } else if (ch == '"' && mode != QuoteMode::Single) {
      mode = (mode == QuoteMode::Double) ? QuoteMode::None : QuoteMode::Double;
    } else if (mode == QuoteMode::None && std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        args.push_back(std::exchange(current, {}));
      }
    } else {
      current.push_back(ch);
    }
  }

  if (escape || mode != QuoteMode::None) {
    return std::nullopt;
  }
  if (!current.empty()) {
    args.push_back(std::move(current));
  }
  if (args.empty()) {
    return std::nullopt;
  }
  return args;
}

int OpenInEditor(const std::filesystem::path& file_path) {
  const char* editor = std::getenv("VISUAL");
  if (editor == nullptr || editor[0] == '\0') {
    editor = std::getenv("EDITOR");
  }
  if (editor == nullptr || editor[0] == '\0') {
    editor = "vi";
  }

  auto args = SplitEditorCommand(editor);
  if (!args.has_value()) {
    return -1;
  }
  args->push_back(file_path.string());

  try {
    return subprocess::call(*args);
  } catch (const std::exception&) {
    return -1;
  }
}
