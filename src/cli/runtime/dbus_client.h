#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/dbus/dbus_interface.h"

struct sd_bus;

namespace vinput::cli {

using ClientStatusMap = std::unordered_map<std::string, vinput::dbus::ClientStatus>;

class DbusClient {
public:
  DbusClient();
  ~DbusClient();

  DbusClient(const DbusClient&) = delete;
  DbusClient& operator=(const DbusClient&) = delete;
  DbusClient(DbusClient&&) = delete;
  DbusClient& operator=(DbusClient&&) = delete;

  bool IsDaemonRunning(std::string* error = nullptr);
  bool GetDaemonStatus(std::string* status, std::string* error = nullptr);
  bool GetAsrBackendState(vinput::dbus::AsrBackendState* state, std::string* error = nullptr);

  bool StartRecording(std::string* error = nullptr);
  bool StartCommandRecording(const std::string& selected_text, std::string* error = nullptr);
  bool StopRecording(const std::string& scene_id, std::string* error = nullptr);
  bool ReloadAsrBackend(std::string* error = nullptr);
  bool StartClient(const std::string& client_id, std::string* error = nullptr);
  bool StopClient(const std::string& client_id, std::string* error = nullptr);
  bool GetClientStatuses(std::vector<vinput::dbus::ClientStatus>* statuses,
                         std::string* error = nullptr);
  ClientStatusMap GetClientStatusesMap(std::string* error = nullptr);

private:
  sd_bus* bus_ = nullptr;
};

std::pair<bool, pid_t> ResolveClientRuntimeStatus(const ClientStatusMap& status_map,
                                                  std::string_view client_id);

} // namespace vinput::cli
