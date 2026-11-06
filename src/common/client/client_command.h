#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace vinput::client {

struct StartCommand {
  bool ime = true;
  std::string mode = "normal"; // "normal" or "command"
  std::string selected_text;
  std::string capture_device;
};

struct StopCommand {
  bool ime = true;
  std::string scene;
};

struct CancelCommand {
  bool ime = true;
  bool commit_raw = false;
};

enum class InjectStage : std::uint8_t {
  Partial,
  Final,
};

struct InjectCommand {
  bool ime = true;
  InjectStage stage = InjectStage::Partial;
  std::string text;
  std::string scene;
};

using ClientCommand = std::variant<StartCommand, StopCommand, CancelCommand, InjectCommand>;

// Parses a single JSONL line into a strongly typed ClientCommand according to
// client-protocol.md. Returns nullopt if parsing fails or fields are invalid.
std::optional<ClientCommand> ParseClientCommand(std::string_view line,
                                                std::string* error = nullptr);

} // namespace vinput::client
