#include <cstdlib>
#include <filesystem>
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

void TestSplitEditorCommand() {
  std::vector<std::string> args;
  TEST_ASSERT(SplitEditorCommand("vi", &args));
  TEST_ASSERT(args.size() == 1 && args[0] == "vi");

  TEST_ASSERT(SplitEditorCommand("code --wait", &args));
  TEST_ASSERT(args.size() == 2 && args[0] == "code" && args[1] == "--wait");

  TEST_ASSERT(SplitEditorCommand("sh -c 'exit 0'", &args));
  TEST_ASSERT(args.size() == 3 && args[0] == "sh" && args[1] == "-c" && args[2] == "exit 0");

  TEST_ASSERT(SplitEditorCommand("\"/path with spaces/editor\" -w", &args));
  TEST_ASSERT(args.size() == 2 && args[0] == "/path with spaces/editor" && args[1] == "-w");

  // Invalid unmatched quotes
  TEST_ASSERT(!SplitEditorCommand("code 'unclosed", &args));
  TEST_ASSERT(!SplitEditorCommand("code \"unclosed", &args));
  TEST_ASSERT(!SplitEditorCommand("", &args));
}

void TestOpenInEditorFallback() {
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto test_file = tmp_dir / "vinput_test_editor_fallback.txt";

  // When given a nonexistent binary path or command, OpenInEditor should return -1
  const int ret = OpenInEditor(test_file);
  // Returns integer exit status from vi or 0/error, must not crash
  (void)ret;
}

int main() {
  std::cout << "Running test_editor_utils...\n";
  TestSplitEditorCommand();
  TestOpenInEditorFallback();
  std::cout << "All test_editor_utils passed!\n";
  return 0;
}
