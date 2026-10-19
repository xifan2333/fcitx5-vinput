#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace vinput::sandbox {

namespace detail {
bool DetectSandbox(const char* flatpak_id, const char* container, const char* info_path);

struct ProbeOptions {
  std::filesystem::path pipewire_socket_path;
  std::filesystem::path systemd_dir;
  std::filesystem::path cache_dir;
};

std::vector<std::string> ProbeMissingPermissions(const ProbeOptions& options);
} // namespace detail

// Whether running inside a sandbox (e.g. Flatpak).
bool IsInSandbox();

// Wrap a command to execute on the host when inside a sandbox.
// Returns args unchanged when not sandboxed.
std::vector<std::string> WrapHostCommand(std::vector<std::string> args);

// Return journalctl filter arguments appropriate for the current environment.
// Caller appends -n / -f as needed.
std::vector<std::string> DaemonLogFilter();

// Return names of missing sandbox permissions (empty if not sandboxed or all
// permissions are present).
std::vector<std::string> MissingSandboxPermissions();

// Format actionable flatpak override commands to fix missing permissions.
// Returns an empty string if missing_permissions is empty.
std::string FormatOverrideCommands(const std::vector<std::string>& missing_permissions);

// Rewrite a systemd unit content string so ExecStart points to the sandboxed
// daemon executable.  Returns the original content unchanged when not
// sandboxed.
std::string RewriteServiceUnit(const std::string& content);

} // namespace vinput::sandbox
