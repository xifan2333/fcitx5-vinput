#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <utility>
#include <vector>

#include "common/utils/string_utils.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestTrimAsciiWhitespace() {
  using vinput::str::TrimAsciiWhitespace;
  using vinput::str::TrimAsciiWhitespaceView;

  // Empty string
  TEST_ASSERT(TrimAsciiWhitespace("").empty());
  TEST_ASSERT(TrimAsciiWhitespaceView("").empty());

  // Pure whitespace
  TEST_ASSERT(TrimAsciiWhitespace("   \t\r\n ").empty());
  TEST_ASSERT(TrimAsciiWhitespaceView("   \t\r\n ").empty());

  // Leading and trailing whitespace
  TEST_ASSERT(TrimAsciiWhitespace("  hello world  \n") == "hello world");
  TEST_ASSERT(TrimAsciiWhitespaceView("  hello world  \n") == "hello world");
  TEST_ASSERT(TrimAsciiWhitespace("\t\tvinput\r") == "vinput");

  // No whitespace to trim
  TEST_ASSERT(TrimAsciiWhitespace("abc") == "abc");
  TEST_ASSERT(TrimAsciiWhitespaceView("abc") == "abc");

  // Zero-copy check: returned string_view points inside original buffer
  const std::string_view original = "  inner text  ";
  const std::string_view trimmed = TrimAsciiWhitespaceView(original);
  TEST_ASSERT(trimmed == "inner text");
  TEST_ASSERT(trimmed.data() == original.data() + 2);

  // Return std::string check
  const std::string string_res = TrimAsciiWhitespace("  to string test  ");
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

