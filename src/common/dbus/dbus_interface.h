#pragma once

#include <string>
#include <vector>

#include "common/dbus/error_info.h"

namespace vinput::dbus {

constexpr const char* kFcitxBusName = "org.fcitx.Fcitx5";
constexpr const char* kBusName = "org.fcitx.Vinput";
constexpr const char* kObjectPath = "/org/fcitx/Vinput";
constexpr const char* kInterface = "org.fcitx.Vinput.Service";
constexpr const char* kNotifierObjectPath = "/org/fcitx/Fcitx5/Vinput";
constexpr const char* kNotifierInterface = "org.fcitx.Fcitx5.Vinput1";

constexpr const char* kMethodStartRecording = "StartRecording";
constexpr const char* kMethodStartCommandRecording = "StartCommandRecording";
constexpr const char* kMethodStopRecording = "StopRecording";
constexpr const char* kMethodCancelPostprocessing = "CancelPostprocessing";
constexpr const char* kMethodGetStatus = "GetStatus";
constexpr const char* kMethodGetAsrBackendState = "GetAsrBackendState";
constexpr const char* kMethodReloadAsrBackend = "ReloadAsrBackend";
constexpr const char* kMethodStartAdapter = "StartAdapter";
constexpr const char* kMethodStopAdapter = "StopAdapter";
constexpr const char* kMethodNotify = "Notify";

constexpr const char* kSignalRecognitionResult = "RecognitionResult";
constexpr const char* kSignalRecognitionPartial = "RecognitionPartial";
constexpr const char* kSignalStatusChanged = "StatusChanged";
constexpr const char* kSignalDaemonNotification = "DaemonNotification";

constexpr const char* kErrorOperationFailed = "org.fcitx.Vinput.Error.OperationFailed";
constexpr const char* kStatusIdle = "idle";
constexpr const char* kStatusRecording = "recording";
constexpr const char* kStatusInferring = "inferring";
constexpr const char* kStatusPostprocessing = "postprocessing";
constexpr const char* kStatusError = "error";

enum class Status { Idle, Recording, Inferring, Postprocessing, Error };

struct AsrBackendState {
  std::string target_provider_id;
  std::string target_model_id;
  std::string effective_provider_id;
  std::string effective_model_id;
  std::string last_error;
  bool reload_in_progress = false;
  bool has_effective_backend = false;
  std::vector<std::string> remote_endpoints;
};

inline const char* StatusToString(Status s) {
  switch (s) {
  case Status::Idle:
    return kStatusIdle;
  case Status::Recording:
    return kStatusRecording;
  case Status::Inferring:
    return kStatusInferring;
  case Status::Postprocessing:
    return kStatusPostprocessing;
  case Status::Error:
    return kStatusError;
  }
  return "unknown";
}

inline Status StringToStatus(const std::string& s) {
  if (s == kStatusIdle)
    return Status::Idle;
  if (s == kStatusRecording)
    return Status::Recording;
  if (s == kStatusInferring)
    return Status::Inferring;
  if (s == kStatusPostprocessing)
    return Status::Postprocessing;
  if (s == kStatusError)
    return Status::Error;
  return Status::Idle;
}

} // namespace vinput::dbus
