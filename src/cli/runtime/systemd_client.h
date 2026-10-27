#pragma once

#include <string>

namespace vinput::cli {

struct DaemonControlResult {
  int exit_code = -1;
  std::string failure_message;

  [[nodiscard]] bool ok() const { return exit_code == 0; }
};

int SystemctlStop();
int JournalctlLogs(bool follow, int lines);
std::string JournalctlLogsText(int lines);
DaemonControlResult SystemctlStartWithDiagnostics();
DaemonControlResult SystemctlRestartWithDiagnostics();

} // namespace vinput::cli
