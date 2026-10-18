#include "common/registry/installed_manifest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/time_utils.h"

namespace vinput::registry {

namespace {

using json = nlohmann::ordered_json;

std::filesystem::path ResolveManifestPath(const std::filesystem::path& path) {
  if (!path.empty()) {
    return path;
  }
  return vinput::path::InstalledManifestCachePath();
}

} // namespace

InstalledManifest InstalledManifest::Load(const std::filesystem::path& path) {
  InstalledManifest manifest;
  const auto actual_path = ResolveManifestPath(path);

  std::string content;
  std::string read_error;
  if (!vinput::file::ReadTextFile(actual_path, &content, &read_error)) {
    return manifest;
  }

  try {
    const json j = json::parse(content);
    if (!j.is_object() || !j.contains("resources") || !j.at("resources").is_object()) {
      return manifest;
    }

    for (const auto& [id, item] : j.at("resources").items()) {
      if (!item.is_object()) {
        continue;
      }
      InstalledRecord record;
      record.id = id;
      record.kind = item.value("kind", "");
      record.version = item.value("version", "");
      record.sha256 = item.value("sha256", "");
      record.installed_at = item.value("installed_at", static_cast<uint64_t>(0));
      manifest.records_.emplace(id, std::move(record));
    }
  } catch (...) {
    return manifest;
  }

  return manifest;
}

bool InstalledManifest::Save(const std::filesystem::path& path, std::string* error) const {
  const auto actual_path = ResolveManifestPath(path);

  json j;
  j["version"] = 1;
  json resources = json::object();
  for (const auto& [id, record] : records_) {
    json item;
    item["kind"] = record.kind;
    item["version"] = record.version;
    item["sha256"] = record.sha256;
    item["installed_at"] = record.installed_at;
    resources[id] = std::move(item);
  }
  j["resources"] = std::move(resources);

  return vinput::file::AtomicWriteTextFile(actual_path, j.dump(2), error);
}

const InstalledRecord* InstalledManifest::Get(std::string_view id) const {
  auto it = records_.find(std::string(id));
  if (it != records_.end()) {
    return &it->second;
  }
  return nullptr;
}

void InstalledManifest::RecordInstalled(const std::string& id, std::string_view kind,
                                        const std::string& version, const std::string& sha256) {
  const auto now = static_cast<uint64_t>(vinput::time::CurrentUnixTimestampSeconds());

  InstalledRecord record;
  record.id = id;
  record.kind = std::string(kind);
  record.version = version;
  record.sha256 = sha256;
  record.installed_at = now;
  records_[id] = std::move(record);
}

bool InstalledManifest::Remove(std::string_view id) {
  return records_.erase(std::string(id)) > 0;
}

std::vector<InstalledRecord> InstalledManifest::List() const {
  std::vector<InstalledRecord> list;
  list.reserve(records_.size());
  for (const auto& [id, record] : records_) {
    list.push_back(record);
  }
  return list;
}

} // namespace vinput::registry
