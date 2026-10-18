#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"

namespace fs = std::filesystem;

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestIsPathWithinRoot() {
  using vinput::path::IsPathWithinRoot;

  // Exact match
  TEST_ASSERT(IsPathWithinRoot("/a/b/c", "/a/b/c"));

  // Subpath
  TEST_ASSERT(IsPathWithinRoot("/a/b/c/d", "/a/b/c"));
  TEST_ASSERT(IsPathWithinRoot("/a/b/c/d/e.txt", "/a/b/c"));

  // Trailing slash in root
  TEST_ASSERT(IsPathWithinRoot("/a/b/c", "/a/b/"));
  TEST_ASSERT(IsPathWithinRoot("/a/b/c/d", "/a/b/"));

  // Root directory
  TEST_ASSERT(IsPathWithinRoot("/a/b/c", "/"));

  // Traversal with .. escaping root
  TEST_ASSERT(!IsPathWithinRoot("/a/b/c/../../x", "/a/b"));

  // Sibling with common prefix name (must not match because components differ)
  TEST_ASSERT(!IsPathWithinRoot("/a/b/c_extra", "/a/b/c"));
  TEST_ASSERT(!IsPathWithinRoot("/a/b/cd", "/a/b/c"));

  // Completely unrelated path
  TEST_ASSERT(!IsPathWithinRoot("/other/path", "/a/b/c"));
  TEST_ASSERT(!IsPathWithinRoot("/a/b", "/a/b/c"));

  // Relative paths
  TEST_ASSERT(IsPathWithinRoot("models/sherpa/model.onnx", "models/sherpa"));
  TEST_ASSERT(!IsPathWithinRoot("models/other/model.onnx", "models/sherpa"));
  TEST_ASSERT(!IsPathWithinRoot("models/sherpa/../other/model.onnx", "models/sherpa"));
}

void TestScopedTempDirLifecycle() {
  using vinput::file::ScopedTempDir;

  fs::path created_path;
  {
    std::string error;
    auto temp_dir = ScopedTempDir::Create("", ".tmp-test-", &error);
    TEST_ASSERT(temp_dir != nullptr);
    TEST_ASSERT(error.empty());
    TEST_ASSERT(temp_dir->IsValid());

    created_path = temp_dir->Path();
    TEST_ASSERT(!created_path.empty());
    TEST_ASSERT(fs::exists(created_path));
    TEST_ASSERT(fs::is_directory(created_path));

    // Create a file inside temp dir
    const fs::path test_file = created_path / "hello.txt";
    {
      std::ofstream out(test_file);
      out << "sample content";
    }
    TEST_ASSERT(fs::exists(test_file));
  } // temp_dir goes out of scope here

  // Ensure directory and contained file are cleaned up
  TEST_ASSERT(!fs::exists(created_path));
}

void TestScopedTempDirMove() {
  using vinput::file::ScopedTempDir;

  fs::path created_path;
  {
    std::string error;
    auto dir1 = ScopedTempDir::Create("", ".tmp-move-", &error);
    TEST_ASSERT(dir1 != nullptr);
    TEST_ASSERT(dir1->IsValid());

    created_path = dir1->Path();
    TEST_ASSERT(fs::exists(created_path));

    // Move construct
    const ScopedTempDir dir2 = std::move(*dir1);
    TEST_ASSERT(!dir1->IsValid());
    TEST_ASSERT(dir2.IsValid());
    TEST_ASSERT(dir2.Path() == created_path);
    TEST_ASSERT(fs::exists(created_path));
  } // dir2 goes out of scope here

  TEST_ASSERT(!fs::exists(created_path));
}

void TestScopedTempDirDismiss() {
  using vinput::file::ScopedTempDir;

  fs::path created_path;
  {
    std::string error;
    auto temp_dir = ScopedTempDir::Create("", ".tmp-dismiss-", &error);
    TEST_ASSERT(temp_dir != nullptr);
    TEST_ASSERT(temp_dir->IsValid());

    created_path = temp_dir->Path();
    TEST_ASSERT(fs::exists(created_path));

    // Dismiss auto cleanup
    temp_dir->Dismiss();
    TEST_ASSERT(!temp_dir->IsValid());
  } // temp_dir goes out of scope here

  // Should still exist because Dismiss() was called
  TEST_ASSERT(fs::exists(created_path));

  // Manual cleanup
  std::error_code ec;
  fs::remove_all(created_path, ec);
  TEST_ASSERT(!fs::exists(created_path));
}

int main() {
  TestIsPathWithinRoot();
  TestScopedTempDirLifecycle();
  TestScopedTempDirMove();
  TestScopedTempDirDismiss();
  std::printf("test_file_utils: all tests passed\n");
  return 0;
}
