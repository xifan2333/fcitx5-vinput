#include "cli/utils/editor_utils.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <subprocess.hpp>
#include <vector>
#include <wordexp.h>

#include "common/utils/scope_guard.h"

int OpenInEditor(const std::filesystem::path& file_path) {
  const char* editor = getenv("VISUAL");
  if (!editor || editor[0] == '\0') {
    editor = getenv("EDITOR");
  }
  if (!editor || editor[0] == '\0') {
    editor = "vi";
  }

  wordexp_t we{};
  if (wordexp(editor, &we, WRDE_NOCMD) != 0) {
    return -1;
  }
  auto we_guard = vinput::make_scope_exit([&we] { wordfree(&we); });
  if (we.we_wordc == 0) {
    return -1;
  }

  std::vector<std::string> args;
  args.reserve(we.we_wordc + 1);
  for (std::size_t i = 0; i < we.we_wordc; ++i) {
    args.emplace_back(we.we_wordv[i]);
  }
  args.push_back(file_path.string());

  try {
    return subprocess::call(args);
  } catch (const std::exception&) {
    return -1;
  }
}
