#include "common/audio/recording_keys.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "common/utils/string_utils.h"

namespace vinput::pw {

namespace {

struct RecordingKeyMapping {
  std::string_view canonical;
  std::string_view fcitx;
};

constexpr std::array kMappings = {
    RecordingKeyMapping{.canonical = "DICTATE", .fcitx = "Dictate"},
    RecordingKeyMapping{.canonical = "VOICECOMMAND", .fcitx = "VoiceCommand"},
    RecordingKeyMapping{.canonical = "ASSISTANT", .fcitx = "Assistant"},
};

} // namespace

std::vector<std::string> FcitxKeyNamesFromRecordingKeys(std::string_view value) {
  std::vector<std::string> names;
  for (const auto token : vinput::str::Split(value, ',')) {
    const auto canonical = vinput::str::Trim(token);
    if (canonical.empty()) {
      continue;
    }
    for (const auto& mapping : kMappings) {
      if (canonical != mapping.canonical) {
        continue;
      }
      if (std::ranges::find(names, mapping.fcitx) == names.end()) {
        names.emplace_back(mapping.fcitx);
      }
      break;
    }
  }
  return names;
}

} // namespace vinput::pw
