#include "common/client/client_command.h"

#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/json/read.hpp>
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

namespace {

void SetError(std::string* error, std::string msg) {
  if (error != nullptr) {
    *error = std::move(msg);
  }
}

void ClearError(std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
}

std::optional<ClientCommand> ParseStart(WireEnvelope& wire, std::string* error) {
  StartCommand cmd;
  cmd.ime = wire.ime.value_or(true);
  cmd.mode = wire.mode.value_or("normal");
  if (cmd.mode != "normal" && cmd.mode != "command") {
    SetError(error, "start mode must be 'normal' or 'command', got: " + cmd.mode);
    return std::nullopt;
  }
  cmd.selected_text = wire.selected_text.value_or(std::string{});
  cmd.capture_device = wire.capture_device.value_or(std::string{});
  ClearError(error);
  return cmd;
}

std::optional<ClientCommand> ParseStop(WireEnvelope& wire, std::string* error) {
  StopCommand cmd;
  cmd.ime = wire.ime.value_or(true);
  cmd.scene = wire.scene.value_or(std::string{});
  ClearError(error);
  return cmd;
}

std::optional<ClientCommand> ParseCancel(WireEnvelope& wire, std::string* error) {
  CancelCommand cmd;
  cmd.ime = wire.ime.value_or(true);
  cmd.commit_raw = wire.commit_raw.value_or(false);
  ClearError(error);
  return cmd;
}

std::optional<ClientCommand> ParseInject(WireEnvelope& wire, std::string* error) {
  if (!wire.stage.has_value() || !wire.text.has_value()) {
    SetError(error, "inject command requires 'stage' and 'text'");
    return std::nullopt;
  }

  InjectStage stage = InjectStage::Partial;
  if (*wire.stage == "partial") {
    stage = InjectStage::Partial;
  } else if (*wire.stage == "final") {
    stage = InjectStage::Final;
  } else {
    SetError(error, "inject stage must be 'partial' or 'final', got: " + *wire.stage);
    return std::nullopt;
  }

  InjectCommand cmd;
  cmd.ime = wire.ime.value_or(true);
  cmd.stage = stage;
  cmd.text = std::move(*wire.text);
  cmd.scene = wire.scene.value_or(std::string{});
  ClearError(error);
  return cmd;
}

using CommandParser = std::optional<ClientCommand> (*)(WireEnvelope&, std::string*);

struct CommandHandler {
  std::string_view type;
  CommandParser parser;
};

constexpr CommandHandler kCommandParsers[] = {
    {.type = "start", .parser = &ParseStart},
    {.type = "stop", .parser = &ParseStop},
    {.type = "cancel", .parser = &ParseCancel},
    {.type = "inject", .parser = &ParseInject},
};

} // namespace

std::optional<ClientCommand> ParseClientCommand(std::string_view line, std::string* error) {
  if (line.empty()) {
    SetError(error, "command line is empty");
    return std::nullopt;
  }

  WireEnvelope wire;
  const auto ec = glz::read_json(wire, line);
  if (bool(ec)) {
    SetError(error, glz::format_error(ec, line));
    return std::nullopt;
  }

  for (const auto& entry : kCommandParsers) {
    if (entry.type == wire.type) {
      return entry.parser(wire, error);
    }
  }

  SetError(error, "unknown command type: " + wire.type);
  return std::nullopt;
}

} // namespace vinput::client
