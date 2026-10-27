#pragma once

#include <string>

#include "common/dbus/dbus_interface.h"

struct sd_bus;

namespace vinput::cli {

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
  bool StartAdapter(const std::string& adapter_id, std::string* error = nullptr);
  bool StopAdapter(const std::string& adapter_id, std::string* error = nullptr);

private:
  sd_bus* bus_ = nullptr;
};

} // namespace vinput::cli
