#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "common/audio/recording_keys.h"

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
  using vinput::pw::FcitxKeyNamesFromRecordingKeys;

  {
    const auto names = FcitxKeyNamesFromRecordingKeys("DICTATE,VOICECOMMAND,ASSISTANT");
    CHECK(names.size() == 3);
    CHECK(names[0] == "Dictate");
    CHECK(names[1] == "VoiceCommand");
    CHECK(names[2] == "Assistant");
  }

  {
    const auto names = FcitxKeyNamesFromRecordingKeys("  DICTATE , , VOICECOMMAND ");
    CHECK(names.size() == 2);
    CHECK(names[0] == "Dictate");
    CHECK(names[1] == "VoiceCommand");
  }

  {
    const auto names = FcitxKeyNamesFromRecordingKeys("DICTATE,DICTATE");
    CHECK(names.size() == 1);
    CHECK(names[0] == "Dictate");
  }

  {
    const auto names = FcitxKeyNamesFromRecordingKeys("UNKNOWN,DICTATE");
    CHECK(names.size() == 1);
    CHECK(names[0] == "Dictate");
  }

  {
    const auto names = FcitxKeyNamesFromRecordingKeys("ASSISTANT,VOICECOMMAND,DICTATE");
    CHECK(names.size() == 3);
    CHECK(names[0] == "Assistant");
    CHECK(names[1] == "VoiceCommand");
    CHECK(names[2] == "Dictate");
  }

  CHECK(FcitxKeyNamesFromRecordingKeys("").empty());
  CHECK(FcitxKeyNamesFromRecordingKeys("dictate").empty());
  CHECK(FcitxKeyNamesFromRecordingKeys(",,").empty());

  std::cout << "All recording key tests passed!\n";
  return 0;
}
