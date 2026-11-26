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

void TestResolvePulseEndpoints() {
  using vinput::sandbox::detail::ResolvePulseEndpoints;

  const auto default_eps = ResolvePulseEndpoints(nullptr, "/run/user/1000", 1000);
  TEST_ASSERT(default_eps.size() == 1);
  TEST_ASSERT(default_eps[0].socket_path == fs::path("/run/user/1000/pulse/native"));
  TEST_ASSERT(!default_eps[0].tcp_configured);

  const auto fallback_eps = ResolvePulseEndpoints(nullptr, "", 1001);
  TEST_ASSERT(fallback_eps.size() == 1);
  TEST_ASSERT(fallback_eps[0].socket_path == fs::path("/run/user/1001/pulse/native"));
  TEST_ASSERT(!fallback_eps[0].tcp_configured);

  const auto unix_eps =
      ResolvePulseEndpoints("unix:/tmp/custom-pulse/native", "/run/user/1000", 1000);
  TEST_ASSERT(unix_eps.size() == 1);
  TEST_ASSERT(unix_eps[0].socket_path == fs::path("/tmp/custom-pulse/native"));
  TEST_ASSERT(!unix_eps[0].tcp_configured);

  const auto bare_unix_eps = ResolvePulseEndpoints("/mnt/wslg/PulseServer", "/run/user/1000", 1000);
  TEST_ASSERT(bare_unix_eps.size() == 1);
  TEST_ASSERT(bare_unix_eps[0].socket_path == fs::path("/mnt/wslg/PulseServer"));
  TEST_ASSERT(!bare_unix_eps[0].tcp_configured);

  // Matching {machine-id} keeps entry and preserves subsequent fallback entries.
  const auto matched_eps =
      ResolvePulseEndpoints("{deadbeef}unix:/run/user/1000/pulse/native tcp:127.0.0.1:4713",
                            "/run/user/1000", 1000, "deadbeef");
  TEST_ASSERT(matched_eps.size() == 2);
  TEST_ASSERT(matched_eps[0].socket_path == fs::path("/run/user/1000/pulse/native"));
  TEST_ASSERT(!matched_eps[0].tcp_configured);
  TEST_ASSERT(matched_eps[1].socket_path.empty());
  TEST_ASSERT(matched_eps[1].tcp_configured);

  // Mismatched {machine-id} skips that entry rather than stripping and accepting it.
  const auto mismatched_eps =
      ResolvePulseEndpoints("{other-machine}tcp:192.0.2.1 unix:/run/flatpak/pulse/native",
                            "/run/user/1000", 1000, "local-machine");
  TEST_ASSERT(mismatched_eps.size() == 1);
  TEST_ASSERT(mismatched_eps[0].socket_path == fs::path("/run/flatpak/pulse/native"));
  TEST_ASSERT(!mismatched_eps[0].tcp_configured);

  // Bare host:port (without tcp: prefix) and tcp6: are recognized as TCP endpoints.
  const auto bare_tcp_eps = ResolvePulseEndpoints("127.0.0.1:4713", "/run/user/1000", 1000);
  TEST_ASSERT(bare_tcp_eps.size() == 1);
  TEST_ASSERT(bare_tcp_eps[0].socket_path.empty());
  TEST_ASSERT(bare_tcp_eps[0].tcp_configured);

  const auto tcp6_eps = ResolvePulseEndpoints("tcp6:[::1]:4713", "/run/user/1000", 1000);
  TEST_ASSERT(tcp6_eps.size() == 1);
  TEST_ASSERT(tcp6_eps[0].socket_path.empty());
  TEST_ASSERT(tcp6_eps[0].tcp_configured);
}

void TestProbeMissingPermissions() {
  using vinput::sandbox::detail::ProbeMissingPermissions;
  using vinput::sandbox::detail::ProbeOptions;
  using vinput::sandbox::detail::PulseEndpoint;

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
    opts.pulse_endpoints = {PulseEndpoint{.socket_path = pulse_socket, .tcp_configured = false}};
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 2: Missing PulseAudio socket
  {
    ProbeOptions opts;
    opts.pulse_endpoints = {
        PulseEndpoint{.socket_path = temp.path / "nonexistent-socket", .tcp_configured = false}};
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 1);
    TEST_ASSERT(missing[0] == "pulseaudio");
  }

  // Case 3: Empty PulseAudio endpoint list without TCP configured
  {
    ProbeOptions opts;
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.size() == 1);
    TEST_ASSERT(missing[0] == "pulseaudio");
  }

  // Case 4: Fallback TCP or accessible second UNIX endpoint satisfies pulseaudio check
  {
    ProbeOptions opts;
    opts.pulse_endpoints = {
        PulseEndpoint{.socket_path = temp.path / "nonexistent-socket", .tcp_configured = false},
        PulseEndpoint{.socket_path = {}, .tcp_configured = true},
    };
    opts.systemd_dir = systemd_dir;
    opts.cache_dir = cache_dir;
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 5: Parent directory exists and writable for systemd and cache
  {
    ProbeOptions opts;
    opts.pulse_endpoints = {PulseEndpoint{.socket_path = pulse_socket, .tcp_configured = false}};
    opts.systemd_dir = temp.path / "systemd" / "nonexistent_sub";
    opts.cache_dir = temp.path / "cache" / "nonexistent_sub";
    const auto missing = ProbeMissingPermissions(opts);
    TEST_ASSERT(missing.empty());
  }

  // Case 6: All missing / nonexistent roots
  {
    ProbeOptions opts;
    opts.pulse_endpoints = {
        PulseEndpoint{.socket_path = "/nonexistent/path/pulse/native", .tcp_configured = false}};
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
  TestResolvePulseEndpoints();
  TestProbeMissingPermissions();
  TestFormatOverrideCommands();
  TestGeneralSandboxApis();
  std::printf("test_sandbox passed successfully.\n");
  return 0;
}
