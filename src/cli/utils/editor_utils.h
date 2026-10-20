#pragma once

#include <CLI/StringTools.hpp>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

inline std::vector<std::string> SplitEditorCommand(std::string str) {
  auto args = CLI::detail::split_up(std::move(str));
  CLI::detail::remove_quotes(args);
  return args;
}

int OpenInEditor(const std::filesystem::path& file_path);
