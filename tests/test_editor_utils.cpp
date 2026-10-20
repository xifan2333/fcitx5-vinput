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
  auto args = SplitEditorCommand("code --wait");
  TEST_ASSERT(args.size() == 2 && args[0] == "code" && args[1] == "--wait");

  args = SplitEditorCommand("sh -c 'exit 0'");
  TEST_ASSERT(args.size() == 3 && args[0] == "sh" && args[1] == "-c" && args[2] == "exit 0");

  args = SplitEditorCommand("\"/path with spaces/editor\" -w");
  TEST_ASSERT(args.size() == 2 && args[0] == "/path with spaces/editor" && args[1] == "-w");

  args = SplitEditorCommand("vi");
  TEST_ASSERT(args.size() == 1 && args[0] == "vi");
}

int main() {
  std::cout << "Running test_editor_utils...\n";
  TestEditorCommandSplitting();
  std::cout << "All test_editor_utils passed!\n";
  return 0;
}
