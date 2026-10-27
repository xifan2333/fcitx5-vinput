#include "cli/runtime/systemd_client.h"

#include <exception>
#include <string>
#include <subprocess.hpp>
#include <vector>

#include "common/utils/path_utils.h"
#include "common/utils/sandbox.h"
#include "common/utils/string_utils.h"

namespace vinput::cli {

namespace {

std::vector<std::string> BuildCommand(const std::vector<std::string>& args) {
  return vinput::sandbox::WrapHostCommand(std::vector<std::string>(args.begin(), args.end()));
}

std::vector<std::string> BuildJournalctlCommand(bool follow, int lines) {
  auto args = vinput::sandbox::DaemonLogFilter();
  const std::string lines_str = std::to_string(lines);
  args.emplace_back("-n");
  args.push_back(lines_str);
  if (follow) {
    args.emplace_back("-f");
  }
  return args;
}

int RunCommand(const std::vector<std::string>& args);

std::string BuildFailureMessage(const char* fmt, int exit_code) {
  std::string message = vinput::str::FmtStr(fmt, exit_code);
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
      "systemctl start failed (exit code: %d)");
}

DaemonControlResult SystemctlRestartWithDiagnostics() {
  return RunWithDiagnostics(
      {"systemctl", "--user", "restart", std::string(vinput::path::DaemonServiceUnitName())},
      "systemctl restart failed (exit code: %d)");
}

} // namespace vinput::cli
