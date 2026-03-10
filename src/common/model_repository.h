#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct RemoteModelEntry {
  std::string name;
  std::string display_name;
  std::string description;
  std::string url;          // download URL
  std::string sha256;
  uint64_t size_bytes = 0;
  std::string model_type;
  std::string language;
  nlohmann::json vinput_model; // pre-built vinput-model.json content
};

struct InstallProgress {
  uint64_t downloaded_bytes = 0;
  uint64_t total_bytes = 0;
  double speed_bps = 0;
};

using ProgressCallback = std::function<void(const InstallProgress &)>;

class ModelRepository {
public:
  explicit ModelRepository(const std::string &base_dir);

  // Fetch remote model registry
  std::vector<RemoteModelEntry> FetchRegistry(const std::string &registry_url,
                                              std::string *error) const;

  // Download and install a model
  bool InstallModel(const std::string &registry_url,
                    const std::string &model_name, ProgressCallback progress_cb,
                    std::string *error) const;

private:
  bool DownloadFile(const std::string &url,
                    const std::filesystem::path &dest, ProgressCallback cb,
                    std::string *error) const;
  bool VerifySha256(const std::filesystem::path &file,
                    const std::string &expected, std::string *error) const;
  bool ExtractArchive(const std::filesystem::path &archive,
                      const std::filesystem::path &dest,
                      std::string *error) const;
  std::filesystem::path base_dir_;
};
