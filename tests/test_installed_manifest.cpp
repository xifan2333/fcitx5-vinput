#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "common/registry/installed_manifest.h"
#include "common/registry/registry_scripts.h"
#include "common/utils/file_utils.h"
#include "common/utils/version_utils.h"

namespace {

void ExpectTrue(bool condition) {
  if (!condition) {
    std::cerr << "ExpectTrue failed!\n";
    std::abort();
  }
}

void ExpectEq(const std::string& actual, const std::string& expected) {
  if (actual != expected) {
    std::cerr << "ExpectEq failed: expected '" << expected << "', got '" << actual << "'\n";
    std::abort();
  }
}

void TestManifestSaveLoad() {
  const auto tmp_dir = std::filesystem::temp_directory_path() / "vinput_test_manifest";
  std::filesystem::create_directories(tmp_dir);
  const auto manifest_path = tmp_dir / "installed.json";

  {
    vinput::registry::InstalledManifest manifest;
    manifest.RecordInstalled("provider.test.foo", "provider", "1.0.0", "abcdef1234");
    manifest.RecordInstalled("adapter.test.bar", "adapter", "2.0.0", "98765fedcba");
    std::string err;
    ExpectTrue(manifest.Save(manifest_path, &err));
    ExpectTrue(err.empty());
  }

  {
    auto loaded = vinput::registry::InstalledManifest::Load(manifest_path);
    const auto* p = loaded.Get("provider.test.foo");
    ExpectTrue(p != nullptr);
    ExpectEq(p->id, "provider.test.foo");
    ExpectEq(p->kind, "provider");
    ExpectEq(p->version, "1.0.0");
    ExpectEq(p->sha256, "abcdef1234");

    const auto* a = loaded.Get("adapter.test.bar");
    ExpectTrue(a != nullptr);
    ExpectEq(a->id, "adapter.test.bar");
    ExpectEq(a->kind, "adapter");
    ExpectEq(a->version, "2.0.0");

    ExpectTrue(loaded.Get("nonexistent") == nullptr);
    ExpectEq(std::to_string(loaded.List().size()), "2");

    ExpectTrue(loaded.Remove("provider.test.foo"));
    ExpectTrue(loaded.Get("provider.test.foo") == nullptr);
    ExpectTrue(!loaded.Remove("provider.test.foo"));
  }

  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);
}

void TestComputeFileSha256() {
  const auto tmp_file = std::filesystem::temp_directory_path() / "vinput_test_sha.txt";
  {
    std::ofstream out(tmp_file, std::ios::binary);
    out << "hello world\n";
  }

  const auto hex = vinput::file::ComputeFileSha256(tmp_file);
  ExpectTrue(hex.has_value());
  // printf "hello world\n" | sha256sum ->
  // a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447
  ExpectEq(*hex, "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447");

  std::error_code ec;
  std::filesystem::remove(tmp_file, ec);
}

void TestCheckResourceVersion() {
  vinput::registry::InstalledManifest manifest;
  manifest.RecordInstalled("provider.test.p1", "provider", "1.0.0", "sha1");

  vinput::script::RegistryEntry remote_entry;
  remote_entry.id = "provider.test.p1";
  remote_entry.short_id = "test-p1";
  remote_entry.version = "1.0.0";
  remote_entry.sha256 = "sha1";

  // Case 1: not installed in config
  auto info_not_installed = vinput::script::CheckResourceVersion(
      manifest, vinput::script::Kind::kAsrProvider, remote_entry, false);
  ExpectTrue(info_not_installed.status == vinput::script::UpdateStatus::kNotInstalled);

  // Case 2: update available when remote version is newer
  remote_entry.version = "1.1.0";
  remote_entry.sha256 = "sha2";
  auto info_update = vinput::script::CheckResourceVersion(
      manifest, vinput::script::Kind::kAsrProvider, remote_entry, true);
  ExpectTrue(info_update.status == vinput::script::UpdateStatus::kUpdateAvailable);
  ExpectEq(info_update.installed_version, "1.0.0");
  ExpectEq(info_update.remote_version, "1.1.0");

  // Case 3: incompatible host requirement
  remote_entry.vinput_version = ">= 99.0.0";
  auto info_incompatible = vinput::script::CheckResourceVersion(
      manifest, vinput::script::Kind::kAsrProvider, remote_entry, false);
  ExpectTrue(info_incompatible.status == vinput::script::UpdateStatus::kIncompatible);
}

void TestSemanticVersionComparison() {
  using vinput::version::Satisfies;

  ExpectTrue(Satisfies("2.4.1", ""));
  ExpectTrue(Satisfies("2.4.1", "*"));
  ExpectTrue(Satisfies("2.4.1", ">= 2.0.0"));
  ExpectTrue(Satisfies("2.4.1", ">= 2.4.1"));
  ExpectTrue(Satisfies("2.4.1", "<= 2.4.1"));
  ExpectTrue(Satisfies("2.4.1", "< 3.0.0"));
  ExpectTrue(Satisfies("2.4.1", "> 2.4.0"));
  ExpectTrue(Satisfies("2.4.1", "= 2.4.1"));
  ExpectTrue(Satisfies("2.4.1", "2.4.1"));

  // Range
  ExpectTrue(Satisfies("2.4.1", ">= 2.0.0, < 3.0.0"));
  ExpectTrue(Satisfies("2.4.1", ">= 2.4.0, <= 2.5.0"));
  ExpectTrue(!Satisfies("2.4.1", ">= 2.5.0"));
  ExpectTrue(!Satisfies("2.4.1", "< 2.4.0"));
  ExpectTrue(!Satisfies("2.4.1", ">= 2.0.0, < 2.4.0"));
}

} // namespace

int main() {
  TestManifestSaveLoad();
  TestComputeFileSha256();
  TestCheckResourceVersion();
  TestSemanticVersionComparison();
  std::cout << "All installed_manifest tests passed!\n";
  return 0;
}
