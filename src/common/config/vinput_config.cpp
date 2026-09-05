#include "common/config/vinput_config.h"

#include <fcitx-utils/standardpath.h>
#include <filesystem>
#include <string>

#include "common/i18n.h"

namespace {

fcitx::ListConstrain<fcitx::KeyConstrain> TriggerKeyListConstrain() {
  return fcitx::KeyListConstrain(fcitx::KeyConstrainFlags{
      fcitx::KeyConstrainFlag::AllowModifierOnly,
      fcitx::KeyConstrainFlag::AllowModifierLess,
  });
}

fcitx::ListConstrain<fcitx::KeyConstrain> MenuKeyListConstrain() {
  return fcitx::KeyListConstrain(fcitx::KeyConstrainFlags{
      fcitx::KeyConstrainFlag::AllowModifierOnly,
      fcitx::KeyConstrainFlag::AllowModifierLess,
  });
}

std::string UserPkgConfigPath(std::string_view relative_path) {
  return (std::filesystem::path(
              fcitx::StandardPath::global().userDirectory(fcitx::StandardPath::Type::PkgConfig)) /
          std::string(relative_path))
      .string();
}

std::string TriggerKeyLabel() {
  return _("Trigger Key");
}

std::string TriggerKeyTooltip() {
  const std::string path = UserPkgConfigPath(kVinputConfigPath);
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                _("Press and hold this key to record. Release it to start recognition. "
                  "Supports regular keys, modifier keys, and modified key combinations. "
                  "You can configure multiple trigger keys. The config is stored at %s."),
                path.c_str());
  return buf;
}

std::string CommandKeysLabel() {
  return _("Command Keys");
}

std::string CommandKeysTooltip() {
  return _("Press and hold this key to record a voice command to operate on "
           "the selected text. If there is no active selection, vinput falls "
           "back to the current primary-selection clipboard text. Default is "
           "Right Control.");
}

std::string MenuKeyLabel() {
  return _("Command Palette Keys");
}

std::string MenuKeyTooltip() {
  return _("Configure one or more keys to open the unified command palette (/model, /asr, /scene, "
           "/proc). The default is Right Shift.");
}

std::string PagePrevKeysLabel() {
  return _("Previous Page Keys");
}

std::string PagePrevKeysTooltip() {
  return _("Keys for paging to the previous page in the postprocess menu and "
           "the postprocess candidate menu. Defaults to Page Up and keypad "
           "Page Up.");
}

std::string PageNextKeysLabel() {
  return _("Next Page Keys");
}

std::string PageNextKeysTooltip() {
  return _("Keys for paging to the next page in the postprocess menu and the "
           "postprocess candidate menu. Defaults to Page Down and keypad Page "
           "Down.");
}

std::string TriggerModeLabel() {
  return _("Trigger Mode");
}

std::string MaxStreamingDisplayWidthLabel() {
  return _("Max Streaming Display Width");
}

std::string MaxStreamingDisplayWidthTooltip() {
  return _("Maximum visual column width for live streaming recognition preview. Older text is "
           "folded into 'head...tail'. Set to 0 to disable folding. Default is 60.");
}

} // namespace

VinputConfig::VinputConfig()
    : triggerMode(this, "TriggerMode", TriggerModeLabel(), TriggerMode::Both, {}, {},
                  TriggerModeI18NAnnotation()),
      triggerKeys(this, "TriggerKey", TriggerKeyLabel(), {fcitx::Key(FcitxKey_Alt_R)},
                  TriggerKeyListConstrain(), {}, fcitx::ToolTipAnnotation(TriggerKeyTooltip())),
      commandKeys(this, "CommandKeys", CommandKeysLabel(), {fcitx::Key(FcitxKey_Control_R)},
                  TriggerKeyListConstrain(), {}, fcitx::ToolTipAnnotation(CommandKeysTooltip())),
      menuKeys(this, "MenuKey", MenuKeyLabel(), {fcitx::Key(FcitxKey_Shift_R)},
               MenuKeyListConstrain(), {}, fcitx::ToolTipAnnotation(MenuKeyTooltip())),
      pagePrevKeys(this, "PagePrevKeys", PagePrevKeysLabel(),
                   {fcitx::Key(FcitxKey_Page_Up), fcitx::Key(FcitxKey_KP_Page_Up)},
                   TriggerKeyListConstrain(), {}, fcitx::ToolTipAnnotation(PagePrevKeysTooltip())),
      pageNextKeys(this, "PageNextKeys", PageNextKeysLabel(),
                   {fcitx::Key(FcitxKey_Page_Down), fcitx::Key(FcitxKey_KP_Page_Down)},
                   TriggerKeyListConstrain(), {}, fcitx::ToolTipAnnotation(PageNextKeysTooltip())),
      maxStreamingDisplayWidth(this, "MaxStreamingDisplayWidth", MaxStreamingDisplayWidthLabel(),
                               60, fcitx::IntConstrain(0, 500), {},
                               fcitx::ToolTipAnnotation(MaxStreamingDisplayWidthTooltip())),
      modelManager(this, "ModelManager", _("Open Vinput Settings"), "vinput-gui") {}
