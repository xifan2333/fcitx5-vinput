#include <fcitx-config/rawconfig.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/config/fcitx_controller.h"

namespace {

bool Check(bool cond, std::string_view expr, const char* file, int line) {
  if (!cond) {
    std::cerr << "Test failure: " << expr << " at " << file << ":" << line << '\n';
    return false;
  }
  return true;
}

#define CHECK(expr)                                                                                \
  if (!Check((expr), #expr, __FILE__, __LINE__)) {                                                 \
    return 1;                                                                                      \
  }

} // namespace

int main() {
  using vinput::cli::AppendTriggerKeys;

  {
    fcitx::RawConfig raw;
    const auto update = AppendTriggerKeys(&raw, {"Dictate", "VoiceCommand", "Assistant"});
    CHECK(update.entries.size() == 3);
    CHECK(update.entries[0].index == "0");
    CHECK(update.entries[0].name == "Dictate");
    CHECK(update.entries[1].index == "1");
    CHECK(update.entries[1].name == "VoiceCommand");
    CHECK(update.entries[2].index == "2");
    CHECK(update.entries[2].name == "Assistant");

    auto trigger = raw.get("TriggerKey");
    CHECK(trigger != nullptr);
    CHECK(trigger->get("0")->value() == "Dictate");
    CHECK(trigger->get("1")->value() == "VoiceCommand");
    CHECK(trigger->get("2")->value() == "Assistant");

    const auto again = AppendTriggerKeys(&raw, {"Dictate", "VoiceCommand", "Assistant"});
    CHECK(again.Empty());
  }

  {
    fcitx::RawConfig raw;
    raw.setValueByPath("TriggerKey/0", "Alt_R");
    raw.setValueByPath("TriggerKey/1", "Assistant");
    const auto update =
        AppendTriggerKeys(&raw, {"Dictate", "VoiceCommand", "Assistant", "Dictate"});
    CHECK(update.entries.size() == 2);
    CHECK(update.entries[0].index == "2");
    CHECK(update.entries[0].name == "Dictate");
    CHECK(update.entries[1].index == "3");
    CHECK(update.entries[1].name == "VoiceCommand");

    auto trigger = raw.get("TriggerKey");
    CHECK(trigger->get("0")->value() == "Alt_R");
    CHECK(trigger->get("1")->value() == "Assistant");
    CHECK(trigger->get("2")->value() == "Dictate");
    CHECK(trigger->get("3")->value() == "VoiceCommand");
  }

  {
    fcitx::RawConfig raw;
    raw.setValueByPath("TriggerKey/0", "Alt_R");
    raw.setValueByPath("TriggerKey/5", "F1");
    const auto update = AppendTriggerKeys(&raw, {"Dictate"});
    CHECK(update.entries.size() == 1);
    CHECK(update.entries[0].index == "6");
    CHECK(update.entries[0].name == "Dictate");
  }

  {
    fcitx::RawConfig raw;
    raw.setValueByPath("TriggerKey/custom", "F2");
    const auto update = AppendTriggerKeys(&raw, {"Dictate"});
    CHECK(update.entries.size() == 1);
    CHECK(update.entries[0].index == "0");
    CHECK(update.entries[0].name == "Dictate");
  }

  {
    fcitx::RawConfig raw;
    const auto update = AppendTriggerKeys(&raw, {});
    CHECK(update.Empty());
  }

  std::cout << "All fcitx controller tests passed!\n";
  return 0;
}
