#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "common/asr/recognition_result.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << '\n';
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestRoundTrip() {
  using namespace vinput::result;

  const Payload original{
      .commitText = "Hello world",
      .candidates =
          {
              Candidate{.text = "Hello world", .source = kSourceRaw},
              Candidate{.text = "Hello World!", .source = kSourceLlm},
          },
  };

  const std::string json_str = Serialize(original);
  TEST_ASSERT(!json_str.empty());

  const Payload parsed = Parse(json_str);
  TEST_ASSERT(parsed.commitText == original.commitText);
  TEST_ASSERT(parsed.candidates.size() == 2);
  TEST_ASSERT(parsed.candidates[0].text == "Hello world");
  TEST_ASSERT(parsed.candidates[0].source == kSourceRaw);
  TEST_ASSERT(parsed.candidates[1].text == "Hello World!");
  TEST_ASSERT(parsed.candidates[1].source == kSourceLlm);
}

void TestEmptyAndInvalid() {
  using namespace vinput::result;

  // Empty string
  TEST_ASSERT(Parse("").commitText.empty());
  TEST_ASSERT(Parse("").candidates.empty());

  // Whitespace only
  TEST_ASSERT(Parse("   \t\n  ").commitText.empty());

  // Empty JSON object
  TEST_ASSERT(Parse("{}").commitText.empty());

  // Malformed JSON syntax
  TEST_ASSERT(Parse("{malformed: json").commitText.empty());
  TEST_ASSERT(Parse("{\"commit_text\":").commitText.empty());

  // Non-object JSON primitives
  TEST_ASSERT(Parse("12345").commitText.empty());
  TEST_ASSERT(Parse("\"string primitive\"").commitText.empty());
  TEST_ASSERT(Parse("[1, 2, 3]").commitText.empty());
  TEST_ASSERT(Parse("null").commitText.empty());
  TEST_ASSERT(Parse("true").commitText.empty());

  // Trailing garbage after valid JSON object (rejected by force_conformance)
  TEST_ASSERT(Parse(R"({"commit_text":"valid"} trailing)").commitText.empty());
  TEST_ASSERT(Parse(R"({"commit_text":"valid"} 12345)").commitText.empty());
}

void TestFallbackCommitTextFromCandidates() {
  using namespace vinput::result;

  // commit_text is omitted; should fallback to first candidate's text
  const std::string_view json_str =
      R"({"candidates":[{"text":"primary candidate","source":"asr"},{"text":"second","source":"llm"}]})";
  const Payload parsed = Parse(json_str);
  TEST_ASSERT(parsed.commitText == "primary candidate");
  TEST_ASSERT(parsed.candidates.size() == 2);
  TEST_ASSERT(parsed.candidates[0].text == "primary candidate");
  TEST_ASSERT(parsed.candidates[0].source == "asr");
}

void TestFallbackCandidatesFromCommitText() {
  using namespace vinput::result;

  // candidates is omitted; should fallback to single candidate with commitText and kSourceRaw
  const std::string_view json_str = R"({"commit_text":"direct commit"})";
  const Payload parsed = Parse(json_str);
  TEST_ASSERT(parsed.commitText == "direct commit");
  TEST_ASSERT(parsed.candidates.size() == 1);
  TEST_ASSERT(parsed.candidates[0].text == "direct commit");
  TEST_ASSERT(parsed.candidates[0].source == kSourceRaw);
}

void TestCandidateFiltering() {
  using namespace vinput::result;

  // Candidates with empty text must be filtered out
  const std::string_view json_str =
      R"({"commit_text":"valid text","candidates":[{"text":"","source":"raw"},{"text":"valid text","source":"asr"},{"text":"","source":"llm"}]})";
  const Payload parsed = Parse(json_str);
  TEST_ASSERT(parsed.commitText == "valid text");
  TEST_ASSERT(parsed.candidates.size() == 1);
  TEST_ASSERT(parsed.candidates[0].text == "valid text");
  TEST_ASSERT(parsed.candidates[0].source == "asr");
}

void TestUnknownKeysTolerance() {
  using namespace vinput::result;

  // Forward-compatibility with unknown root fields and candidate fields
  const std::string_view json_str =
      R"({"version":2,"commit_text":"futuristic","extra_field":"ignored","candidates":[{"text":"futuristic","source":"raw","confidence":0.99}]})";
  const Payload parsed = Parse(json_str);
  TEST_ASSERT(parsed.commitText == "futuristic");
  TEST_ASSERT(parsed.candidates.size() == 1);
  TEST_ASSERT(parsed.candidates[0].text == "futuristic");
  TEST_ASSERT(parsed.candidates[0].source == "raw");
}

void TestNullFieldsTolerance() {
  using namespace vinput::result;

  // Null commit_text with valid candidates
  const std::string_view json_null_commit =
      R"({"commit_text":null,"candidates":[{"text":"from_candidate","source":null}]})";
  const Payload parsed_commit = Parse(json_null_commit);
  TEST_ASSERT(parsed_commit.commitText == "from_candidate");
  TEST_ASSERT(parsed_commit.candidates.size() == 1);
  TEST_ASSERT(parsed_commit.candidates[0].text == "from_candidate");
  TEST_ASSERT(parsed_commit.candidates[0].source.empty());

  // Null candidates with valid commit_text
  const std::string_view json_null_cand = R"({"commit_text":"sole_text","candidates":null})";
  const Payload parsed_cand = Parse(json_null_cand);
  TEST_ASSERT(parsed_cand.commitText == "sole_text");
  TEST_ASSERT(parsed_cand.candidates.size() == 1);
  TEST_ASSERT(parsed_cand.candidates[0].source == kSourceRaw);

  // Both null
  const std::string_view json_both_null = R"({"commit_text":null,"candidates":null})";
  const Payload parsed_both = Parse(json_both_null);
  TEST_ASSERT(parsed_both.commitText.empty());
  TEST_ASSERT(parsed_both.candidates.empty());
}

} // namespace

int main() {
  TestRoundTrip();
  TestEmptyAndInvalid();
  TestFallbackCommitTextFromCandidates();
  TestFallbackCandidatesFromCommitText();
  TestCandidateFiltering();
  TestUnknownKeysTolerance();
  TestNullFieldsTolerance();

  std::cout << "All recognition_result tests passed successfully.\n";
  return 0;
}
