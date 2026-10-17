#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "common/utils/string_utils.h"

#define TEST_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__);          \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)

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

  TEST_ASSERT(ToUpperAscii("").empty());
  TEST_ASSERT(ToUpperAscii("Hello, World! 123") == "HELLO, WORLD! 123");
  TEST_ASSERT(ToUpperAscii("fcitx5-vinput") == "FCITX5-VINPUT");
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
  TEST_ASSERT(Base64Encode(nullptr, 0).empty());
}

int main() {
  TestTrimAsciiWhitespace();
  TestToLowerAndToUpperAscii();
  TestEqualsIgnoreCase();
  TestSplitByChar();
  TestSplitLines();
  TestBase64Encode();
  return 0;
}
