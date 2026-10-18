#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
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

void TestComputeFileSha256() {
  using vinput::file::ComputeFileSha256;
  using vinput::file::ScopedTempDir;

  auto temp_dir = ScopedTempDir::Create("", ".tmp-test-sha-");
  TEST_ASSERT(temp_dir != nullptr);

  // 1. Known content ("hello world\n")
  const fs::path file1 = temp_dir->Path() / "hello.txt";
  {
    std::ofstream out(file1, std::ios::binary);
    out << "hello world\n";
  }
  std::string err;
  const std::string hash1 = ComputeFileSha256(file1, &err);
  TEST_ASSERT(err.empty());
  TEST_ASSERT(hash1 == "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447");

  // 2. Empty file
  const fs::path empty_file = temp_dir->Path() / "empty.txt";
  std::ofstream out_empty(empty_file, std::ios::binary);
  out_empty.close();
  const std::string hash_empty = ComputeFileSha256(empty_file, &err);
  TEST_ASSERT(err.empty());
  TEST_ASSERT(hash_empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // 3. Multi-chunk file (> 8192 bytes buffer, e.g. 10000 bytes of 'a')
  const fs::path large_file = temp_dir->Path() / "large.txt";
  {
    std::ofstream out(large_file, std::ios::binary);
    std::string data(10000, 'a');
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  const std::string hash_large = ComputeFileSha256(large_file, &err);
  TEST_ASSERT(err.empty());
  TEST_ASSERT(hash_large == "27dd1f61b867b6a0f6e9d8a41c43231de52107e53ae424de8f847b821db4b711");

  // 4. Non-existent file
  const fs::path missing_file = temp_dir->Path() / "missing.txt";
  const std::string hash_missing = ComputeFileSha256(missing_file, &err);
  TEST_ASSERT(hash_missing.empty());
  TEST_ASSERT(!err.empty());

  // 5. Nullptr error pointer
  const std::string hash_null_err = ComputeFileSha256(missing_file, nullptr);
  TEST_ASSERT(hash_null_err.empty());
}

void TestReadTextFile() {
  using vinput::file::ReadTextFile;
  using vinput::file::ScopedTempDir;

  auto temp_dir = ScopedTempDir::Create("", ".tmp-test-read-");
  TEST_ASSERT(temp_dir != nullptr);

  const fs::path test_file = temp_dir->Path() / "sample.txt";
  {
    std::ofstream out(test_file, std::ios::binary);
    out << "hello world";
  }

  // 1. Unlimited read (max_bytes = 0)
  std::string content;
  std::string err;
  TEST_ASSERT(ReadTextFile(test_file, &content, &err, 0));
  TEST_ASSERT(err.empty());
  TEST_ASSERT(content == "hello world");

  // 2. Defensive truncated read (max_bytes = 5)
  content.clear();
  TEST_ASSERT(ReadTextFile(test_file, &content, &err, 5));
  TEST_ASSERT(err.empty());
  TEST_ASSERT(content == "hello");

  // 3. Max bytes exceeds file size (max_bytes = 100)
  content.clear();
  TEST_ASSERT(ReadTextFile(test_file, &content, &err, 100));
  TEST_ASSERT(err.empty());
  TEST_ASSERT(content == "hello world");

  // 4. Empty file with max_bytes limit
  const fs::path empty_file = temp_dir->Path() / "empty.txt";
  std::ofstream out_empty(empty_file, std::ios::binary);
  out_empty.close();
  content.clear();
  TEST_ASSERT(ReadTextFile(empty_file, &content, &err, 10));
  TEST_ASSERT(err.empty());
  TEST_ASSERT(content.empty());

  // 5. Multi-chunk read with max_bytes limit (12000 of 20000 bytes)
  const fs::path large_file = temp_dir->Path() / "large.txt";
  {
    std::ofstream out(large_file, std::ios::binary);
    std::string data(20000, 'x');
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }
  content.clear();
  TEST_ASSERT(ReadTextFile(large_file, &content, &err, 12000));
  TEST_ASSERT(err.empty());
  TEST_ASSERT(content.size() == 12000);
  TEST_ASSERT(content == std::string(12000, 'x'));

  // 6. Non-existent file
  const fs::path missing_file = temp_dir->Path() / "missing.txt";
  content.clear();
  TEST_ASSERT(!ReadTextFile(missing_file, &content, &err));
  TEST_ASSERT(!err.empty());

  // 7. Null content buffer
  TEST_ASSERT(!ReadTextFile(test_file, nullptr, &err));
  TEST_ASSERT(err == "ReadTextFile requires a destination buffer.");
}

int main() {
  TestIsPathWithinRoot();
  TestScopedTempDirLifecycle();
  TestScopedTempDirMove();
  TestScopedTempDirDismiss();
  TestComputeFileSha256();
  TestReadTextFile();
  std::printf("test_file_utils: all tests passed\n");
  return 0;
}
