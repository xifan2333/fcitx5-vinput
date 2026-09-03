#pragma once

#include <atomic>
#include <chrono>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/dbus/error_info.h"
#include "common/scene/postprocess_scene.h"

class VinputNotifierDBusObject;

struct AsrMenuItem {
  std::string provider_id;
  std::string model_id;
  std::string display_label;
  std::string search_text;
  bool active;
};

class VinputEngine : public fcitx::AddonInstance {
public:
  VinputEngine(fcitx::Instance* instance);
  ~VinputEngine() override;
  void selectScene(std::size_t index, fcitx::InputContext* ic);
  void selectAsrItem(std::size_t index, fcitx::InputContext* ic);
  void selectResultCandidate(std::size_t index, fcitx::InputContext* ic);

  void reloadConfig() override;
  void save() override;
  const fcitx::Configuration* getConfig() const override;
  void setConfig(const fcitx::RawConfig& config) override;

private:
  void applySettings();
  void reloadSceneConfig();
  void handleKeyEvent(fcitx::Event& event);
  void showSceneMenu(fcitx::InputContext* ic);
  void hideSceneMenu();
  void resetSceneMenuState();
  bool handleSceneMenuKeyEvent(fcitx::KeyEvent& keyEvent);
  void rebuildSceneMenu(fcitx::InputContext* ic);
  void showAsrMenu(fcitx::InputContext* ic);
  void hideAsrMenu();
  void resetAsrMenuState();
  bool handleAsrMenuKeyEvent(fcitx::KeyEvent& keyEvent);
  void reloadAsrMenuItems();
  void rebuildAsrMenu(fcitx::InputContext* ic);
  void requestAsrMenuStateRefresh(fcitx::InputContext* ic);
  void showResultMenu(fcitx::InputContext* ic, const vinput::result::Payload& payload);
  void hideResultMenu();
  void resetResultMenuState();
  bool handleResultMenuKeyEvent(fcitx::KeyEvent& keyEvent);
  bool isReleaseOfActiveTrigger(const fcitx::Key& key) const;
  void cancelPendingStop();
  void cancelPendingStart();
  void scheduleStopRecording();
  void finishStopRecording();
  void setupDBusWatcher();
  bool callStartRecording();
  bool callStartCommandRecording(const std::string& selected_text);
  bool callStopRecording(const std::string& scene_id);
  bool callReloadAsrBackend(std::string* error = nullptr);
  void onRecognitionResult(fcitx::dbus::Message& msg);
  void onRecognitionPartial(fcitx::dbus::Message& msg);
  void onStatusChanged(fcitx::dbus::Message& msg);
  void onDaemonNotification(fcitx::dbus::Message& msg);
  void showDaemonNotification(const vinput::dbus::ErrorInfo& notification);
  void notifyError(const vinput::dbus::ErrorInfo& error);
  void notifyError(const std::string& message);
  void notifyInfo(const std::string& message);
  std::string queryDaemonStatus() const;
  bool daemonSyncAllowed() const;
  void noteDaemonSyncFailure();
  void clearDaemonSyncFailure();
  void applyDaemonStatusLocally(const std::string& status,
                                fcitx::InputContext* fallback_ic = nullptr,
                                bool prefer_command_mode = false);
  void ensureStatusSync();
  void stopStatusSyncIfIdle();
  void enterPendingStartState(fcitx::InputContext* ic, const fcitx::Key& trigger,
                              bool command_mode);
  void enterRecordingState(fcitx::InputContext* ic, const fcitx::Key& trigger, bool command_mode);
  void enterBusyState(fcitx::InputContext* ic, bool command_mode, const std::string& preedit_text);
  void finishFrontendSession(fcitx::InputContext* fallback_ic = nullptr);
  void syncFrontendWithDaemonStatus(fcitx::InputContext* fallback_ic = nullptr,
                                    bool prefer_command_mode = false);
  void rememberInputContext(fcitx::InputContext* ic);
  fcitx::InputContext*
  resolveFrontendInputContext(fcitx::InputContext* fallback_ic = nullptr) const;
  void updatePreedit(fcitx::InputContext* ic, const std::string& text);
  void clearPreedit(fcitx::InputContext* ic);
  void appendContextEntry(const std::string& text, const char* source);
  void flushContextBuffer();
  void accumulateContextBuffer(const std::string& text, fcitx::InputContext* ic);
  void suppressNextCommitContext(const std::string& text);
  void onCommitString(const std::string& text, fcitx::InputContext* ic);

