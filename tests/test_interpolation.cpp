#include <iostream>
#include <optional>
#include <stdlib.h>
#include <string>

#include "common/utils/interpolation.h"

#include "daemon/postprocess/prompt_template.h"

namespace {

void ExpectTrue(bool condition) {
  if (!condition) {
    std::cerr << "ExpectTrue failed!\n";
    std::abort();
  }
}

void ExpectEq(const std::string& actual, const std::string& expected) {
  if (actual != expected) {
    std::cerr << "ExpectEq failed: expected '" << expected << "', got '" << actual << "'\n";
    std::abort();
  }
}

} // namespace

void TestHasInterpolation() {
  using vinput::interpolate::HasInterpolation;

  ExpectTrue(!HasInterpolation(""));
  ExpectTrue(!HasInterpolation("plain text"));
  ExpectTrue(!HasInterpolation("single $ sign"));
  ExpectTrue(!HasInterpolation("single { brace }"));
  ExpectTrue(HasInterpolation("${VAR}"));
  ExpectTrue(HasInterpolation("prefix ${VAR} suffix"));
  ExpectTrue(HasInterpolation("{{asr}}"));
  ExpectTrue(HasInterpolation("prefix {{env.KEY}} suffix"));
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

  ExpectEq(Interpolate("Hello {{name}}!", resolver), "Hello Alice!");
  ExpectEq(Interpolate("Custom {{custom.foo}}!", resolver), "Custom bar!");
  ExpectEq(Interpolate("Custom {{custom:foo}}!", resolver), "Custom bar!");
  ExpectEq(Interpolate("Trim {{   name   }}!", resolver), "Trim Alice!");
  ExpectEq(Interpolate("Unknown {{unknown}}!", resolver), "Unknown {{unknown}}!");
  ExpectEq(Interpolate("No placeholder", resolver), "No placeholder");
  ExpectEq(Interpolate("", resolver), "");
}

void TestInterpolateEnv() {
  using vinput::interpolate::InterpolateEnv;

  setenv("VINPUT_TEST_VAR_A", "value_a", 1);
  setenv("VINPUT_TEST_VAR_B", "12345", 1);
  unsetenv("VINPUT_TEST_UNSET_VAR");

  // Dollar syntax
  ExpectEq(InterpolateEnv("${VINPUT_TEST_VAR_A}"), "value_a");
  ExpectEq(InterpolateEnv("Bearer ${VINPUT_TEST_VAR_A}"), "Bearer value_a");
  ExpectEq(InterpolateEnv("${VINPUT_TEST_VAR_A}_${VINPUT_TEST_VAR_B}"), "value_a_12345");

  // Dot syntax and colon syntax
  ExpectEq(InterpolateEnv("{{env.VINPUT_TEST_VAR_A}}"), "value_a");
  ExpectEq(InterpolateEnv("{{env:VINPUT_TEST_VAR_B}}"), "12345");
  ExpectEq(InterpolateEnv("{{  env.VINPUT_TEST_VAR_A  }}"), "value_a");

  // Unset environment variable resolves to empty string
  ExpectEq(InterpolateEnv("${VINPUT_TEST_UNSET_VAR}"), "");
  ExpectEq(InterpolateEnv("prefix_${VINPUT_TEST_UNSET_VAR}_suffix"), "prefix__suffix");

  // Non-env placeholders are preserved verbatim
  ExpectEq(InterpolateEnv("{{asr}}"), "{{asr}}");
  ExpectEq(InterpolateEnv("{{other.key}}"), "{{other.key}}");

  // Plain strings remain unchanged
  ExpectEq(InterpolateEnv("plain_api_key_sk_1234"), "plain_api_key_sk_1234");
}

void TestPromptTemplateIntegration() {
  const vinput::prompt_template::Vars vars{
      .asr = "recognized voice command",
      .selected = "selected editor text",
      .context = "recent typing history",
  };

  // Basic prompt template interpolation
  const std::string tpl = "Instruction: {{asr}}\nInput: {{selected}}\nHistory: {{context}}";
  const std::string expected =
      "Instruction: recognized voice command\nInput: selected editor text\nHistory: recent typing "
      "history";
  ExpectEq(vinput::prompt_template::Interpolate(tpl, vars), expected);

  // Whitespace tolerance in prompt templates
  ExpectEq(vinput::prompt_template::Interpolate("{{  asr  }}", vars), "recognized voice command");

  // Unknown tags and bash dollar syntax in prompts are preserved verbatim
  ExpectEq(vinput::prompt_template::Interpolate("Write bash: echo \"${USER}\"", vars),
           "Write bash: echo \"${USER}\"");
  ExpectEq(vinput::prompt_template::Interpolate("Tag: {{unknown_tag}}", vars),
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
