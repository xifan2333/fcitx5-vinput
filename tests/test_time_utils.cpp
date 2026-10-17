#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "common/utils/time_utils.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestRemainingTimeoutMs() {
  using Clock = std::chrono::steady_clock;
  using vinput::time::RemainingTimeoutMs;

  const auto now = Clock::now();

  // Negative or zero max timeout
  TEST_ASSERT(RemainingTimeoutMs(now + std::chrono::milliseconds(100), 0, now) == 0);
  TEST_ASSERT(RemainingTimeoutMs(now + std::chrono::milliseconds(100), -5, now) == 0);

  // Past deadline
  TEST_ASSERT(RemainingTimeoutMs(now - std::chrono::milliseconds(10), 100, now) == 0);
  TEST_ASSERT(RemainingTimeoutMs(now, 100, now) == 0);

  // Within range
  TEST_ASSERT(RemainingTimeoutMs(now + std::chrono::milliseconds(42), 100, now) == 42);

  // Exceeds max timeout: clamped
  TEST_ASSERT(RemainingTimeoutMs(now + std::chrono::milliseconds(150), 100, now) == 100);

  // Sentinel max
  TEST_ASSERT(RemainingTimeoutMs(Clock::time_point::max(), 250, now) == 250);

  // Far future deadline exceeding int32 max in ms
  const auto far_future = now + std::chrono::hours(24 * 365);
  TEST_ASSERT(RemainingTimeoutMs(far_future, 500, now) == 500);
}

void TestMillisecondsBetweenAndSince() {
  using Clock = std::chrono::steady_clock;
  using vinput::time::MillisecondsBetween;
  using vinput::time::MillisecondsSince;

  const auto t0 = Clock::now();
  const auto t1 = t0 + std::chrono::milliseconds(1234);

  TEST_ASSERT(MillisecondsBetween(t0, t1) == 1234);
  TEST_ASSERT(MillisecondsBetween(t1, t0) == -1234);

  TEST_ASSERT(MillisecondsSince(t0, t1) == 1234);

  // Optional overload
  const std::optional<Clock::time_point> empty_start = std::nullopt;
  TEST_ASSERT(MillisecondsSince(empty_start, t1) == 0);

  const std::optional<Clock::time_point> valid_start = t0;
  TEST_ASSERT(MillisecondsSince(valid_start, t1) == 1234);
}

void TestUnixTimestamps() {
  using vinput::time::CurrentUnixTimestampMs;
  using vinput::time::CurrentUnixTimestampSeconds;

  const auto sec = CurrentUnixTimestampSeconds();
  const auto ms = CurrentUnixTimestampMs();

  // Realistic Unix timestamp (greater than year 2026 epoch: ~1767225600)
  TEST_ASSERT(sec > 1700000000LL);
  TEST_ASSERT(ms >= sec * 1000LL);
}

int main() {
  TestRemainingTimeoutMs();
  TestMillisecondsBetweenAndSince();
  TestUnixTimestamps();
  return 0;
}
