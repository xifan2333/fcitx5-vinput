#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/utils/expected.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestBasicValueAndError() {
  vinput::Expected<int> ok = 42;
  TEST_ASSERT(ok.has_value());
  TEST_ASSERT(bool(ok));
  TEST_ASSERT(*ok == 42);
  TEST_ASSERT(ok.value() == 42);
  TEST_ASSERT(ok.value_or(0) == 42);

  vinput::Expected<int> err = vinput::UnexpectedError("failure");
  TEST_ASSERT(!err.has_value());
  TEST_ASSERT(!bool(err));
  TEST_ASSERT(err.error() == "failure");
  TEST_ASSERT(err.value_or(99) == 99);
}

void TestMonadicTransform() {
  vinput::Expected<int> ok = 10;
  auto transformed = ok.transform([](int v) { return v * 2; });
  TEST_ASSERT(transformed.has_value());
  TEST_ASSERT(*transformed == 20);

  vinput::Expected<int> err = vinput::UnexpectedError("err");
  auto err_transformed = err.transform([](int v) { return v * 2; });
  TEST_ASSERT(!err_transformed.has_value());
  TEST_ASSERT(err_transformed.error() == "err");
}

void TestMonadicAndThen() {
  auto parse_int = [](const std::string& s) -> vinput::Expected<int> {
    if (s.empty()) {
      return vinput::UnexpectedError("empty input");
    }
    return std::stoi(s);
  };

  auto double_if_positive = [](int v) -> vinput::Expected<int> {
    if (v < 0) {
      return vinput::UnexpectedError("negative number");
    }
    return v * 2;
  };

  auto success_flow =
      vinput::Expected<std::string>("21").and_then(parse_int).and_then(double_if_positive);
  TEST_ASSERT(success_flow.has_value());
  TEST_ASSERT(*success_flow == 42);

  auto fail_step1 =
      vinput::Expected<std::string>("").and_then(parse_int).and_then(double_if_positive);
  TEST_ASSERT(!fail_step1.has_value());
  TEST_ASSERT(fail_step1.error() == "empty input");

  auto fail_step2 =
      vinput::Expected<std::string>("-5").and_then(parse_int).and_then(double_if_positive);
  TEST_ASSERT(!fail_step2.has_value());
  TEST_ASSERT(fail_step2.error() == "negative number");
}

void TestVoidExpected() {
  const vinput::Expected<void> ok{};
  TEST_ASSERT(ok.has_value());
  TEST_ASSERT(bool(ok));

  vinput::Expected<void> err = vinput::UnexpectedError("operation failed");
  TEST_ASSERT(!err.has_value());
  TEST_ASSERT(err.error() == "operation failed");
}

} // namespace

int main() {
  TestBasicValueAndError();
  TestMonadicTransform();
  TestMonadicAndThen();
  TestVoidExpected();
  std::printf("All test_expected cases passed successfully.\n");
  return 0;
}
