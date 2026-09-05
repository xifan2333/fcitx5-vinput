#include "core/vinput.h"

#include <chrono>
#include <cstdint>
#include <dbus_public.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontext.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/file_utils.h"
#include "common/utils/path_utils.h"
#include "common/utils/sandbox.h"
#include "common/utils/string_utils.h"

#include "dbus/notifier_dbus_object.h"

using namespace vinput::dbus;

namespace {

constexpr const char* kContextSourceUser = "user";
constexpr uint64_t kContextFlushDelayUsec = 5 * 1000 * 1000; // 5 seconds

int64_t CurrentUnixTimestamp() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Auto-install systemd service when running inside a sandbox.
void ensureDaemonServiceInstalled() {
  if (!vinput::sandbox::IsInSandbox())
    return;

  const std::filesystem::path dest = vinput::path::DaemonServiceUnitInstallPath();
  if (dest.empty()) {
    return;
  }

  std::error_code ec_exists;
  bool destExists = std::filesystem::exists(dest, ec_exists);
  if (ec_exists) {
    FCITX_LOG(Error) << "vinput: failed to check existence of " << dest << ": "
                     << ec_exists.message();
    return;
  }
  if (destExists)
    return;

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
  if (!vinput::file::AtomicWriteTextFile(dest, content, &file_error)) {
    FCITX_LOG(Error) << "vinput: failed to write service file to " << dest;
    FCITX_LOG(Error) << "vinput: write error: " << file_error;
    return;
  }
  FCITX_LOG(Info) << "vinput: installed vinput-daemon.service to " << dest;

  auto reload_cmd = vinput::sandbox::WrapHostCommand({"systemctl", "--user", "daemon-reload"});
  std::string cmd;
  for (const auto& arg : reload_cmd) {
    if (!cmd.empty())
      cmd += ' ';
    cmd += arg;
  }
  int ret = system(cmd.c_str());
  if (ret != 0) {
    FCITX_LOG(Error) << "vinput: failed to reload systemd user daemon, return code: " << ret;
  }
}
} // namespace

