#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
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

void TestToLowerAndToUpperAscii() {
  using vinput::str::ToLowerAscii;
  using vinput::str::ToUpperAscii;

  TEST_ASSERT(ToLowerAscii("").empty());
  TEST_ASSERT(ToLowerAscii("Hello, World! 123") == "hello, world! 123");
  TEST_ASSERT(ToLowerAscii("FCITX5-VINPUT") == "fcitx5-vinput");
  // Strict ASCII-only case folding: I <-> i regardless of system locale
  TEST_ASSERT(ToLowerAscii("AUTHORIZATION") == "authorization");

  TEST_ASSERT(ToUpperAscii("").empty());
  TEST_ASSERT(ToUpperAscii("Hello, World! 123") == "HELLO, WORLD! 123");
  TEST_ASSERT(ToUpperAscii("fcitx5-vinput") == "FCITX5-VINPUT");
  TEST_ASSERT(ToUpperAscii("authorization") == "AUTHORIZATION");
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

void TestBase64Encode() {
  using vinput::str::Base64Encode;

  // RFC 4648 test vectors
  TEST_ASSERT(Base64Encode("").empty());
  TEST_ASSERT(Base64Encode("f") == "Zg==");
  TEST_ASSERT(Base64Encode("fo") == "Zm8=");
  TEST_ASSERT(Base64Encode("foo") == "Zm9v");
  TEST_ASSERT(Base64Encode("foob") == "Zm9vYg==");
  TEST_ASSERT(Base64Encode("fooba") == "Zm9vYmE=");
  TEST_ASSERT(Base64Encode("foobar") == "Zm9vYmFy");

  // Raw unsigned char pointer test
  const unsigned char data[] = {'H', 'e', 'l', 'l', 'o'};
  TEST_ASSERT(Base64Encode(data, sizeof(data)) == "SGVsbG8=");
  TEST_ASSERT(Base64Encode(static_cast<const unsigned char*>(nullptr), 0).empty());

  // Raw const char pointer test
  const char char_data[] = "Hello";
  TEST_ASSERT(Base64Encode(char_data, sizeof(char_data) - 1) == "SGVsbG8=");
  TEST_ASSERT(Base64Encode(static_cast<const char*>(nullptr), 0).empty());
}

void TestStripPrefix() {
  using vinput::str::StripPrefix;

  // Normal prefix stripping
  TEST_ASSERT(StripPrefix("Bearer 123456", "Bearer ") == "123456");
  TEST_ASSERT(StripPrefix("prefix_text", "prefix_") == "text");

  // Non-matching prefix
  TEST_ASSERT(StripPrefix("Basic 123456", "Bearer ") == "Basic 123456");
  TEST_ASSERT(StripPrefix("hello", "world") == "hello");

  // Boundary cases
  TEST_ASSERT(StripPrefix("", "").empty());
  TEST_ASSERT(StripPrefix("", "prefix").empty());
  TEST_ASSERT(StripPrefix("text", "") == "text");
  TEST_ASSERT(StripPrefix("exact_match", "exact_match").empty());
  TEST_ASSERT(StripPrefix("short", "longer_prefix") == "short");
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

void TestMaskSecret() {
  using vinput::str::MaskSecret;

  // Normal key (length 24)
  TEST_ASSERT(MaskSecret("sk-proj-1234567890abcdef") == "sk-p****************cdef");

  // Critical length: exactly head_keep + tail_keep (8) -> completely masked
  TEST_ASSERT(MaskSecret("12345678") == "********");

  // Length 9: head 4, mask 1, tail 4
  TEST_ASSERT(MaskSecret("123456789") == "1234*6789");

  // Extremely short keys (< 8) -> completely masked
  TEST_ASSERT(MaskSecret("abc") == "***");
  TEST_ASSERT(MaskSecret("a") == "*");

  // Empty string -> empty
  TEST_ASSERT(MaskSecret("").empty());

  // Custom parameters: head_keep = 2, tail_keep = 2, mask_char = '#'
  // Length 8 > 4 -> head 2, mask 4, tail 2
  TEST_ASSERT(MaskSecret("12345678", 2, 2, '#') == "12####78");

  // Custom parameters: length <= head_keep + tail_keep
  TEST_ASSERT(MaskSecret("1234", 2, 2, '#') == "####");
  TEST_ASSERT(MaskSecret("123", 2, 2, '#') == "###");

  // Overflow protection with extreme size_t keep lengths
  const size_t kMax = std::numeric_limits<size_t>::max();
  TEST_ASSERT(MaskSecret("secret", kMax, 1) == "******");
  TEST_ASSERT(MaskSecret("secret", 1, kMax) == "******");
  TEST_ASSERT(MaskSecret("secret", kMax, kMax) == "******");
  TEST_ASSERT(MaskSecret("secret", kMax, 0) == "******");
}

void TestDrainLinesFromBuffer() {
  using vinput::str::DrainLinesFromBuffer;

  // 1. Standard LF multi-line draining
  {
    std::string buffer = "first\nsecond\nthird\n";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); });
    TEST_ASSERT(lines.size() == 3);
    TEST_ASSERT(lines[0] == "first");
    TEST_ASSERT(lines[1] == "second");
    TEST_ASSERT(lines[2] == "third");
    TEST_ASSERT(buffer.empty());
  }

  // 2. CRLF multi-line draining (\r stripped)
  {
    std::string buffer = "hello\r\nworld\r\n";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); });
    TEST_ASSERT(lines.size() == 2);
    TEST_ASSERT(lines[0] == "hello");
    TEST_ASSERT(lines[1] == "world");
    TEST_ASSERT(buffer.empty());
  }

  // 3. Partial tail without trailing newline, flush_tail = false
  {
    std::string buffer = "first\npartial tail";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); }, false);
    TEST_ASSERT(lines.size() == 1);
    TEST_ASSERT(lines[0] == "first");
    TEST_ASSERT(buffer == "partial tail");

    // Append more and drain again
    buffer += " completed\n";
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); }, false);
    TEST_ASSERT(lines.size() == 2);
    TEST_ASSERT(lines[1] == "partial tail completed");
    TEST_ASSERT(buffer.empty());
  }

  // 4. Partial tail with flush_tail = true
  {
    std::string buffer = "first\nfinal tail";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); }, true);
    TEST_ASSERT(lines.size() == 2);
    TEST_ASSERT(lines[0] == "first");
    TEST_ASSERT(lines[1] == "final tail");
    TEST_ASSERT(buffer.empty());
  }

  // 5. Partial tail with \r and flush_tail = true
  {
    std::string buffer = "carriage tail\r";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); }, true);
    TEST_ASSERT(lines.size() == 1);
    TEST_ASSERT(lines[0] == "carriage tail");
    TEST_ASSERT(buffer.empty());
  }

  // 6. Empty buffer
  {
    std::string buffer;
    int call_count = 0;
    DrainLinesFromBuffer(buffer, [&](std::string_view) { ++call_count; }, true);
    TEST_ASSERT(call_count == 0);
    TEST_ASSERT(buffer.empty());
  }

  // 7. Pure newlines
  {
    std::string buffer = "\n\n";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(buffer, [&](std::string_view line) { lines.emplace_back(line); });
    TEST_ASSERT(lines.size() == 2);
    TEST_ASSERT(lines[0].empty());
    TEST_ASSERT(lines[1].empty());
    TEST_ASSERT(buffer.empty());
  }

  // 8. Early break with bool return
  {
    std::string buffer = "line1\nline2\nline3\n";
    std::vector<std::string> lines;
    DrainLinesFromBuffer(
        buffer,
        [&](std::string_view line) -> bool {
          lines.emplace_back(line);
          return false; // stop immediately after line1
        },
        true);
    TEST_ASSERT(lines.size() == 1);
    TEST_ASSERT(lines[0] == "line1");
    // line1 was processed and removed, line2 and line3 remain in buffer
    TEST_ASSERT(buffer == "line2\nline3\n");
  }
}

int main() {
  TestTrimAsciiWhitespace();
  TestToLowerAndToUpperAscii();
  TestEqualsIgnoreCase();
  TestSplitByChar();
  TestSplitLines();
  TestBase64Encode();
  TestStripPrefix();
  TestStripSuffix();
  TestConsumePrefix();
  TestSplitPair();
  TestSplitPairRight();
  TestJoin();
  TestJoinKeyValue();
  TestMaskSecret();
  TestDrainLinesFromBuffer();
  return 0;
}
