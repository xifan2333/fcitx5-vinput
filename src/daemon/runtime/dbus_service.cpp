#include "dbus_service.h"

#include <cstdio>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

#include "common/dbus/dbus_interface.h"

using namespace vinput::dbus;

DbusService::DbusService() = default;

DbusService::~DbusService() {
  if (slot_) {
    sd_bus_slot_unref(slot_);
  }
  if (bus_) {
    sd_bus_flush_close_unref(bus_);
  }
  if (notify_fd_ >= 0) {
    close(notify_fd_);
  }
}

namespace {

int ReplyWithMethodResult(sd_bus_message* m, sd_bus_error* error,
                          const DbusService::MethodResult& result, const char* reply_signature) {
  if (!result.ok) {
    return sd_bus_error_set(error, kErrorOperationFailed, result.message.c_str());
  }

  if (!reply_signature || reply_signature[0] == '\0') {
    return sd_bus_reply_method_return(m, "");
  }
  return sd_bus_reply_method_return(m, reply_signature, result.payload.c_str());
}

} // namespace

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD(kMethodStartRecording, "", "", &DbusService::handleStartRecording,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodStartCommandRecording, "s", "", &DbusService::handleStartCommandRecording,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodStopRecording, "s", "s", &DbusService::handleStopRecording,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodCancelPostprocessing, "b", "", &DbusService::handleCancelPostprocessing,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodGetStatus, "", "s", &DbusService::handleGetStatus,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodGetAsrBackendState, "", "sssssbbas",
                  &DbusService::handleGetAsrBackendState, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodReloadAsrBackend, "", "", &DbusService::handleReloadAsrBackend,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodStartAdapter, "s", "", &DbusService::handleStartAdapter,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD(kMethodStopAdapter, "s", "", &DbusService::handleStopAdapter,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL(kSignalRecognitionResult, "s", 0),
    SD_BUS_SIGNAL(kSignalRecognitionPartial, "s", 0),
    SD_BUS_SIGNAL(kSignalStatusChanged, "s", 0),
    SD_BUS_SIGNAL(kSignalDaemonNotification, kErrorInfoSignature, 0),
    SD_BUS_VTABLE_END,
};

bool DbusService::Start(std::string* error) {
  notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (notify_fd_ < 0) {
    if (error) {
      *error = std::string("failed to create eventfd: ") + strerror(errno);
    }
    return false;
  }

  int ret = sd_bus_open_user(&bus_);
  if (ret < 0) {
    if (error) {
      *error = std::string("failed to open user bus: ") + strerror(-ret);
    }
    return false;
  }

  ret = sd_bus_add_object_vtable(bus_, &slot_, kObjectPath, kInterface, vtable, this);
  if (ret < 0) {
    if (error) {
      *error = std::string("failed to add D-Bus vtable: ") + strerror(-ret);
    }
    return false;
  }

  ret = sd_bus_request_name(bus_, kBusName,
                            SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT);
  if (ret < 0) {
    if (error) {
      *error = std::string("failed to request D-Bus name: ") + strerror(-ret);
    }
    return false;
  }

  fprintf(stderr, "vinput: DBus service started on %s\n", kBusName);
  return true;
}

int DbusService::GetFd() const {
  return sd_bus_get_fd(bus_);
}

int DbusService::GetNotifyFd() const {
  return notify_fd_;
}

bool DbusService::ProcessOnce() {
  int ret = sd_bus_process(bus_, nullptr);
  if (ret < 0) {
    fprintf(stderr, "vinput: sd_bus_process failed: %s\n", strerror(-ret));
    return false;
  }
  return ret > 0;
}

void DbusService::FlushEmitQueue() {
  // Drain the eventfd counter
  uint64_t val;
  (void)read(notify_fd_, &val, sizeof(val));

  std::vector<PendingEmit> local;
  {
    std::lock_guard<std::mutex> lock(emit_mutex_);
    local.swap(emit_queue_);
  }

  for (const auto& item : local) {
    if (item.type == PendingEmit::Type::Result) {
      sd_bus_emit_signal(bus_, kObjectPath, kInterface, kSignalRecognitionResult, "s",
                         item.payload.c_str());
    } else if (item.type == PendingEmit::Type::Partial) {
      sd_bus_emit_signal(bus_, kObjectPath, kInterface, kSignalRecognitionPartial, "s",
                         item.payload.c_str());
    } else if (item.type == PendingEmit::Type::Status) {
      sd_bus_emit_signal(bus_, kObjectPath, kInterface, kSignalStatusChanged, "s",
                         item.payload.c_str());
    } else {
      sd_bus_emit_signal(bus_, kObjectPath, kInterface, kSignalDaemonNotification,
                         kErrorInfoSignature, item.notification.code.c_str(),
                         item.notification.subject.c_str(), item.notification.detail.c_str(),
                         item.notification.raw_message.c_str());
    }
  }
}

void DbusService::EmitRecognitionResult(const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(emit_mutex_);
    emit_queue_.push_back({PendingEmit::Type::Result, text, {}});
  }
  uint64_t val = 1;
  (void)write(notify_fd_, &val, sizeof(val));
}

