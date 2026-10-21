#include "common/registry/installed_manifest.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <glaze/core/opts.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/json_t.hpp>
#include <glaze/json/read.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/json/json_access.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/time_utils.h"

namespace vinput::registry {

namespace {

// The manifest keeps `resources` keyed by runtime ids, so its shape is dynamic.
constexpr auto kManifestWriteOpts = glz::opts{.prettify = uint8_t{1}, .indentation_width = 2};

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

  glz::json_t root;
  if (bool(glz::read_json(root, content))) {
    return manifest;
  }
  const auto* resources = vinput::json::Find(root, "resources");
  if (resources == nullptr) {
    return manifest;
  }
  const auto* items = resources->get_if<glz::json_t::object_t>();
  if (items == nullptr) {
    return manifest;
  }

  for (const auto& [id, item] : *items) {
    if (!item.is_object()) {
      continue;
    }
    InstalledRecord record;
    record.id = id;
    record.kind = vinput::json::String(item, "kind");
    record.version = vinput::json::String(item, "version");
    record.sha256 = vinput::json::String(item, "sha256");
    record.installed_at = static_cast<uint64_t>(vinput::json::Int64(item, "installed_at", 0));
    manifest.records_.emplace(id, std::move(record));
  }

  return manifest;
}

bool InstalledManifest::Save(const std::filesystem::path& path, std::string* error) const {
  const auto actual_path = ResolveManifestPath(path);

  glz::json_t::object_t resources;
  for (const auto& [id, record] : records_) {
    glz::json_t item = glz::json_t(glz::json_t::object_t{
        {"kind", glz::json_t(record.kind)},
        {"version", glz::json_t(record.version)},
        {"sha256", glz::json_t(record.sha256)},
        {"installed_at", glz::json_t(static_cast<double>(record.installed_at))},
    });
    resources.emplace(id, std::move(item));
  }

  glz::json_t root = glz::json_t(glz::json_t::object_t{
      {"version", glz::json_t(1.0)},
      {"resources", glz::json_t(std::move(resources))},
  });

  std::string content;
  if (bool(glz::write<kManifestWriteOpts>(root, content))) {
    if (error != nullptr) {
      *error = "failed to serialize installed manifest";
    }
    return false;
  }

  return vinput::file::AtomicWriteTextFile(actual_path, content, error);
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
