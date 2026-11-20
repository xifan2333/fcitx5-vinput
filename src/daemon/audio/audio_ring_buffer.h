#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace vinput::daemon::audio {

// Lock-free, wait-free Single Producer Single Consumer (SPSC) audio sample ring buffer.
// Configured in seconds to provide compact, predictable real-time memory usage (< 500 KB).
class AudioSampleRingBuffer {
public:
  static constexpr std::size_t kDefaultCapacitySeconds = 15;
  static constexpr std::size_t kDefaultSampleRate = 16000;

  explicit AudioSampleRingBuffer(std::size_t capacity_seconds = kDefaultCapacitySeconds,
                                 std::size_t sample_rate = kDefaultSampleRate)
      : capacity_(capacity_seconds * sample_rate), storage_(capacity_) {}

  [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t MemoryBytes() const noexcept {
    return storage_.size() * sizeof(int16_t);
  }

  // RT-safe, wait-free write of audio samples into the circular buffer.
  // Returns number of samples successfully written (<= samples.size()).
  std::size_t Write(std::span<const int16_t> samples) noexcept {
    const uint64_t current_write = write_pos_.load(std::memory_order_relaxed);
    const uint64_t current_read = read_pos_.load(std::memory_order_acquire);
    const auto occupied = static_cast<std::size_t>(current_write - current_read);
    if (occupied >= capacity_) {
      return 0;
    }
    const std::size_t to_write = std::min(samples.size(), capacity_ - occupied);
    if (to_write == 0) {
      return 0;
    }

    const auto write_idx = static_cast<std::size_t>(current_write % capacity_);
    const std::size_t first_chunk = std::min(to_write, capacity_ - write_idx);
    std::memcpy(storage_.data() + write_idx, samples.data(), first_chunk * sizeof(int16_t));

    const std::size_t second_chunk = to_write - first_chunk;
    if (second_chunk > 0) {
      std::memcpy(storage_.data(), samples.data() + first_chunk, second_chunk * sizeof(int16_t));
    }

    write_pos_.store(current_write + to_write, std::memory_order_release);
    return to_write;
  }

  // Drains available samples by invoking consumer callback with contiguous spans.
  // Updates read pointer atomically only after callback completes.
  template <typename Callback> std::size_t DrainAvailable(Callback&& callback) {
    const uint64_t current_read = read_pos_.load(std::memory_order_relaxed);
    const uint64_t current_write = write_pos_.load(std::memory_order_acquire);
    const auto available = static_cast<std::size_t>(current_write - current_read);
    if (available == 0) {
      return 0;
    }

    const auto read_idx = static_cast<std::size_t>(current_read % capacity_);
    const std::size_t first_chunk = std::min(available, capacity_ - read_idx);
    std::forward<Callback>(callback)(
        std::span<const int16_t>(storage_.data() + read_idx, first_chunk));

    const std::size_t second_chunk = available - first_chunk;
    if (second_chunk > 0) {
      std::forward<Callback>(callback)(std::span<const int16_t>(storage_.data(), second_chunk));
    }

    read_pos_.store(current_read + available, std::memory_order_release);
    return available;
  }

  // Drains all available samples into a std::vector<int16_t>.
  std::vector<int16_t> DrainAll() {
    std::vector<int16_t> result;
    const uint64_t current_read = read_pos_.load(std::memory_order_relaxed);
    const uint64_t current_write = write_pos_.load(std::memory_order_acquire);
    const auto available = static_cast<std::size_t>(current_write - current_read);
    result.reserve(available);
    DrainAvailable([&](std::span<const int16_t> chunk) {
      result.insert(result.end(), chunk.begin(), chunk.end());
    });
    return result;
  }

  // Clears all unread samples.
  void Clear() noexcept {
    read_pos_.store(write_pos_.load(std::memory_order_relaxed), std::memory_order_release);
  }

  [[nodiscard]] std::size_t Available() const noexcept {
    const uint64_t current_read = read_pos_.load(std::memory_order_relaxed);
    const uint64_t current_write = write_pos_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(current_write - current_read);
  }

private:
  alignas(64) std::atomic<uint64_t> write_pos_{0};
  alignas(64) std::atomic<uint64_t> read_pos_{0};
  std::size_t capacity_{0};
  std::vector<int16_t> storage_;
};

} // namespace vinput::daemon::audio
