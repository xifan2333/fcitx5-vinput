#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "common/config/core_config_types.h"
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
    manifest.RecordInstalled("trigger.test.t1", "trigger", "1.1.0", "11112222");
    manifest.RecordInstalled("receiver.test.r1", "receiver", "1.2.0", "33334444");
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

    const auto* t = loaded.Get("trigger.test.t1");
    ExpectTrue(t != nullptr);
    ExpectEq(t->id, "trigger.test.t1");
    ExpectEq(t->kind, "trigger");
    ExpectEq(t->version, "1.1.0");

    const auto* r = loaded.Get("receiver.test.r1");
    ExpectTrue(r != nullptr);
    ExpectEq(r->id, "receiver.test.r1");
    ExpectEq(r->kind, "receiver");
    ExpectEq(r->version, "1.2.0");

    ExpectTrue(loaded.Get("nonexistent") == nullptr);
    ExpectEq(std::to_string(loaded.List().size()), "4");

    ExpectTrue(loaded.Remove("provider.test.foo"));
    ExpectTrue(loaded.Get("provider.test.foo") == nullptr);
    ExpectTrue(!loaded.Remove("provider.test.foo"));

    ExpectTrue(loaded.Remove("trigger.test.t1"));
    ExpectTrue(loaded.Get("trigger.test.t1") == nullptr);
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
  manifest.RecordInstalled("trigger.test.t1", "trigger", "1.0.0", "sha_t1");

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

  // Case 4: trigger resource version checking
  vinput::script::RegistryEntry trigger_entry;
  trigger_entry.id = "trigger.test.t1";
  trigger_entry.version = "1.0.0";
  trigger_entry.sha256 = "sha_t1";
  auto info_trigger_not_installed = vinput::script::CheckResourceVersion(
      manifest, vinput::script::Kind::kTrigger, trigger_entry, false);
  ExpectTrue(info_trigger_not_installed.status == vinput::script::UpdateStatus::kNotInstalled);

  // Case 5: singular vs plural ID prefixes
  ExpectTrue(
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kTrigger, "triggers.test.t1")
          .empty());
  ExpectTrue(
      !vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kTrigger, "trigger.test.t1")
           .empty());
}

void TestMaterializeTriggerAndReceiver() {
  CoreConfig config;

  vinput::script::RegistryEntry trigger_entry;
  trigger_entry.id = "trigger.vendor.mock_trigger";
  trigger_entry.command = "python3";
  trigger_entry.envs = {{"API_KEY", true}, {"OPTIONAL_PARAM", false}};

  const auto script_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kTrigger, trigger_entry.id);
  ExpectTrue(!script_path.empty());

  std::string err;
  // 1. Materialize new trigger
  ExpectTrue(vinput::script::MaterializeTrigger(&config, trigger_entry, script_path, &err));
  ExpectTrue(err.empty());
  ExpectEq(std::to_string(config.triggers.size()), "1");
  ExpectEq(config.triggers[0].id, "trigger.vendor.mock_trigger");
  ExpectEq(config.triggers[0].command, "python3");
  ExpectEq(config.triggers[0].args[0], script_path.string());
  ExpectTrue(!config.triggers[0].autoStart);
  ExpectTrue(config.triggers[0].env.contains("API_KEY"));

  // Set autoStart and custom env value
  config.triggers[0].autoStart = true;
  config.triggers[0].env["API_KEY"] = "secret123";

  // 2. Materialize existing managed trigger: preserves autoStart and existing env value
  trigger_entry.command = "python3.11";
  ExpectTrue(vinput::script::MaterializeTrigger(&config, trigger_entry, script_path, &err));
  ExpectTrue(config.triggers[0].autoStart);
  ExpectEq(config.triggers[0].command, "python3.11");
  ExpectEq(config.triggers[0].env["API_KEY"], "secret123");

  // 3. Refuse to overwrite user-defined trigger
  config.triggers[0].args = {"/usr/local/bin/custom_script.sh"};
  ExpectTrue(!vinput::script::MaterializeTrigger(&config, trigger_entry, script_path, &err));
  ExpectTrue(err.find("refusing to overwrite user-defined trigger") != std::string::npos);

  // 4. Materialize new receiver
  vinput::script::RegistryEntry receiver_entry;
  receiver_entry.id = "receiver.vendor.mock_receiver";
  receiver_entry.command = "bash";
  const auto receiver_path =
      vinput::script::DefaultLocalScriptPath(vinput::script::Kind::kReceiver, receiver_entry.id);
  ExpectTrue(!receiver_path.empty());

  ExpectTrue(vinput::script::MaterializeReceiver(&config, receiver_entry, receiver_path, &err));
  ExpectEq(std::to_string(config.receivers.size()), "1");
  ExpectEq(config.receivers[0].id, "receiver.vendor.mock_receiver");
  ExpectEq(config.receivers[0].command, "bash");
  ExpectEq(config.receivers[0].args[0], receiver_path.string());
  ExpectTrue(!config.receivers[0].autoStart);

  // 5. Refuse to overwrite user-defined receiver
  config.receivers[0].args = {"/custom/receiver.sh"};
  ExpectTrue(!vinput::script::MaterializeReceiver(&config, receiver_entry, receiver_path, &err));
  ExpectTrue(err.find("refusing to overwrite user-defined receiver") != std::string::npos);
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
  TestMaterializeTriggerAndReceiver();
  TestSemanticVersionComparison();
  std::cout << "All installed_manifest tests passed!\n";
  return 0;
}
