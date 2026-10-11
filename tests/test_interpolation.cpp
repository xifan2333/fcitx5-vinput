#include <cassert>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "common/utils/interpolation.h"

#include "daemon/postprocess/prompt_template.h"

void TestHasInterpolation() {
  using vinput::interpolate::HasInterpolation;

  assert(!HasInterpolation(""));
  assert(!HasInterpolation("plain text"));
  assert(!HasInterpolation("single $ sign"));
  assert(!HasInterpolation("single { brace }"));
  assert(HasInterpolation("${VAR}"));
  assert(HasInterpolation("prefix ${VAR} suffix"));
  assert(HasInterpolation("{{asr}}"));
  assert(HasInterpolation("prefix {{env.KEY}} suffix"));
}

void TestInterpolateCustomResolver() {
  using vinput::interpolate::Interpolate;
  using vinput::interpolate::Placeholder;

  auto resolver = [](const Placeholder& p) -> std::optional<std::string> {
    if (p.scheme == "custom" && p.key == "foo") {
      return "bar";
    }
    if (p.scheme.empty() && p.key == "name") {
      return "Alice";
    }
    return std::nullopt; // keep verbatim
  };

  assert(Interpolate("Hello {{name}}!", resolver) == "Hello Alice!");
  assert(Interpolate("Custom {{custom.foo}}!", resolver) == "Custom bar!");
  assert(Interpolate("Custom {{custom:foo}}!", resolver) == "Custom bar!");
  assert(Interpolate("Trim {{   name   }}!", resolver) == "Trim Alice!");
  assert(Interpolate("Unknown {{unknown}}!", resolver) == "Unknown {{unknown}}!");
  assert(Interpolate("No placeholder", resolver) == "No placeholder");
  assert(Interpolate("", resolver) == "");
}

void TestInterpolateEnv() {
  using vinput::interpolate::InterpolateEnv;

  setenv("VINPUT_TEST_VAR_A", "value_a", 1);
  setenv("VINPUT_TEST_VAR_B", "12345", 1);
  unsetenv("VINPUT_TEST_UNSET_VAR");

  // Dollar syntax
  assert(InterpolateEnv("${VINPUT_TEST_VAR_A}") == "value_a");
  assert(InterpolateEnv("Bearer ${VINPUT_TEST_VAR_A}") == "Bearer value_a");
  assert(InterpolateEnv("${VINPUT_TEST_VAR_A}_${VINPUT_TEST_VAR_B}") == "value_a_12345");

  // Dot syntax and colon syntax
  assert(InterpolateEnv("{{env.VINPUT_TEST_VAR_A}}") == "value_a");
  assert(InterpolateEnv("{{env:VINPUT_TEST_VAR_B}}") == "12345");
  assert(InterpolateEnv("{{  env.VINPUT_TEST_VAR_A  }}") == "value_a");

  // Unset environment variable resolves to empty string
  assert(InterpolateEnv("${VINPUT_TEST_UNSET_VAR}") == "");
  assert(InterpolateEnv("prefix_${VINPUT_TEST_UNSET_VAR}_suffix") == "prefix__suffix");

  // Non-env placeholders are preserved verbatim
  assert(InterpolateEnv("{{asr}}") == "{{asr}}");
  assert(InterpolateEnv("{{other.key}}") == "{{other.key}}");

  // Plain strings remain unchanged
  assert(InterpolateEnv("plain_api_key_sk_1234") == "plain_api_key_sk_1234");
}

void TestPromptTemplateIntegration() {
  vinput::prompt_template::Vars vars{
      .asr = "recognized voice command",
      .selected = "selected editor text",
      .context = "recent typing history",
  };

  // Basic prompt template interpolation
  const std::string tpl = "Instruction: {{asr}}\nInput: {{selected}}\nHistory: {{context}}";
  const std::string expected =
      "Instruction: recognized voice command\nInput: selected editor text\nHistory: recent typing "
      "history";
  assert(vinput::prompt_template::Interpolate(tpl, vars) == expected);

  // Whitespace tolerance in prompt templates
  assert(vinput::prompt_template::Interpolate("{{  asr  }}", vars) == "recognized voice command");

  // Unknown tags and bash dollar syntax in prompts are preserved verbatim
  assert(vinput::prompt_template::Interpolate("Write bash: echo \"${USER}\"", vars) ==
         "Write bash: echo \"${USER}\"");
  assert(vinput::prompt_template::Interpolate("Tag: {{unknown_tag}}", vars) ==
         "Tag: {{unknown_tag}}");
}

int main() {
  TestHasInterpolation();
  TestInterpolateCustomResolver();
  TestInterpolateEnv();
  TestPromptTemplateIntegration();

  std::cout << "All interpolation tests passed successfully!\n";
  return 0;
}
