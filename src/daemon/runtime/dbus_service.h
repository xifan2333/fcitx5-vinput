#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <vector>

#include "common/dbus/dbus_interface.h"
#include "common/dbus/error_info.h"

class DbusService {
public:
  struct MethodResult {
    bool ok = true;
    std::string message;
    std::string payload;

    static MethodResult Success(std::string payload = {}) {
      MethodResult result;
      result.payload = std::move(payload);
      return result;
    }

    static MethodResult Failure(std::string message) {
      MethodResult result;
      result.ok = false;
      result.message = std::move(message);
      return result;
    }
  };

  DbusService();
  ~DbusService();

  bool Start(std::string* error = nullptr);
  int GetFd() const;
  int GetNotifyFd() const;
  bool ProcessOnce();
  void FlushEmitQueue(); // main thread only
  void EmitRecognitionResult(const std::string& text);
  void EmitRecognitionPartial(const std::string& text);
  void EmitStatusChanged(const std::string& status);
  void EmitNotification(const vinput::dbus::ErrorInfo& notification);

  void SetStartHandler(std::function<MethodResult()> handler);
  void SetStartCommandHandler(std::function<MethodResult(const std::string&)> handler);
  void SetStopHandler(std::function<MethodResult(const std::string& scene_id)> handler);
  void SetCancelOperationHandler(std::function<MethodResult(bool commit_raw_text)> handler);
  void SetCancelPostprocessingHandler(std::function<MethodResult(bool commit_raw_text)> handler);
  void SetStatusHandler(std::function<std::string()> handler);
  void SetAsrBackendStateHandler(std::function<vinput::dbus::AsrBackendState()> handler);
  void SetReloadAsrBackendHandler(std::function<MethodResult()> handler);
  void SetStartAdapterHandler(std::function<MethodResult(const std::string& adapter_id)> handler);
  void SetStopAdapterHandler(std::function<MethodResult(const std::string& adapter_id)> handler);

  static int handleStartRecording(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleStartCommandRecording(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleStopRecording(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleCancelOperation(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleCancelPostprocessing(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleGetStatus(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleGetAsrBackendState(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleReloadAsrBackend(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleStartAdapter(sd_bus_message* m, void* userdata, sd_bus_error* error);
  static int handleStopAdapter(sd_bus_message* m, void* userdata, sd_bus_error* error);

private:
  sd_bus* bus_ = nullptr;
  sd_bus_slot* slot_ = nullptr;
  int notify_fd_ = -1;

  struct PendingEmit {
    enum class Type { Result, Partial, Status, Notification };
    Type type;
    std::string payload;
    vinput::dbus::ErrorInfo notification;
  };
  std::mutex emit_mutex_;
  std::vector<PendingEmit> emit_queue_;

  std::function<MethodResult()> start_handler_;
  std::function<MethodResult(const std::string&)> start_command_handler_;
  std::function<MethodResult(const std::string& scene_id)> stop_handler_;
  std::function<MethodResult(bool commit_raw_text)> cancel_operation_handler_;
  std::function<MethodResult(bool commit_raw_text)> cancel_postprocessing_handler_;
  std::function<std::string()> status_handler_;
  std::function<vinput::dbus::AsrBackendState()> asr_backend_state_handler_;
  std::function<MethodResult()> reload_asr_backend_handler_;
  std::function<MethodResult(const std::string& adapter_id)> start_adapter_handler_;
  std::function<MethodResult(const std::string& adapter_id)> stop_adapter_handler_;
};
