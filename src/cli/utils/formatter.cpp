#include "cli/utils/formatter.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <tabulate/tabulate.hpp>
#include <vector>

#include "cli/utils/cli_context.h"

// ---- TextFormatter ----

TextFormatter::TextFormatter(bool use_color) : use_color_(use_color) {}

std::string TextFormatter::Green(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[32m" + s + "\033[0m";
}

std::string TextFormatter::Red(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[31m" + s + "\033[0m";
}

std::string TextFormatter::Yellow(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[33m" + s + "\033[0m";
}

std::string TextFormatter::Gray(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[90m" + s + "\033[0m";
}

std::string TextFormatter::Bold(const std::string& s) const {
  if (!use_color_) {
    return s;
  }
  return "\033[1m" + s + "\033[0m";
}

void TextFormatter::PrintTable(const std::vector<std::string>& headers,
                               const std::vector<std::vector<std::string>>& rows) {
  if (headers.empty()) {
    return;
  }

  using namespace tabulate;

  Table table;

  // Add header row
  Table::Row_t header_row;
  header_row.reserve(headers.size());
  for (const auto& h : headers) {
    header_row.emplace_back(h);
  }
  table.add_row(header_row);

  // Style header: bold, no color override
  for (size_t i = 0; i < headers.size(); ++i) {
    if (use_color_) {
      table[0][i].format().font_style({FontStyle::bold});
    }
  }

  // Add data rows
  for (const auto& row : rows) {
    Table::Row_t table_row;
    table_row.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); ++i) {
      table_row.emplace_back(i < row.size() ? row[i] : "");
    }
    table.add_row(table_row);
  }

  // Borderless style: no borders, no corners, two-space column separator,
  // zero vertical padding to match original compact output
  table.format()
      .border("")
      .corner("")
      .column_separator("  ")
      .padding_top(0)
      .padding_bottom(0)
      .padding_left(0)
      .padding_right(0);

  std::cout << table << "\n";
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
  const nlohmann::json obj = {{"key", key}, {"value", value}};
  std::cout << obj.dump() << "\n";
}

void JsonFormatter::PrintSuccess(const std::string& msg) {
  const nlohmann::json obj = {{"status", "success"}, {"message", msg}};
  std::cout << obj.dump() << "\n";
}

void JsonFormatter::PrintError(const std::string& msg) {
  const nlohmann::json obj = {{"status", "error"}, {"message", msg}};
  std::cerr << obj.dump() << "\n";
}

void JsonFormatter::PrintWarning(const std::string& msg) {
  const nlohmann::json obj = {{"status", "warning"}, {"message", msg}};
  std::cout << obj.dump() << "\n";
}

void JsonFormatter::PrintJson(const nlohmann::json& j) {
  std::cout << j.dump(2) << "\n";
}

void JsonFormatter::PrintInfo(const std::string& msg) {
  const nlohmann::json obj = {{"status", "info"}, {"message", msg}};
  std::cout << obj.dump() << "\n";
}

// ---- Factory ----

std::unique_ptr<Formatter> CreateFormatter(const CliContext& ctx) {
  if (ctx.json_output) {
    return std::make_unique<JsonFormatter>();
  }
  return std::make_unique<TextFormatter>(ctx.is_tty);
}
