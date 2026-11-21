#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vinput::dbus {

constexpr const char* kBusName = "org.fcitx.Vinput";
constexpr const char* kObjectPath = "/org/fcitx/Vinput";
constexpr const char* kInterface = "org.fcitx.Vinput.Service";

constexpr const char* kMethodStartRecording = "StartRecording";
constexpr const char* kMethodStartCommandRecording = "StartCommandRecording";
constexpr const char* kMethodStopRecording = "StopRecording";
constexpr const char* kMethodCancelOperation = "CancelOperation";
constexpr const char* kMethodGetStatus = "GetStatus";
constexpr const char* kMethodGetAsrBackendState = "GetAsrBackendState";
constexpr const char* kMethodReloadAsrBackend = "ReloadAsrBackend";
constexpr const char* kMethodStartClient = "StartClient";
constexpr const char* kMethodStopClient = "StopClient";
constexpr const char* kMethodGetClientStatuses = "GetClientStatuses";
constexpr const char* kMethodTranscribeStream = "TranscribeStream";

constexpr const char* kSignalRecognitionResult = "RecognitionResult";
constexpr const char* kSignalRecognitionPartial = "RecognitionPartial";
constexpr const char* kSignalStatusChanged = "StatusChanged";
constexpr const char* kSignalStreamEvent = "StreamEvent";

constexpr const char* kErrorOperationFailed = "org.fcitx.Vinput.Error.OperationFailed";
constexpr const char* kStatusIdle = "idle";
constexpr const char* kStatusStarting = "starting";
constexpr const char* kStatusRecording = "recording";
constexpr const char* kStatusStopping = "stopping";
constexpr const char* kStatusInferring = "inferring";
constexpr const char* kStatusPostprocessing = "postprocessing";
constexpr const char* kStatusError = "error";

enum class Status : std::uint8_t {
  Idle,
  Starting,
  Recording,
  Stopping,
  Inferring,
  Postprocessing,
  Error
};

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

struct ClientStatus {
  std::string id;
  bool running = false;
  std::int64_t pid = -1;
};

inline const char* StatusToString(Status s) {
  switch (s) {
  case Status::Idle:
    return kStatusIdle;
  case Status::Starting:
    return kStatusStarting;
  case Status::Recording:
    return kStatusRecording;
  case Status::Stopping:
    return kStatusStopping;
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
  if (s == kStatusIdle) {
    return Status::Idle;
  }
  if (s == kStatusStarting) {
    return Status::Starting;
  }
  if (s == kStatusRecording) {
    return Status::Recording;
  }
  if (s == kStatusStopping) {
    return Status::Stopping;
  }
  if (s == kStatusInferring) {
    return Status::Inferring;
  }
  if (s == kStatusPostprocessing) {
    return Status::Postprocessing;
  }
  if (s == kStatusError) {
    return Status::Error;
  }
  return Status::Idle;
}

} // namespace vinput::dbus
