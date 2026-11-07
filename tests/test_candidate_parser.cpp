#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "daemon/postprocess/candidate_parser.h"

namespace {

void Expect(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << " at " << file << ":" << line << '\n';
    std::abort();
  }
}

#define TEST_EXPECT(cond) Expect((cond), #cond, __FILE__, __LINE__)

void TestEmptyAndInvalidInput() {
  using vinput::postprocess::ExtractCandidatesAuto;

  TEST_EXPECT(ExtractCandidatesAuto("", 3).empty());
  TEST_EXPECT(ExtractCandidatesAuto("   \n\t  \n", 3).empty());
  TEST_EXPECT(ExtractCandidatesAuto("valid text", 0).empty());
  TEST_EXPECT(ExtractCandidatesAuto("valid text", -1).empty());
}

void TestJsonObjectCandidates() {
  using vinput::postprocess::ExtractCandidatesAuto;

  constexpr const char* kJson = R"({
    "candidates": ["Option 1", "  Option 2  ", "", "Option 3"]
  })";

  const auto result = ExtractCandidatesAuto(kJson, 5);
  TEST_EXPECT(result.size() == 3);
  TEST_EXPECT(result[0] == "Option 1");
  TEST_EXPECT(result[1] == "Option 2");
  TEST_EXPECT(result[2] == "Option 3");

  // Truncation at max_candidates
  const auto truncated = ExtractCandidatesAuto(kJson, 2);
  TEST_EXPECT(truncated.size() == 2);
  TEST_EXPECT(truncated[0] == "Option 1");
  TEST_EXPECT(truncated[1] == "Option 2");
}

void TestMarkdownFencedJson() {
  using vinput::postprocess::ExtractCandidatesAuto;

  constexpr const char* kFencedJson = "```json\n"
                                      "{\"candidates\": [\"Cand A\", \"Cand B\"]}\n"
                                      "```";

  const auto result = ExtractCandidatesAuto(kFencedJson, 2);
  TEST_EXPECT(result.size() == 2);
  TEST_EXPECT(result[0] == "Cand A");
  TEST_EXPECT(result[1] == "Cand B");
}

void TestSingleCandidatePlain() {
  using vinput::postprocess::ExtractCandidatesAuto;

  const auto result = ExtractCandidatesAuto("  Direct rewritten text.  ", 1);
  TEST_EXPECT(result.size() == 1);
  TEST_EXPECT(result[0] == "Direct rewritten text.");

  // Single candidate with markdown fence
  const auto fenced = ExtractCandidatesAuto("```text\nDirect in fence\n```", 1);
  TEST_EXPECT(fenced.size() == 1);
  TEST_EXPECT(fenced[0] == "Direct in fence");
}

void TestLineStreamCandidates() {
  using vinput::postprocess::ExtractCandidatesAuto;

  constexpr const char* kLines = "First candidate\n"
                                 "\n"
                                 "Second candidate\n"
                                 "   Third candidate   \n";

  const auto result = ExtractCandidatesAuto(kLines, 3);
  TEST_EXPECT(result.size() == 3);
  TEST_EXPECT(result[0] == "First candidate");
  TEST_EXPECT(result[1] == "Second candidate");
  TEST_EXPECT(result[2] == "Third candidate");

  // Truncation
  const auto truncated = ExtractCandidatesAuto(kLines, 2);
  TEST_EXPECT(truncated.size() == 2);
  TEST_EXPECT(truncated[0] == "First candidate");
  TEST_EXPECT(truncated[1] == "Second candidate");
}

void TestBulletedAndNumberedList() {
  using vinput::postprocess::ExtractCandidatesAuto;

  constexpr const char* kBulletList = "- Option One\n"
                                      "* Option Two\n"
                                      "-   Option Three  \n";

  const auto result = ExtractCandidatesAuto(kBulletList, 3);
  TEST_EXPECT(result.size() == 3);
  TEST_EXPECT(result[0] == "Option One");
  TEST_EXPECT(result[1] == "Option Two");
  TEST_EXPECT(result[2] == "Option Three");

  constexpr const char* kNumberedList = "1. Item One\n"
                                        "2. Item Two\n"
                                        "10. Item Ten\n";

  const auto num_result = ExtractCandidatesAuto(kNumberedList, 3);
  TEST_EXPECT(num_result.size() == 3);
  TEST_EXPECT(num_result[0] == "Item One");
  TEST_EXPECT(num_result[1] == "Item Two");
  TEST_EXPECT(num_result[2] == "Item Ten");
}

} // namespace

int main() {
  TestEmptyAndInvalidInput();
  TestJsonObjectCandidates();
  TestMarkdownFencedJson();
  TestSingleCandidatePlain();
  TestLineStreamCandidates();
  TestBulletedAndNumberedList();
  std::cout << "test_candidate_parser: all assertions passed\n";
  return 0;
}
