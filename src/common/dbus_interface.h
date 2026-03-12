#pragma once

#include <string>

namespace vinput::dbus {

constexpr const char *kBusName = "org.fcitx.Vinput";
constexpr const char *kObjectPath = "/org/fcitx/Vinput";
constexpr const char *kInterface = "org.fcitx.Vinput.Service";

constexpr const char *kMethodStartRecording = "StartRecording";
constexpr const char *kMethodStartCommandRecording = "StartCommandRecording";
constexpr const char *kMethodStopRecording = "StopRecording";
constexpr const char *kMethodGetStatus = "GetStatus";

constexpr const char *kSignalRecognitionResult = "RecognitionResult";
constexpr const char *kSignalStatusChanged = "StatusChanged";
constexpr const char *kSignalLlmError = "LlmError";

enum class Status { Idle, Recording, Inferring, Postprocessing, Error };

inline const char *StatusToString(Status s) {
  switch (s) {
  case Status::Idle:
    return "idle";
  case Status::Recording:
    return "recording";
  case Status::Inferring:
    return "inferring";
  case Status::Postprocessing:
    return "postprocessing";
  case Status::Error:
    return "error";
  }
  return "unknown";
}

inline Status StringToStatus(const std::string &s) {
  if (s == "idle")
    return Status::Idle;
  if (s == "recording")
    return Status::Recording;
  if (s == "inferring")
    return Status::Inferring;
  if (s == "postprocessing")
    return Status::Postprocessing;
  if (s == "error")
    return Status::Error;
  return Status::Idle;
}

} // namespace vinput::dbus
