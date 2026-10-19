#include <cstdio>
#include <cstdlib>
#include <spdlog/common.h>
#include <string>

#include "common/utils/debug_log.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestParseLogLevel() {
  using vinput::log::ParseLogLevel;

  TEST_ASSERT(ParseLogLevel("trace") == spdlog::level::trace);
  TEST_ASSERT(ParseLogLevel("TRACE") == spdlog::level::trace);
  TEST_ASSERT(ParseLogLevel("debug") == spdlog::level::debug);
  TEST_ASSERT(ParseLogLevel("Debug") == spdlog::level::debug);
  TEST_ASSERT(ParseLogLevel("info") == spdlog::level::info);
  TEST_ASSERT(ParseLogLevel("INFO") == spdlog::level::info);
  TEST_ASSERT(ParseLogLevel("warn") == spdlog::level::warn);
  TEST_ASSERT(ParseLogLevel("warning") == spdlog::level::warn);
  TEST_ASSERT(ParseLogLevel("WARN") == spdlog::level::warn);
  TEST_ASSERT(ParseLogLevel("error") == spdlog::level::err);
  TEST_ASSERT(ParseLogLevel("err") == spdlog::level::err);
  TEST_ASSERT(ParseLogLevel("critical") == spdlog::level::critical);
  TEST_ASSERT(ParseLogLevel("fatal") == spdlog::level::critical);
  TEST_ASSERT(ParseLogLevel("off") == spdlog::level::off);

  // Fallbacks
  TEST_ASSERT(ParseLogLevel("", spdlog::level::warn) == spdlog::level::warn);
  TEST_ASSERT(ParseLogLevel("unknown_val", spdlog::level::err) == spdlog::level::err);
}

void TestDynamicLogLevel() {
  using namespace vinput::log;

  SetLogLevel(spdlog::level::warn);
  TEST_ASSERT(GetLogLevel() == spdlog::level::warn);

  SetLogLevel("debug");
  TEST_ASSERT(GetLogLevel() == spdlog::level::debug);

  SetLogLevel("error");
  TEST_ASSERT(GetLogLevel() == spdlog::level::err);

  // Reset to info for subsequent tests
  SetLogLevel(spdlog::level::info);
  TEST_ASSERT(GetLogLevel() == spdlog::level::info);
}

void TestLoggingCalls() {
  using namespace vinput::log;

  SetLogLevel(spdlog::level::trace);

  // Exercise every level and format parameters
  VLOG_TRACE("test trace message");
  VLOG_DEBUG("test debug message with number: {}", 42);
  VLOG_INFO("test info message with string: {}", "vinput");
  VLOG_WARN("test warn message: {} {:.2f}", "ratio", 0.75);
  VLOG_ERROR("test error message: code={}", -1);
  VLOG_CRITICAL("test critical message");
}

void TestLogPattern() {
  using namespace vinput::log;

  TEST_ASSERT(!kDefaultLogPattern.empty());
  SetLogPattern("[%n] [%l] %v");
  VLOG_INFO("test with custom pattern");
  // Restore default pattern
  SetLogPattern(std::string(kDefaultLogPattern));
}

int main() {
  TestParseLogLevel();
  TestDynamicLogLevel();
  TestLogPattern();
  TestLoggingCalls();
  std::printf("test_logging passed successfully.\n");
  return 0;
}
