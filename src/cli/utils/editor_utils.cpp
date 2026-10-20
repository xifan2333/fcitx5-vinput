#include "cli/utils/editor_utils.h"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <subprocess.hpp>
#include <utility>
#include <vector>

bool SplitEditorCommand(std::string_view command, std::vector<std::string>* args) {
  if (args == nullptr) {
    return false;
  }
  args->clear();
  std::string current;
  bool in_single_quote = false;
  bool in_double_quote = false;
  bool escaping = false;

  for (const char ch : command) {
    if (escaping) {
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (!in_single_quote && ch == '\\') {
      escaping = true;
      continue;
    }
    if (!in_double_quote && ch == '\'') {
      in_single_quote = !in_single_quote;
      continue;
    }
    if (!in_single_quote && ch == '"') {
      in_double_quote = !in_double_quote;
      continue;
    }
    if (!in_single_quote && !in_double_quote && std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!current.empty()) {
        args->push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }

  if (escaping || in_single_quote || in_double_quote) {
    return false;
  }
  if (!current.empty()) {
    args->push_back(std::move(current));
  }
  return !args->empty();
}

int OpenInEditor(const std::filesystem::path& file_path) {
  const char* editor = std::getenv("VISUAL");
  if (editor == nullptr || editor[0] == '\0') {
    editor = std::getenv("EDITOR");
  }
  if (editor == nullptr || editor[0] == '\0') {
    editor = "vi";
  }

  std::vector<std::string> args;
  if (!SplitEditorCommand(editor, &args)) {
    return -1;
  }
  args.push_back(file_path.string());

  try {
    return subprocess::call(args);
  } catch (const std::exception&) {
    return -1;
  }
}
