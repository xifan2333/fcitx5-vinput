#include <chrono>
#include <cstdio>
#include <fcitx-utils/dbus/matchrule.h>
#include <fcitx-utils/dbus/message.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <string>
#include <tuple>

#include "common/asr/recognition_result.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/dbus/error_info.h"
#include "common/i18n.h"
#include "common/runtime/runtime_defaults.h"
#include "common/utils/debug_log.h"
#include "common/utils/path_utils.h"
#include "common/utils/string_utils.h"

#include "core/vinput.h"
#include "notifications_public.h"

using namespace vinput::dbus;

namespace {

constexpr uint64_t kStatusSyncIntervalUsec = 200 * 1000;
constexpr auto kPolledIdleRecoveryDelay =
    std::chrono::milliseconds(vinput::runtime::kDbusCallTimeoutMs);
std::string StartingPreeditText() {
  return _("... Starting ...");
}
std::string RecordingPreeditText() {
  return _("... Recording ...");
}

std::string CommandingPreeditText() {
  return _("... Commanding ...");
}

std::string InferringPreeditText() {
  return _("... Recognizing ...");
}

std::string PostprocessingPreeditText(bool command_mode, bool raw_prev = true) {
  if (command_mode) {
    return _("... Applying command ... (Esc: Cancel)");
  }
  return raw_prev ? _("... Postprocessing ... (Enter: Use raw ASR text, Esc: Cancel)")
                  : _("... Postprocessing ... (Esc: Cancel)");
}

std::string ApplyingCommandPreeditText() {
  return _("... Applying command ... (Esc: Cancel)");
}

std::string CommandTranscriptPreeditText(const std::string& text) {
  return vinput::str::FmtStr(_("Command: %s"), text.c_str());
}

std::string DaemonUnavailablePreeditText() {
  return _("Voice input daemon is unavailable.");
}

std::string DaemonNotRespondingPreeditText() {
  return _("Voice input daemon is not responding.");
}

std::string LimitPreviewText(const std::string& text, int max_display_width) {
  if (max_display_width <= 0) {
    return text;
  }
  return vinput::str::TruncateMiddleUtf8(text, static_cast<size_t>(max_display_width), "...");
}

std::string ComposeLivePreedit(const std::string& transcript_text, const std::string& fallback_text,
                               int max_display_width = 0) {
  if (transcript_text.empty()) {
    return fallback_text;
  }

  return LimitPreviewText(transcript_text, max_display_width);
}

std::string AppendDetail(std::string summary, const std::string& detail) {
  if (detail.empty()) {
    return summary;
  }
  summary += "\n";
  summary += detail;
  return summary;
}

std::string RenderErrorMessage(const vinput::dbus::ErrorInfo& error) {
  using namespace vinput::dbus;

  if (error.code == kErrorCodeLocalAsrModelConfigMissing) {
    return error.subject.empty()
               ? _("Local ASR model configuration is missing. Please configure "
                   "a model first.")
               : vinput::str::FmtStr(_("Local ASR model configuration is missing for provider "
                                       "'%s'. Please configure a model first."),
                                     error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelTypeMissing) {
    return error.subject.empty() ? _("Local ASR model metadata is missing family.")
                                 : vinput::str::FmtStr(_("Local ASR model metadata is missing "
                                                         "family for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelInvalidPath) {
    return error.subject.empty() ? _("Local ASR model metadata contains an invalid path.")
                                 : vinput::str::FmtStr(_("Local ASR model metadata contains an "
                                                         "invalid path for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelTokensMissing) {
    return error.subject.empty() ? _("Local ASR model tokens file is missing.")
                                 : vinput::str::FmtStr(_("Local ASR model tokens file is missing "
                                                         "for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelFilesMissing) {
    return error.subject.empty()
               ? _("Local ASR model files are missing.")
               : vinput::str::FmtStr(_("Local ASR model files are missing for provider '%s'."),
                                     error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelRootResolveFailed) {
    return error.subject.empty() ? _("Failed to resolve the local ASR model directory.")
                                 : vinput::str::FmtStr(_("Failed to resolve the local ASR model "
                                                         "directory for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelParseFailed) {
    return error.subject.empty() ? _("Failed to parse local ASR model metadata.")
                                 : vinput::str::FmtStr(_("Failed to parse local ASR model "
                                                         "metadata for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeLocalAsrUnsupportedModelType) {
    return error.subject.empty() ? _("The local ASR model type is not supported.")
                                 : vinput::str::FmtStr(_("The local ASR model type is not "
                                                         "supported for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeLocalAsrRecognizerCreateFailed) {
    return error.subject.empty() ? _("Failed to initialize the local ASR recognizer.")
                                 : vinput::str::FmtStr(_("Failed to initialize the local ASR "
                                                         "recognizer for provider '%s'."),
                                                       error.subject);
  }
  if (error.code == kErrorCodeVadCreateFailed) {
    return error.subject.empty()
               ? _("Failed to initialize VAD.")
               : vinput::str::FmtStr(_("Failed to initialize VAD for provider '%s'."),
                                     error.subject);
  }
  if (error.code == kErrorCodeLocalAsrModelCheckFailed) {
    return error.subject.empty()
               ? _("Local ASR model check failed.")
               : vinput::str::FmtStr(_("Local ASR model check failed for provider '%s'."),
                                     error.subject);
  }
  if (error.code == kErrorCodeLocalAsrProviderInitFailed) {
    return error.subject.empty()
               ? _("Failed to initialize the local ASR provider.")
               : vinput::str::FmtStr(_("Failed to initialize the local ASR provider '%s'."),
                                     error.subject);
  }
  if (error.code == kErrorCodeAudioCaptureLoopNotInitialized) {
    return _("Audio capture is not initialized.");
  }
  if (error.code == kErrorCodePipeWireThreadLoopCreateFailed) {
    return _("Failed to create the PipeWire thread loop.");
  }
  if (error.code == kErrorCodePipeWireThreadLoopStartFailed) {
    return AppendDetail(_("Failed to start the PipeWire thread loop."), error.detail);
  }
  if (error.code == kErrorCodePipeWirePropertiesAllocFailed) {
    return _("Failed to allocate PipeWire properties.");
  }
  if (error.code == kErrorCodePipeWireStreamCreateFailed) {
    return _("Failed to create the PipeWire stream.");
  }
  if (error.code == kErrorCodePipeWireStreamConnectFailed) {
    return AppendDetail(_("Failed to connect the PipeWire stream."), error.detail);
  }
  if (error.code == kErrorCodeDbusEventfdCreateFailed) {
    return AppendDetail(_("Failed to initialize daemon notifications."), error.detail);
  }
  if (error.code == kErrorCodeDbusUserBusOpenFailed) {
    return AppendDetail(_("Failed to connect to the user D-Bus."), error.detail);
  }
  if (error.code == kErrorCodeDbusVtableAddFailed) {
    return AppendDetail(_("Failed to register the daemon D-Bus interface."), error.detail);
  }
  if (error.code == kErrorCodeDbusNameRequestFailed) {
    return AppendDetail(_("Failed to acquire the daemon D-Bus name."), error.detail);
  }
  if (error.code == kErrorCodeStartRecordingFailed) {
    return _("Failed to start recording.");
  }
  if (error.code == kErrorCodeStartCommandRecordingFailed) {
    return _("Failed to start command recording.");
  }
  if (error.code == kErrorCodeAsrProviderStartFailed) {
    return error.subject.empty()
               ? _("ASR provider failed to start.")
               : vinput::str::FmtStr(_("ASR provider '%s' failed to start."), error.subject);
  }
  if (error.code == kErrorCodeAsrProviderTimeout) {
    return error.subject.empty()
               ? _("ASR provider timed out.")
               : vinput::str::FmtStr(_("ASR provider '%s' timed out."), error.subject);
  }
  if (error.code == kErrorCodeAsrProviderFailed) {
    return error.subject.empty()
               ? _("ASR provider failed.")
               : vinput::str::FmtStr(_("ASR provider '%s' failed."), error.subject);
  }
  if (error.code == kErrorCodeAsrProviderNoText) {
    return error.subject.empty()
               ? _("ASR provider returned no text.")
               : vinput::str::FmtStr(_("ASR provider '%s' returned no text."), error.subject);
  }
  if (error.code == kErrorCodeLlmRequestFailed) {
    return AppendDetail(_("LLM request failed."), error.detail);
  }
  if (error.code == kErrorCodeLlmHttpFailed) {
    return _("LLM request returned an HTTP error.");
  }
  if (error.code == kErrorCodePromptFileLoadFailed) {
    if (error.subject.empty()) {
      return _("Failed to load prompt file.");
    }
    if (error.detail.empty()) {
      return vinput::str::FmtStr(_("Failed to load prompt file '%s'."), error.subject);
    }
    return vinput::str::FmtStr(_("Failed to load prompt file '%s': %s"), error.subject,
                               error.detail);
  }
  if (error.code == kErrorCodeProcessingUnknown) {
    return _("Unknown error during processing.");
  }
  if (error.code == kErrorCodeDaemonStartFailed) {
    return _("Failed to start the daemon.");
  }
  if (error.code == kErrorCodeDaemonRestartFailed) {
    return _("Failed to restart the daemon.");
  }
  if (error.code == kErrorCodeDaemonBusy) {
    return _("Voice input daemon is busy. Please wait for the current request "
             "to finish.");
  }
  if (error.code == kErrorCodeAsrBackendLoading) {
    return _("ASR backend is still loading. Please try again in a moment.");
  }
  if (error.code == kErrorCodeAsrBackendReloadFailed) {
    return AppendDetail(_("ASR backend reload failed. The previous backend may "
                          "still be active."),
                        error.detail);
  }

  if (!error.raw_message.empty()) {
    return error.raw_message;
  }
  if (!error.detail.empty()) {
    return error.detail;
  }
  return _("Unknown error.");
}

bool IsErrorLikeNotification(const vinput::dbus::ErrorInfo& notification) {
  return notification.code != vinput::dbus::kErrorCodeUnknown || !notification.subject.empty() ||
         !notification.detail.empty();
}

std::string RenderMethodCallFailure(std::string_view error_name, std::string_view error_message,
                                    std::string fallback) {
  if (error_name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
      error_name == "org.freedesktop.DBus.Error.NameHasNoOwner") {
    return DaemonUnavailablePreeditText();
  }
  if (error_name == "org.freedesktop.DBus.Error.NoReply" ||
      error_name == "org.freedesktop.DBus.Error.Timeout") {
    return DaemonNotRespondingPreeditText();
  }

  auto classified = vinput::dbus::ClassifyErrorText(error_message);
  if (!classified.empty()) {
    const std::string rendered = RenderErrorMessage(classified);
    if (!rendered.empty() && rendered != error_message) {
      return rendered;
    }
  }

  if (!error_message.empty()) {
    return std::string(error_message);
  }
  return fallback;
}

} // namespace

void VinputEngine::setupDBusWatcher() {
  if (!bus_)
    return;

  fcitx::dbus::MatchRule result_rule(kBusName, kObjectPath, kInterface, kSignalRecognitionResult);

  result_slot_ = bus_->addMatch(result_rule, [this](fcitx::dbus::Message& msg) {
    onRecognitionResult(msg);
    return true;
  });

  fcitx::dbus::MatchRule partial_rule(kBusName, kObjectPath, kInterface, kSignalRecognitionPartial);

  partial_slot_ = bus_->addMatch(partial_rule, [this](fcitx::dbus::Message& msg) {
    onRecognitionPartial(msg);
    return true;
  });

  fcitx::dbus::MatchRule status_rule(kBusName, kObjectPath, kInterface, kSignalStatusChanged);

  status_slot_ = bus_->addMatch(status_rule, [this](fcitx::dbus::Message& msg) {
    onStatusChanged(msg);
    return true;
  });

  fcitx::dbus::MatchRule error_rule(kBusName, kObjectPath, kInterface, kSignalDaemonNotification);

  error_slot_ = bus_->addMatch(error_rule, [this](fcitx::dbus::Message& msg) {
    onDaemonNotification(msg);
    return true;
  });
}

bool VinputEngine::callStartRecording() {
  command_selected_text_.clear();
  flushContextBuffer();
  if (!bus_ || !daemonSyncAllowed()) {
    noteDaemonSyncFailure();
    return false;
  }
  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface, kMethodStartRecording);
  pending_start_call_slot_ =
      msg.callAsync(vinput::runtime::kDbusCallTimeoutUsec, [this](fcitx::dbus::Message& reply) {
        return handleStartRecordingReply(reply);
      });
  if (!pending_start_call_slot_) {
    noteDaemonSyncFailure();
    return false;
  }
  return true;
}

bool VinputEngine::callStartCommandRecording(const std::string& selected_text) {
  command_selected_text_ = selected_text;
  flushContextBuffer();
  if (!bus_ || !daemonSyncAllowed()) {
    noteDaemonSyncFailure();
    return false;
  }
  auto msg =
      bus_->createMethodCall(kBusName, kObjectPath, kInterface, kMethodStartCommandRecording);
  msg << selected_text;
  pending_start_call_slot_ =
      msg.callAsync(vinput::runtime::kDbusCallTimeoutUsec, [this](fcitx::dbus::Message& reply) {
        return handleStartRecordingReply(reply);
      });
  if (!pending_start_call_slot_) {
    noteDaemonSyncFailure();
    return false;
  }
  return true;
}

bool VinputEngine::handleStartRecordingReply(fcitx::dbus::Message& reply) {
  auto slot = std::move(pending_start_call_slot_);
  if (!reply || reply.isError()) {
    noteDaemonSyncFailure();
    auto* ic = resolveFrontendInputContext();
    const bool cancelled = recording_cancel_requested_;
    recording_cancel_requested_ = false;
    finishFrontendSession(ic);
    if (ic && !cancelled) {
      updateVoicePresentation(ic, reply ? RenderMethodCallFailure(reply.errorName(),
                                                                  reply.errorMessage(),
                                                                  DaemonUnavailablePreeditText())
                                        : DaemonUnavailablePreeditText());
    }
    return true;
  }
  clearDaemonSyncFailure();
  if (recording_cancel_requested_) {
    callCancelOperation(false);
  }
  return true;
}

void VinputEngine::callCancelOperation(bool commit_raw_text) {
  if (!bus_ || pending_cancel_call_slot_) {
    return;
  }
  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface, kMethodCancelOperation);
  msg << commit_raw_text;
  const bool cancelling_recording = recording_cancel_requested_;
  pending_cancel_call_slot_ = msg.callAsync(
      vinput::runtime::kDbusCallTimeoutUsec,
      [this, cancelling_recording](fcitx::dbus::Message& reply) {
        auto slot = std::move(pending_cancel_call_slot_);
        if (!reply || reply.isError()) {
          noteDaemonSyncFailure();
          notifyError(reply ? RenderMethodCallFailure(reply.errorName(), reply.errorMessage(),
                                                      DaemonUnavailablePreeditText())
                            : DaemonUnavailablePreeditText());
          // Retain the discard intent; never turn a failed cancel into a commit.
          ensureStatusSync();
          return true;
        }
        clearDaemonSyncFailure();
        if (cancelling_recording) {
          recording_cancel_requested_ = false;
          finishFrontendSession();
        }
        return true;
      });
  if (!pending_cancel_call_slot_) {
    noteDaemonSyncFailure();
    notifyError(DaemonUnavailablePreeditText());
  }
}

bool VinputEngine::callStopRecording(const std::string& scene_id) {
  if (!bus_ || !daemonSyncAllowed()) {
    noteDaemonSyncFailure();
    return false;
  }
  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface, kMethodStopRecording);
  msg << scene_id;
  pending_stop_call_slot_ =
      msg.callAsync(vinput::runtime::kDbusCallTimeoutUsec, [this](fcitx::dbus::Message& reply) {
        auto slot = std::move(pending_stop_call_slot_);
        if (!reply || reply.isError()) {
          noteDaemonSyncFailure();
          fprintf(stderr, "vinput: StopRecording rejected by daemon\n");
          auto* ic = resolveFrontendInputContext();
          finishFrontendSession(ic);
          if (ic) {
            updateVoicePresentation(
                ic, reply ? RenderMethodCallFailure(reply.errorName(), reply.errorMessage(),
                                                    DaemonUnavailablePreeditText())
                          : DaemonUnavailablePreeditText());
          }
          return true;
        }
        clearDaemonSyncFailure();
        return true;
      });
  if (!pending_stop_call_slot_) {
    noteDaemonSyncFailure();
    return false;
  }
  return true;
}

void VinputEngine::ensureStatusSync() {
  const bool needs_sync = recording_cancel_requested_ || session_ || status_ic_;
  if (!needs_sync) {
    stopStatusSyncIfIdle();
    return;
  }

  if (!status_sync_event_) {
    status_sync_event_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kStatusSyncIntervalUsec, 0,
        [this](fcitx::EventSourceTime* event, uint64_t) {
          syncFrontendWithDaemonStatus();
          if (!(recording_cancel_requested_ || session_ || status_ic_)) {
            return false;
          }
          event->setNextInterval(kStatusSyncIntervalUsec);
          return true;
        });
    return;
  }

  status_sync_event_->setTime(fcitx::now(CLOCK_MONOTONIC) + kStatusSyncIntervalUsec);
  status_sync_event_->setEnabled(true);
}

void VinputEngine::stopStatusSyncIfIdle() {
  if (status_sync_event_ && !(recording_cancel_requested_ || session_ || status_ic_)) {
    status_sync_event_->setEnabled(false);
  }
}

void VinputEngine::enterPendingStartState(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                          bool command_mode) {
  if (!ic) {
    return;
  }
  rememberInputContext(ic);
  if (status_ic_ && status_ic_ != ic) {
    clearVoicePresentation(status_ic_);
  }
  if (!session_) {
    session_.emplace(Session{Session::Phase::PendingStart,
                             ic,
                             trigger,
                             std::chrono::steady_clock::now(),
                             command_mode,
                             {},
                             {}});
  } else {
    session_->phase = Session::Phase::PendingStart;
    session_->ic = ic;
    session_->trigger = trigger;
    session_->command_mode = command_mode;
  }
  status_ic_ = ic;
  updateVoicePresentation(
      ic, ComposeLivePreedit({}, StartingPreeditText(), max_streaming_display_width_));
  ensureStatusSync();
}

void VinputEngine::enterRecordingState(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                       bool command_mode) {
  if (!ic) {
    return;
  }
  rememberInputContext(ic);
  if (status_ic_ && status_ic_ != ic) {
    clearVoicePresentation(status_ic_);
  }
  const bool stop_after_start = session_ && session_->phase == Session::Phase::PendingStart &&
                                session_->trigger_released && session_->stop_on_release;
  if (!session_) {
    session_.emplace(Session{Session::Phase::Recording,
                             ic,
                             trigger,
                             std::chrono::steady_clock::now(),
                             command_mode,
                             {},
                             {}});
  } else {
    session_->phase = Session::Phase::Recording;
    session_->ic = ic;
    session_->trigger = trigger;
    session_->command_mode = command_mode;
  }
  status_ic_ = ic;
  updateVoicePresentation(
      ic, ComposeLivePreedit(session_ ? session_->transcript_text : std::string{},
                             command_mode ? CommandingPreeditText() : RecordingPreeditText(),
                             max_streaming_display_width_));
  ensureStatusSync();
  if (stop_after_start) {
    scheduleStopRecording();
  }
}

void VinputEngine::enterBusyState(fcitx::InputContext* ic, bool command_mode,
                                  const std::string& preedit_text, bool postprocessing,
                                  bool raw_prev) {
  if (!ic) {
    return;
  }
  rememberInputContext(ic);
  if (status_ic_ && status_ic_ != ic) {
    clearVoicePresentation(status_ic_);
  }
  const auto phase = postprocessing ? Session::Phase::Postprocessing : Session::Phase::Inferring;
  if (!session_) {
    session_.emplace(Session{
        phase, ic, fcitx::Key(), std::chrono::steady_clock::now(), command_mode, {}, raw_prev, {}});
  } else {
    session_->phase = phase;
    session_->ic = ic;
    session_->trigger = fcitx::Key();
    session_->command_mode = command_mode;
    session_->raw_prev = raw_prev;
  }
  status_ic_ = ic;
  if (postprocessing && !session_->raw_prev) {
    updateVoicePresentation(ic, preedit_text);
  } else if (postprocessing && !session_->transcript_text.empty()) {
    const auto transcript =
        LimitPreviewText(command_mode ? CommandTranscriptPreeditText(session_->transcript_text)
                                      : session_->transcript_text,
                         max_streaming_display_width_);
    updateVoicePresentation(ic, command_mode ? ApplyingCommandPreeditText() : preedit_text,
                            transcript);
  } else {
    updateVoicePresentation(ic, ComposeLivePreedit(session_->transcript_text, preedit_text,
                                                   max_streaming_display_width_));
  }
  ensureStatusSync();
}

void VinputEngine::finishFrontendSession(fcitx::InputContext* fallback_ic) {
  auto* ic = session_ ? session_->ic : (fallback_ic ? fallback_ic : status_ic_);
  session_.reset();
  polled_idle_since_.reset();
  command_selected_text_.clear();
  if (status_ic_ == ic) {
    status_ic_ = nullptr;
  }
  if (ic) {
    clearVoicePresentation(ic);
  }
  stopStatusSyncIfIdle();
}

std::string VinputEngine::queryDaemonStatus() const {
  if (!bus_ || !daemonSyncAllowed()) {
    return {};
  }

  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface, kMethodGetStatus);
  auto reply = msg.call(vinput::runtime::kDbusCallTimeoutUsec);
  if (!reply || reply.isError()) {
    vinput::debug::Log("daemon status query failed reply=%s error=%s\n",
                       reply ? reply.errorName().c_str() : "(null)",
                       reply ? reply.errorMessage().c_str() : "(no reply)");
    const_cast<VinputEngine*>(this)->noteDaemonSyncFailure();
    return {};
  }
  const_cast<VinputEngine*>(this)->clearDaemonSyncFailure();

  std::string status;
  reply >> status;
  const_cast<VinputEngine*>(this)->last_known_daemon_status_ = status;
  return status;
}

bool VinputEngine::callReloadAsrBackend(std::string* error) {
  if (!bus_ || !daemonSyncAllowed()) {
    if (error) {
      *error = bus_ ? _("Daemon access is temporarily throttled.") : "D-Bus is unavailable.";
    }
    noteDaemonSyncFailure();
    return false;
  }

  auto msg = bus_->createMethodCall(kBusName, kObjectPath, kInterface, kMethodReloadAsrBackend);
  auto reply = msg.call(vinput::runtime::kDbusCallTimeoutUsec);
  if (!reply) {
    vinput::debug::Log("ASR backend reload RPC failed: no reply\n");
    noteDaemonSyncFailure();
    if (error) {
      *error = _("Failed to contact vinput-daemon.");
    }
    return false;
  }

  if (reply.isError()) {
    vinput::debug::Log("ASR backend reload RPC failed reply=%s error=%s\n",
                       reply.errorName().c_str(), reply.errorMessage().c_str());
    noteDaemonSyncFailure();
    if (error) {
      *error = reply.errorMessage();
      if (error->empty()) {
        *error = _("Failed to reload ASR backend.");
      }
    }
    return false;
  }
  clearDaemonSyncFailure();

  if (error) {
    error->clear();
  }
  return true;
}

bool VinputEngine::callStartAdapter(const std::string& adapter_id, std::string* error) {
  if (bus_ == nullptr || !daemonSyncAllowed()) {
    if (error != nullptr) {
      *error = (bus_ != nullptr) ? _("Daemon access is temporarily throttled.")
                                 : "D-Bus is unavailable.";
    }
    noteDaemonSyncFailure();
    return false;
  }

  auto msg =
      bus_->createMethodCall(kBusName, kObjectPath, kInterface, vinput::dbus::kMethodStartAdapter);
  msg << adapter_id;
  auto reply = msg.call(vinput::runtime::kDbusCallTimeoutUsec);
  if (!reply) {
    noteDaemonSyncFailure();
    if (error != nullptr) {
      *error = _("Failed to contact vinput-daemon.");
    }
    return false;
  }

  if (reply.isError()) {
    noteDaemonSyncFailure();
    if (error != nullptr) {
      *error = reply.errorMessage();
      if (error->empty()) {
        *error = _("Failed to start adapter.");
      }
    }
    return false;
  }
  clearDaemonSyncFailure();
  return true;
}

bool VinputEngine::callStopAdapter(const std::string& adapter_id, std::string* error) {
  if (bus_ == nullptr || !daemonSyncAllowed()) {
    if (error != nullptr) {
      *error = (bus_ != nullptr) ? _("Daemon access is temporarily throttled.")
                                 : "D-Bus is unavailable.";
    }
    noteDaemonSyncFailure();
    return false;
  }

  auto msg =
      bus_->createMethodCall(kBusName, kObjectPath, kInterface, vinput::dbus::kMethodStopAdapter);
  msg << adapter_id;
  auto reply = msg.call(vinput::runtime::kDbusCallTimeoutUsec);
  if (!reply) {
    noteDaemonSyncFailure();
    if (error != nullptr) {
      *error = _("Failed to contact vinput-daemon.");
    }
    return false;
  }

  if (reply.isError()) {
    noteDaemonSyncFailure();
    if (error != nullptr) {
      *error = reply.errorMessage();
      if (error->empty()) {
        *error = _("Failed to stop adapter.");
      }
    }
    return false;
  }
  clearDaemonSyncFailure();
  return true;
}

bool VinputEngine::daemonSyncAllowed() const {
  return std::chrono::steady_clock::now() >= daemon_sync_blocked_until_;
}

void VinputEngine::noteDaemonSyncFailure() {
  daemon_sync_blocked_until_ =
      std::chrono::steady_clock::now() + vinput::runtime::kDaemonFailureCooldown;
  vinput::debug::Log("daemon sync temporarily throttled for %lld ms\n",
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                vinput::runtime::kDaemonFailureCooldown)
                                                .count()));
}

void VinputEngine::clearDaemonSyncFailure() {
  daemon_sync_blocked_until_ = std::chrono::steady_clock::time_point{};
}

void VinputEngine::syncFrontendWithDaemonStatus(fcitx::InputContext* fallback_ic,
                                                bool prefer_command_mode) {
  const std::string status = queryDaemonStatus();
  if (status.empty()) {
    return;
  }

  if (status == kStatusIdle && session_) {
    const auto now = std::chrono::steady_clock::now();
    if (!polled_idle_since_) {
      polled_idle_since_ = now;
    }
    if (now - *polled_idle_since_ < kPolledIdleRecoveryDelay) {
      return;
    }
  }
  polled_idle_since_.reset();

  applyDaemonStatusLocally(status, fallback_ic, prefer_command_mode);
}

void VinputEngine::applyDaemonStatusLocally(const std::string& status,
                                            fcitx::InputContext* fallback_ic,
                                            bool prefer_command_mode) {
  if (recording_cancel_requested_) {
    if (status == kStatusIdle && !pending_start_call_slot_ && !pending_cancel_call_slot_) {
      recording_cancel_requested_ = false;
      finishFrontendSession(fallback_ic);
    }
    return;
  }
  if (status == kStatusIdle && !session_ && status_ic_ == nullptr) {
    return;
  }

  auto* ic = resolveFrontendInputContext(fallback_ic);
  if (!ic) {
    return;
  }

  if (status == kStatusRecording) {
    enterRecordingState(ic, session_ ? session_->trigger : fcitx::Key(),
                        session_ ? session_->command_mode : prefer_command_mode);
    return;
  }

  if (status == kStatusInferring) {
    enterBusyState(ic, session_ ? session_->command_mode : prefer_command_mode,
                   InferringPreeditText());
    return;
  }

  if (status == kStatusPostprocessing) {
    const bool command_mode = session_ ? session_->command_mode : prefer_command_mode;
    const bool raw_prev = session_ ? session_->raw_prev : true;
    enterBusyState(ic, command_mode, PostprocessingPreeditText(command_mode, raw_prev), true,
                   raw_prev);
    return;
  }

  finishFrontendSession(ic);
}

void VinputEngine::onRecognitionResult(fcitx::dbus::Message& msg) {
  if (recording_cancel_requested_) {
    return;
  }
  std::string payload_text;
  msg >> payload_text;

  const bool has_session = session_.has_value();
  const bool is_command = has_session && session_->command_mode;
  const std::string command_selected_text = is_command ? command_selected_text_ : std::string{};
  auto* ic = resolveFrontendInputContext();

  if (!ic) {
    return;
  }

  rememberInputContext(ic);

  const auto payload = vinput::result::Parse(payload_text);
  finishFrontendSession(ic);

  if (payload.commitText.empty()) {
    return;
  }

  const vinput::result::Candidate* asr_candidate = nullptr;
  for (const auto& candidate : payload.candidates) {
    if (candidate.source == vinput::result::kSourceAsr) {
      asr_candidate = &candidate;
      break;
    }
  }
  if (!asr_candidate) {
    for (const auto& candidate : payload.candidates) {
      if (candidate.source != vinput::result::kSourceRaw) {
        continue;
      }
      if (!is_command || candidate.text != command_selected_text) {
        asr_candidate = &candidate;
        break;
      }
    }
  }
  if (asr_candidate && !asr_candidate->text.empty()) {
    appendContextEntry(asr_candidate->text, "asr");
  }

  int distinct_llm_candidate_count = 0;
  bool commit_from_llm = false;
  for (const auto& candidate : payload.candidates) {
    if (candidate.source == vinput::result::kSourceLlm) {
      ++distinct_llm_candidate_count;
    }
    if (candidate.source == vinput::result::kSourceLlm && candidate.text == payload.commitText) {
      commit_from_llm = true;
    }
  }

  // Dictation presents every distinct choice: raw is option 1 and the primary LLM rewrite is
  // focused. Command mode keeps its existing behavior because its payload also contains the
  // selected text and spoken command as metadata candidates.
  const bool should_show_result_menu =
      is_command ? distinct_llm_candidate_count > 1 : payload.candidates.size() > 1;
  if (should_show_result_menu) {
    // Save command mode for result menu interaction
    result_is_command_ = is_command;
    showResultMenu(ic, payload);
    return;
  }

  if (commit_from_llm) {
    appendContextEntry(payload.commitText, "llm");
  }
  suppressNextCommitContext(payload.commitText);

  if (is_command) {
    auto& surrounding = ic->surroundingText();
    if (surrounding.isValid() && surrounding.cursor() != surrounding.anchor()) {
      int cursor = surrounding.cursor();
      int anchor = surrounding.anchor();
      int from = std::min(cursor, anchor);
      int len = std::abs(cursor - anchor);
      ic->deleteSurroundingText(from - cursor, len);
    }
  }

  ic->commitString(payload.commitText);
}

void VinputEngine::onRecognitionPartial(fcitx::dbus::Message& msg) {
  if (recording_cancel_requested_) {
    return;
  }
  std::string transcript_text;
  msg >> transcript_text;

  if (!session_ || transcript_text.empty()) {
    return;
  }

  auto* ic = resolveFrontendInputContext();
  if (!ic) {
    return;
  }

  rememberInputContext(ic);
  session_->transcript_text = transcript_text;
  if (session_->phase == Session::Phase::Postprocessing) {
    enterBusyState(ic, session_->command_mode,
                   PostprocessingPreeditText(session_->command_mode, session_->raw_prev), true,
                   session_->raw_prev);
    return;
  }

  updateVoicePresentation(ic, ComposeLivePreedit(transcript_text,
                                                 session_->command_mode ? CommandingPreeditText()
                                                                        : RecordingPreeditText(),
                                                 max_streaming_display_width_));
}

void VinputEngine::onStatusChanged(fcitx::dbus::Message& msg) {
  std::string status;
  msg >> status;
  last_known_daemon_status_ = status;
  polled_idle_since_.reset();

  auto* ic = resolveFrontendInputContext();
  if (ic) {
    rememberInputContext(ic);
  }
  applyDaemonStatusLocally(status);
}

void VinputEngine::onDaemonNotification(fcitx::dbus::Message& msg) {
  std::tuple<std::string, std::string, std::string, std::string> payload;
  msg >> payload;
  auto notification =
      vinput::dbus::MakeErrorInfo(std::move(std::get<0>(payload)), std::move(std::get<1>(payload)),
                                  std::move(std::get<2>(payload)), std::move(std::get<3>(payload)));

  if (notification.empty()) {
    return;
  }

  if (IsErrorLikeNotification(notification) && !recording_cancel_requested_) {
    auto* ic = resolveFrontendInputContext();
    finishFrontendSession(ic);
  }

  showDaemonNotification(notification);
}

void VinputEngine::showDaemonNotification(const vinput::dbus::ErrorInfo& notification) {
  if (notification.empty()) {
    return;
  }

  std::string title = _("Voice Input");
  std::string message = RenderErrorMessage(notification);
  const bool error_like = IsErrorLikeNotification(notification);
  const char* icon = error_like ? "dialog-error" : "dialog-information";
  const int timeout = error_like ? vinput::runtime::kErrorNotificationTimeoutMs
                                 : vinput::runtime::kInfoNotificationTimeoutMs;

  auto* notifications = instance_->addonManager().addon("notifications", true);
  if (notifications) {
    notifications->call<fcitx::INotifications::sendNotification>(
        "fcitx5-vinput", 0, icon, title, message, std::vector<std::string>{}, timeout,
        fcitx::NotificationActionCallback{}, fcitx::NotificationClosedCallback{});
  } else {
    fprintf(stderr, "vinput: %s: %s\n", title.c_str(), message.c_str());
  }
}

void VinputEngine::notifyError(const vinput::dbus::ErrorInfo& error) {
  if (error.empty()) {
    return;
  }

  std::string title = _("Voice Input");
  std::string message = RenderErrorMessage(error);

  auto* notifications = instance_->addonManager().addon("notifications", true);
  if (notifications) {
    notifications->call<fcitx::INotifications::sendNotification>(
        "fcitx5-vinput", 0, "dialog-error", title, message, std::vector<std::string>{},
        vinput::runtime::kErrorNotificationTimeoutMs, fcitx::NotificationActionCallback{},
        fcitx::NotificationClosedCallback{});
  } else {
    fprintf(stderr, "vinput: %s: %s\n", title.c_str(), message.c_str());
  }
}

void VinputEngine::notifyError(const std::string& message) {
  notifyError(vinput::dbus::MakeRawError(message));
}

void VinputEngine::notifyInfo(const std::string& message) {
  if (message.empty()) {
    return;
  }

  std::string title = _("Voice Input");
  auto* notifications = instance_->addonManager().addon("notifications", true);
  if (notifications) {
    notifications->call<fcitx::INotifications::sendNotification>(
        "fcitx5-vinput", 0, "dialog-information", title, message, std::vector<std::string>{},
        vinput::runtime::kInfoNotificationTimeoutMs, fcitx::NotificationActionCallback{},
        fcitx::NotificationClosedCallback{});
  } else {
    fprintf(stderr, "vinput: %s: %s\n", title.c_str(), message.c_str());
  }
}

void VinputEngine::dismissMenusForVoiceActivity() {
  hidePaletteMenu();
  hideResultMenu();
}

void VinputEngine::updateVoicePresentation(fcitx::InputContext* ic, const std::string& preedit_text,
                                           const std::string& aux_down_text) {
  if (!ic)
    return;
  dismissMenusForVoiceActivity();
  fcitx::Text preedit;
  preedit.append(preedit_text);
  fcitx::Text aux_down;
  aux_down.append(aux_down_text);
  ic->inputPanel().setPreedit(preedit);
  ic->inputPanel().setAuxDown(aux_down);
  ic->updatePreedit();
  ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VinputEngine::clearVoicePresentation(fcitx::InputContext* ic) {
  updateVoicePresentation(ic, {});
}
