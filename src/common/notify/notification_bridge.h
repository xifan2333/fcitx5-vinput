#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace vinput::notify {

enum class NotificationLevel : std::uint8_t {
  Information,
  Warning,
  Error,
};

struct Notification {
  std::string title;
  std::string body;
  NotificationLevel level = NotificationLevel::Warning;
};

using NotificationSink = std::function<void(const Notification&)>;
using TimeProvider = std::function<std::chrono::steady_clock::time_point()>;

// The host-side single consumer of helper process `stderr` (ASR providers and LLM adapters).
//
// Extracts complete lines, detects JSON `{"title": "...", "body": "..."}` vs plain-text shapes,
// suppresses duplicates inside a sliding window, and forwards clean notifications to `sink`.
class NotificationBridge {
public:
  static constexpr std::chrono::milliseconds kDefaultDedupWindow{3000};

  explicit NotificationBridge(NotificationSink sink,
                              std::chrono::milliseconds dedup_window = kDefaultDedupWindow,
                              TimeProvider time_provider = nullptr);
  ~NotificationBridge();

  NotificationBridge(const NotificationBridge&) = delete;
  NotificationBridge& operator=(const NotificationBridge&) = delete;
  NotificationBridge(NotificationBridge&&) noexcept;
  NotificationBridge& operator=(NotificationBridge&&) noexcept;

  // Feeds raw stderr bytes from a helper identified by `source_id`.
  // Complete lines (delimited by '\n' or '\r\n') are drained, parsed, and forwarded.
  void FeedStderr(std::string_view source_id, std::span<const std::byte> bytes,
                  std::string_view default_title = {});

  // Delivers a complete line directly (e.g. from one-shot batch providers).
  void DeliverLine(std::string_view source_id, std::string_view raw_line,
                   std::string_view default_title = {});

  // Flushes any remaining incomplete line in `source_id`'s buffer (e.g. on child process exit).
  void Flush(std::string_view source_id, std::string_view default_title = {});

  // Drops the buffer for a retired helper to prevent memory accumulation.
  void RetireSource(std::string_view source_id);

  // Clears deduplication cache (e.g. at session reset or for deterministic tests).
  void ResetDedup();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vinput::notify
