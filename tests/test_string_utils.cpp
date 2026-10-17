#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common/utils/string_utils.h"

void TestTrimAsciiWhitespace() {
  using vinput::str::TrimAsciiWhitespace;
  using vinput::str::TrimAsciiWhitespaceView;

  // Empty string
  assert(TrimAsciiWhitespace("").empty());
  assert(TrimAsciiWhitespaceView("").empty());

  // Pure whitespace
  assert(TrimAsciiWhitespace("   \t\r\n ").empty());
  assert(TrimAsciiWhitespaceView("   \t\r\n ").empty());

  // Leading and trailing whitespace
  assert(TrimAsciiWhitespace("  hello world  \n") == "hello world");
  assert(TrimAsciiWhitespaceView("  hello world  \n") == "hello world");
  assert(TrimAsciiWhitespace("\t\tvinput\r") == "vinput");

  // No whitespace to trim
  assert(TrimAsciiWhitespace("abc") == "abc");
  assert(TrimAsciiWhitespaceView("abc") == "abc");

  // Zero-copy check: returned string_view points inside original buffer
  std::string_view original = "  inner text  ";
  std::string_view trimmed = TrimAsciiWhitespaceView(original);
  assert(trimmed == "inner text");
  assert(trimmed.data() == original.data() + 2);

  // Return std::string check
  std::string string_res = TrimAsciiWhitespace("  to string test  ");
  assert(string_res == "to string test");
}

void TestToLowerAndToUpperAscii() {
  using vinput::str::ToLowerAscii;
  using vinput::str::ToUpperAscii;

  assert(ToLowerAscii("") == "");
  assert(ToLowerAscii("Hello, World! 123") == "hello, world! 123");
  assert(ToLowerAscii("FCITX5-VINPUT") == "fcitx5-vinput");

  assert(ToUpperAscii("") == "");
  assert(ToUpperAscii("Hello, World! 123") == "HELLO, WORLD! 123");
  assert(ToUpperAscii("fcitx5-vinput") == "FCITX5-VINPUT");
}

void TestEqualsIgnoreCase() {
  using vinput::str::EqualsIgnoreCase;

  assert(EqualsIgnoreCase("", ""));
  assert(EqualsIgnoreCase("abc", "ABC"));
  assert(EqualsIgnoreCase("VInput-Daemon", "vinput-daemon"));
  assert(!EqualsIgnoreCase("abc", "abcd"));
  assert(!EqualsIgnoreCase("abcd", "abc"));
  assert(!EqualsIgnoreCase("hello", "world"));
}

void TestSplitByChar() {
  using vinput::str::SplitByChar;

  // Single segment
  auto res1 = SplitByChar("hello", '.');
  assert(res1.size() == 1 && res1[0] == "hello");

  // Multiple segments
  auto res2 = SplitByChar("a.b.c", '.');
  assert(res2.size() == 3 && res2[0] == "a" && res2[1] == "b" && res2[2] == "c");

  // Leading, trailing, consecutive delimiters
  auto res3 = SplitByChar("/usr/local/bin/", '/');
  assert(res3.size() == 5);
  assert(res3[0] == "");
  assert(res3[1] == "usr");
  assert(res3[2] == "local");
  assert(res3[3] == "bin");
  assert(res3[4] == "");

  // Empty string
  auto res4 = SplitByChar("", ',');
  assert(res4.size() == 1 && res4[0] == "");
}

void TestSplitLines() {
  using vinput::str::SplitLines;

  // Empty string
  auto res1 = SplitLines("");
  assert(res1.empty());

  // LF
  auto res2 = SplitLines("line1\nline2\nline3");
  assert(res2.size() == 3 && res2[0] == "line1" && res2[1] == "line2" && res2[2] == "line3");

  // CRLF
  auto res3 = SplitLines("line1\r\nline2\r\n");
  assert(res3.size() == 2 && res3[0] == "line1" && res3[1] == "line2");

  // Single line without newline
  auto res4 = SplitLines("single");
  assert(res4.size() == 1 && res4[0] == "single");
}

void TestBase64Encode() {
  using vinput::str::Base64Encode;

  // RFC 4648 test vectors
  assert(Base64Encode("") == "");
  assert(Base64Encode("f") == "Zg==");
  assert(Base64Encode("fo") == "Zm8=");
  assert(Base64Encode("foo") == "Zm9v");
  assert(Base64Encode("foob") == "Zm9vYg==");
  assert(Base64Encode("fooba") == "Zm9vYmE=");
  assert(Base64Encode("foobar") == "Zm9vYmFy");

  // Raw unsigned char pointer test
  const unsigned char data[] = {'H', 'e', 'l', 'l', 'o'};
  assert(Base64Encode(data, sizeof(data)) == "SGVsbG8=");
  assert(Base64Encode(nullptr, 0) == "");
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
