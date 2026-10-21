#include "core/vinput.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <dbus_public.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx/inputcontext.h>
#include <filesystem>
#include <fstream>
#include <glaze/json/json_t.hpp>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unicodelib.h>
#include <unicodelib_encodings.h>

#include "common/config/core_config.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/dbus/error_info.h"
#include "common/i18n.h"
#include "common/json/json_access.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/sandbox.h"
#include "common/utils/time_utils.h"

#include "core/context_writer.h"
#include "dbus/notifier_dbus_object.h"
#include "fcitx-config/rawconfig.h"
#include "fcitx-utils/capabilityflags.h"
#include "fcitx/addonfactory.h"
#include "fcitx/addoninstance.h"
#include "fcitx/event.h"
#include "fcitx/instance.h"

using namespace vinput::dbus;

namespace {

constexpr auto kContextClock = 1; // POSIX CLOCK_MONOTONIC
constexpr const char* kContextSourceUser = "user";

// Re-encodes `text` through the vendored Unicode codec, dropping ill formed byte
// sequences. Context history is best effort input data that must never break the
// JSONL cache file.
std::string SanitizeUtf8(const std::string& text) {
  if (text.empty()) {
    return text;
  }
  const auto decoded = unicode::utf8::decode(text);
  return unicode::utf8::encode(decoded);
}
constexpr uint64_t kContextFlushDelayUsec = 5ULL * 1000ULL * 1000ULL; // 5 seconds

bool IsCjkAtBoundary(std::string_view left, std::string_view right) noexcept {
  auto decode_first = [](std::string_view s) -> char32_t {
    if (s.empty()) {
      return 0;
    }
    char32_t cp = 0;
    return unicode::utf8::decode_codepoint(s.data(), s.size(), cp) > 0 ? cp : 0;
  };
  auto decode_last = [](std::string_view s) -> char32_t {
    if (s.empty()) {
      return 0;
    }
    std::u32string u32;
    unicode::utf8::decode(s.data(), s.size(), u32);
    return u32.empty() ? 0 : u32.back();
  };
  auto is_cjk = [](char32_t cp) -> bool {
    return unicode::is_unified_ideograph(cp) || unicode::is_ideographic(cp);
  };
  return is_cjk(decode_last(left)) || is_cjk(decode_first(right));
}

bool EndsWithSentenceTerminal(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  std::u32string u32;
  unicode::utf8::decode(text.data(), text.size(), u32);
  if (u32.empty()) {
    return false;
  }
  const char32_t cp = u32.back();
  return unicode::is_sentence_terminal(cp) || cp == U'…' || cp == U'\n';
}

// Auto-install systemd service when running inside a sandbox.
void ensureDaemonServiceInstalled() {
  if (!vinput::sandbox::IsInSandbox()) {
    return;
  }

  const auto missing = vinput::sandbox::MissingSandboxPermissions();
  if (!missing.empty()) {
    FCITX_LOG(Warn) << "vinput: missing sandbox permissions, run override commands:\n"
                    << vinput::sandbox::FormatOverrideCommands(missing);
  }

  const std::filesystem::path dest = vinput::path::DaemonServiceUnitInstallPath();
  if (dest.empty()) {
    return;
  }

  std::error_code ec_exists;
  const bool destExists = std::filesystem::exists(dest, ec_exists);
  if (ec_exists) {
    FCITX_LOG(Error) << "vinput: failed to check existence of " << dest << ": "
                     << ec_exists.message();
    return;
  }
  if (destExists) {
    return;
  }

  const auto src = vinput::path::DaemonServiceUnitTemplatePath();
  std::ifstream src_f(src);
  if (!src_f) {
    FCITX_LOG(Error) << "vinput: service file not found at " << src;
    return;
  }

  std::string content((std::istreambuf_iterator<char>(src_f)), {});
  content = vinput::sandbox::RewriteServiceUnit(content);

  std::string file_error;
  if (!vinput::file::EnsureParentDirectory(dest, &file_error)) {
    FCITX_LOG(Error) << "vinput: failed to prepare systemd user dir: " << file_error;
    return;
  }
  if (auto res = vinput::file::AtomicWriteTextFile(dest, content); !res) {
    FCITX_LOG(Error) << "vinput: failed to write service file to " << dest;
    FCITX_LOG(Error) << "vinput: write error: " << res.error();
    return;
  }
  FCITX_LOG(Info) << "vinput: installed vinput-daemon.service to " << dest;

  auto reload_cmd = vinput::sandbox::WrapHostCommand({"systemctl", "--user", "daemon-reload"});
  std::string cmd;
  for (const auto& arg : reload_cmd) {
    if (!cmd.empty()) {
      cmd += ' ';
    }
    cmd += arg;
  }
  const int ret = system(cmd.c_str());
  if (ret != 0) {
    FCITX_LOG(Error) << "vinput: failed to reload systemd user daemon, return code: " << ret;
  }
}
} // namespace

