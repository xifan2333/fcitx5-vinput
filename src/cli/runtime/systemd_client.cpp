#include "cli/runtime/systemd_client.h"

#include <array>
#include <exception>
#include <fmt/printf.h>
#include <string>
#include <string_view>
#include <subprocess.hpp>
#include <utility>
#include <vector>

#include "common/utils/path_utils.h"
#include "common/utils/sandbox.h"

namespace vinput::cli {

namespace {

std::vector<std::string> BuildCommand(const std::vector<std::string>& args) {
  return vinput::sandbox::WrapHostCommand(std::vector<std::string>(args.begin(), args.end()));
}

constexpr std::string_view MapJournalctlPriority(std::string_view level) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kPriorityMap{{
      {"error", "err"},
      {"warn", "warning"},
      {"trace", "debug"},
  }};
  for (const auto& [from, to] : kPriorityMap) {
    if (level == from) {
      return to;
    }
  }
  return level;
}

std::vector<std::string> BuildJournalctlCommand(bool follow, int lines,
                                                const std::string& level = {}) {
  auto args = vinput::sandbox::DaemonLogFilter();
  const std::string lines_str = std::to_string(lines);
  args.emplace_back("-n");
  args.push_back(lines_str);
  if (follow) {
    args.emplace_back("-f");
  }
  if (!level.empty()) {
    args.emplace_back("-p");
    args.emplace_back(MapJournalctlPriority(level));
  }
  return args;
}

int RunCommand(const std::vector<std::string>& args);

std::string BuildFailureMessage(const char* fmt, int exit_code) {
  std::string message = ::fmt::sprintf(fmt, exit_code);
  const std::string logs = JournalctlLogsText(20);
  if (!logs.empty()) {
    message += "\n\n";
    message += logs;
  }
  return message;
}

DaemonControlResult RunWithDiagnostics(const std::vector<std::string>& args,
                                       const char* failure_fmt) {
  DaemonControlResult result;
  result.exit_code = RunCommand(args);
  if (result.exit_code == 0) {
    return result;
  }
  result.failure_message = BuildFailureMessage(failure_fmt, result.exit_code);
  return result;
}

int RunCommand(const std::vector<std::string>& args) {
  const auto actual_args = BuildCommand(args);
  if (actual_args.empty()) {
    return -1;
  }
  try {
    return subprocess::call(actual_args);
  } catch (const std::exception&) {
    return -1;
  }
}

int RunCommandCapture(const std::vector<std::string>& args, std::string* output) {
  const auto actual_args = BuildCommand(args);
  if (actual_args.empty()) {
    return -1;
  }
  try {
    auto proc = subprocess::Popen(actual_args, subprocess::output{subprocess::PIPE},
                                  subprocess::error{subprocess::PIPE});
    auto [out, err] = proc.communicate();
    if (output != nullptr) {
      output->assign(out.buf.data(), out.length);
      output->append(err.buf.data(), err.length);
    }
    return proc.retcode();
  } catch (const std::exception&) {
    return -1;
  }
}

} // namespace

int SystemctlStop() {
  return RunCommand(
      {"systemctl", "--user", "stop", std::string(vinput::path::DaemonServiceUnitName())});
}

int JournalctlLogs(bool follow, int lines, const std::string& level) {
  return RunCommand(BuildJournalctlCommand(follow, lines, level));
}

std::string JournalctlLogsText(int lines) {
  std::string output;
  if (RunCommandCapture(BuildJournalctlCommand(false, lines), &output) < 0) {
    return {};
  }
  return output;
}

DaemonControlResult SystemctlStartWithDiagnostics() {
  return RunWithDiagnostics(
      {"systemctl", "--user", "start", std::string(vinput::path::DaemonServiceUnitName())},
      "systemctl start failed (exit code: %d)");
}

DaemonControlResult SystemctlRestartWithDiagnostics() {
  return RunWithDiagnostics(
      {"systemctl", "--user", "restart", std::string(vinput::path::DaemonServiceUnitName())},
      "systemctl restart failed (exit code: %d)");
}

namespace {

DaemonControlResult RunSystemctlUnitCommand(const std::vector<std::string>& args,
                                            const char* failure_action) {
  std::string output;
  DaemonControlResult result;
  result.exit_code = RunCommandCapture(args, &output);
  if (result.exit_code != 0) {
    result.failure_message = output.empty() ? ::fmt::sprintf("systemctl %s failed (exit code: %d)",
                                                             failure_action, result.exit_code)
                                            : output;
  }
  return result;
}

} // namespace

DaemonControlResult SystemctlEnable(bool force) {
  std::vector<std::string> args = {"systemctl", "--user", "enable"};
  if (force) {
    args.emplace_back("--force");
  }
  args.emplace_back(std::string(vinput::path::DaemonServiceUnitName()));
  return RunSystemctlUnitCommand(args, "enable");
}

DaemonControlResult SystemctlDisable() {
  return RunSystemctlUnitCommand(
      {"systemctl", "--user", "disable", std::string(vinput::path::DaemonServiceUnitName())},
      "disable");
}

bool SystemctlIsEnabled() {
  return RunCommandCapture({"systemctl", "--user", "is-enabled",
                            std::string(vinput::path::DaemonServiceUnitName())},
                           nullptr) == 0;
}

} // namespace vinput::cli
