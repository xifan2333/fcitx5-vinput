#include "vinput_config.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/standardpath.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

fcitx::ListConstrain<fcitx::KeyConstrain> TriggerKeyListConstrain() {
  return fcitx::KeyListConstrain(fcitx::KeyConstrainFlags{
      fcitx::KeyConstrainFlag::AllowModifierOnly,
      fcitx::KeyConstrainFlag::AllowModifierLess,
  });
}

fcitx::ListConstrain<fcitx::KeyConstrain> SceneMenuKeyListConstrain() {
  return fcitx::KeyListConstrain(fcitx::KeyConstrainFlags{
      fcitx::KeyConstrainFlag::AllowModifierOnly,
      fcitx::KeyConstrainFlag::AllowModifierLess,
  });
}

std::string UserPkgConfigPath(std::string_view relative_path) {
  return (std::filesystem::path(fcitx::StandardPath::global().userDirectory(
              fcitx::StandardPath::Type::PkgConfig)) /
          std::string(relative_path))
      .string();
}

std::string TriggerKeyLabel(bool chinese_ui) {
  return chinese_ui ? "触发键" : "Trigger Key";
}

std::string TriggerKeyTooltip(bool chinese_ui) {
  return chinese_ui
             ? "可配置多个触发键。支持普通按键、修饰键，以及带修饰键的组合键。"
               "配置保存在 " +
                   UserPkgConfigPath(kVinputConfigPath) + "。"
             : "Press and hold this key to record. Release it to start "
               "recognition. Supports regular keys, modifier keys, and "
               "modified key combinations. You can configure multiple "
               "trigger keys. The config is stored at " +
                   UserPkgConfigPath(kVinputConfigPath) + ".";
}

std::string SceneMenuKeyLabel(bool chinese_ui) {
  return chinese_ui ? "后处理选单键" : "Postprocess Menu Keys";
}

std::string SceneMenuKeyTooltip(bool chinese_ui) {
  return chinese_ui ? "可配置多个按键，用于打开后处理选单。默认是 F9。"
                    : "Configure one or more keys to open the postprocess "
                      "menu. The default is F9.";
}

std::string PagePrevKeysLabel(bool chinese_ui) {
  return chinese_ui ? "上一页键" : "Previous Page Keys";
}

std::string PagePrevKeysTooltip(bool chinese_ui) {
  return chinese_ui ? "用于后处理选单和后处理候选菜单翻到上一页。默认是 Page "
                      "Up 和小键盘 Page Up。"
                    : "Keys for paging to the previous page in the postprocess "
                      "menu and "
                      "the postprocess candidate menu. Defaults to Page Up and "
                      "keypad Page Up.";
}

std::string PageNextKeysLabel(bool chinese_ui) {
  return chinese_ui ? "下一页键" : "Next Page Keys";
}

std::string PageNextKeysTooltip(bool chinese_ui) {
  return chinese_ui
             ? "用于后处理选单和后处理候选菜单翻到下一页。默认是 Page Down "
               "和小键盘 Page Down。"
             : "Keys for paging to the next page in the postprocess menu "
               "and the "
               "postprocess candidate menu. Defaults to Page Down and "
               "keypad Page Down.";
}

} // namespace

VinputConfig::VinputConfig(const VinputSettings &settings, bool chinese_ui)
    : triggerKey(this, "TriggerKey", TriggerKeyLabel(chinese_ui),
                 settings.triggerKeys, TriggerKeyListConstrain(), {},
                 fcitx::ToolTipAnnotation(TriggerKeyTooltip(chinese_ui))),
      sceneMenuKey(this, "SceneMenuKey", SceneMenuKeyLabel(chinese_ui),
                   settings.sceneMenuKey, SceneMenuKeyListConstrain(), {},
                   fcitx::ToolTipAnnotation(SceneMenuKeyTooltip(chinese_ui))),
      pagePrevKeys(this, "PagePrevKeys", PagePrevKeysLabel(chinese_ui),
                   settings.pagePrevKeys, TriggerKeyListConstrain(), {},
                   fcitx::ToolTipAnnotation(PagePrevKeysTooltip(chinese_ui))),
      pageNextKeys(this, "PageNextKeys", PageNextKeysLabel(chinese_ui),
                   settings.pageNextKeys, TriggerKeyListConstrain(), {},
                   fcitx::ToolTipAnnotation(PageNextKeysTooltip(chinese_ui))),
      modelManager(this, "ModelManager",
                   chinese_ui ? "配置与管理 vinput (启动 CLI)"
                              : "Manage Vinput (Launch CLI)",
                   "vinput") {}

VinputSettings VinputConfig::settings() const {
  VinputSettings settings;
  settings.triggerKeys = triggerKey.value();
  settings.sceneMenuKey = sceneMenuKey.value();
  settings.pagePrevKeys = pagePrevKeys.value();
  settings.pageNextKeys = pageNextKeys.value();
  return settings;
}

bool UseChineseUi() {
  const char *locale = std::getenv("LC_ALL");
  if (!locale || locale[0] == '\0') {
    locale = std::getenv("LC_MESSAGES");
  }
  if (!locale || locale[0] == '\0') {
    locale = std::getenv("LANG");
  }
  return locale && std::strncmp(locale, "zh", 2) == 0;
}

VinputSettings LoadVinputSettings() {
  VinputConfig config(VinputSettings{}, false);
  fcitx::readAsIni(config, fcitx::StandardPath::Type::PkgConfig,
                   kVinputConfigPath);
  return config.settings();
}

bool SaveVinputSettings(const VinputSettings &settings) {
  VinputConfig config(settings, false);
  return fcitx::safeSaveAsIni(config, fcitx::StandardPath::Type::PkgConfig,
                              kVinputConfigPath);
}

std::unique_ptr<VinputConfig>
BuildVinputConfig(const VinputSettings &settings) {
  return std::make_unique<VinputConfig>(settings, UseChineseUi());
}
