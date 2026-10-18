#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vinput::registry {

struct InstalledRecord {
  std::string id;
  std::string kind; // "provider" or "adapter"
  std::string version;
  std::string sha256;
  uint64_t installed_at = 0;
};

class InstalledManifest {
public:
  static InstalledManifest Load(const std::filesystem::path& path = {});
  bool Save(const std::filesystem::path& path = {}, std::string* error = nullptr) const;

  [[nodiscard]] const InstalledRecord* Get(std::string_view id) const;
  void RecordInstalled(const std::string& id, std::string_view kind, const std::string& version,
                       const std::string& sha256);
  bool Remove(std::string_view id);
  [[nodiscard]] std::vector<InstalledRecord> List() const;

private:
  std::map<std::string, InstalledRecord> records_;
};

} // namespace vinput::registry
