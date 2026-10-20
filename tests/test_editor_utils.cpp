#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "cli/utils/editor_utils.h"

extern "C" int setenv(const char* name, const char* value, int overwrite);

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << "\n";
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestEditorSuccess() {
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto test_file = tmp_dir / "vinput_test_editor_file.txt";

  setenv("VISUAL", "true", 1);
  const int ret = OpenInEditor(test_file);
  TEST_ASSERT(ret == 0);
}

void TestEditorFailure() {
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto test_file = tmp_dir / "vinput_test_editor_file.txt";

  setenv("VISUAL", "false", 1);
  const int ret = OpenInEditor(test_file);
  TEST_ASSERT(ret == 1);
}

void TestEditorNonExistent() {
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto test_file = tmp_dir / "vinput_test_editor_file.txt";

  setenv("VISUAL", "/nonexistent/path/to/editor_xyz", 1);
  const int ret = OpenInEditor(test_file);
  TEST_ASSERT(ret == -1);
}

void TestEditorQuotedArguments() {
  const auto tmp_dir = std::filesystem::temp_directory_path();
  const auto test_file = tmp_dir / "vinput_test_editor_file.txt";

  setenv("VISUAL", "sh -c 'exit 0'", 1);
  const int ret = OpenInEditor(test_file);
  TEST_ASSERT(ret == 0);
}

int main() {
  std::cout << "Running test_editor_utils...\n";
  TestEditorSuccess();
  TestEditorFailure();
  TestEditorNonExistent();
  TestEditorQuotedArguments();
  std::cout << "All test_editor_utils passed!\n";
  return 0;
}
