#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace vinput::file {
bool EnsureParentDirectory(const std::filesystem::path& path, std::string* error);
bool AtomicWriteTextFile(const std::filesystem::path& target, std::string_view content, std::string* error);
}
