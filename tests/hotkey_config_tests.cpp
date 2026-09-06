#include <cstdio>
#include <cstdlib>
#include <fcitx-config/iniparser.h>
#include <iostream>
#include <memory>

#include "common/config/vinput_config.h"

void expect(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
  VinputConfig config;
  expect(config.holdActivationDelay.value() == 300, "new configs retain the 300 ms default");
  expect(config.triggerMode.value() == TriggerMode::Both, "default mode remains Both");

  // Use the FILE API so the test also builds with Fcitx versions before 5.1.13.
  // The temporary file is automatically removed; no user configuration is read.
  const std::unique_ptr<FILE, decltype(&std::fclose)> ini(std::tmpfile(), &std::fclose);
  expect(ini != nullptr, "create temporary INI file");
  expect(std::fputs(R"ini(TriggerMode=Tap
MaxStreamingDisplayWidth=42
[TriggerKey]
0=F8
1=Alt_R
[CommandKeys]
0=Control+Control_L
[MenuKey]
0=Alt+space
[PagePrevKeys]
0=Control+Page_Up
[PageNextKeys]
0=Control+Page_Down
)ini",
                    ini.get()) >= 0,
         "write legacy INI fixture");
  std::rewind(ini.get());
  fcitx::RawConfig legacy;
  fcitx::readFromIni(legacy, ini.get());
  config.load(legacy, true);
  expect(config.holdActivationDelay.value() == 300, "old configs need no delay migration");
  expect(config.triggerMode.value() == TriggerMode::Tap, "legacy mode is preserved");
  expect(config.maxStreamingDisplayWidth.value() == 42, "legacy display setting is preserved");
  expect(config.triggerKeys.value() == fcitx::KeyList{fcitx::Key("F8"), fcitx::Key("Alt_R")},
         "legacy dedicated and modifier keys are preserved");
  expect(config.commandKeys.value() == fcitx::KeyList{fcitx::Key("Control+Control_L")},
         "legacy modifier binding is preserved");
  expect(config.menuKeys.value() == fcitx::KeyList{fcitx::Key("Alt+space")},
         "legacy palette binding is preserved");
  expect(config.pagePrevKeys.value() == fcitx::KeyList{fcitx::Key("Control+Page_Up")},
         "legacy previous-page binding is preserved");
  expect(config.pageNextKeys.value() == fcitx::KeyList{fcitx::Key("Control+Page_Down")},
         "legacy next-page binding is preserved");

  for (int delay : {100, 750, 2000}) {
    expect(config.holdActivationDelay.setValue(delay),
           "valid delay, including endpoints, accepted");
    for (int invalid : {0, 99, 2001}) {
      expect(!config.holdActivationDelay.setValue(invalid), "out-of-range delay rejected");
      expect(config.holdActivationDelay.value() == delay, "rejection preserves the valid delay");
    }
  }

  expect(config.holdActivationDelay.setValue(750), "set custom delay");
  expect(config.triggerMode.setValue(TriggerMode::Hold), "set Hold mode");
  expect(config.triggerKeys.setValue(fcitx::KeyList{fcitx::Key("Control+Shift_L")}),
         "accept two modifiers");
  expect(config.commandKeys.setValue(fcitx::KeyList{fcitx::Key("Control+Alt+Shift_L")}),
         "accept three modifiers for command mode");
  expect(config.menuKeys.setValue(fcitx::KeyList{fcitx::Key("Control+Alt+Super+Shift_L")}),
         "accept four modifiers for the palette");
  fcitx::RawConfig saved;
  config.save(saved);
  // Start a fresh file to avoid retaining bytes from an earlier fixture.
  const std::unique_ptr<FILE, decltype(&std::fclose)> output(std::tmpfile(), &std::fclose);
  expect(output != nullptr, "create saved INI file");
  expect(fcitx::writeAsIni(saved, output.get()), "serialize config to INI");
  std::rewind(output.get());
  fcitx::RawConfig loaded;
  fcitx::readFromIni(loaded, output.get());
  VinputConfig restored;
  restored.load(loaded, true);
  expect(restored.holdActivationDelay.value() == 750, "custom delay survives INI round-trip");
  expect(restored.triggerMode.value() == TriggerMode::Hold, "Hold mode survives INI round-trip");
  expect(restored.triggerKeys.value() == config.triggerKeys.value(), "two-modifier round-trip");
  expect(restored.commandKeys.value() == config.commandKeys.value(), "three-modifier round-trip");
  expect(restored.menuKeys.value() == config.menuKeys.value(), "four-modifier round-trip");
  std::cout << "PASS hotkey config defaults, bounds, and INI round-trip\n";
}
