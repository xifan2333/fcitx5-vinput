#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <glaze/json/json_t.hpp>
#include <string>
#include <vector>

#include "common/json/json_access.h"

struct RemoteModelEntry {
  std::string id;
  std::string short_id;
  std::vector<std::string> urls; // download URLs (tried in order, fallback)
  std::string sha256;
  uint64_t size_bytes = 0;
  std::string language;
  glz::json_t vinput_model; // pre-built vinput-model.json content

  // Convenience accessors reading from vinput_model
  [[nodiscard]] std::string model_type() const {
    return vinput::json::String(vinput_model, "family",
                                vinput::json::String(vinput_model, "model_type"));
  }
  [[nodiscard]] bool supports_hotwords() const {
    return vinput::json::Bool(vinput_model, "supports_hotwords");
  }
};

struct CoreConfig;

struct InstallProgress {
  uint64_t downloaded_bytes = 0;
  uint64_t total_bytes = 0;
  double speed_bps = 0;
};

using ProgressCallback = std::function<void(const InstallProgress&)>;

class ModelRepository {
public:
  explicit ModelRepository(const std::string& base_dir);

  // Fetch remote model registry.
  static std::vector<RemoteModelEntry> FetchRegistry(const std::string& registry_url,
                                                     std::string* error);
  static std::vector<RemoteModelEntry> FetchRegistry(const std::vector<std::string>& registry_urls,
                                                     std::string* error,
                                                     std::string* resolved_registry_url = nullptr);
  static std::vector<RemoteModelEntry> FetchRegistry(const CoreConfig& config,
                                                     const std::vector<std::string>& registry_urls,
                                                     std::string* error,
                                                     std::string* resolved_registry_url = nullptr,
                                                     std::vector<std::string>* warnings = nullptr);

  // Download and install a model
  bool InstallModel(const std::string& registry_url, const std::string& model_id,
                    const ProgressCallback& progress_cb, std::string* error,
                    const std::atomic<bool>* cancel_token = nullptr) const;
  bool InstallModel(const std::vector<std::string>& registry_urls, const std::string& model_id,
                    const ProgressCallback& progress_cb, std::string* error,
                    std::string* resolved_registry_url = nullptr,
                    const std::atomic<bool>* cancel_token = nullptr) const;
  bool InstallModel(const CoreConfig& config, const std::vector<std::string>& registry_urls,
                    const std::string& model_id, const ProgressCallback& progress_cb,
                    std::string* error, std::string* resolved_registry_url = nullptr,
                    const std::atomic<bool>* cancel_token = nullptr) const;

private:
  bool InstallModelEntry(const RemoteModelEntry& found, const ProgressCallback& progress_cb,
                         std::string* error, const std::atomic<bool>* cancel_token) const;
  static bool VerifySha256(const std::filesystem::path& file, const std::string& expected,
                           std::string* error);
  static bool ExtractArchive(const std::filesystem::path& archive,
                             const std::filesystem::path& dest, std::string* error);
  std::filesystem::path base_dir_;
};
