#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "common/utils/sandbox.h"

namespace fs = std::filesystem;

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

struct TempDirGuard {
  fs::path path;
  TempDirGuard() {
    path = fs::temp_directory_path() /
           ("vinput_test_sandbox_" + std::to_string(::getpid()) + "_" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(path, ec);
  }
  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDirGuard(const TempDirGuard&) = delete;
  TempDirGuard& operator=(const TempDirGuard&) = delete;
  TempDirGuard(TempDirGuard&&) = delete;
  TempDirGuard& operator=(TempDirGuard&&) = delete;
};

void TestDetectSandbox() {
  using vinput::sandbox::detail::DetectSandbox;

  // FLATPAK_ID non-empty
  TEST_ASSERT(DetectSandbox("org.fcitx.Fcitx5", nullptr, nullptr));
  TEST_ASSERT(!DetectSandbox("", nullptr, nullptr));

  // container=flatpak
  TEST_ASSERT(DetectSandbox(nullptr, "flatpak", nullptr));
  TEST_ASSERT(!DetectSandbox(nullptr, "docker", nullptr));
  TEST_ASSERT(!DetectSandbox(nullptr, "", nullptr));

  // Nonexistent info path
  TEST_ASSERT(!DetectSandbox(nullptr, nullptr, "/nonexistent/flatpak-info/xyz"));
  TEST_ASSERT(!DetectSandbox(nullptr, nullptr, ""));
  TEST_ASSERT(!DetectSandbox(nullptr, nullptr, nullptr));

  // Existing file as info path
  const TempDirGuard temp;
  const fs::path dummy_info = temp.path / "dummy-flatpak-info";
  {
    std::ofstream ofs(dummy_info);
    ofs << "sample";
  }
  TEST_ASSERT(DetectSandbox(nullptr, nullptr, dummy_info.c_str()));
}

void TestProbeMissingPermissions() {
  using vinput::sandbox::detail::ProbeMissingPermissions;
  using vinput::sandbox::detail::ProbeOptions;

  const TempDirGuard temp;
  const fs::path pw_socket = temp.path / "pipewire-0";
  {
    std::ofstream ofs(pw_socket);
    ofs << "socket";
  }
  const fs::path systemd_dir = temp.path / "systemd" / "user";
  std::error_code ec;
  fs::create_directories(systemd_dir, ec);

  const fs::path cache_dir = temp.path / "cache" / "vinput";
  fs::create_directories(cache_dir, ec);

  // Case 1: All valid and present
  {
    ProbeOptions opts;
    opts.pipewire_socket_path = pw_socket;
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 2: Missing PipeWire socket
  {
    ProbeOptions opts;
    opts.pipewire_socket_path = temp.path / "nonexistent-socket";
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 1);
    TEST_ASSERT(missing[0] == "pipewire");
  }

  // Case 3: Empty PipeWire socket path
  {
    ProbeOptions opts;
    opts.pipewire_socket_path = "";
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 1);
    TEST_ASSERT(missing[0] == "pipewire");
  }

  // Case 4: Parent directory exists and writable for systemd and cache
  {
    ProbeOptions opts;
    opts.pipewire_socket_path = pw_socket;
    // Sub-sub directory that does not exist, but parent exists
    opts.systemd_dir = temp.path / "systemd" / "nonexistent_sub";
    opts.cache_dir = temp.path / "cache" / "nonexistent_sub";
    auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 5: All missing / nonexistent roots
  {
    ProbeOptions opts;
    opts.pipewire_socket_path = "/nonexistent/path/pw-0";
    opts.systemd_dir = "/nonexistent/path/systemd/user";
    opts.cache_dir = "/nonexistent/path/cache/vinput";
    auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 3);
    TEST_ASSERT(std::find(missing.begin(), missing.end(), "pipewire") != missing.end());
    TEST_ASSERT(std::find(missing.begin(), missing.end(), "xdg-config/systemd") != missing.end());
    TEST_ASSERT(std::find(missing.begin(), missing.end(), "xdg-cache") != missing.end());
  }
}

void TestFormatOverrideCommands() {
  using vinput::sandbox::FormatOverrideCommands;

  // Empty missing list
  TEST_ASSERT(FormatOverrideCommands({}).empty());

  // Single permission
  {
    const std::string cmds = FormatOverrideCommands({"pipewire"});
    TEST_ASSERT(
        cmds.find("flatpak override --user --filesystem=xdg-run/pipewire-0 org.fcitx.Fcitx5") !=
        std::string::npos);
    TEST_ASSERT(cmds.find("flatpak kill org.fcitx.Fcitx5") != std::string::npos);
    TEST_ASSERT(cmds.find("systemd") == std::string::npos);
  }

  // All three permissions
  {
    const std::string cmds =
        FormatOverrideCommands({"pipewire", "xdg-config/systemd", "xdg-cache"});
    TEST_ASSERT(
        cmds.find("flatpak override --user --filesystem=xdg-run/pipewire-0 org.fcitx.Fcitx5") !=
        std::string::npos);
    TEST_ASSERT(
        cmds.find(
            "flatpak override --user --filesystem=xdg-config/systemd:create org.fcitx.Fcitx5") !=
        std::string::npos);
    TEST_ASSERT(cmds.find("flatpak override --user --filesystem=xdg-cache org.fcitx.Fcitx5") !=
                std::string::npos);
    TEST_ASSERT(cmds.find("flatpak kill org.fcitx.Fcitx5") != std::string::npos);
  }
}

void TestGeneralSandboxApis() {
  // IsInSandbox() should return a consistent boolean
  const bool in_sb1 = vinput::sandbox::IsInSandbox();
  const bool in_sb2 = vinput::sandbox::IsInSandbox();
  TEST_ASSERT(in_sb1 == in_sb2);

  // MissingSandboxPermissions() should be consistent
  const auto missing1 = vinput::sandbox::MissingSandboxPermissions();
  const auto missing2 = vinput::sandbox::MissingSandboxPermissions();
  TEST_ASSERT(missing1 == missing2);

  if (!in_sb1) {
    TEST_ASSERT(missing1.empty());
  }

  // WrapHostCommand
  const std::vector<std::string> args = {"echo", "hello"};
  const auto wrapped = vinput::sandbox::WrapHostCommand(args);
  if (!in_sb1) {
    TEST_ASSERT(wrapped == args);
  } else {
    TEST_ASSERT(wrapped.size() == 4);
    TEST_ASSERT(wrapped[0] == "flatpak-spawn");
    TEST_ASSERT(wrapped[1] == "--host");
  }

  // DaemonLogFilter
  const auto log_filter = vinput::sandbox::DaemonLogFilter();
  TEST_ASSERT(!log_filter.empty());

  // RewriteServiceUnit
  const std::string unit = "ExecStart=/usr/bin/daemon\nRestart=always\n";
  const auto rewritten = vinput::sandbox::RewriteServiceUnit(unit);
  if (!in_sb1) {
    TEST_ASSERT(rewritten == unit);
  }
}

int main() {
  TestDetectSandbox();
  TestProbeMissingPermissions();
  TestFormatOverrideCommands();
  TestGeneralSandboxApis();
  std::printf("test_sandbox passed successfully.\n");
  return 0;
}
