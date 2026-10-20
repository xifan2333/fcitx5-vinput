#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

bool SplitEditorCommand(std::string_view command, std::vector<std::string>* args);
int OpenInEditor(const std::filesystem::path& file_path);
