#include "cli/runtime/systemd_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <string>
#include <string_view>
#include <subprocess.hpp>
#include <unordered_set>
#include <vector>

#include "common/utils/path_utils.h"
#include "common/utils/sandbox.h"
#include "common/utils/string_utils.h"

#include "cli/runtime/dbus_client.h"

namespace vinput::cli {

namespace {

std::vector<std::string> BuildCommand(const std::vector<std::string>& args) {
  return vinput::sandbox::WrapHostCommand(std::vector<std::string>(args.begin(), args.end()));
}

std::vector<std::string> BuildJournalctlCommand(bool follow, int lines) {
  auto args = vinput::sandbox::DaemonLogFilter();
  const std::string lines_str = std::to_string(lines);
  args.push_back("-n");
  args.push_back(lines_str);
  if (follow) {
    args.push_back("-f");
  }
  return args;
}

int RunCommand(const std::vector<std::string>& args);

std::string StripJournalPrefix(std::string_view line) {
  auto pos = line.find("]: ");
  if (pos != std::string_view::npos) {
    line.remove_prefix(pos + 3);
  } else {
    pos = line.find(": ");
    if (pos != std::string_view::npos) {
      line.remove_prefix(pos + 2);
    }
  }
  constexpr std::string_view kDaemonPrefix = "vinput-daemon: ";
  constexpr std::string_view kCliPrefix = "vinput: ";
  if (line.starts_with(kDaemonPrefix)) {
    line.remove_prefix(kDaemonPrefix.size());
  } else if (line.starts_with(kCliPrefix)) {
    line.remove_prefix(kCliPrefix.size());
  }
  return std::string(vinput::str::Trim(line));
}

bool IsActionableFailureLine(const std::string& line) {
  if (line.empty()) {
    return false;
  }
  if (line.find("ASR provider=") != std::string::npos ||
      line.find("running with ASR disabled") != std::string::npos ||
      line.find("recording started") != std::string::npos ||
      line.find("phase ->") != std::string::npos ||
      line.find("negotiated format") != std::string::npos) {
    return false;
  }
  return line.find("Missing ") != std::string::npos || line.find("missing ") != std::string::npos ||
         line.find("not found") != std::string::npos || line.find("failed") != std::string::npos ||
         line.find("error") != std::string::npos;
}

std::vector<std::string> ExtractDaemonFailureReasons(const std::string& logs) {
  std::string fallback;
  std::vector<std::string> candidates;
  std::unordered_set<std::string> seen;

  auto lines = vinput::str::SplitLines(logs);
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    std::string line = StripJournalPrefix(*it);
    if (line.empty()) {
      continue;
    }
    if (fallback.empty()) {
      fallback = line;
    }
    if (!IsActionableFailureLine(line) || !seen.insert(line).second) {
      continue;
    }
    candidates.emplace_back(std::move(line));
    if (candidates.size() >= 3) {
      break;
    }
  }

  std::ranges::reverse(candidates);
  if (candidates.empty() && !fallback.empty()) {
    candidates.emplace_back(std::move(fallback));
  }
  return candidates;
}

std::string BuildFailureMessage(const char* fmt, int exit_code) {
  std::string message = vinput::str::FmtStr(fmt, exit_code);
  std::string logs = JournalctlLogsText(20);
  if (!logs.empty()) {
    message += "\n\n";
    message += logs;
  }
  return message;
}

vinput::dbus::ErrorInfo BuildFailureNotification(const char* fallback_code, const char* fmt,
                                                 int exit_code) {
  std::string logs = JournalctlLogsText(20);
  auto reasons = ExtractDaemonFailureReasons(logs);
  vinput::dbus::ErrorInfo fallback_error;
  for (auto it = reasons.rbegin(); it != reasons.rend(); ++it) {
    auto error = vinput::dbus::ClassifyErrorText(*it);
    if (error.empty()) {
      continue;
    }
    if (fallback_error.empty()) {
      fallback_error = error;
    }
    if (error.code != vinput::dbus::kErrorCodeUnknown) {
      return error;
    }
  }
  if (!fallback_error.empty()) {
    return fallback_error;
  }
  return vinput::dbus::MakeErrorInfo(fallback_code, {}, std::to_string(exit_code),
                                     vinput::str::FmtStr(fmt, exit_code));
}

DaemonControlResult RunWithDiagnostics(const std::vector<std::string>& args,
                                       const char* failure_fmt, const char* fallback_code) {
  DaemonControlResult result;
  result.exit_code = RunCommand(args);
  if (result.exit_code == 0) {
    return result;
  }
  result.failure_message = BuildFailureMessage(failure_fmt, result.exit_code);
  result.notification = BuildFailureNotification(fallback_code, failure_fmt, result.exit_code);
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

int JournalctlLogs(bool follow, int lines) {
  return RunCommand(BuildJournalctlCommand(follow, lines));
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
      "systemctl start failed (exit code: %d)", vinput::dbus::kErrorCodeDaemonStartFailed);
}

DaemonControlResult SystemctlRestartWithDiagnostics() {
  return RunWithDiagnostics(
      {"systemctl", "--user", "restart", std::string(vinput::path::DaemonServiceUnitName())},
      "systemctl restart failed (exit code: %d)", vinput::dbus::kErrorCodeDaemonRestartFailed);
}

bool NotifyDaemonNotification(const vinput::dbus::ErrorInfo& notification) {
  if (notification.empty()) {
    return false;
  }
  vinput::cli::DbusClient dbus;
  return dbus.Notify(notification);
}

} // namespace vinput::cli
