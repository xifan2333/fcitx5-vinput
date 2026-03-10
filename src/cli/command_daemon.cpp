#include "cli/command_daemon.h"
#include "cli/systemd_client.h"
#include "cli/i18n.h"
#include <string>

int RunDaemonStart(Formatter& fmt, const CliContext& ctx) {
    int r = vinput::cli::SystemctlStart();
    if (r == 0) {
        fmt.PrintSuccess(_(msgs::kDaemonStarted, ctx.use_chinese));
        return 0;
    }
    std::string msg = "systemctl start failed (exit code: ";
    msg += std::to_string(r);
    msg += ")";
    fmt.PrintError(msg);
    return 1;
}

int RunDaemonStop(Formatter& fmt, const CliContext& ctx) {
    int r = vinput::cli::SystemctlStop();
    if (r == 0) {
        fmt.PrintSuccess(_(msgs::kDaemonStopped, ctx.use_chinese));
        return 0;
    }
    std::string msg = "systemctl stop failed (exit code: ";
    msg += std::to_string(r);
    msg += ")";
    fmt.PrintError(msg);
    return 1;
}

int RunDaemonRestart(Formatter& fmt, const CliContext& ctx) {
    int r = vinput::cli::SystemctlRestart();
    if (r == 0) {
        fmt.PrintSuccess(_(msgs::kDaemonRestarted, ctx.use_chinese));
        return 0;
    }
    std::string msg = "systemctl restart failed (exit code: ";
    msg += std::to_string(r);
    msg += ")";
    fmt.PrintError(msg);
    return 1;
}

int RunDaemonLogs(bool follow, int lines, Formatter& fmt, const CliContext& ctx) {
    (void)fmt;
    (void)ctx;
    return vinput::cli::JournalctlLogs(follow, lines);
}
