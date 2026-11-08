#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/utils/string_utils.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestTrim() {
  using vinput::str::Trim;

  // Empty string
  TEST_ASSERT(Trim("").empty());

  // Pure whitespace
  TEST_ASSERT(Trim("   \t\r\n ").empty());

  // Leading and trailing whitespace
  TEST_ASSERT(Trim("  hello world  \n") == "hello world");
  TEST_ASSERT(Trim("\t\tvinput\r") == "vinput");

  // No whitespace to trim
  TEST_ASSERT(Trim("abc") == "abc");

  // Zero-copy check: returned string_view points inside original buffer
  const std::string_view original = "  inner text  ";
  const std::string_view trimmed = Trim(original);
  TEST_ASSERT(trimmed == "inner text");
  TEST_ASSERT(trimmed.data() == original.data() + 2);

  // Owning copy is built by the caller
  const std::string string_res(Trim("  to string test  "));
  TEST_ASSERT(string_res == "to string test");
}

void TestEqualsIgnoreCase() {
  using vinput::str::EqualsIgnoreCase;

  TEST_ASSERT(EqualsIgnoreCase("", ""));
  TEST_ASSERT(EqualsIgnoreCase("abc", "ABC"));
  TEST_ASSERT(EqualsIgnoreCase("VInput-Daemon", "vinput-daemon"));
  TEST_ASSERT(!EqualsIgnoreCase("abc", "abcd"));
  TEST_ASSERT(!EqualsIgnoreCase("abcd", "abc"));
  TEST_ASSERT(!EqualsIgnoreCase("hello", "world"));
}

void TestSplit() {
  using vinput::str::Split;

  // Single segment
  const auto res1 = Split("hello", '.');
  TEST_ASSERT(res1.size() == 1 && res1[0] == "hello");

  // Multiple segments
  const auto res2 = Split("a.b.c", '.');
  TEST_ASSERT(res2.size() == 3 && res2[0] == "a" && res2[1] == "b" && res2[2] == "c");

  // Leading, trailing, consecutive delimiters
  const auto res3 = Split("/usr/local/bin/", '/');
  TEST_ASSERT(res3.size() == 5);
  TEST_ASSERT(res3[0].empty());
  TEST_ASSERT(res3[1] == "usr");
  TEST_ASSERT(res3[2] == "local");
  TEST_ASSERT(res3[3] == "bin");
  TEST_ASSERT(res3[4].empty());

  // Empty string
  const auto res4 = Split("", ',');
  TEST_ASSERT(res4.size() == 1 && res4[0].empty());
}

void TestSplitLines() {
  using vinput::str::SplitLines;

  // Empty string
  const auto res1 = SplitLines("");
  TEST_ASSERT(res1.empty());

  // LF
  const auto res2 = SplitLines("line1\nline2\nline3");
  TEST_ASSERT(res2.size() == 3 && res2[0] == "line1" && res2[1] == "line2" && res2[2] == "line3");

  // CRLF
  const auto res3 = SplitLines("line1\r\nline2\r\n");
  TEST_ASSERT(res3.size() == 2 && res3[0] == "line1" && res3[1] == "line2");

  // Single line without newline
  const auto res4 = SplitLines("single");
  TEST_ASSERT(res4.size() == 1 && res4[0] == "single");
}

void TestStripSuffix() {
  using vinput::str::StripSuffix;

  // Normal suffix stripping
  TEST_ASSERT(StripSuffix("input.monitor", ".monitor") == "input");
  TEST_ASSERT(StripSuffix("filename.tar.gz", ".gz") == "filename.tar");

  // Non-matching suffix
  TEST_ASSERT(StripSuffix("input.sink", ".monitor") == "input.sink");
  TEST_ASSERT(StripSuffix("hello", "world") == "hello");

  // Boundary cases
  TEST_ASSERT(StripSuffix("message.", ".") == "message");
  TEST_ASSERT(StripSuffix("", "").empty());
  TEST_ASSERT(StripSuffix("", "suffix").empty());
  TEST_ASSERT(StripSuffix("text", "") == "text");
  TEST_ASSERT(StripSuffix("exact_match", "exact_match").empty());
  TEST_ASSERT(StripSuffix("short", "longer_suffix") == "short");
}

