#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vinput::pw {

// Maps the canonical recording-key names advertised in
// `atvv.recording_keys` to fcitx5 key names. Unknown names, empty entries, and
// duplicates are dropped; declaration order is preserved.
std::vector<std::string> FcitxKeyNamesFromRecordingKeys(std::string_view value);

} // namespace vinput::pw