VinputEngine::VinputEngine(fcitx::Instance* instance) : instance_(instance) {
  vinput::i18n::Init();
  ensureDaemonServiceInstalled();
  initializePaletteRegistry();
  VinputEngine::reloadConfig();
  event_dispatcher_.attach(&instance_->eventLoop());

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::PreInputMethod,
      [this](fcitx::Event& event) { handleKeyEvent(event); }));

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextCreated, fcitx::EventWatcherPhase::PreInputMethod,
      [this](fcitx::Event& event) {
        auto& icEvent = dynamic_cast<fcitx::InputContextEvent&>(event);
        auto* ic = icEvent.inputContext();
        ic->setCapabilityFlags(ic->capabilityFlags() | fcitx::CapabilityFlag::SurroundingText);
        rememberInputContext(ic);
      }));

  eventHandlers_.emplace_back(
      instance_->watchEvent(fcitx::EventType::InputContextDestroyed,
                            fcitx::EventWatcherPhase::PreInputMethod, [this](fcitx::Event& event) {
                              auto& icEvent = dynamic_cast<fcitx::InputContextEvent&>(event);
                              auto* ic = icEvent.inputContext();
                              if (session_ && session_->ic == ic) {
                                if (session_->phase == Session::Phase::PendingStart ||
                                    session_->phase == Session::Phase::Recording ||
                                    session_->phase == Session::Phase::Postprocessing) {
                                  callCancelOperation(false);
                                }
                                session_.reset();
                                polled_idle_since_.reset();
                              }
                              if (pending_start_ic_.get() == ic) {
                                cancelPendingStart();
                              }
                              if (status_ic_ == ic) {
                                status_ic_ = nullptr;
                                stopStatusSyncIfIdle();
                              }
                              if (last_active_ic_ == ic) {
                                last_active_ic_ = nullptr;
                              }
                              if (palette_menu_ic_ == ic) {
                                resetPaletteMenuState();
                              }
                              if (result_menu_ic_ == ic) {
                                resetResultMenuState();
                              }
                              if (context_buffer_ic_ == ic) {
                                flushContextBuffer();
                              }
                            }));

  eventHandlers_.emplace_back(
      instance_->watchEvent(fcitx::EventType::InputContextFocusOut,
                            fcitx::EventWatcherPhase::PreInputMethod, [this](fcitx::Event& event) {
                              if (auto* icEvent = dynamic_cast<fcitx::InputContextEvent*>(&event)) {
                                auto* ic = icEvent->inputContext();
                                if (pending_start_ic_.get() == ic) {
                                  cancelPendingStart();
                                  chord_interrupted_ = true;
                                }
                                held_key_sym_.reset();
                                held_role_ = HotkeyRole::None;
                                chord_interrupted_ = true;
                                if (session_ && session_->ic == ic) {
                                  if (session_->stop_on_release) {
                                    callCancelOperation(false);
                                    finishFrontendSession(ic);
                                    clearVoicePresentation(ic);
                                    chord_interrupted_ = true;
                                  }
                                }
                              }
                            }));

  eventHandlers_.emplace_back(
      instance_->watchEvent(fcitx::EventType::InputContextCommitString,
                            fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event& event) {
                              auto& commitEvent = dynamic_cast<fcitx::CommitStringEvent&>(event);
                              onCommitString(commitEvent.text(), commitEvent.inputContext());
                            }));

  auto* dbus_addon = instance_->addonManager().addon("dbus");
  if (dbus_addon != nullptr) {
    bus_ = dbus_addon->call<fcitx::IDBusModule::bus>();
    const std::weak_ptr<bool> token = lifetime_token_;
    notifier_dbus_ = std::make_unique<VinputNotifierDBusObject>(
        [this, token](const vinput::dbus::ErrorInfo& notification) {
          if (token.expired()) {
            return;
          }
          showDaemonNotification(notification);
        });
    if (!bus_->addObjectVTable(vinput::dbus::kNotifierObjectPath, vinput::dbus::kNotifierInterface,
                               *notifier_dbus_)) {
      FCITX_LOG(Error) << "vinput: failed to register notifier DBus object";
      notifier_dbus_.reset();
    }
    setupDBusWatcher();
  }
}

