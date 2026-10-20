#include "recognition_result.h"

#include <algorithm>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

template <> struct glz::meta<vinput::result::Candidate> {
  using T = vinput::result::Candidate;
  static constexpr auto value = object("text", &T::text, "source", &T::source);
};

template <> struct glz::meta<vinput::result::Payload> {
  using T = vinput::result::Payload;
  static constexpr auto value = object("commit_text", &T::commitText, "candidates", &T::candidates);
};

namespace vinput::result {

std::string Serialize(const Payload& payload) {
  std::string out;
  if (bool(glz::write_json(payload, out))) {
    return {};
  }
  return out;
}

Payload Parse(std::string_view payload_text) {
  if (payload_text.empty()) {
    return {};
  }

  Payload payload;
  const auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(payload, payload_text);
  if (bool(ec)) {
    return {};
  }

  std::erase_if(payload.candidates,
                [](const Candidate& candidate) { return candidate.text.empty(); });

  if (payload.commitText.empty() && !payload.candidates.empty()) {
    payload.commitText = payload.candidates.front().text;
  }

  if (payload.candidates.empty() && !payload.commitText.empty()) {
    payload.candidates.push_back(Candidate{.text = payload.commitText, .source = kSourceRaw});
  }

  if (payload.commitText.empty() && payload.candidates.empty()) {
    return {};
  }

  return payload;
}

} // namespace vinput::result
