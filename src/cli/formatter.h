#pragma once

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "cli/cli_context.h"

class Formatter {
public:
  virtual ~Formatter() = default;
  virtual void PrintTable(const std::vector<std::string>& headers,
                          const std::vector<std::vector<std::string>>& rows) = 0;
  virtual void PrintKeyValue(const std::string& key, const std::string& value) = 0;
  virtual void PrintSuccess(const std::string& msg) = 0;
  virtual void PrintError(const std::string& msg) = 0;
  virtual void PrintWarning(const std::string& msg) = 0;
  virtual void PrintJson(const nlohmann::json& j) = 0;
  virtual void PrintInfo(const std::string& msg) = 0;
};

class TextFormatter : public Formatter {
public:
  explicit TextFormatter(bool use_color);
  void PrintTable(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows) override;
  void PrintKeyValue(const std::string& key, const std::string& value) override;
  void PrintSuccess(const std::string& msg) override;
  void PrintError(const std::string& msg) override;
  void PrintWarning(const std::string& msg) override;
  void PrintJson(const nlohmann::json& j) override;
  void PrintInfo(const std::string& msg) override;

private:
  bool use_color_;
  std::string Green(const std::string& s) const;
  std::string Red(const std::string& s) const;
  std::string Yellow(const std::string& s) const;
  std::string Gray(const std::string& s) const;
  std::string Bold(const std::string& s) const;
};

class JsonFormatter : public Formatter {
public:
  void PrintTable(const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows) override;
  void PrintKeyValue(const std::string& key, const std::string& value) override;
  void PrintSuccess(const std::string& msg) override;
  void PrintError(const std::string& msg) override;
  void PrintWarning(const std::string& msg) override;
  void PrintJson(const nlohmann::json& j) override;
  void PrintInfo(const std::string& msg) override;
};

std::unique_ptr<Formatter> CreateFormatter(const CliContext& ctx);