void TestConsumePrefix() {
  using vinput::str::ConsumePrefix;

  // Matching prefix consumes and advances pointer
  std::string_view sv1 = "ASR provider: err";
  TEST_ASSERT(ConsumePrefix(&sv1, "ASR provider: "));
  TEST_ASSERT(sv1 == "err");

  // Non-matching prefix returns false and keeps text untouched
  std::string_view sv2 = "worker err";
  TEST_ASSERT(!ConsumePrefix(&sv2, "ASR: "));
  TEST_ASSERT(sv2 == "worker err");

  // Null pointer safety
  TEST_ASSERT(!ConsumePrefix(nullptr, "ASR: "));

  // Empty prefix
  std::string_view sv3 = "unchanged";
  TEST_ASSERT(ConsumePrefix(&sv3, ""));
  TEST_ASSERT(sv3 == "unchanged");

  // Exact match consumes all
  std::string_view sv4 = "exact";
  TEST_ASSERT(ConsumePrefix(&sv4, "exact"));
  TEST_ASSERT(sv4.empty());
}

void TestSplitOnce() {
  using vinput::str::SplitOnce;

  // Normal char delimiter
  const auto pair1 = SplitOnce("KEY=VALUE", '=');
  TEST_ASSERT(pair1.has_value());
  if (pair1.has_value()) {
    TEST_ASSERT(pair1->first == "KEY" && pair1->second == "VALUE");
  }

  // No delimiter
  TEST_ASSERT(!SplitOnce("NO_DELIM", '=').has_value());
  TEST_ASSERT(!SplitOnce("", '=').has_value());

  // Leading and trailing delimiter
  const auto pair2 = SplitOnce("=VALUE", '=');
  TEST_ASSERT(pair2.has_value());
  if (pair2.has_value()) {
    TEST_ASSERT(pair2->first.empty() && pair2->second == "VALUE");
  }

  const auto pair3 = SplitOnce("KEY=", '=');
  TEST_ASSERT(pair3.has_value());
  if (pair3.has_value()) {
    TEST_ASSERT(pair3->first == "KEY" && pair3->second.empty());
  }

  // Multiple consecutive delimiters: split on first delimiter only
  const auto pair4 = SplitOnce("a=b=c", '=');
  TEST_ASSERT(pair4.has_value());
  if (pair4.has_value()) {
    TEST_ASSERT(pair4->first == "a" && pair4->second == "b=c");
  }
}

void TestRSplitOnce() {
  using vinput::str::RSplitOnce;

  // Split on rightmost delimiter
  const auto pair1 = RSplitOnce("a:b:c", ':');
  TEST_ASSERT(pair1.has_value());
  if (pair1.has_value()) {
    TEST_ASSERT(pair1->first == "a:b" && pair1->second == "c");
  }

  // No delimiter present
  TEST_ASSERT(!RSplitOnce("no_delim", ':').has_value());
  TEST_ASSERT(!RSplitOnce("", ':').has_value());
}

