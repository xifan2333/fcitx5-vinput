#pragma once

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

inline constexpr const char* kVinputConfigPath = "conf/vinput.conf";

FCITX_CONFIG_ENUM(TriggerMode, Tap, Hold, Both)
FCITX_CONFIG_ENUM_I18N_ANNOTATION(TriggerMode, N_("Tap"), N_("Hold"), N_("Both"))

class VinputConfig : public fcitx::Configuration {
public:
  VinputConfig();
  VinputConfig(const VinputConfig&) = delete;
  VinputConfig& operator=(const VinputConfig&) = delete;

  const char* typeName() const override { return "VinputConfig"; }

  fcitx::Option<TriggerMode, fcitx::NoConstrain<TriggerMode>, fcitx::DefaultMarshaller<TriggerMode>,
                TriggerModeI18NAnnotation>
      triggerMode;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      triggerKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      commandKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      sceneMenuKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      asrMenuKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      pagePrevKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      pageNextKeys;

  fcitx::Option<int, fcitx::IntConstrain, fcitx::DefaultMarshaller<int>, fcitx::ToolTipAnnotation>
      maxStreamingDisplayWidth;

  fcitx::ExternalOption modelManager;
};
