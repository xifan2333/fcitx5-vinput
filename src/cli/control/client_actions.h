#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cli/utils/cli_context.h"
#include "cli/utils/formatter.h"

namespace vinput::cli::control {

enum class ClientKind : std::uint8_t {
  Trigger,
  Receiver,
};

int RunClientList(ClientKind kind, bool available, Formatter& fmt, const CliContext& ctx);
int RunClientInstall(ClientKind kind, const std::string& selector,
                     const std::vector<std::string>& env_overrides, Formatter& fmt,
                     const CliContext& ctx);
int RunClientRemove(ClientKind kind, const std::string& selector, Formatter& fmt,
                    const CliContext& ctx);
int RunClientUpdate(ClientKind kind, const std::string& selector, bool all, bool force,
                    Formatter& fmt, const CliContext& ctx);
int RunClientEdit(ClientKind kind, const std::string& selector, Formatter& fmt,
                  const CliContext& ctx);
int RunClientStart(ClientKind kind, const std::string& id, Formatter& fmt, const CliContext& ctx);
int RunClientStop(ClientKind kind, const std::string& id, Formatter& fmt, const CliContext& ctx);
int RunListen(std::string_view type_filter, Formatter& fmt, const CliContext& ctx);

} // namespace vinput::cli::control
