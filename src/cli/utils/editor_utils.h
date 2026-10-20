#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

std::optional<std::vector<std::string>> SplitEditorCommand(std::string_view command);
int OpenInEditor(const std::filesystem::path& file_path);
