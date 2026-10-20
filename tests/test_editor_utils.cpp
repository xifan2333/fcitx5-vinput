#include <CLI/StringTools.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cli/utils/editor_utils.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << "\n";
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestEditorCommandSplitting() {
  auto args = CLI::detail::split_up("code --wait");
  CLI::detail::remove_quotes(args);
  TEST_ASSERT(args.size() == 2 && args[0] == "code" && args[1] == "--wait");

  args = CLI::detail::split_up("sh -c 'exit 0'");
  CLI::detail::remove_quotes(args);
  TEST_ASSERT(args.size() == 3 && args[0] == "sh" && args[1] == "-c" && args[2] == "exit 0");

  args = CLI::detail::split_up("\"/path with spaces/editor\" -w");
  CLI::detail::remove_quotes(args);
  TEST_ASSERT(args.size() == 2 && args[0] == "/path with spaces/editor" && args[1] == "-w");

  args = CLI::detail::split_up("vi");
  CLI::detail::remove_quotes(args);
  TEST_ASSERT(args.size() == 1 && args[0] == "vi");
}

int main() {
  std::cout << "Running test_editor_utils...\n";
  TestEditorCommandSplitting();
  std::cout << "All test_editor_utils passed!\n";
  return 0;
}
