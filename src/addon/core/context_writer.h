#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace vinput::addon {

// A single background writer keeps filesystem operations off the Fcitx event
// loop. The bounded queue preserves order; on sustained disk stalls it drops
// the oldest pending entries rather than blocking typing.
class AsyncContextWriter {
public:
  explicit AsyncContextWriter(std::filesystem::path path);
  ~AsyncContextWriter();
  AsyncContextWriter(const AsyncContextWriter&) = delete;
  AsyncContextWriter& operator=(const AsyncContextWriter&) = delete;
  AsyncContextWriter(AsyncContextWriter&&) = delete;
  AsyncContextWriter& operator=(AsyncContextWriter&&) = delete;

  // Returns false only if the entry could not be queued. Slow disks never
  // synchronously delay the caller. JSON is already serialized by the caller.
  bool Enqueue(std::string json_line, int max_lines);
  bool WaitUntilIdle(std::chrono::milliseconds timeout);
  std::size_t DroppedEntries() const;

private:
  struct ContextWriteTask {
    std::string json_line;
    int max_lines = 0;
  };

  void Run();
  void Persist(const ContextWriteTask& task);

  static constexpr std::size_t kMaxPendingBytes = 4ULL * 1024ULL * 1024ULL;
  static constexpr std::size_t kMaxPendingEntries = 1024;

  const std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::condition_variable wake_cv_;
  std::condition_variable idle_cv_;
  std::deque<ContextWriteTask> pending_;
  std::size_t pending_bytes_ = 0;
  std::size_t dropped_ = 0;
  int write_count_ = 0;
  bool writing_ = false;
  bool stopping_ = false;
  std::thread worker_;
};

} // namespace vinput::addon
