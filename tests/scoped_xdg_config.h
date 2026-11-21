#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

extern "C" {
#include <stdlib.h> // setenv, unsetenv
}

#include "common/utils/path_utils.h"

namespace vinput::test {

class ScopedXdgConfigDir {
public:
  explicit ScopedXdgConfigDir(std::string_view prefix) {
    if (const char* prev = std::getenv("XDG_CONFIG_HOME"); prev != nullptr) {
      had_prev_ = true;
      prev_ = prev;
    }
    dir_ = std::filesystem::temp_directory_path() /
           (std::string(prefix) + "_" + std::to_string(static_cast<long>(getpid())));
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_ / "vinput", ec);
    setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
  }

  ScopedXdgConfigDir(const ScopedXdgConfigDir&) = delete;
  ScopedXdgConfigDir& operator=(const ScopedXdgConfigDir&) = delete;
  ScopedXdgConfigDir(ScopedXdgConfigDir&&) = delete;
  ScopedXdgConfigDir& operator=(ScopedXdgConfigDir&&) = delete;

  ~ScopedXdgConfigDir() {
    if (had_prev_) {
      setenv("XDG_CONFIG_HOME", prev_.c_str(), 1);
    } else {
      unsetenv("XDG_CONFIG_HOME");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  static void WriteLocalI18n(std::string_view json_content) {
    std::ofstream out(vinput::path::VinputConfigDir() / "i18n.local.json");
    out << json_content;
  }

private:
  std::filesystem::path dir_;
  bool had_prev_ = false;
  std::string prev_;
};

} // namespace vinput::test