VinputEngine::VinputEngine(fcitx::Instance* instance) : instance_(instance) {
  vinput::i18n::Init();
  ensureDaemonServiceInstalled();
  initializePaletteRegistry();
  reloadConfig();
  event_dispatcher_.attach(&instance_->eventLoop());

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::PreInputMethod,
      [this](fcitx::Event& event) { handleKeyEvent(event); }));

  eventHandlers_.emplace_back(instance_->watchEvent(
      fcitx::EventType::InputContextCreated, fcitx::EventWatcherPhase::PreInputMethod,
      [this](fcitx::Event& event) {
        auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);
        auto* ic = icEvent.inputContext();
        ic->setCapabilityFlags(ic->capabilityFlags() | fcitx::CapabilityFlag::SurroundingText);
        rememberInputContext(ic);
      }));

  eventHandlers_.emplace_back(
      instance_->watchEvent(fcitx::EventType::InputContextDestroyed,
                            fcitx::EventWatcherPhase::PreInputMethod, [this](fcitx::Event& event) {
                              auto& icEvent = static_cast<fcitx::InputContextEvent&>(event);
                              auto* ic = icEvent.inputContext();
                              if (session_ && session_->ic == ic) {
                                session_.reset();
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
      instance_->watchEvent(fcitx::EventType::InputContextCommitString,
                            fcitx::EventWatcherPhase::PostInputMethod, [this](fcitx::Event& event) {
                              auto& commitEvent = static_cast<fcitx::CommitStringEvent&>(event);
                              onCommitString(commitEvent.text(), commitEvent.inputContext());
                            }));

  auto* dbus_addon = instance_->addonManager().addon("dbus");
  if (dbus_addon) {
    bus_ = dbus_addon->call<fcitx::IDBusModule::bus>();
    notifier_dbus_ = std::make_unique<VinputNotifierDBusObject>(
        [this](const vinput::dbus::ErrorInfo& notification) {
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
  flushContextBuffer();
  context_flush_timer_.reset();
  status_sync_event_.reset();
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
  lifetime_token_.reset();
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
    if (s.context_lines > max_cl) {
      max_cl = s.context_lines;
    }
  }
  max_context_lines_ = max_cl;
}

void VinputEngine::rememberInputContext(fcitx::InputContext* ic) {
  if (!ic) {
    return;
  }
  last_active_ic_ = ic;
}

fcitx::InputContext*
VinputEngine::resolveFrontendInputContext(fcitx::InputContext* fallback_ic) const {
  if (session_) {
    return session_->ic;
  }
  if (status_ic_) {
    return status_ic_;
  }
  if (fallback_ic) {
    return fallback_ic;
  }
  return last_active_ic_;
}

void VinputEngine::appendContextEntry(const std::string& text, const char* source) {
  if (text.empty()) {
    return;
  }
  // Flush user buffer before writing non-user entries to preserve ordering.
  if (source && std::string_view(source) != kContextSourceUser && !context_buffer_text_.empty()) {
    flushContextBuffer();
  }
  const auto path = vinput::path::ContextCachePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return;
  }

  nlohmann::json entry;
  entry["text"] = text;
  entry["source"] = source ? source : kContextSourceUser;
  entry["timestamp"] = CurrentUnixTimestamp();
  {
    std::ofstream ofs(path, std::ios::app);
    if (ofs) {
      ofs << entry.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    }
  }

  constexpr int kTruncateInterval = 100;
  const int keep_lines = max_context_lines_ > 0 ? max_context_lines_ : 0;
  if (keep_lines > 0 && ++commit_write_count_ >= kTruncateInterval) {
    commit_write_count_ = 0;
    std::ifstream ifs(path);
    if (!ifs) {
      return;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
      if (!line.empty()) {
        lines.push_back(std::move(line));
      }
    }
    ifs.close();
    if (static_cast<int>(lines.size()) > keep_lines) {
      const auto start = lines.end() - keep_lines;
      const auto tmp = path.string() + ".tmp";
      std::ofstream ofs_tmp(tmp, std::ios::trunc);
      if (ofs_tmp) {
        for (auto it = start; it != lines.end(); ++it) {
          ofs_tmp << *it << '\n';
        }
        ofs_tmp.close();
        std::filesystem::rename(tmp, path, ec);
      }
    }
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
    const bool cjk_boundary =
        vinput::str::IsCjkCodepoint(vinput::str::LastUtf8Codepoint(context_buffer_text_)) ||
        vinput::str::IsCjkCodepoint(vinput::str::FirstUtf8Codepoint(text));
    if (!cjk_boundary && context_buffer_text_.back() != ' ') {
      context_buffer_text_ += ' ';
    }
  }
  context_buffer_text_ += text;

  // Sentence-ending punctuation → flush immediately.
  const uint32_t last_cp = vinput::str::LastUtf8Codepoint(context_buffer_text_);
  if (vinput::str::IsSentenceEndingPunctuation(last_cp)) {
    flushContextBuffer();
    return;
  }

  // Reset 5s inactivity timer.
  const auto fire_at = fcitx::now(CLOCK_MONOTONIC) + kContextFlushDelayUsec;
  if (!context_flush_timer_) {
    context_flush_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fire_at, 0, [this](fcitx::EventSourceTime*, uint64_t) {
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
  if (text.empty()) {
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

fcitx::AddonInstance* VinputEngineFactory::create(fcitx::AddonManager* manager) {
  vinput::i18n::Init();
  return new VinputEngine(manager->instance());
}

#ifdef VINPUT_FCITX5_CORE_HAVE_ADDON_FACTORY_V2
FCITX_ADDON_FACTORY_V2(vinput, VinputEngineFactory);
#else
FCITX_ADDON_FACTORY(VinputEngineFactory);
#endif
