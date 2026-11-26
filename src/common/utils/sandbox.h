#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vinput::sandbox {

namespace detail {
struct PulseEndpoint {
  std::filesystem::path socket_path;
  bool tcp_configured{false};
};

struct ProbeOptions {
  std::vector<PulseEndpoint> pulse_endpoints;
  std::filesystem::path systemd_dir;
  std::filesystem::path cache_dir;
};

bool DetectSandbox(const char* flatpak_id, const char* container, const char* info_path);
std::string LocalPulseMachineId();
std::vector<PulseEndpoint> ResolvePulseEndpoints(const char* pulse_server,
                                                 const char* xdg_runtime_dir, uint32_t uid,
                                                 std::string_view local_machine_id = {});
std::vector<std::string> ProbeMissingPermissions(const ProbeOptions& options);
} // namespace detail

// Detect whether the current process runs inside a Flatpak sandbox.
// Result is cached on first call (environment doesn't change at runtime).
bool IsInSandbox();

// Prepend {"flatpak-spawn", "--host"} when inside a sandbox so the command
// executes on the host (needed for systemctl --user, journalctl, etc.).
// No-op outside a sandbox.
std::vector<std::string> WrapHostCommand(std::vector<std::string> args);

// Return journalctl filter args appropriate for the environment:
//   host:    {"journalctl", "--user-unit", "vinput-daemon.service"}
//   sandbox: {"journalctl", "--user", "-t", "flatpak", "--grep", "vinput"}
std::vector<std::string> DaemonLogFilter();

// Probe missing Flatpak permissions required by vinput-daemon.
// Returns permission names that are missing (empty when all present or not
// sandboxed).  Checked permissions:
//   "pulseaudio"         – PulseAudio / pipewire-pulse socket accessible
//   "xdg-config/systemd" – ~/.config/systemd/user writable
//   "xdg-cache"          – ~/.cache/vinput writable
std::vector<std::string> MissingSandboxPermissions();

// Format copy-pasteable `flatpak override` commands for the given missing
// permissions.  Returns empty string if `missing_permissions` is empty.
std::string FormatOverrideCommands(const std::vector<std::string>& missing_permissions);

// Rewrite a systemd unit content string so ExecStart points to the sandboxed
// daemon executable.  Returns the original content unchanged when not
// sandboxed.
std::string RewriteServiceUnit(const std::string& content);

} // namespace vinput::sandbox
