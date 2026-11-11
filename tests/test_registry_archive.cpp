#include <archive.h>
#include <archive_entry.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/registry/registry_models.h"
#include "common/utils/file_utils.h"

namespace fs = std::filesystem;

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

struct TempTestDir {
  std::unique_ptr<vinput::file::ScopedTempDir> holder;
  fs::path path;

  TempTestDir() {
    std::string error;
    holder = vinput::file::ScopedTempDir::Create("", ".tmp-registry-archive-test-", &error);
    TEST_ASSERT(holder != nullptr);
    path = fs::weakly_canonical(holder->Path());
  }
};

struct ArchiveEntryDesc {
  std::string path;
  std::string content;
  mode_t file_type = AE_IFREG;
  std::string symlink_target;
  std::string hardlink_target;
};

void CreateTarArchive(const fs::path& tar_path, const std::vector<ArchiveEntryDesc>& entries) {
  struct archive* a = archive_write_new();
  TEST_ASSERT(a != nullptr);
  TEST_ASSERT(archive_write_set_format_pax_restricted(a) == ARCHIVE_OK);
  TEST_ASSERT(archive_write_add_filter_gzip(a) == ARCHIVE_OK);
  TEST_ASSERT(archive_write_open_filename(a, tar_path.c_str()) == ARCHIVE_OK);

  for (const auto& item : entries) {
    struct archive_entry* entry = archive_entry_new();
    TEST_ASSERT(entry != nullptr);
    archive_entry_set_pathname(entry, item.path.c_str());
    archive_entry_set_filetype(entry, item.file_type);
    archive_entry_set_perm(entry, 0644);
    if (!item.symlink_target.empty()) {
      archive_entry_set_symlink(entry, item.symlink_target.c_str());
    }
    if (!item.hardlink_target.empty()) {
      archive_entry_set_hardlink(entry, item.hardlink_target.c_str());
    }
    if (item.file_type == AE_IFREG) {
      archive_entry_set_size(entry, static_cast<la_int64_t>(item.content.size()));
    }
    TEST_ASSERT(archive_write_header(a, entry) == ARCHIVE_OK);
    if (item.file_type == AE_IFREG && !item.content.empty()) {
      const auto written = archive_write_data(a, item.content.data(), item.content.size());
      TEST_ASSERT(written == static_cast<la_ssize_t>(item.content.size()));
    }
    archive_entry_free(entry);
  }

  TEST_ASSERT(archive_write_close(a) == ARCHIVE_OK);
  TEST_ASSERT(archive_write_free(a) == ARCHIVE_OK);
}

void TestVerifySha256() {
  const TempTestDir temp;
  const fs::path file = temp.path / "sample.txt";
  const std::string content = "fcitx5-vinput archive verification test";

  std::ofstream out(file, std::ios::binary);
  out << content;
  out.close();

  std::string error;
  // Empty expectation is a no-op success.
  TEST_ASSERT(ModelRepository::VerifySha256(file, "", &error));

  const auto actual_sha256 = vinput::file::ComputeFileSha256(file);
  TEST_ASSERT(actual_sha256.has_value());

  // Matching sha256 succeeds.
  TEST_ASSERT(ModelRepository::VerifySha256(file, *actual_sha256, &error));

  // Mismatched sha256 fails with descriptive error.
  error.clear();
  const std::string wrong_sha = "0000000000000000000000000000000000000000000000000000000000000000";
  TEST_ASSERT(!ModelRepository::VerifySha256(file, wrong_sha, &error));
  TEST_ASSERT(error.find("SHA256 mismatch") != std::string::npos);

  // Missing file fails.
  error.clear();
  const fs::path missing_file = temp.path / "does_not_exist.bin";
  TEST_ASSERT(!ModelRepository::VerifySha256(missing_file, wrong_sha, &error));
  TEST_ASSERT(!error.empty());
}

