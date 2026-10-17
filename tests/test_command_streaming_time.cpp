#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "common/utils/time_utils.h"

namespace {

void Expect(bool condition, const char* description) {
  if (!condition) {
    std::cerr << "Command streaming timeout test failed: " << description << '\n';
    std::abort();
  }
}

void TestRemainingPollMs() {
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono;
  const auto now = Clock::time_point{} + seconds(123);
  const auto pending = now + milliseconds(49);
  const auto huge = now + milliseconds(static_cast<std::int64_t>(INT_MAX) + 10000);

  const auto remaining = [now](Clock::time_point deadline, int max_timeout) {
    return vinput::time::RemainingTimeoutMs(deadline, max_timeout, now);
  };
  Expect(remaining(now - milliseconds(1), 50) == 0, "past deadline");
  Expect(remaining(now, 50) == 0, "expired deadline");
  Expect(remaining(now + microseconds(500), 50) == 0, "sub-millisecond truncation unchanged");
  Expect(remaining(pending, 50) == 49, "under upper bound");
  Expect(remaining(now + milliseconds(50), 50) == 50, "at upper bound");
  Expect(remaining(now + milliseconds(51), 50) == 50, "above upper bound");
  Expect(remaining(huge, 50) == 50, "no narrowing before clamp");
  Expect(remaining(huge, 0) == 0, "nonblocking must remain zero");
  Expect(remaining(huge, INT_MAX) == INT_MAX, "int max upper bound");
  Expect(remaining(Clock::time_point::max(), 50) == 50, "unbounded sentinel");
  Expect(remaining(Clock::time_point::max(), 0) == 0, "unbounded nonblocking");
  Expect(remaining(Clock::time_point::max(), -1) == 0, "negative upper is nonblocking");

  constexpr std::array<int, 4> upper_bounds{0, 1, 50, INT_MAX};
  constexpr std::array<std::int64_t, 10> delta_ms{-200,
                                                  -1,
                                                  0,
                                                  1,
                                                  49,
                                                  50,
                                                  51,
                                                  1000,
                                                  static_cast<std::int64_t>(INT_MAX) - 1,
                                                  static_cast<std::int64_t>(INT_MAX) + 10000};
  for (const int upper : upper_bounds) {
    for (const std::int64_t delta : delta_ms) {
      const int actual = remaining(now + milliseconds(delta), upper);
      Expect(actual >= 0 && actual <= upper, "result must stay inside clamped interval");
    }
  }
}

} // namespace

int main() {
  TestRemainingPollMs();
  std::cout << "Command streaming timeout boundary tests passed\n";
}