VinputEngine::~VinputEngine() {
  lifetime_token_.reset();

  flushContextBuffer();
  context_writer_.reset(); // Persist all queued entries before addon teardown.
  context_flush_timer_.reset();
  status_sync_event_.reset();
  status_poll_gate_.Cancel();
  pending_status_call_slot_.reset();
  pending_stop_event_.reset();
  pending_start_event_.reset();

  pending_stop_call_slot_.reset();
  pending_start_call_slot_.reset();

  error_slot_.reset();
  status_slot_.reset();
  partial_slot_.reset();
  result_slot_.reset();

  event_dispatcher_.detach();
  notifier_dbus_.reset();
  bus_ = nullptr;
}

void VinputEngine::reloadConfig() {
  fcitx::readAsIni(config_, kVinputConfigPath);
  applySettings();
}

void VinputEngine::save() {
  fcitx::safeSaveAsIni(config_, kVinputConfigPath);
}

const fcitx::Configuration* VinputEngine::getConfig() const {
  return &config_;
}

void VinputEngine::setConfig(const fcitx::RawConfig& rawConfig) {
  config_.load(rawConfig, true);
  applySettings();
  save();
}

void VinputEngine::applySettings() {
  trigger_keys_ = config_.triggerKeys.value();
  command_keys_ = config_.commandKeys.value();
  menu_keys_ = config_.menuKeys.value();
  page_prev_keys_ = config_.pagePrevKeys.value();
  page_next_keys_ = config_.pageNextKeys.value();
  trigger_mode_ = config_.triggerMode.value();
  max_streaming_display_width_ = config_.maxStreamingDisplayWidth.value();
  reloadSceneConfig();
  reloadPaletteItems();
}

void VinputEngine::reloadSceneConfig() {
  auto core_config = LoadCoreConfig();
  scene_config_.activeSceneId = core_config.scenes.activeScene;
  scene_config_.scenes = core_config.scenes.definitions;
  active_scene_id_ = scene_config_.activeSceneId;

  int max_cl = 0;
  for (const auto& s : scene_config_.scenes) {
    max_cl = std::max(max_cl, s.context_lines);
  }
  max_context_lines_ = max_cl;
  if (max_context_lines_ <= 0) {
    context_buffer_text_.clear();
    context_buffer_ic_ = nullptr;
    if (context_flush_timer_) {
      context_flush_timer_->setEnabled(false);
    }
  }
}

void VinputEngine::rememberInputContext(fcitx::InputContext* ic) {
  if (ic == nullptr) {
    return;
  }
  last_active_ic_ = ic;
}

fcitx::InputContext*
VinputEngine::resolveFrontendInputContext(fcitx::InputContext* fallback_ic) const {
  if (session_) {
    return session_->ic;
  }
  if (status_ic_ != nullptr) {
    return status_ic_;
  }
  if (fallback_ic != nullptr) {
    return fallback_ic;
  }
  return last_active_ic_;
}