void DbusService::EmitRecognitionPartial(const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(emit_mutex_);
    emit_queue_.push_back({PendingEmit::Type::Partial, text, {}});
  }
  uint64_t val = 1;
  (void)write(notify_fd_, &val, sizeof(val));
}

void DbusService::EmitStatusChanged(const std::string& status) {
  {
    std::lock_guard<std::mutex> lock(emit_mutex_);
    emit_queue_.push_back({PendingEmit::Type::Status, status, {}});
  }
  uint64_t val = 1;
  (void)write(notify_fd_, &val, sizeof(val));
}

void DbusService::EmitNotification(const vinput::dbus::ErrorInfo& notification) {
  {
    std::lock_guard<std::mutex> lock(emit_mutex_);
    emit_queue_.push_back({PendingEmit::Type::Notification, {}, notification});
  }
  uint64_t val = 1;
  (void)write(notify_fd_, &val, sizeof(val));
}

void DbusService::SetStartHandler(std::function<MethodResult()> handler) {
  start_handler_ = std::move(handler);
}

void DbusService::SetStartCommandHandler(std::function<MethodResult(const std::string&)> handler) {
  start_command_handler_ = std::move(handler);
}

void DbusService::SetStopHandler(std::function<MethodResult(const std::string& scene_id)> handler) {
  stop_handler_ = std::move(handler);
}

void DbusService::SetCancelPostprocessingHandler(
    std::function<MethodResult(bool commit_raw_text)> handler) {
  cancel_postprocessing_handler_ = std::move(handler);
}

void DbusService::SetStatusHandler(std::function<std::string()> handler) {
  status_handler_ = std::move(handler);
}

void DbusService::SetAsrBackendStateHandler(
    std::function<vinput::dbus::AsrBackendState()> handler) {
  asr_backend_state_handler_ = std::move(handler);
}

void DbusService::SetReloadAsrBackendHandler(std::function<MethodResult()> handler) {
  reload_asr_backend_handler_ = std::move(handler);
}

void DbusService::SetStartAdapterHandler(std::function<MethodResult(const std::string&)> handler) {
  start_adapter_handler_ = std::move(handler);
}

void DbusService::SetStopAdapterHandler(std::function<MethodResult(const std::string&)> handler) {
  stop_adapter_handler_ = std::move(handler);
}

int DbusService::handleStartRecording(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  MethodResult result;
  if (self->start_handler_) {
    result = self->start_handler_();
  }
  return ReplyWithMethodResult(m, error, result, "");
}

int DbusService::handleStartCommandRecording(sd_bus_message* m, void* userdata,
                                             sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  const char* selected_text = "";
  int ret = sd_bus_message_read(m, "s", &selected_text);
  if (ret < 0) {
    fprintf(stderr, "vinput: failed to read StartCommandRecording text: %s\n", strerror(-ret));
    return ret;
  }

  MethodResult result;
  if (self->start_command_handler_) {
    result = self->start_command_handler_(selected_text ? selected_text : "");
  }
  return ReplyWithMethodResult(m, error, result, "");
}

