#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

#include "common/client/client_command.h"

namespace {

void ExpectTrue(bool condition, const std::string& msg = "") {
  if (!condition) {
    std::cerr << "ExpectTrue failed! " << msg << "\n";
    std::abort();
  }
}

void ExpectEq(const std::string& actual, const std::string& expected, const std::string& msg = "") {
  if (actual != expected) {
    std::cerr << "ExpectEq failed: expected '" << expected << "', got '" << actual << "' (" << msg
              << ")\n";
    std::abort();
  }
}

void TestParseStartCommand() {
  // Default values
  {
    auto cmd = vinput::client::ParseClientCommand(R"({"type":"start"})");
    ExpectTrue(cmd.has_value(), "Default start should parse");
    if (!cmd) {
      std::abort();
    }
    const auto* start = std::get_if<vinput::client::StartCommand>(&cmd.value());
    ExpectTrue(start != nullptr, "Command should be StartCommand");
    ExpectTrue(start->ime, "Default ime should be true");
    ExpectEq(start->mode, "normal", "Default mode should be normal");
    ExpectTrue(start->selected_text.empty(), "Default selected_text should be empty");
    ExpectTrue(start->capture_device.empty(), "Default capture_device should be empty");
  }

  // Explicit values
  {
    auto cmd = vinput::client::ParseClientCommand(
        R"({"type":"start","ime":false,"mode":"command","selected_text":"hello","capture_device":"alsa_input.1"})");
    ExpectTrue(cmd.has_value(), "Full start should parse");
    if (!cmd) {
      std::abort();
    }
    const auto* start = std::get_if<vinput::client::StartCommand>(&cmd.value());
    ExpectTrue(start != nullptr, "Command should be StartCommand");
    ExpectTrue(!start->ime, "ime should be false");
    ExpectEq(start->mode, "command", "mode should be command");
    ExpectEq(start->selected_text, "hello", "selected_text should match");
    ExpectEq(start->capture_device, "alsa_input.1", "capture_device should match");
  }
}

void TestParseStopCommand() {
  {
    auto cmd = vinput::client::ParseClientCommand(R"({"type":"stop","ime":true,"scene":"polish"})");
    ExpectTrue(cmd.has_value(), "Stop should parse");
    if (!cmd) {
      std::abort();
    }
    const auto* stop = std::get_if<vinput::client::StopCommand>(&cmd.value());
    ExpectTrue(stop != nullptr, "Command should be StopCommand");
    ExpectTrue(stop->ime, "ime should be true");
    ExpectEq(stop->scene, "polish", "scene should match");
  }
}

void TestParseCancelCommand() {
  {
    auto cmd =
        vinput::client::ParseClientCommand(R"({"type":"cancel","ime":false,"commit_raw":true})");
    ExpectTrue(cmd.has_value(), "Cancel should parse");
    if (!cmd) {
      std::abort();
    }
    const auto* cancel = std::get_if<vinput::client::CancelCommand>(&cmd.value());
    ExpectTrue(cancel != nullptr, "Command should be CancelCommand");
    ExpectTrue(!cancel->ime, "ime should be false");
    ExpectTrue(cancel->commit_raw, "commit_raw should be true");
  }
}

void TestParseInjectCommand() {
  // Partial stage
  {
    auto cmd = vinput::client::ParseClientCommand(
        R"({"type":"inject","stage":"partial","text":"speaking...","ime":true})");
    ExpectTrue(cmd.has_value(), "Inject partial should parse");
    if (!cmd) {
      std::abort();
    }
    const auto* inject = std::get_if<vinput::client::InjectCommand>(&cmd.value());
    ExpectTrue(inject != nullptr, "Command should be InjectCommand");
    ExpectTrue(inject->stage == vinput::client::InjectStage::Partial, "Stage should be Partial");
    ExpectEq(inject->text, "speaking...", "text should match");
    ExpectTrue(inject->ime, "ime should be true");
  }

  // Final stage with scene
  {
    auto cmd = vinput::client::ParseClientCommand(
        R"({"type":"inject","stage":"final","text":"done speaking","ime":false,"scene":"translate"})");
    ExpectTrue(cmd.has_value(), "Inject final should parse");
    if (!cmd) {
      std::abort();
    }
    const auto* inject = std::get_if<vinput::client::InjectCommand>(&cmd.value());
    ExpectTrue(inject != nullptr, "Command should be InjectCommand");
    ExpectTrue(inject->stage == vinput::client::InjectStage::Final, "Stage should be Final");
    ExpectEq(inject->text, "done speaking", "text should match");
    ExpectTrue(!inject->ime, "ime should be false");
    ExpectEq(inject->scene, "translate", "scene should match");
  }

  // Invalid stage
  {
    std::string err;
    auto cmd = vinput::client::ParseClientCommand(
        R"({"type":"inject","stage":"unknown","text":"text"})", &err);
    ExpectTrue(!cmd.has_value(), "Invalid stage should fail");
    ExpectTrue(!err.empty(), "Error message should not be empty");
  }

  // Missing text
  {
    std::string err;
    auto cmd = vinput::client::ParseClientCommand(R"({"type":"inject","stage":"final"})", &err);
    ExpectTrue(!cmd.has_value(), "Missing text should fail");
  }
}

void TestParseInvalidCommand() {
  std::string err;
  auto cmd = vinput::client::ParseClientCommand(R"({"type":"unknown"})", &err);
  ExpectTrue(!cmd.has_value(), "Unknown type should fail");
  ExpectTrue(err.find("unknown command type") != std::string::npos,
             "Error should describe unknown type");
}

} // namespace

int main() {
  TestParseStartCommand();
  TestParseStopCommand();
  TestParseCancelCommand();
  TestParseInjectCommand();
  TestParseInvalidCommand();
  std::cout << "All client_command tests passed!\n";
  return 0;
}
