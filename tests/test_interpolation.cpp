#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "common/utils/interpolation.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

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

  const char* path_val = std::getenv("PATH");
  if (path_val != nullptr) {
    const std::string path_str(path_val);
    // Dollar syntax
    ExpectEq(InterpolateEnv("${PATH}"), path_str);
    ExpectEq(InterpolateEnv("bin:${PATH}"), "bin:" + path_str);

    // Dot syntax and colon syntax
    ExpectEq(InterpolateEnv("{{env.PATH}}"), path_str);
    ExpectEq(InterpolateEnv("{{env:PATH}}"), path_str);
    ExpectEq(InterpolateEnv("{{  env.PATH  }}"), path_str);
  }

  // Unset environment variable resolves to empty string
  ExpectEq(InterpolateEnv("${VINPUT_DEFINITELY_UNSET_VAR_12345}"), "");
  ExpectEq(InterpolateEnv("prefix_${VINPUT_DEFINITELY_UNSET_VAR_12345}_suffix"), "prefix__suffix");

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

  // Malformed scheme tags with empty scheme prefix must not be expanded
  ExpectEq(vinput::prompt_template::Interpolate("Tag: {{.asr}}", vars), "Tag: {{.asr}}");
  ExpectEq(vinput::prompt_template::Interpolate("Tag: {{:selected}}", vars), "Tag: {{:selected}}");
}

void TestPerformanceUnmatchedTokens() {
  using vinput::interpolate::InterpolateEnv;

  // Unmatched opening markers should complete linearly without quadratic stall
  std::string malformed_braces;
  malformed_braces.reserve(10000);
  for (int i = 0; i < 5000; ++i) {
    malformed_braces.append("{{");
  }
  ExpectEq(InterpolateEnv(malformed_braces), malformed_braces);

  std::string malformed_dollar;
  malformed_dollar.reserve(10000);
  for (int i = 0; i < 5000; ++i) {
    malformed_dollar.append("${");
  }
  ExpectEq(InterpolateEnv(malformed_dollar), malformed_dollar);
}

void TestProcessEnvInterpolation() {
  const char* path_val = std::getenv("PATH");
  if (path_val == nullptr) {
    return;
  }

  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", "echo $TEST_VAR"};
  spec.env["TEST_VAR"] = "${PATH}";
  spec.timeout_ms = 5000;

  const vinput::process::CommandResult res = vinput::process::Run(spec, {});
  ExpectTrue(res.exit_code == 0);
  ExpectEq(std::string(vinput::str::Trim(res.stdout_text)), std::string(path_val));
}

} // namespace

int main() {
  TestHasInterpolation();
  TestInterpolateCustomResolver();
  TestInterpolateEnv();
  TestPromptTemplateIntegration();
  TestPerformanceUnmatchedTokens();
  TestProcessEnvInterpolation();

  std::cout << "All interpolation tests passed successfully!\n";
  return 0;
}