void TestExtractValidArchive() {
  const TempTestDir temp;
  const fs::path tar_file = temp.path / "valid.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::vector<ArchiveEntryDesc> entries = {
      {.path = "model/vinput-model.json", .content = "{\"family\":\"transducer\"}"},
      {.path = "model/tokens.txt", .content = "<blk> 0\n"}};
  CreateTarArchive(tar_file, entries);

  std::string error;
  TEST_ASSERT(ModelRepository::ExtractArchive(tar_file, dest_dir, &error));

  const fs::path json_path = dest_dir / "model" / "vinput-model.json";
  const fs::path tokens_path = dest_dir / "model" / "tokens.txt";
  TEST_ASSERT(fs::is_regular_file(json_path));
  TEST_ASSERT(fs::is_regular_file(tokens_path));

  std::ifstream in(json_path);
  std::string read_content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  TEST_ASSERT(read_content == "{\"family\":\"transducer\"}");
}

void TestExtractRejectsAbsolutePath() {
  const TempTestDir temp;
  const fs::path tar_file = temp.path / "malicious_abs.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::vector<ArchiveEntryDesc> entries = {{.path = "/etc/evil_config.txt", .content = "payload"}};
  CreateTarArchive(tar_file, entries);

  std::string error;
  TEST_ASSERT(!ModelRepository::ExtractArchive(tar_file, dest_dir, &error));
  TEST_ASSERT(error.find("archive contains absolute path") != std::string::npos);
}

void TestExtractRejectsPathTraversal() {
  const TempTestDir temp;
  const fs::path tar_file = temp.path / "malicious_traversal.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::vector<ArchiveEntryDesc> entries = {
      {.path = "model/../../escape.txt", .content = "escaped payload"}};
  CreateTarArchive(tar_file, entries);

  std::string error;
  TEST_ASSERT(!ModelRepository::ExtractArchive(tar_file, dest_dir, &error));
  TEST_ASSERT(error.find("archive contains path traversal component") != std::string::npos);
}

void TestExtractRejectsSymlink() {
  const TempTestDir temp;
  const fs::path tar_file = temp.path / "malicious_symlink.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::vector<ArchiveEntryDesc> entries = {
      {.path = "model/link_target", .file_type = AE_IFLNK, .symlink_target = "/etc/passwd"}};
  CreateTarArchive(tar_file, entries);

  std::string error;
  TEST_ASSERT(!ModelRepository::ExtractArchive(tar_file, dest_dir, &error));
  TEST_ASSERT(error.find("archive contains link entry") != std::string::npos);
}

void TestExtractRejectsHardlink() {
  const TempTestDir temp;
  const fs::path tar_file = temp.path / "malicious_hardlink.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::vector<ArchiveEntryDesc> entries = {
      {.path = "model/existing", .content = "real content"},
      {.path = "model/hardlink", .hardlink_target = "model/existing"}};
  CreateTarArchive(tar_file, entries);

  std::string error;
  TEST_ASSERT(!ModelRepository::ExtractArchive(tar_file, dest_dir, &error));
  TEST_ASSERT(error.find("archive contains link entry") != std::string::npos);
}

void TestExtractRejectsCorruptArchive() {
  const TempTestDir temp;
  const fs::path bad_tar = temp.path / "corrupt.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::ofstream out(bad_tar, std::ios::binary);
  out << "not a valid gzip archive header bytes";
  out.close();

  std::string error;
  TEST_ASSERT(!ModelRepository::ExtractArchive(bad_tar, dest_dir, &error));
  TEST_ASSERT(!error.empty());
}

void TestExtractRejectsNonExistentFile() {
  const TempTestDir temp;
  const fs::path non_existent = temp.path / "does_not_exist.tar.gz";
  const fs::path dest_dir = temp.path / "extracted";

  std::string error;
  TEST_ASSERT(!ModelRepository::ExtractArchive(non_existent, dest_dir, &error));
  TEST_ASSERT(error.find("failed to open archive") != std::string::npos);
}

} // namespace

int main() {
  TestVerifySha256();
  TestExtractValidArchive();
  TestExtractRejectsAbsolutePath();
  TestExtractRejectsPathTraversal();
  TestExtractRejectsSymlink();
  TestExtractRejectsHardlink();
  TestExtractRejectsCorruptArchive();
  TestExtractRejectsNonExistentFile();
  std::printf("All test_registry_archive cases passed successfully.\n");
  return 0;
}
