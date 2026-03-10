#pragma once

namespace vinput::cli {

constexpr const char* kServiceUnit = "vinput-daemon.service";

int SystemctlStart();
int SystemctlStop();
int SystemctlRestart();
int JournalctlLogs(bool follow, int lines);

} // namespace vinput::cli