  fcitx::Instance* instance_;
  fcitx::EventDispatcher event_dispatcher_;
  std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> eventHandlers_;
  fcitx::dbus::Bus* bus_ = nullptr;
  std::unique_ptr<VinputNotifierDBusObject> notifier_dbus_;
  std::unique_ptr<fcitx::dbus::Slot> result_slot_;
  std::unique_ptr<fcitx::dbus::Slot> partial_slot_;
  std::unique_ptr<fcitx::dbus::Slot> status_slot_;
  std::unique_ptr<fcitx::dbus::Slot> error_slot_;
  std::unique_ptr<fcitx::dbus::Slot> pending_start_call_slot_;
  std::unique_ptr<fcitx::dbus::Slot> pending_stop_call_slot_;
  struct Session {
    enum class Phase { PendingStart, Recording, Busy };
    Phase phase;
    fcitx::InputContext* ic;
    fcitx::Key trigger;
    std::chrono::steady_clock::time_point press_time;
    bool command_mode = false;
    bool trigger_released = false;
    std::string partial_text;
  };
  std::optional<Session> session_;
  fcitx::InputContext* status_ic_ = nullptr;
  fcitx::InputContext* last_active_ic_ = nullptr;
  fcitx::InputContext* scene_menu_ic_ = nullptr;
  fcitx::InputContext* asr_menu_ic_ = nullptr;
  fcitx::InputContext* result_menu_ic_ = nullptr;
  fcitx::KeyList trigger_keys_{fcitx::Key(FcitxKey_Control_R)};
  fcitx::KeyList command_keys_{fcitx::Key(FcitxKey_F10)};
  fcitx::KeyList scene_menu_key_{fcitx::Key(FcitxKey_F9)};
  fcitx::KeyList asr_menu_key_{fcitx::Key(FcitxKey_F8)};
  fcitx::KeyList page_prev_keys_{
      fcitx::Key(FcitxKey_Page_Up),
      fcitx::Key(FcitxKey_KP_Page_Up),
  };
  fcitx::KeyList page_next_keys_{
      fcitx::Key(FcitxKey_Page_Down),
      fcitx::Key(FcitxKey_KP_Page_Down),
  };
  bool scene_menu_visible_ = false;
  bool asr_menu_visible_ = false;
  bool result_menu_visible_ = false;
  std::string active_scene_id_;
  vinput::scene::Config scene_config_;
  std::vector<std::size_t> scene_menu_filtered_indices_;
  std::string scene_menu_query_;
  bool scene_menu_filter_mode_ = false;
  std::vector<AsrMenuItem> asr_menu_items_;
  std::vector<std::size_t> asr_menu_filtered_indices_;
  std::string asr_menu_query_;
  bool asr_menu_filter_mode_ = false;
  std::optional<std::string> pending_suppressed_commit_text_;
  std::string command_selected_text_;
  std::string context_buffer_text_;
  fcitx::InputContext* context_buffer_ic_ = nullptr;
  std::unique_ptr<fcitx::EventSourceTime> context_flush_timer_;
  int max_context_lines_ = 0;
  std::vector<vinput::result::Candidate> result_candidates_;
  bool result_is_command_ = false;
  std::chrono::steady_clock::time_point last_trigger_time_;
  std::chrono::steady_clock::time_point daemon_sync_blocked_until_{};
  std::string last_known_daemon_status_;
  vinput::dbus::AsrBackendState cached_asr_backend_state_;
  bool has_cached_asr_backend_state_ = false;
  std::atomic<uint64_t> asr_state_refresh_seq_{0};
  std::shared_ptr<bool> lifetime_token_ = std::make_shared<bool>(true);
  std::unique_ptr<fcitx::EventSourceTime> pending_stop_event_;
  std::unique_ptr<fcitx::EventSourceTime> pending_start_event_;
  std::unique_ptr<fcitx::EventSourceTime> status_sync_event_;
  VinputConfig config_;
  int commit_write_count_ = 0;
  int max_streaming_display_width_{60};
  TriggerMode trigger_mode_{TriggerMode::Both};
};

class VinputEngineFactory : public fcitx::AddonFactory {
  fcitx::AddonInstance* create(fcitx::AddonManager* manager) override;
};