int DbusService::handleStopRecording(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  const char* scene_id = "";
  int ret = sd_bus_message_read(m, "s", &scene_id);
  if (ret < 0) {
    fprintf(stderr, "vinput: failed to read StopRecording scene id: %s\n", strerror(-ret));
    return ret;
  }

  MethodResult result;
  if (self->stop_handler_) {
    result = self->stop_handler_(scene_id ? scene_id : "");
  }
  return ReplyWithMethodResult(m, error, result, "s");
}

int DbusService::handleCancelPostprocessing(sd_bus_message* m, void* userdata,
                                            sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  int commit_raw_text = 0;
  const int ret = sd_bus_message_read(m, "b", &commit_raw_text);
  if (ret < 0) {
    fprintf(stderr, "vinput: failed to read CancelPostprocessing action: %s\n", strerror(-ret));
    return ret;
  }

  MethodResult result;
  if (self->cancel_postprocessing_handler_) {
    result = self->cancel_postprocessing_handler_(commit_raw_text != 0);
  }
  return ReplyWithMethodResult(m, error, result, "");
}

int DbusService::handleGetStatus(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  (void)error;
  auto* self = static_cast<DbusService*>(userdata);
  std::string status = kStatusIdle;
  if (self->status_handler_) {
    status = self->status_handler_();
  }
  return sd_bus_reply_method_return(m, "s", status.c_str());
}

int DbusService::handleGetAsrBackendState(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  (void)error;
  auto* self = static_cast<DbusService*>(userdata);
  vinput::dbus::AsrBackendState state;
  if (self->asr_backend_state_handler_) {
    state = self->asr_backend_state_handler_();
  }
  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) {
    return r;
  }
  r = sd_bus_message_append(reply, "sssssbb", state.target_provider_id.c_str(),
                            state.target_model_id.c_str(), state.effective_provider_id.c_str(),
                            state.effective_model_id.c_str(), state.last_error.c_str(),
                            state.reload_in_progress ? 1 : 0, state.has_effective_backend ? 1 : 0);
  if (r < 0) {
    sd_bus_message_unref(reply);
    return r;
  }
  r = sd_bus_message_open_container(reply, 'a', "s");
  if (r < 0) {
    sd_bus_message_unref(reply);
    return r;
  }
  for (const auto& endpoint : state.remote_endpoints) {
    r = sd_bus_message_append_basic(reply, 's', endpoint.c_str());
    if (r < 0) {
      sd_bus_message_unref(reply);
      return r;
    }
  }
  r = sd_bus_message_close_container(reply);
  if (r < 0) {
    sd_bus_message_unref(reply);
    return r;
  }
  r = sd_bus_send(nullptr, reply, nullptr);
  sd_bus_message_unref(reply);
  return r;
}

int DbusService::handleReloadAsrBackend(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  MethodResult result;
  if (self->reload_asr_backend_handler_) {
    result = self->reload_asr_backend_handler_();
  }
  return ReplyWithMethodResult(m, error, result, "");
}

int DbusService::handleStartAdapter(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  const char* adapter_id = "";
  int ret = sd_bus_message_read(m, "s", &adapter_id);
  if (ret < 0) {
    fprintf(stderr, "vinput: failed to read StartAdapter id: %s\n", strerror(-ret));
    return ret;
  }

  MethodResult result;
  if (self->start_adapter_handler_) {
    result = self->start_adapter_handler_(adapter_id ? adapter_id : "");
  }
  return ReplyWithMethodResult(m, error, result, "");
}

int DbusService::handleStopAdapter(sd_bus_message* m, void* userdata, sd_bus_error* error) {
  auto* self = static_cast<DbusService*>(userdata);
  const char* adapter_id = "";
  int ret = sd_bus_message_read(m, "s", &adapter_id);
  if (ret < 0) {
    fprintf(stderr, "vinput: failed to read StopAdapter id: %s\n", strerror(-ret));
    return ret;
  }

  MethodResult result;
  if (self->stop_adapter_handler_) {
    result = self->stop_adapter_handler_(adapter_id ? adapter_id : "");
  }
  return ReplyWithMethodResult(m, error, result, "");
}
