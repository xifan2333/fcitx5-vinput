#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace vinput::time {

// Calculate remaining milliseconds until deadline, clamped within [0, max_timeout_ms].
// Clamping is kept strictly in the clock's duration domain to prevent integer narrowing overflow.
inline int RemainingTimeoutMs(
    std::chrono::steady_clock::time_point deadline, int max_timeout_ms,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept {
  using Clock = std::chrono::steady_clock;
  if (max_timeout_ms <= 0) {
    return 0;
  }
  if (deadline == Clock::time_point::max()) {
    return max_timeout_ms;
  }
  if (deadline <= now) {
    return 0;
  }

  const auto remaining = deadline - now;
  if (remaining >= std::chrono::milliseconds(max_timeout_ms)) {
    return max_timeout_ms;
  }
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
}

// Calculate elapsed milliseconds between start and end (end - start).
inline int64_t MillisecondsBetween(std::chrono::steady_clock::time_point start,
                                   std::chrono::steady_clock::time_point end) noexcept {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

// Calculate elapsed milliseconds from start until now.
inline int64_t MillisecondsSince(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept {
  return MillisecondsBetween(start, now);
}

// Overload for std::optional start time point (returns 0 if start is nullopt).
inline int64_t MillisecondsSince(
    const std::optional<std::chrono::steady_clock::time_point>& start,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept {
  if (!start.has_value()) {
    return 0;
  }
  return MillisecondsBetween(*start, now);
}

// Current UTC epoch timestamp in seconds.
inline int64_t CurrentUnixTimestampSeconds() noexcept {
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
}

// Current UTC epoch timestamp in milliseconds.
inline int64_t CurrentUnixTimestampMs() noexcept {
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
}

} // namespace vinput::time