void TestSplitByChar() {
  using vinput::str::SplitByChar;

  // Single segment
  const auto res1 = SplitByChar("hello", '.');
  TEST_ASSERT(res1.size() == 1 && res1[0] == "hello");

  // Multiple segments
  const auto res2 = SplitByChar("a.b.c", '.');
  TEST_ASSERT(res2.size() == 3 && res2[0] == "a" && res2[1] == "b" && res2[2] == "c");

  // Leading, trailing, consecutive delimiters
  const auto res3 = SplitByChar("/usr/local/bin/", '/');
  TEST_ASSERT(res3.size() == 5);
  TEST_ASSERT(res3[0].empty());
  TEST_ASSERT(res3[1] == "usr");
  TEST_ASSERT(res3[2] == "local");
  TEST_ASSERT(res3[3] == "bin");
  TEST_ASSERT(res3[4].empty());

  // Empty string
  const auto res4 = SplitByChar("", ',');
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

void TestSplitPair() {
  using vinput::str::SplitPair;

  // Normal char delimiter
  const auto pair1 = SplitPair("KEY=VALUE", '=');
  TEST_ASSERT(pair1.has_value());
  if (pair1.has_value()) {
    TEST_ASSERT(pair1->first == "KEY" && pair1->second == "VALUE");
  }

  // Delimiter with whitespace and trim_whitespace = true
  const auto pair2 = SplitPair("  host : 127.0.0.1\t", ':', /*trim_whitespace=*/true);
  TEST_ASSERT(pair2.has_value());
  if (pair2.has_value()) {
    TEST_ASSERT(pair2->first == "host" && pair2->second == "127.0.0.1");
  }

  // Delimiter with whitespace and trim_whitespace = false
  const auto pair3 = SplitPair("  host : 127.0.0.1\t", ':', /*trim_whitespace=*/false);
  TEST_ASSERT(pair3.has_value());
  if (pair3.has_value()) {
    TEST_ASSERT(pair3->first == "  host " && pair3->second == " 127.0.0.1\t");
  }

  // No delimiter
  TEST_ASSERT(!SplitPair("NO_DELIM", '=').has_value());
  TEST_ASSERT(!SplitPair("", '=').has_value());

  // Leading and trailing delimiter
  const auto pair4 = SplitPair("=VALUE", '=');
  TEST_ASSERT(pair4.has_value());
  if (pair4.has_value()) {
    TEST_ASSERT(pair4->first.empty() && pair4->second == "VALUE");
  }

  const auto pair5 = SplitPair("KEY=", '=');
  TEST_ASSERT(pair5.has_value());
  if (pair5.has_value()) {
    TEST_ASSERT(pair5->first == "KEY" && pair5->second.empty());
  }

  // Multiple consecutive delimiters: split on first delimiter only
  const auto pair6 = SplitPair("a=b=c", '=');
  TEST_ASSERT(pair6.has_value());
  if (pair6.has_value()) {
    TEST_ASSERT(pair6->first == "a" && pair6->second == "b=c");
  }

  // String_view delimiter
  const auto pair7 = SplitPair("key::value", "::");
  TEST_ASSERT(pair7.has_value());
  if (pair7.has_value()) {
    TEST_ASSERT(pair7->first == "key" && pair7->second == "value");
  }

  // Empty string_view delimiter -> nullopt
  TEST_ASSERT(!SplitPair("key::value", "").has_value());

  // Non-matching string_view delimiter
  TEST_ASSERT(!SplitPair("key:value", "::").has_value());

  // String_view delimiter with trim
  const auto pair8 = SplitPair(" key --> value ", "-->", /*trim_whitespace=*/true);
  TEST_ASSERT(pair8.has_value());
  if (pair8.has_value()) {
    TEST_ASSERT(pair8->first == "key" && pair8->second == "value");
  }
}

void TestSplitPairRight() {
  using vinput::str::SplitPairRight;

  // Split on rightmost delimiter
  const auto pair1 = SplitPairRight("a:b:c", ':');
  TEST_ASSERT(pair1.has_value());
  if (pair1.has_value()) {
    TEST_ASSERT(pair1->first == "a:b" && pair1->second == "c");
  }

  // Hotword score pattern
  const auto pair2 = SplitPairRight("alpha:beta:3.5", ':');
  TEST_ASSERT(pair2.has_value());
  if (pair2.has_value()) {
    TEST_ASSERT(pair2->first == "alpha:beta" && pair2->second == "3.5");
  }

  // Trim whitespace
  const auto pair3 = SplitPairRight("  foo:bar  :  10  ", ':', /*trim_whitespace=*/true);
  TEST_ASSERT(pair3.has_value());
  if (pair3.has_value()) {
    TEST_ASSERT(pair3->first == "foo:bar" && pair3->second == "10");
  }

  // Without trim whitespace
  const auto pair4 = SplitPairRight("  foo:bar  :  10  ", ':', /*trim_whitespace=*/false);
  TEST_ASSERT(pair4.has_value());
  if (pair4.has_value()) {
    TEST_ASSERT(pair4->first == "  foo:bar  " && pair4->second == "  10  ");
  }

  // No delimiter present
  TEST_ASSERT(!SplitPairRight("no_delim", ':').has_value());
  TEST_ASSERT(!SplitPairRight("", ':').has_value());
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

namespace {
void TestFmtStr() {
  using vinput::str::FmtStr;
  using vinput::str::FormatSize;

  // 1. Basic formatting: nullptr, no args, integers, floats, string literals
  TEST_ASSERT(FmtStr(nullptr).empty());
  TEST_ASSERT(FmtStr("hello world") == "hello world");
  TEST_ASSERT(FmtStr("Number: %d, Float: %.2f", 42, 3.14159) == "Number: 42, Float: 3.14");
  TEST_ASSERT(FmtStr("Literal: %s", "constant") == "Literal: constant");

  // FormatSize checks
  TEST_ASSERT(FormatSize(500) == "500 B");
  TEST_ASSERT(FormatSize(1024) == "1.0 KB");
  TEST_ASSERT(FormatSize(1024ULL * 1024) == "1.0 MB");
  TEST_ASSERT(FormatSize(1024ULL * 1024 * 1024 * 2) == "2.0 GB");

  // 2. std::string arguments (lvalue reference, temporary/rvalue)
  const std::string s_lvalue = "my_string";
  TEST_ASSERT(FmtStr("String: %s", s_lvalue) == "String: my_string");
  TEST_ASSERT(FmtStr("Temp string: %s", std::string("temporary")) == "Temp string: temporary");

  // 3. std::string_view normal arguments
  const std::string_view sv_literal = "literal_view";
  TEST_ASSERT(FmtStr("View: %s", sv_literal) == "View: literal_view");

  // 4. Non null-terminated std::string_view slices (critical SIGSEGV & memory safety regression
  // check)
  const std::string base = "hello world from fcitx5";
  const std::string_view slice(base.data(), 5); // "hello", followed by ' ', NOT null-terminated!
  TEST_ASSERT(FmtStr("Result: [%s] Number: %d", slice, 42) == "Result: [hello] Number: 42");

  const std::string_view mid_slice(base.data() + 6, 5); // "world"
  TEST_ASSERT(FmtStr("[%s]-[%s]", slice, mid_slice) == "[hello]-[world]");

  const std::string_view empty_view{};
  TEST_ASSERT(FmtStr("Empty: [%s]", empty_view) == "Empty: []");
  TEST_ASSERT(FmtStr("Direct default view: [%s]", std::string_view{}) == "Direct default view: []");

  // 5. Interleaved multiple string_views, strings, numbers, and pointers (stack & register ABI
  // alignment check)
  const std::string text = "alpha_beta_gamma";
  const std::string_view p1(text.data(), 5);      // "alpha"
  const std::string_view p2(text.data() + 6, 4);  // "beta"
  const std::string_view p3(text.data() + 11, 5); // "gamma"
  const std::string mixed_str = "middle";

  const std::string combined = FmtStr("%s-%d-%s-%s-%d-%s", p1, 10, mixed_str, p2, 20, p3);
  TEST_ASSERT(combined == "alpha-10-middle-beta-20-gamma");
}

namespace {

size_t DisplayWidth(std::string_view text) {
  std::u32string u32;
  unicode::utf8::decode(text.data(), text.size(), u32);
  return static_cast<size_t>(std::max(0, unicode::width(u32, unicode::AmbiguousWidth::Wide)));
}

} // namespace

void TestCjkBoundaryAndSentenceTerminal() {
  using vinput::str::FirstUtf8Codepoint;
  using vinput::str::IsCjkCodepoint;
  using vinput::str::IsSentenceEndingPunctuation;
  using vinput::str::LastUtf8Codepoint;

  // First / Last codepoint
  TEST_ASSERT(FirstUtf8Codepoint("") == 0);
  TEST_ASSERT(FirstUtf8Codepoint("hello") == 'h');
  TEST_ASSERT(FirstUtf8Codepoint("你好") == 0x4F60);
  TEST_ASSERT(LastUtf8Codepoint("") == 0);
  TEST_ASSERT(LastUtf8Codepoint("hello") == 'o');
  TEST_ASSERT(LastUtf8Codepoint("你好") == 0x597D);

  // CJK Codepoint check
  TEST_ASSERT(IsCjkCodepoint(0x4F60)); // '你'
  TEST_ASSERT(IsCjkCodepoint(0x597D)); // '好'
  TEST_ASSERT(IsCjkCodepoint(0x4E2D)); // '中'
  TEST_ASSERT(IsCjkCodepoint(0x6587)); // '文'
  TEST_ASSERT(!IsCjkCodepoint('a'));
  TEST_ASSERT(!IsCjkCodepoint('1'));
  TEST_ASSERT(!IsCjkCodepoint(0x1F680));

  // Sentence ending punctuation check
  TEST_ASSERT(IsSentenceEndingPunctuation(0x3002)); // '。'
  TEST_ASSERT(IsSentenceEndingPunctuation(0xFF01)); // '！'
  TEST_ASSERT(IsSentenceEndingPunctuation(0xFF1F)); // '？'
  TEST_ASSERT(IsSentenceEndingPunctuation(0x2026)); // '…'
  TEST_ASSERT(IsSentenceEndingPunctuation('.'));
  TEST_ASSERT(IsSentenceEndingPunctuation('!'));
  TEST_ASSERT(IsSentenceEndingPunctuation('?'));
  TEST_ASSERT(IsSentenceEndingPunctuation('\n'));
  TEST_ASSERT(!IsSentenceEndingPunctuation(','));
  TEST_ASSERT(!IsSentenceEndingPunctuation(0xFF0C)); // '，'
}

void TestTruncateMiddleUtf8() {
  using vinput::str::TruncateMiddleUtf8;

  TEST_ASSERT(TruncateMiddleUtf8("hello", 10) == "hello");
  TEST_ASSERT(TruncateMiddleUtf8("short", 0) == "short");

  const std::string long_text = "这是一段非常非常长的语音识别通知文本需要被合理截断展示";
  const std::string truncated = TruncateMiddleUtf8(long_text, 20, "...");
  TEST_ASSERT(truncated.find("...") != std::string::npos);
  TEST_ASSERT(DisplayWidth(truncated) <= 20);

  // Grapheme clusters (emoji and keycaps) must never be split apart.
  const std::string emoji_text = "通知: 🚀🚀🚀 语音识别已完成 1️⃣2️⃣3️⃣";
  const std::string emoji_trunc = TruncateMiddleUtf8(emoji_text, 16, "...");
  TEST_ASSERT(emoji_trunc.find("...") != std::string::npos);
  TEST_ASSERT(DisplayWidth(emoji_trunc) <= 16);

  // Narrow budget branch keeps the tail only.
  const std::string tail_only = TruncateMiddleUtf8(long_text, 3, "...");
  TEST_ASSERT(DisplayWidth(tail_only) <= 3);
}

void TestFormatSize() {
  using vinput::str::FormatSize;

  TEST_ASSERT(FormatSize(500) == "500 B");
  TEST_ASSERT(FormatSize(1536) == "1.5 KB");
  TEST_ASSERT(FormatSize(1048576ULL * 2) == "2.0 MB");
  TEST_ASSERT(FormatSize(1024ULL * 1024 * 1024 * 3) == "3.0 GB");
}
} // namespace

int main() {
  TestTrimAsciiWhitespace();
  TestEqualsIgnoreCase();
  TestSplitByChar();
  TestSplitLines();
  TestStripSuffix();
  TestConsumePrefix();
  TestSplitPair();
  TestSplitPairRight();
  TestJoin();
  TestJoinKeyValue();
  TestFmtStr();
  TestCjkBoundaryAndSentenceTerminal();
  TestTruncateMiddleUtf8();
  TestFormatSize();
  return 0;
}
