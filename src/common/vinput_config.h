#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/key.h>

#include <memory>

inline constexpr const char *kVinputConfigPath = "conf/vinput.conf";

struct VinputSettings {
  fcitx::KeyList triggerKeys{fcitx::Key(FcitxKey_Alt_R)};
  fcitx::KeyList commandKeys{fcitx::Key(FcitxKey_Control_R)};
  fcitx::KeyList sceneMenuKey{
      fcitx::Key(FcitxKey_Alt_R, fcitx::KeyState::Ctrl)};
  fcitx::KeyList pagePrevKeys{
      fcitx::Key(FcitxKey_Page_Up),
      fcitx::Key(FcitxKey_KP_Page_Up),
  };
  fcitx::KeyList pageNextKeys{
      fcitx::Key(FcitxKey_Page_Down),
      fcitx::Key(FcitxKey_KP_Page_Down),
  };
};

class VinputConfig : public fcitx::Configuration {
public:
  VinputConfig(const VinputSettings &settings);
  VinputConfig(const VinputConfig &) = delete;
  VinputConfig &operator=(const VinputConfig &) = delete;

  const char *typeName() const override { return "VinputConfig"; }
  VinputSettings settings() const;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>,
                fcitx::ToolTipAnnotation>
      triggerKey;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>,
                fcitx::ToolTipAnnotation>
      commandKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>,
                fcitx::ToolTipAnnotation>
      sceneMenuKey;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>,
                fcitx::ToolTipAnnotation>
      pagePrevKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>,
                fcitx::ToolTipAnnotation>
      pageNextKeys;

  fcitx::ExternalOption modelManager;
};

VinputSettings LoadVinputSettings();
bool SaveVinputSettings(const VinputSettings &settings);
std::unique_ptr<VinputConfig> BuildVinputConfig(const VinputSettings &settings);
