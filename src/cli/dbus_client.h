#pragma once
#include <string>

struct sd_bus;

namespace vinput::cli {

class DbusClient {
public:
    DbusClient();
    ~DbusClient();

    DbusClient(const DbusClient&) = delete;
    DbusClient& operator=(const DbusClient&) = delete;

    // Check if daemon is running (via NameHasOwner, avoids auto-activation)
    bool IsDaemonRunning(std::string* error = nullptr);
    // Get daemon status string (only call if IsDaemonRunning returns true)
    bool GetDaemonStatus(std::string* status, std::string* error = nullptr);

private:
    sd_bus* bus_ = nullptr;
};

} // namespace vinput::cli
