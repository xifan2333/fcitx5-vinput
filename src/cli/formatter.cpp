#include "cli/formatter.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

// ---- TextFormatter ----

TextFormatter::TextFormatter(bool use_color) : use_color_(use_color) {}

std::string TextFormatter::Green(const std::string& s) const {
  if (!use_color_) return s;
  return "\033[32m" + s + "\033[0m";
}

std::string TextFormatter::Red(const std::string& s) const {
  if (!use_color_) return s;
  return "\033[31m" + s + "\033[0m";
}

std::string TextFormatter::Yellow(const std::string& s) const {
  if (!use_color_) return s;
  return "\033[33m" + s + "\033[0m";
}

std::string TextFormatter::Gray(const std::string& s) const {
  if (!use_color_) return s;
  return "\033[90m" + s + "\033[0m";
}

std::string TextFormatter::Bold(const std::string& s) const {
  if (!use_color_) return s;
  return "\033[1m" + s + "\033[0m";
}

void TextFormatter::PrintTable(const std::vector<std::string>& headers,
                               const std::vector<std::vector<std::string>>& rows) {
  if (headers.empty()) return;

  std::vector<size_t> widths(headers.size(), 0);
  for (size_t i = 0; i < headers.size(); ++i) {
    widths[i] = headers[i].size();
  }
  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }

  // Print header
  for (size_t i = 0; i < headers.size(); ++i) {
    if (i > 0) std::cout << "  ";
    std::string cell = headers[i];
    cell.resize(widths[i], ' ');
    std::cout << Bold(cell);
  }
  std::cout << "\n";

  // Print rows
  for (const auto& row : rows) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (i > 0) std::cout << "  ";
      std::string cell = (i < row.size()) ? row[i] : "";
      cell.resize(widths[i], ' ');
      std::cout << cell;
    }
    std::cout << "\n";
  }
}

void TextFormatter::PrintKeyValue(const std::string& key, const std::string& value) {
  std::cout << Bold(key) << ": " << value << "\n";
}

void TextFormatter::PrintSuccess(const std::string& msg) {
  std::cout << Green(msg) << "\n";
}

void TextFormatter::PrintError(const std::string& msg) {
  std::cerr << Red(msg) << "\n";
}

void TextFormatter::PrintWarning(const std::string& msg) {
  std::cout << Yellow(msg) << "\n";
}

void TextFormatter::PrintJson(const nlohmann::json& j) {
  std::cout << j.dump(2) << "\n";
}

void TextFormatter::PrintInfo(const std::string& msg) {
  std::cout << Gray(msg) << "\n";
}

// ---- JsonFormatter ----

void JsonFormatter::PrintTable(const std::vector<std::string>& headers,
                               const std::vector<std::vector<std::string>>& rows) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& row : rows) {
    nlohmann::json obj = nlohmann::json::object();
    for (size_t i = 0; i < headers.size(); ++i) {
      obj[headers[i]] = (i < row.size()) ? row[i] : "";
    }
    arr.push_back(obj);
  }
  std::cout << arr.dump(2) << "\n";
}

void JsonFormatter::PrintKeyValue(const std::string& key, const std::string& value) {
  nlohmann::json obj = {{"key", key}, {"value", value}};
  std::cout << obj.dump() << "\n";
}

void JsonFormatter::PrintSuccess(const std::string& msg) {
  nlohmann::json obj = {{"status", "success"}, {"message", msg}};
  std::cout << obj.dump() << "\n";
}

void JsonFormatter::PrintError(const std::string& msg) {
  nlohmann::json obj = {{"status", "error"}, {"message", msg}};
  std::cerr << obj.dump() << "\n";
}

void JsonFormatter::PrintWarning(const std::string& msg) {
  nlohmann::json obj = {{"status", "warning"}, {"message", msg}};
  std::cout << obj.dump() << "\n";
}

void JsonFormatter::PrintJson(const nlohmann::json& j) {
  std::cout << j.dump(2) << "\n";
}

void JsonFormatter::PrintInfo(const std::string& msg) {
  nlohmann::json obj = {{"status", "info"}, {"message", msg}};
  std::cout << obj.dump() << "\n";
}

// ---- Factory ----

std::unique_ptr<Formatter> CreateFormatter(const CliContext& ctx) {
  if (ctx.json_output) {
    return std::make_unique<JsonFormatter>();
  }
  return std::make_unique<TextFormatter>(ctx.is_tty);
}