void TestJoin() {
  using vinput::str::Join;

  // std::vector<std::string>
  const std::vector<std::string> vec_str = {"apple", "banana", "cherry"};
  TEST_ASSERT(Join(vec_str, ", ") == "apple, banana, cherry");
  TEST_ASSERT(Join(vec_str, "") == "applebananacherry");

  // std::vector<std::string_view>
  const std::vector<std::string_view> vec_sv = {"line1", "line2", "line3"};
  TEST_ASSERT(Join(vec_sv, "\n") == "line1\nline2\nline3");

  // Empty container
  const std::vector<std::string> empty_vec;
  TEST_ASSERT(Join(empty_vec, ", ").empty());

  // Single element
  const std::vector<std::string> single_vec = {"hello"};
  TEST_ASSERT(Join(single_vec, ", ") == "hello");

  // std::set<std::string> (ordered)
  const std::set<std::string> set_str = {"a", "b", "c"};
  TEST_ASSERT(Join(set_str, "-") == "a-b-c");

  // std::span
  const std::string words[] = {"one", "two"};
  TEST_ASSERT(Join(std::span(words), " ") == "one two");

  // std::initializer_list
  TEST_ASSERT(Join({"alpha", "beta", "gamma"}, " | ") == "alpha | beta | gamma");
}

void TestJoinKeyValue() {
  using vinput::str::JoinKeyValue;

  // std::map<std::string, std::string>
  const std::map<std::string, std::string> env_map = {
      {"FOO", "1"},
      {"BAR", "2"},
  };
  TEST_ASSERT(JoinKeyValue(env_map, "\n", "=") == "BAR=2\nFOO=1");

  // Empty map
  const std::map<std::string, std::string> empty_map;
  TEST_ASSERT(JoinKeyValue(empty_map, "\n", "=").empty());

  // Single entry
  const std::map<std::string, std::string> single_map = {{"PORT", "8080"}};
  TEST_ASSERT(JoinKeyValue(single_map, "\n", "=") == "PORT=8080");

  // Empty key or empty value
  const std::vector<std::pair<std::string, std::string>> pair_list = {
      {"", "val"},
      {"key", ""},
  };
  TEST_ASSERT(JoinKeyValue(pair_list, "&", "=") == "=val&key=");
}

void TestTerminalEscapesAndDisplayWidth() {
  using vinput::str::CollapseWhitespace;
  using vinput::str::DisplayWidth;
  using vinput::str::FlattenLineBreaks;
  using vinput::str::StripTerminalEscapes;

  // FlattenLineBreaks: only flattens \r, \n, \t into single space, preserving intentional spaces
  TEST_ASSERT(FlattenLineBreaks("").empty());
  TEST_ASSERT(FlattenLineBreaks("hello world") == "hello world");
  TEST_ASSERT(FlattenLineBreaks("  code   block  ") == "  code   block  ");
  TEST_ASSERT(FlattenLineBreaks("Line 1\r\nLine 2\nLine 3\tTab") == "Line 1 Line 2 Line 3 Tab");

  // CollapseWhitespace: folds all whitespace runs into single space
  TEST_ASSERT(CollapseWhitespace("hello \t\r\n world") == "hello world");
  TEST_ASSERT(CollapseWhitespace("\n\t a \r\n b \t\n").find("a b") != std::string::npos);

  // StripTerminalEscapes: chunk-based stripping
  const std::string colored = "\033[1;32mHello\033[0m";
  TEST_ASSERT(StripTerminalEscapes(colored) == "Hello");

  const std::string link = "\033]8;;https://example.com\aClick\033]8;;\a";
  TEST_ASSERT(StripTerminalEscapes(link) == "Click");

  // DisplayWidth
  TEST_ASSERT(DisplayWidth("") == 0);
  TEST_ASSERT(DisplayWidth("abc") == 3);
  TEST_ASSERT(DisplayWidth("中文测试") == 8);
  TEST_ASSERT(DisplayWidth(colored) == 5);
  TEST_ASSERT(DisplayWidth(link) == 5);
}

} // namespace

int main() {
  TestTrim();
  TestEqualsIgnoreCase();
  TestSplit();
  TestSplitLines();
  TestStripSuffix();
  TestConsumePrefix();
  TestSplitOnce();
  TestRSplitOnce();
  TestJoin();
  TestJoinKeyValue();
  TestTerminalEscapesAndDisplayWidth();
  return 0;
}
