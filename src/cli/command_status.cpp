#include "cli/command_status.h"
#include "cli/dbus_client.h"
#include "cli/i18n.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <string>

int RunStatus(Formatter& fmt, const CliContext& ctx) {
    vinput::cli::DbusClient dbus;

    std::string dbus_error;
    bool running = dbus.IsDaemonRunning(&dbus_error);

    if (!running) {
        if (ctx.json_output) {
            fmt.PrintJson({{"running", false}});
        } else {
            fmt.PrintError(_(msgs::kDaemonNotRunning, ctx.use_chinese));
        }
        return 1;
    }

    std::string status;
    std::string get_error;
    bool ok = dbus.GetDaemonStatus(&status, &get_error);
    if (!ok) {
        if (ctx.json_output) {
            fmt.PrintJson({{"running", true}, {"status", nullptr}, {"error", get_error}});
        } else {
            fmt.PrintError(get_error);
        }
        return 1;
    }

    if (ctx.json_output) {
        fmt.PrintJson({{"running", true}, {"status", status}});
    } else {
        char buf[256];
        std::snprintf(buf, sizeof(buf), _(msgs::kDaemonStatus, ctx.use_chinese), status.c_str());
        fmt.PrintInfo(buf);
    }

    return 0;
}
