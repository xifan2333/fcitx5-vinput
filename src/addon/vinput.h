#pragma once

#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

#include <memory>
#include <string>
#include <vector>

#include "common/postprocess_scene.h"
#include "common/recognition_result.h"
#include "common/vinput_config.h"

class VinputEngine : public fcitx::AddonInstance {
public:
  VinputEngine(fcitx::Instance *instance);
  ~VinputEngine() override;
  void selectScene(std::size_t index, fcitx::InputContext *ic);
  void selectResultCandidate(std::size_t index, fcitx::InputContext *ic);

  void reloadConfig() override;
  void save() override;
  const fcitx::Configuration *getConfig() const override;
  void setConfig(const fcitx::RawConfig &config) override;

private:
  void applySettings();
  void reloadSceneConfig();
  void rebuildUiConfig() const;
  void handleKeyEvent(fcitx::Event &event);
  void showSceneMenu(fcitx::InputContext *ic);
  void hideSceneMenu();
  bool handleSceneMenuKeyEvent(fcitx::KeyEvent &keyEvent);
  void showResultMenu(fcitx::InputContext *ic,
                      const vinput::result::Payload &payload);
  void hideResultMenu();
  bool handleResultMenuKeyEvent(fcitx::KeyEvent &keyEvent);
  bool isReleaseOfActiveTrigger(const fcitx::Key &key) const;
  void cancelPendingStop();
  void scheduleStopRecording();
  void finishStopRecording();
  void restartDaemon();
  void setupDBusWatcher();
  void callStartRecording();
  void callStartCommandRecording(const std::string &selected_text);
  void callStopRecording(const std::string &scene_id);
  void onRecognitionResult(fcitx::dbus::Message &msg);
  void onStatusChanged(fcitx::dbus::Message &msg);
  void updatePreedit(fcitx::InputContext *ic, const std::string &text);
  void clearPreedit(fcitx::InputContext *ic);

  fcitx::Instance *instance_;
  std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>
      key_event_handler_;
  fcitx::dbus::Bus *bus_ = nullptr;
  std::unique_ptr<fcitx::dbus::Slot> result_slot_;
  std::unique_ptr<fcitx::dbus::Slot> status_slot_;
  bool recording_ = false;
  bool command_mode_ = false;
  fcitx::InputContext *active_ic_ = nullptr;
  fcitx::InputContext *scene_menu_ic_ = nullptr;
  fcitx::InputContext *result_menu_ic_ = nullptr;
  fcitx::KeyList trigger_keys_{fcitx::Key(FcitxKey_Control_R)};
  fcitx::KeyList command_keys_{fcitx::Key(FcitxKey_F10)};
  fcitx::KeyList scene_menu_key_{fcitx::Key(FcitxKey_F9)};
  fcitx::KeyList page_prev_keys_{
      fcitx::Key(FcitxKey_Page_Up),
      fcitx::Key(FcitxKey_KP_Page_Up),
  };
  fcitx::KeyList page_next_keys_{
      fcitx::Key(FcitxKey_Page_Down),
      fcitx::Key(FcitxKey_KP_Page_Down),
  };
  fcitx::Key active_trigger_;
  bool scene_menu_visible_ = false;
  bool result_menu_visible_ = false;
  std::string active_scene_id_;
  vinput::scene::Config scene_config_;
  std::vector<vinput::result::Candidate> result_candidates_;
  std::unique_ptr<fcitx::EventSourceTime> pending_stop_event_;
  VinputSettings settings_;
  mutable std::unique_ptr<VinputConfig> ui_config_;
};

class VinputEngineFactory : public fcitx::AddonFactory {
  fcitx::AddonInstance *create(fcitx::AddonManager *manager) override;
};
