#include "common/client/client_event.h"

#include <cstdint>
#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/json/write.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "common/asr/recognition_result.h"

namespace vinput::client {

namespace {

// Glaze escapes \b \t \n \f \r \" \\ and deliberately writes NUL bytes for any
// other control character below 0x20, which would invalidate the whole JSONL
// line for every receiver. Strip those un-escapable bytes at the boundary.
std::string SanitizeText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto byte = static_cast<std::uint8_t>(c);
    const bool glaze_escapable = c == '\b' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
    if (byte < 0x20 && !glaze_escapable) {
      continue;
    }
    out.push_back(c);
  }
  return out;
}

struct StatusWire {
  std::string type = "status";
  std::string status;
  bool ime = true;
};

struct PartialWire {
  std::string type = "partial";
  std::string text;
  bool ime = true;
};

struct CandidateWire {
  std::string text;
  std::string source;
};

struct CommittedWire {
  std::string type = "committed";
  std::string text;
  std::vector<CandidateWire> candidates;
  bool ime = true;
};

} // namespace

} // namespace vinput::client

template <> struct glz::meta<vinput::client::StatusWire> {
  using T = vinput::client::StatusWire;
  static constexpr auto value = object("type", &T::type, "status", &T::status, "ime", &T::ime);
};

template <> struct glz::meta<vinput::client::PartialWire> {
  using T = vinput::client::PartialWire;
  static constexpr auto value = object("type", &T::type, "text", &T::text, "ime", &T::ime);
};

template <> struct glz::meta<vinput::client::CandidateWire> {
  using T = vinput::client::CandidateWire;
  static constexpr auto value = object("text", &T::text, "source", &T::source);
};

template <> struct glz::meta<vinput::client::CommittedWire> {
  using T = vinput::client::CommittedWire;
  static constexpr auto value =
      object("type", &T::type, "text", &T::text, "candidates", &T::candidates, "ime", &T::ime);
};

namespace vinput::client {

std::string SerializeStatusEvent(std::string_view status, bool ime) {
  StatusWire wire;
  wire.status = SanitizeText(status);
  wire.ime = ime;
  std::string out;
  if (bool(glz::write_json(wire, out))) {
    return {};
  }
  return out;
}

std::string SerializePartialEvent(std::string_view text, bool ime) {
  PartialWire wire;
  wire.text = SanitizeText(text);
  wire.ime = ime;
  std::string out;
  if (bool(glz::write_json(wire, out))) {
    return {};
  }
  return out;
}

std::string SerializeCommittedEvent(const vinput::result::Payload& payload, bool ime) {
  CommittedWire wire;
  wire.text = SanitizeText(payload.commitText);
  wire.ime = ime;
  wire.candidates.reserve(payload.candidates.size());
  for (const auto& cand : payload.candidates) {
    wire.candidates.push_back(CandidateWire{
        .text = SanitizeText(cand.text),
        .source = cand.source,
    });
  }
  std::string out;
  if (bool(glz::write_json(wire, out))) {
    return {};
  }
  return out;
}

} // namespace vinput::client
