#include <cstdlib>
#include <glaze/glaze.hpp>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/json/json_access.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

std::string ToJson(const glz::json_t& value) {
  std::string out;
  Expect(!bool(glz::write_json(value, out)), "glz::json_t must serialize to JSON");
  return out;
}

// `FromStrings` must produce `["a","b"]`, never the double wrapped `[["a","b"]]`
// that a `return {std::move(array)}` initializer list would yield.
void TestFromStringsIsFlat() {
  const auto value = vinput::json::FromStrings(std::vector<std::string>{"a", "b"});
  Expect(value.is_array(), "FromStrings must produce a JSON array");
  Expect(ToJson(value) == R"(["a","b"])", "FromStrings must not double wrap the array");
  Expect(vinput::json::Strings(value) == std::vector<std::string>({"a", "b"}),
         "FromStrings output must round-trip through Strings()");
}

// `FromStringMap` must produce `{"k":"v"}`, never the double wrapped `[{"k":"v"}]`.
void TestFromStringMapIsFlat() {
  const auto value = vinput::json::FromStringMap(std::map<std::string, std::string>{{"k", "v"}});
  Expect(value.is_object(), "FromStringMap must produce a JSON object");
  Expect(ToJson(value) == R"({"k":"v"})", "FromStringMap must not wrap the object in an array");
  const auto round_tripped = vinput::json::StringMap(value);
  Expect(round_tripped.size() == 1 && round_tripped.at("k") == "v",
         "FromStringMap output must round-trip through StringMap()");
}

// Guards the user facing regression: every save/reload cycle used to strip
// `registry.base_urls` and the command provider `args` / `env` because they were
// written double wrapped and then read back as empty.
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
  Expect(CoreConfigFromJson(kConfigText, &config, &error), "flat config must parse");
  Expect(config.registry.baseUrls.size() == 1, "flat base_urls must parse");
  Expect(config.asr.providers.size() == 1, "flat provider must parse");

  const auto* provider = std::get_if<CommandAsrProvider>(&config.asr.providers.front());
  Expect(provider != nullptr, "command provider must parse as CommandAsrProvider");
  Expect(provider->args.size() == 2, "flat args must parse");
  Expect(provider->env.size() == 2, "flat env must parse");

  const std::string serialized = CoreConfigToJson(config);
  Expect(serialized.find("\"base_urls\": [\n") != std::string::npos,
         "CoreConfigToJson must not double wrap base_urls");
  Expect(serialized.find("\"/path/to/provider\"") != std::string::npos,
         "CoreConfigToJson must not double wrap provider args");
  Expect(serialized.find("\"EXAMPLE_API_KEY\": \"secret\"") != std::string::npos,
         "CoreConfigToJson must not wrap provider env in an array");

  CoreConfig reloaded;
  Expect(CoreConfigFromJson(serialized, &reloaded, &error),
         "serialized config must parse after a save/reload cycle");
  Expect(reloaded.registry.baseUrls.size() == 1, "base_urls must survive a save cycle");
  const auto* reloaded_provider = std::get_if<CommandAsrProvider>(&reloaded.asr.providers.front());
  Expect(reloaded_provider != nullptr, "provider must survive a save cycle");
  Expect(reloaded_provider->args.size() == 2, "args must survive a save cycle");
  Expect(reloaded_provider->env.size() == 2, "env must survive a save cycle");
}

} // namespace

int main() {
  TestFromStringsIsFlat();
  TestFromStringMapIsFlat();
  TestCoreConfigRoundTrip();
  std::cout << "test_json_access: all assertions passed\n";
  return 0;
}
