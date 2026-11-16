#pragma once

#include <string>

namespace vinput::cli {

struct DaemonControlResult {
  int exit_code = -1;
  std::string failure_message;

  [[nodiscard]] bool ok() const { return exit_code == 0; }
};

int SystemctlStop();
int JournalctlLogs(bool follow, int lines, const std::string& level = {});
std::string JournalctlLogsText(int lines);
DaemonControlResult SystemctlStartWithDiagnostics();
DaemonControlResult SystemctlRestartWithDiagnostics();
DaemonControlResult SystemctlEnable(bool force = false);
DaemonControlResult SystemctlDisable();
bool SystemctlIsEnabled();

} // namespace vinput::cli
