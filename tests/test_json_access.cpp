#include <cstdlib>
#include <glaze/json/json_t.hpp>
#include <glaze/json/write.hpp>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"

namespace {

void Expect(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << '\n';
    std::abort();
  }
}

#define TEST_EXPECT(cond) Expect((cond), #cond, __FILE__, __LINE__)

std::string ToJson(const glz::json_t& value) {
  std::string out;
  TEST_EXPECT(!bool(glz::write_json(value, out)));
  return out;
}

// `FromStrings` must produce `["a","b"]`, never the double-wrapped `[["a","b"]]`
// that an initializer-list return `{std::move(array)}` would produce.
void TestFromStringsIsFlat() {
  const auto value = vinput::json::FromStrings(std::vector<std::string>{"a", "b"});
  TEST_EXPECT(value.is_array());
  TEST_EXPECT(ToJson(value) == R"(["a","b"])");
  const auto round_tripped = vinput::json::Strings(value);
  TEST_EXPECT(round_tripped == std::vector<std::string>({"a", "b"}));

  const auto empty_array = vinput::json::FromStrings(std::vector<std::string>{});
  TEST_EXPECT(empty_array.is_array());
  TEST_EXPECT(empty_array.empty());
  TEST_EXPECT(ToJson(empty_array) == R"([])");
}

// `FromStringMap` must produce `{"k":"v"}`, never the double-wrapped `[{"k":"v"}]`.
void TestFromStringMapIsFlat() {
  const auto value = vinput::json::FromStringMap(std::map<std::string, std::string>{{"k", "v"}});
  TEST_EXPECT(value.is_object());
  TEST_EXPECT(ToJson(value) == R"({"k":"v"})");
  const auto round_tripped = vinput::json::StringMap(value);
  TEST_EXPECT(round_tripped.size() == 1);
  TEST_EXPECT(round_tripped.at("k") == "v");

  const auto empty_object = vinput::json::FromStringMap({});
  TEST_EXPECT(empty_object.is_object());
  TEST_EXPECT(empty_object.empty());
  TEST_EXPECT(ToJson(empty_object) == R"({})");
}

// `EmptyObject` must produce a valid empty JSON object, not an array.
void TestEmptyObjectIsObject() {
  const auto& empty = vinput::json::EmptyObject();
  TEST_EXPECT(empty.is_object());
  TEST_EXPECT(empty.empty());
  TEST_EXPECT(ToJson(empty) == R"({})");
}

// Guards the user-facing regression in #519: saving and reloading core config
// used to strip `registry.base_urls` and command provider `args` / `env`
// because they were serialized double-wrapped and silently dropped on parse.
void TestCoreConfigRoundTrip() {
  constexpr std::string_view kConfigText = R"({
    "registry": {"base_urls": ["https://registry.example.com/main"]},
    "asr": {
      "active_provider": "example-batch",
      "providers": [
        {
          "id": "example-batch",
          "type": "command",
          "command": "python3",
          "args": ["/path/to/provider", "--flag"],
          "env": {"EXAMPLE_API_KEY": "secret", "EXAMPLE_URL": "http://127.0.0.1:9999/v1"}
        }
      ]
    }
  })";

  CoreConfig config;
  std::string error;
  TEST_EXPECT(CoreConfigFromJson(kConfigText, &config, &error));
  TEST_EXPECT(config.registry.baseUrls.size() == 1);
  TEST_EXPECT(config.asr.providers.size() == 1);

  const auto* provider = std::get_if<CommandAsrProvider>(&config.asr.providers.front());
  TEST_EXPECT(provider != nullptr);
  TEST_EXPECT(provider->args.size() == 2);
  TEST_EXPECT(provider->env.size() == 2);

  const std::string serialized = CoreConfigToJson(config);
  // Ensure flat serialization without double arrays
  TEST_EXPECT(serialized.find(R"("base_urls": [)") != std::string::npos);
  TEST_EXPECT(serialized.find(R"([["https://registry.example.com/main"]])") == std::string::npos);
  TEST_EXPECT(serialized.find(R"("/path/to/provider")") != std::string::npos);
  TEST_EXPECT(serialized.find(R"("EXAMPLE_API_KEY": "secret")") != std::string::npos);

  CoreConfig reloaded;
  TEST_EXPECT(CoreConfigFromJson(serialized, &reloaded, &error));
  TEST_EXPECT(reloaded.registry.baseUrls.size() == 1);
  TEST_EXPECT(reloaded.registry.baseUrls.front() == "https://registry.example.com/main");

  const auto* reloaded_provider = std::get_if<CommandAsrProvider>(&reloaded.asr.providers.front());
  TEST_EXPECT(reloaded_provider != nullptr);
  TEST_EXPECT(reloaded_provider->args.size() == 2);
  TEST_EXPECT(reloaded_provider->args.at(0) == "/path/to/provider");
  TEST_EXPECT(reloaded_provider->args.at(1) == "--flag");
  TEST_EXPECT(reloaded_provider->env.size() == 2);
  TEST_EXPECT(reloaded_provider->env.at("EXAMPLE_API_KEY") == "secret");
  TEST_EXPECT(reloaded_provider->env.at("EXAMPLE_URL") == "http://127.0.0.1:9999/v1");
}

} // namespace

int main() {
  TestFromStringsIsFlat();
  TestFromStringMapIsFlat();
  TestEmptyObjectIsObject();
  TestCoreConfigRoundTrip();
  std::cout << "test_json_access: all assertions passed\n";
  return 0;
}
