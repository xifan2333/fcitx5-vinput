#include "cli/runtime/systemd_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
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

std::vector<char*> BuildExecArgs(std::vector<std::string>& args) {
  std::vector<char*> exec_args;
  exec_args.reserve(args.size() + 1);
  for (auto& arg : args) {
    exec_args.push_back(arg.data());
  }
  exec_args.push_back(nullptr);
  return exec_args;
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

std::string StripJournalPrefix(std::string line) {
  auto pos = line.find("]: ");
  if (pos != std::string::npos) {
    line.erase(0, pos + 3);
  } else {
    pos = line.find(": ");
    if (pos != std::string::npos) {
      line.erase(0, pos + 2);
    }
  }
  if (line.starts_with("vinput-daemon: ")) {
    line.erase(0, std::string("vinput-daemon: ").size());
  } else if (line.starts_with("vinput: ")) {
    line.erase(0, std::string("vinput: ").size());
  }
  return vinput::str::TrimAsciiWhitespace(line);
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
  auto actual_args = BuildCommand(args);
  auto exec_args = BuildExecArgs(actual_args);
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execvp(exec_args[0], exec_args.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int RunCommandCapture(const std::vector<std::string>& args, std::string* output) {
  int pipefd[2];
  if (pipe(pipefd) < 0)
    return -1;

  auto actual_args = BuildCommand(args);
  auto exec_args = BuildExecArgs(actual_args);

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execvp(exec_args[0], exec_args.data());
    _exit(127);
  }

  close(pipefd[1]);
  std::string buffer;
  std::array<char, 4096> chunk{};
  ssize_t count = 0;
  while ((count = read(pipefd[0], chunk.data(), chunk.size())) > 0) {
    buffer.append(chunk.data(), static_cast<size_t>(count));
  }
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0)
    return -1;
  if (output) {
    *output = std::move(buffer);
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
