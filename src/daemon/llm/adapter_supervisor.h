#pragma once

#include <memory>
#include <string>

#include "daemon/runtime/dbus_service.h"

struct CoreConfig;

namespace vinput::daemon::llm {

class AdapterSupervisor {
public:
  explicit AdapterSupervisor(DbusService* dbus);
  ~AdapterSupervisor();

  AdapterSupervisor(const AdapterSupervisor&) = delete;
  AdapterSupervisor& operator=(const AdapterSupervisor&) = delete;
  AdapterSupervisor(AdapterSupervisor&&) = delete;
  AdapterSupervisor& operator=(AdapterSupervisor&&) = delete;

  bool Start(std::string* error);
  void Shutdown();
  DbusService::MethodResult StartAdapter(const std::string& adapter_id);
  DbusService::MethodResult StopAdapter(const std::string& adapter_id);
  void AutoStart(const CoreConfig& config);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vinput::daemon::llm
