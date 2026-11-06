#include "common/client/client_command.h"

#include <cstdint>
#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/core/opts.hpp>
#include <glaze/core/read.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vinput::client {

namespace {

struct WireEnvelope {
  std::string type;
  std::optional<bool> ime;
  std::optional<std::string> mode;
  std::optional<std::string> selected_text;
  std::optional<std::string> capture_device;
  std::optional<std::string> scene;
  std::optional<bool> commit_raw;
  std::optional<std::string> stage;
  std::optional<std::string> text;
};

} // namespace

} // namespace vinput::client

template <> struct glz::meta<vinput::client::WireEnvelope> {
  using T = vinput::client::WireEnvelope;
  static constexpr auto value =
      object("type", &T::type, "ime", &T::ime, "mode", &T::mode, "selected_text", &T::selected_text,
             "capture_device", &T::capture_device, "scene", &T::scene, "commit_raw", &T::commit_raw,
             "stage", &T::stage, "text", &T::text);
};

namespace vinput::client {

std::optional<ClientCommand> ParseClientCommand(std::string_view line, std::string* error) {
  if (line.empty()) {
    if (error != nullptr) {
      *error = "command line is empty";
    }
    return std::nullopt;
  }

  WireEnvelope wire;
  constexpr auto kOpts = glz::opts{.error_on_unknown_keys = uint8_t{0}};
  const auto ec = glz::read<kOpts>(wire, line);
  if (bool(ec)) {
    if (error != nullptr) {
      *error = glz::format_error(ec, line);
    }
    return std::nullopt;
  }

  if (wire.type == "start") {
    StartCommand cmd;
    cmd.ime = wire.ime.value_or(true);
    cmd.mode = wire.mode.value_or("normal");
    if (cmd.mode != "normal" && cmd.mode != "command") {
      if (error != nullptr) {
        *error = "start mode must be 'normal' or 'command', got: " + cmd.mode;
      }
      return std::nullopt;
    }
    cmd.selected_text = wire.selected_text.value_or(std::string{});
    cmd.capture_device = wire.capture_device.value_or(std::string{});
    if (error != nullptr) {
      error->clear();
    }
    return cmd;
  }

  if (wire.type == "stop") {
    StopCommand cmd;
    cmd.ime = wire.ime.value_or(true);
    cmd.scene = wire.scene.value_or(std::string{});
    if (error != nullptr) {
      error->clear();
    }
    return cmd;
  }

  if (wire.type == "cancel") {
    CancelCommand cmd;
    cmd.ime = wire.ime.value_or(true);
    cmd.commit_raw = wire.commit_raw.value_or(false);
    if (error != nullptr) {
      error->clear();
    }
    return cmd;
  }

  if (wire.type == "inject") {
    if (!wire.stage.has_value() || !wire.text.has_value()) {
      if (error != nullptr) {
        *error = "inject command requires 'stage' and 'text'";
      }
      return std::nullopt;
    }

    InjectStage stage = InjectStage::Partial;
    if (*wire.stage == "partial") {
      stage = InjectStage::Partial;
    } else if (*wire.stage == "final") {
      stage = InjectStage::Final;
    } else {
      if (error != nullptr) {
        *error = "inject stage must be 'partial' or 'final', got: " + *wire.stage;
      }
      return std::nullopt;
    }

    InjectCommand cmd;
    cmd.ime = wire.ime.value_or(true);
    cmd.stage = stage;
    cmd.text = std::move(*wire.text);
    cmd.scene = wire.scene.value_or(std::string{});
    if (error != nullptr) {
      error->clear();
    }
    return cmd;
  }

  if (error != nullptr) {
    *error = "unknown command type: " + wire.type;
  }
  return std::nullopt;
}

} // namespace vinput::client
