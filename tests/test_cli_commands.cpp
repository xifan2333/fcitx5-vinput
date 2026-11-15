#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cli/action.h"
#include "cli/config/register.h"
#include "cli/control/register.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << "\n";
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

struct CliParseResult {
  bool success{false};
  bool has_action{false};
  std::string error_message;
};

CliParseResult ParseArgs(const std::vector<std::string>& args) {
  CLI::App app{"vinput test"};
  app.require_subcommand(0, 1);
  bool json_output = false;
  app.add_flag("-j,--json", json_output);

  vinput::cli::CliAction action;
  vinput::cli::config::RegisterConfigCli(app, &action);
  vinput::cli::control::RegisterControlCli(app, &action);

  std::vector<const char*> c_args;
  c_args.reserve(args.size() + 1);
  c_args.push_back("vinput");
  for (const auto& a : args) {
    c_args.push_back(a.c_str());
  }

  CliParseResult res;
  try {
    app.parse(static_cast<int>(c_args.size()), c_args.data());
    res.success = true;
    res.has_action = static_cast<bool>(action);
  } catch (const CLI::ParseError& e) {
    res.success = false;
    res.error_message = e.what();
  }
  return res;
}

void TestDaemonSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"daemon", "status"}).success);
  TEST_ASSERT(ParseArgs({"daemon", "status"}).has_action);
  TEST_ASSERT(ParseArgs({"d", "st"}).success);
  TEST_ASSERT(ParseArgs({"d", "st"}).has_action);
  TEST_ASSERT(ParseArgs({"daemon", "st"}).success);
  TEST_ASSERT(ParseArgs({"d", "status"}).success);

  TEST_ASSERT(ParseArgs({"daemon", "start"}).success);
  TEST_ASSERT(ParseArgs({"d", "stop"}).success);
  TEST_ASSERT(ParseArgs({"d", "restart"}).success);

  TEST_ASSERT(ParseArgs({"d", "log", "-f", "-n", "50", "-l", "warn"}).success);
  TEST_ASSERT(ParseArgs({"daemon", "pipe", "--ime", "-s", "default"}).success);
  TEST_ASSERT(ParseArgs({"d", "pipe"}).success);

  // 'logs' alias is forbidden and must fail
  TEST_ASSERT(!ParseArgs({"d", "logs"}).success);
  TEST_ASSERT(!ParseArgs({"daemon", "logs"}).success);
}

void TestRecordingSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"recording", "start"}).success);
  TEST_ASSERT(ParseArgs({"recording", "start"}).has_action);
  TEST_ASSERT(ParseArgs({"rec", "start"}).success);
  TEST_ASSERT(ParseArgs({"rec", "start"}).has_action);
  TEST_ASSERT(ParseArgs({"rec", "stop", "-s", "default"}).success);
  TEST_ASSERT(ParseArgs({"rec", "toggle"}).success);
}

void TestConfigSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"config", "init"}).success);
  TEST_ASSERT(ParseArgs({"config", "init", "-f"}).success);
  TEST_ASSERT(ParseArgs({"cfg", "init"}).success);
  TEST_ASSERT(ParseArgs({"cfg", "get", "/global/capture_device"}).success);

  // Top-level init is removed
  TEST_ASSERT(!ParseArgs({"init"}).success);
}

void TestDeviceSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"device", "list"}).success);
  TEST_ASSERT(ParseArgs({"dev", "ls"}).success);
  TEST_ASSERT(ParseArgs({"dev", "use", "default"}).success);
}

void TestProviderSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"provider", "list"}).success);
  TEST_ASSERT(ParseArgs({"prov", "ls", "-a"}).success);
  TEST_ASSERT(ParseArgs({"prov", "use", "test-prov"}).success);
}

void TestTriggerSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"trigger", "list"}).success);
  TEST_ASSERT(ParseArgs({"trig", "ls"}).success);
  TEST_ASSERT(ParseArgs({"trig", "start", "hotkey"}).success);
  TEST_ASSERT(ParseArgs({"trig", "stop", "hotkey"}).success);
}

void TestReceiverSubcommandsAndAliases() {
  TEST_ASSERT(ParseArgs({"receiver", "list"}).success);
  TEST_ASSERT(ParseArgs({"recv", "ls"}).success);
  TEST_ASSERT(ParseArgs({"recv", "start", "fcitx"}).success);
  TEST_ASSERT(ParseArgs({"recv", "stop", "fcitx"}).success);
}

void TestListenRemoved() {
  // Top-level listen was removed and subsumed by daemon pipe
  TEST_ASSERT(!ParseArgs({"listen"}).success);
}

} // namespace

int main() {
  TestDaemonSubcommandsAndAliases();
  TestRecordingSubcommandsAndAliases();
  TestConfigSubcommandsAndAliases();
  TestDeviceSubcommandsAndAliases();
  TestProviderSubcommandsAndAliases();
  TestTriggerSubcommandsAndAliases();
  TestReceiverSubcommandsAndAliases();
  TestListenRemoved();
  std::cout << "All CLI command tests passed successfully.\n";
  return 0;
}
