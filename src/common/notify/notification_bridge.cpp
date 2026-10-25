#include "common/notify/notification_bridge.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <glaze/core/common.hpp>
#include <glaze/core/meta.hpp>
#include <glaze/json/read.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/i18n.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

namespace {

struct JsonNotificationWire {
  std::optional<std::string> title;
  std::optional<std::string> body;
};

} // namespace

template <> struct glz::meta<JsonNotificationWire> {
  using T = JsonNotificationWire;
  static constexpr auto value = object("title", &T::title, "body", &T::body);
};

namespace vinput::notify {

namespace {

constexpr std::size_t kMaxDedupCacheSize = 256;
constexpr std::size_t kMaxSourceBufferSize = std::size_t{64} * 1024; // 64 KiB

std::string MakeDedupKey(std::string_view title, std::string_view body) {
  std::string key;
  key.reserve(title.size() + 1 + body.size());
  key.append(title);
  key.push_back('\0');
  key.append(body);
  return key;
}

} // namespace

struct NotificationBridge::Impl {
  NotificationSink sink_;
  std::chrono::milliseconds dedup_window_;
  TimeProvider time_provider_;

  std::mutex mutex_;
  std::map<std::string, std::string, std::less<>> buffers_;

  struct DedupEntry {
    std::chrono::steady_clock::time_point timestamp;
  };
  std::unordered_map<std::string, DedupEntry> dedup_cache_;

  Impl(NotificationSink sink, std::chrono::milliseconds dedup_window, TimeProvider time_provider)
      : sink_(std::move(sink)), dedup_window_(dedup_window),
        time_provider_(std::move(time_provider)) {}

  [[nodiscard]] std::chrono::steady_clock::time_point Now() const {
    if (time_provider_) {
      return time_provider_();
    }
    return std::chrono::steady_clock::now();
  }

  void EvictStaleDedupEntriesLocked(std::chrono::steady_clock::time_point now) {
    std::erase_if(dedup_cache_, [this, now](const auto& item) {
      return (now - item.second.timestamp) >= dedup_window_;
    });

    while (dedup_cache_.size() > kMaxDedupCacheSize) {
      auto oldest = dedup_cache_.begin();
      for (auto it = dedup_cache_.begin(); it != dedup_cache_.end(); ++it) {
        if (it->second.timestamp < oldest->second.timestamp) {
          oldest = it;
        }
      }
      dedup_cache_.erase(oldest);
    }
  }

  void ProcessLineLocked(std::string_view raw_line, std::string_view default_title,
                         std::vector<Notification>& ready_to_send) {
    const std::string_view trimmed = vinput::str::Trim(raw_line);
    if (trimmed.empty()) {
      return;
    }

    Notification n;
    n.level = NotificationLevel::Warning;

    bool parsed_json = false;
    if (trimmed.starts_with('{') && trimmed.ends_with('}')) {
      JsonNotificationWire wire;
      if (!bool(glz::read_json(wire, trimmed))) {
        if (wire.title.has_value() && wire.body.has_value()) {
          parsed_json = true;
          n.title = std::move(*wire.title);
          n.body = std::move(*wire.body);
          if (n.title.empty()) {
            n.title = default_title.empty() ? _("Voice Input") : std::string(default_title);
          }
        }
      }
    }

    if (!parsed_json) {
      n.body = std::string(raw_line);
      n.title = default_title.empty() ? _("Voice Input") : std::string(default_title);
    }

    const auto now = Now();
    const std::string dedup_key = MakeDedupKey(n.title, n.body);

    auto it = dedup_cache_.find(dedup_key);
    if (it != dedup_cache_.end()) {
      if ((now - it->second.timestamp) < dedup_window_) {
        it->second.timestamp = now;
        return;
      }
      it->second.timestamp = now;
    } else {
      dedup_cache_.emplace(dedup_key, DedupEntry{.timestamp = now});
      if (dedup_cache_.size() > kMaxDedupCacheSize) {
        EvictStaleDedupEntriesLocked(now);
      }
    }

    ready_to_send.push_back(std::move(n));
  }

  void Dispatch(const std::vector<Notification>& notifications) const {
    if (!sink_) {
      return;
    }
    for (const auto& n : notifications) {
      sink_(n);
    }
  }
};

NotificationBridge::NotificationBridge(NotificationSink sink,
                                       std::chrono::milliseconds dedup_window,
                                       TimeProvider time_provider)
    : impl_(std::make_unique<Impl>(std::move(sink), dedup_window, std::move(time_provider))) {}

NotificationBridge::~NotificationBridge() = default;

NotificationBridge::NotificationBridge(NotificationBridge&&) noexcept = default;
NotificationBridge& NotificationBridge::operator=(NotificationBridge&&) noexcept = default;

void NotificationBridge::FeedStderr(std::string_view source_id, std::span<const std::byte> bytes,
                                    std::string_view default_title) {
  if (bytes.empty()) {
    return;
  }

  std::vector<Notification> to_send;
  {
    const std::scoped_lock lock(impl_->mutex_);
    auto& buffer = impl_->buffers_[std::string(source_id)];

    buffer.reserve(buffer.size() + bytes.size());
    for (const auto b : bytes) {
      buffer.push_back(static_cast<char>(b));
      if (buffer.size() >= kMaxSourceBufferSize) {
        impl_->ProcessLineLocked(buffer, default_title, to_send);
        buffer.clear();
      }
    }

    vinput::process::DrainLinesFromBuffer(
        buffer,
        [this, default_title, &to_send](std::string_view line) {
          impl_->ProcessLineLocked(line, default_title, to_send);
        },
        /*flush_tail=*/false);
  }

  impl_->Dispatch(to_send);
}

void NotificationBridge::DeliverLine(std::string_view /*source_id*/, std::string_view raw_line,
                                     std::string_view default_title) {
  std::vector<Notification> to_send;
  {
    const std::scoped_lock lock(impl_->mutex_);
    impl_->ProcessLineLocked(raw_line, default_title, to_send);
  }
  impl_->Dispatch(to_send);
}

void NotificationBridge::Flush(std::string_view source_id, std::string_view default_title) {
  std::vector<Notification> to_send;
  {
    const std::scoped_lock lock(impl_->mutex_);
    auto it = impl_->buffers_.find(source_id);
    if (it != impl_->buffers_.end() && !it->second.empty()) {
      vinput::process::DrainLinesFromBuffer(
          it->second,
          [this, default_title, &to_send](std::string_view line) {
            impl_->ProcessLineLocked(line, default_title, to_send);
          },
          /*flush_tail=*/true);
    }
  }
  impl_->Dispatch(to_send);
}

void NotificationBridge::RetireSource(std::string_view source_id) {
  const std::scoped_lock lock(impl_->mutex_);
  impl_->buffers_.erase(std::string(source_id));
}

void NotificationBridge::ResetDedup() {
  const std::scoped_lock lock(impl_->mutex_);
  impl_->dedup_cache_.clear();
}

} // namespace vinput::notify
