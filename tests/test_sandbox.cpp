#include <algorithm>
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

namespace {
void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

struct TempDirGuard {
  fs::path path;
  TempDirGuard() {
    path = fs::temp_directory_path() / ("vinput_test_sandbox_" + std::to_string(::getpid()));
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

void TestResolvePulseEndpoint() {
  using vinput::sandbox::detail::ResolvePulseEndpoint;

  const auto default_ep = ResolvePulseEndpoint(nullptr, "/run/user/1000", 1000);
  TEST_ASSERT(default_ep.socket_path == fs::path("/run/user/1000/pulse/native"));
  TEST_ASSERT(!default_ep.tcp_configured);

  const auto fallback_ep = ResolvePulseEndpoint(nullptr, "", 1001);
  TEST_ASSERT(fallback_ep.socket_path == fs::path("/run/user/1001/pulse/native"));
  TEST_ASSERT(!fallback_ep.tcp_configured);

  const auto unix_ep =
      ResolvePulseEndpoint("unix:/tmp/custom-pulse/native", "/run/user/1000", 1000);
  TEST_ASSERT(unix_ep.socket_path == fs::path("/tmp/custom-pulse/native"));
  TEST_ASSERT(!unix_ep.tcp_configured);

  const auto cookie_ep = ResolvePulseEndpoint(
      "{deadbeef}unix:/run/user/1000/pulse/native tcp:127.0.0.1:4713", "/run/user/1000", 1000);
  TEST_ASSERT(cookie_ep.socket_path == fs::path("/run/user/1000/pulse/native"));
  TEST_ASSERT(!cookie_ep.tcp_configured);

  const auto tcp_ep = ResolvePulseEndpoint("tcp:127.0.0.1:4713", "/run/user/1000", 1000);
  TEST_ASSERT(tcp_ep.socket_path.empty());
  TEST_ASSERT(tcp_ep.tcp_configured);
}

void TestProbeMissingPermissions() {
  using vinput::sandbox::detail::ProbeMissingPermissions;
  using vinput::sandbox::detail::ProbeOptions;

  const TempDirGuard temp;
  const fs::path pulse_socket = temp.path / "native";
  {
    std::ofstream ofs(pulse_socket);
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
    opts.pulse_socket_path = pulse_socket;
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 2: Missing PulseAudio socket
  {
    ProbeOptions opts;
    opts.pulse_socket_path = temp.path / "nonexistent-socket";
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 1);
    TEST_ASSERT(missing[0] == "pulseaudio");
  }

  // Case 3: Empty PulseAudio socket path without TCP configured
  {
    ProbeOptions opts;
    opts.pulse_socket_path = "";
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 1);
    TEST_ASSERT(missing[0] == "pulseaudio");
  }

  // Case 4: TCP PulseAudio endpoint skips local socket requirement
  {
    ProbeOptions opts;
    opts.pulse_tcp_configured = true;
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 5: Parent directory exists and writable for systemd and cache
  {
    ProbeOptions opts;
    opts.pulse_socket_path = pulse_socket;
    opts.systemd_dir = temp.path / "systemd" / "nonexistent_sub";
    opts.cache_dir = temp.path / "cache" / "nonexistent_sub";
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 6: All missing / nonexistent roots
  {
    ProbeOptions opts;
    opts.pulse_socket_path = "/nonexistent/path/pulse/native";
    opts.systemd_dir = "/nonexistent/path/systemd/user";
    opts.cache_dir = "/nonexistent/path/cache/vinput";
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 3);
    TEST_ASSERT(std::ranges::find(missing, "pulseaudio") != missing.end());
    TEST_ASSERT(std::ranges::find(missing, "xdg-config/systemd") != missing.end());
    TEST_ASSERT(std::ranges::find(missing, "xdg-cache") != missing.end());
  }
}

void TestFormatOverrideCommands() {
  using vinput::sandbox::FormatOverrideCommands;

  // Empty missing list
  TEST_ASSERT(FormatOverrideCommands({}).empty());

  // Single permission
  {
    const std::string cmds = FormatOverrideCommands({"pulseaudio"});
    TEST_ASSERT(cmds.find("flatpak override --user --socket=pulseaudio org.fcitx.Fcitx5") !=
                std::string::npos);
    TEST_ASSERT(cmds.find("flatpak kill org.fcitx.Fcitx5") != std::string::npos);
    TEST_ASSERT(cmds.find("systemd") == std::string::npos);
  }

  // All three permissions
  {
    const std::string cmds =
        FormatOverrideCommands({"pulseaudio", "xdg-config/systemd", "xdg-cache"});
    TEST_ASSERT(cmds.find("flatpak override --user --socket=pulseaudio org.fcitx.Fcitx5") !=
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
  const bool in_sb1 = vinput::sandbox::IsInSandbox();
  const bool in_sb2 = vinput::sandbox::IsInSandbox();
  TEST_ASSERT(in_sb1 == in_sb2);

  const auto missing1 = vinput::sandbox::MissingSandboxPermissions();
  const auto missing2 = vinput::sandbox::MissingSandboxPermissions();
  TEST_ASSERT(missing1 == missing2);

  if (!in_sb1) {
    TEST_ASSERT(missing1.empty());
  }

  const std::vector<std::string> args = {"echo", "hello"};
  const auto wrapped = vinput::sandbox::WrapHostCommand(args);
  if (!in_sb1) {
    TEST_ASSERT(wrapped == args);
  } else {
    TEST_ASSERT(wrapped.size() == 4);
    TEST_ASSERT(wrapped[0] == "flatpak-spawn");
    TEST_ASSERT(wrapped[1] == "--host");
  }

  const auto log_filter = vinput::sandbox::DaemonLogFilter();
  TEST_ASSERT(!log_filter.empty());

  const std::string unit = "ExecStart=/usr/bin/daemon\nRestart=always\n";
  const auto rewritten = vinput::sandbox::RewriteServiceUnit(unit);
  if (!in_sb1) {
    TEST_ASSERT(rewritten == unit);
  }
}
} // namespace

int main() {
  TestDetectSandbox();
  TestResolvePulseEndpoint();
  TestProbeMissingPermissions();
  TestFormatOverrideCommands();
  TestGeneralSandboxApis();
  std::printf("test_sandbox passed successfully.\n");
  return 0;
}
