#include "recognition_result.h"

#include <cstdint>
#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/core/opts.hpp>
#include <glaze/core/read.hpp>
#include <glaze/json/write.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct WireCandidate {
  std::optional<std::string> text;
  std::optional<std::string> source;
};

struct WirePayload {
  std::optional<std::string> commit_text;
  std::optional<std::vector<WireCandidate>> candidates;
};

} // namespace

template <> struct glz::meta<vinput::result::Candidate> {
  using T = vinput::result::Candidate;
  static constexpr auto value = object("text", &T::text, "source", &T::source);
};

template <> struct glz::meta<vinput::result::Payload> {
  using T = vinput::result::Payload;
  static constexpr auto value = object("commit_text", &T::commitText, "candidates", &T::candidates);
};

template <> struct glz::meta<WireCandidate> {
  static constexpr auto value =
      object("text", &WireCandidate::text, "source", &WireCandidate::source);
};

template <> struct glz::meta<WirePayload> {
  static constexpr auto value =
      object("commit_text", &WirePayload::commit_text, "candidates", &WirePayload::candidates);
};

namespace {

std::vector<vinput::result::Candidate>
ExtractValidCandidates(std::vector<WireCandidate> wire_candidates) {
  std::vector<vinput::result::Candidate> result;
  result.reserve(wire_candidates.size());
  for (auto& candidate : wire_candidates) {
    if (candidate.text.has_value() && !candidate.text->empty()) {
      result.push_back(vinput::result::Candidate{
          .text = std::move(*candidate.text),
          .source = candidate.source.value_or(""),
      });
    }
  }
  return result;
}

void NormalizePayload(vinput::result::Payload& payload) {
  if (payload.commitText.empty() && !payload.candidates.empty()) {
    payload.commitText = payload.candidates.front().text;
  }
  if (payload.candidates.empty() && !payload.commitText.empty()) {
    payload.candidates.push_back(vinput::result::Candidate{.text = payload.commitText,
                                                           .source = vinput::result::kSourceRaw});
  }
}

} // namespace

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

  WirePayload wire;
  const auto ec =
      glz::read<glz::opts{.error_on_unknown_keys = uint8_t{0}, .force_conformance = uint8_t{1}}>(
          wire, payload_text);
  if (bool(ec)) {
    return {};
  }

  Payload payload;
  if (wire.commit_text.has_value()) {
    payload.commitText = std::move(*wire.commit_text);
  }
  if (wire.candidates.has_value()) {
    payload.candidates = ExtractValidCandidates(std::move(*wire.candidates));
  }

  NormalizePayload(payload);

  if (payload.commitText.empty() && payload.candidates.empty()) {
    return {};
  }

  return payload;
}

} // namespace vinput::result
