#include "core/context_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vinput::addon {

AsyncContextWriter::AsyncContextWriter(std::filesystem::path path)
    : path_(std::move(path)), worker_([this] { Run(); }) {}

AsyncContextWriter::~AsyncContextWriter() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_cv_.notify_one();
  // Draining is deliberately confined to addon teardown, not key processing.
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool AsyncContextWriter::Enqueue(std::string json_line, int max_lines) {
  if (max_lines <= 0 || json_line.empty()) {
    return true;
  }
  bool lost_entries = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return false;
    }
    if (json_line.size() > kMaxPendingBytes) {
      ++dropped_;
      return false;
    }
    while (!pending_.empty() && (pending_.size() >= kMaxPendingEntries ||
                                 pending_bytes_ > kMaxPendingBytes - json_line.size())) {
      pending_bytes_ -= pending_.front().json_line.size();
      pending_.pop_front();
      ++dropped_;
      lost_entries = true;
    }
    pending_bytes_ += json_line.size();
    pending_.push_back({std::move(json_line), max_lines});
  }
  wake_cv_.notify_one();
  return !lost_entries;
}

bool AsyncContextWriter::WaitUntilIdle(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idle_cv_.wait_for(lock, timeout, [&] { return pending_.empty() && !writing_; });
}

std::size_t AsyncContextWriter::DroppedEntries() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

void AsyncContextWriter::Run() {
  while (true) {
    ContextWriteTask task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      task = std::move(pending_.front());
      pending_bytes_ -= task.json_line.size();
      pending_.pop_front();
      writing_ = true;
    }

    Persist(task);

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      writing_ = false;
      if (pending_.empty()) {
        idle_cv_.notify_all();
      }
    }
  }
}

void AsyncContextWriter::Persist(const ContextWriteTask& task) {
  {
    std::ofstream ofs(path_, std::ios::app);
    if (!ofs) {
      std::error_code ec;
      std::filesystem::create_directories(path_.parent_path(), ec);
      if (ec) {
        std::fprintf(stderr, "vinput: failed to create context cache directory: %s\n",
                     ec.message().c_str());
        return;
      }
      ofs.clear();
      ofs.open(path_, std::ios::app);
      if (!ofs) {
        std::fprintf(stderr, "vinput: failed to open context cache for append\n");
        return;
      }
    }
    ofs << task.json_line << '\n';
  }

  constexpr int kTruncateInterval = 100;
  if (task.max_lines <= 0 || ++write_count_ < kTruncateInterval) {
    return;
  }
  write_count_ = 0;
  std::ifstream ifs(path_);
  if (!ifs) {
    return;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(ifs, line)) {
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  ifs.close();

  if (lines.size() <= static_cast<std::size_t>(task.max_lines)) {
    return;
  }
  const auto begin = lines.end() - task.max_lines;
  const auto temp_path = path_.string() + ".tmp";
  std::ofstream out(temp_path, std::ios::trunc);
  if (!out) {
    return;
  }
  for (auto it = begin; it != lines.end(); ++it) {
    out << *it << '\n';
  }
  out.close();
  if (!out) {
    return;
  }
  std::error_code ec;
  std::filesystem::rename(temp_path, path_, ec);
  if (ec) {
    std::fprintf(stderr, "vinput: failed to rotate context cache: %s\n", ec.message().c_str());
  }
}

} // namespace vinput::addon
