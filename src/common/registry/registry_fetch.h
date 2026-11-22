#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/utils/downloader.h"

struct CoreConfig;

namespace vinput::registry {

bool FetchRegistryText(const CoreConfig& config, const std::vector<std::string>& urls,
                       const std::filesystem::path& cache_path,
                       const vinput::download::Options& options, std::string* content,
                       vinput::download::Result* result = nullptr, std::string* error = nullptr,
                       std::vector<std::string>* warnings = nullptr);

} // namespace vinput::registry
