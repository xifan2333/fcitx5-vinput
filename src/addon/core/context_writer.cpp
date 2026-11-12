#include "core/context_writer.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/utils/debug_log.h"

namespace vinput::addon {

namespace {

[[nodiscard]] bool AppendTaskLine(const std::filesystem::path& path, std::string_view json_line) {
  std::ofstream ofs(path, std::ios::app);
  if (!ofs) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      VLOG_ERROR("failed to create context cache directory: {}", ec.message());
      return false;
    }
    ofs.clear();
    ofs.open(path, std::ios::app);
    if (!ofs) {
      VLOG_ERROR("failed to open context cache for append");
      return false;
    }
  }
  ofs << json_line << '\n';
  return true;
}

void RotateContextFile(const std::filesystem::path& path, int max_lines) {
  std::ifstream ifs(path);
  if (!ifs) {
    return;
  }
  std::vector<std::string> lines;
  for (std::string line; std::getline(ifs, line);) {
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  ifs.close();

  if (lines.size() <= static_cast<std::size_t>(max_lines)) {
    return;
  }

  const auto begin = lines.end() - max_lines;
  const auto temp_path = path.string() + ".tmp";
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
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    VLOG_ERROR("failed to rotate context cache: {}", ec.message());
  }
}

} // namespace

AsyncContextWriter::AsyncContextWriter(std::filesystem::path path)
    : path_(std::move(path)), worker_([this] { Run(); }) {}

AsyncContextWriter::~AsyncContextWriter() {
  {
    const std::scoped_lock lock(mutex_);
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
    const std::scoped_lock lock(mutex_);
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
  const std::scoped_lock lock(mutex_);
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
      const std::scoped_lock lock(mutex_);
      writing_ = false;
      if (pending_.empty()) {
        idle_cv_.notify_all();
      }
    }
  }
}

void AsyncContextWriter::Persist(const ContextWriteTask& task) {
  if (!AppendTaskLine(path_, task.json_line)) {
    return;
  }

  constexpr int kTruncateInterval = 100;
  if (task.max_lines <= 0 || ++write_count_ < kTruncateInterval) {
    return;
  }
  write_count_ = 0;
  RotateContextFile(path_, task.max_lines);
}

} // namespace vinput::addon