void VinputEngine::appendContextEntry(const std::string& text, const char* source) {
  if (max_context_lines_ <= 0 || text.empty()) {
    return;
  }
  // Flush user buffer before writing non-user entries to preserve ordering.
  if (source != nullptr && std::string_view(source) != kContextSourceUser &&
      !context_buffer_text_.empty()) {
    flushContextBuffer();
  }
  // JSONL keeps one object per line. Text coming from ASR may carry ill formed
  // UTF-8, so it is round tripped through the vendored Unicode codec to keep the
  // cache file parseable.
  const glz::json_t entry = {
      {"text", SanitizeUtf8(text)},
      {"source",
       SanitizeUtf8(source != nullptr ? std::string(source) : std::string(kContextSourceUser))},
      {"timestamp", static_cast<double>(vinput::time::CurrentUnixTimestampSeconds())},
  };

  if (!context_writer_) {
    context_writer_ =
        std::make_unique<vinput::addon::AsyncContextWriter>(vinput::path::ContextCachePath());
  }
  if (!context_writer_->Enqueue(vinput::json::ToText(entry), max_context_lines_)) {
    // The queue is deliberately bounded. Do not silently discard context
    // when the disk cannot keep up with incoming input history.
    FCITX_LOG(Error) << "vinput: context persistence backlog exceeded its capacity";
  }
}

void VinputEngine::suppressNextCommitContext(const std::string& text) {
  if (text.empty()) {
    pending_suppressed_commit_text_.reset();
    return;
  }
  pending_suppressed_commit_text_ = text;
}

void VinputEngine::flushContextBuffer() {
  context_buffer_ic_ = nullptr;
  if (context_buffer_text_.empty()) {
    return;
  }
  appendContextEntry(context_buffer_text_, kContextSourceUser);
  context_buffer_text_.clear();
  if (context_flush_timer_) {
    context_flush_timer_->setEnabled(false);
  }
}

void VinputEngine::accumulateContextBuffer(const std::string& text, fcitx::InputContext* ic) {
  // IC changed — flush old buffer first.
  if (ic != context_buffer_ic_ && !context_buffer_text_.empty()) {
    flushContextBuffer();
  }
  context_buffer_ic_ = ic;

  // Language-aware joining.
  if (!context_buffer_text_.empty()) {
    if (!IsCjkAtBoundary(context_buffer_text_, text) && context_buffer_text_.back() != ' ') {
      context_buffer_text_ += ' ';
    }
  }
  context_buffer_text_ += text;

  // Sentence-ending punctuation → flush immediately.
  if (EndsWithSentenceTerminal(context_buffer_text_)) {
    flushContextBuffer();
    return;
  }

  // Reset 5s inactivity timer.
  const auto fire_at = fcitx::now(kContextClock) + kContextFlushDelayUsec;
  if (!context_flush_timer_) {
    const std::weak_ptr<bool> token = lifetime_token_;
    context_flush_timer_ = instance_->eventLoop().addTimeEvent(
        kContextClock, fire_at, 0, [this, token](fcitx::EventSourceTime*, uint64_t) {
          if (token.expired()) {
            return false;
          }
          flushContextBuffer();
          return false;
        });
    context_flush_timer_->setOneShot();
  } else {
    context_flush_timer_->setTime(fire_at);
    context_flush_timer_->setEnabled(true);
  }
}

void VinputEngine::onCommitString(const std::string& text, fcitx::InputContext* ic) {
  if (max_context_lines_ <= 0 || text.empty()) {
    pending_suppressed_commit_text_.reset();
    return;
  }
  if (pending_suppressed_commit_text_) {
    const bool suppressed = *pending_suppressed_commit_text_ == text;
    pending_suppressed_commit_text_.reset();
    if (suppressed) {
      return;
    }
  }
  accumulateContextBuffer(text, ic);
}

bool VinputEngine::isRecordingActive() const {
  return session_.has_value() && (session_->phase == Session::Phase::Recording ||
                                  session_->phase == Session::Phase::PendingStart);
}

bool VinputEngine::isHoldRecording() const {
  return session_.has_value() && session_->stop_on_release;
}

bool VinputEngine::isPendingStart() const {
  return held_role_ == HotkeyRole::Trigger || held_role_ == HotkeyRole::Command;
}

fcitx::AddonInstance* VinputEngineFactory::create(fcitx::AddonManager* manager) {
  vinput::i18n::Init();
  return new VinputEngine(manager->instance());
}

#ifdef VINPUT_FCITX5_CORE_HAVE_ADDON_FACTORY_V2
FCITX_ADDON_FACTORY_V2(vinput, VinputEngineFactory);
#else
FCITX_ADDON_FACTORY(VinputEngineFactory);
#endif
