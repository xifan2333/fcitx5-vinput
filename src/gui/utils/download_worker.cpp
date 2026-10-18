#include "gui/utils/download_worker.h"

#include <atomic>
#include <string>
#include <utility>

namespace vinput::gui {

DownloadWorker::DownloadWorker(QObject* parent) : QThread(parent) {}

DownloadWorker::~DownloadWorker() {
  Cancel();
  wait();
}

void DownloadWorker::Cancel() noexcept {
  cancel_token_.store(true, std::memory_order_relaxed);
}

bool DownloadWorker::IsCancelled() const noexcept {
  return cancel_token_.load(std::memory_order_relaxed);
}

const std::atomic<bool>* DownloadWorker::CancelToken() const noexcept {
  return &cancel_token_;
}

void DownloadWorker::SetTask(Task task) {
  if (task) {
    SetTask(
        [task = std::move(task)](std::string* err, const std::atomic<bool>*) { return task(err); });
  } else {
    task_ = nullptr;
    last_percent_ = -1;
    has_progress_emit_ = false;
  }
}

void DownloadWorker::SetTask(TaskWithToken task) {
  cancel_token_.store(false, std::memory_order_relaxed);
  task_ = std::move(task);
  last_percent_ = -1;
  has_progress_emit_ = false;
}

void DownloadWorker::ReportProgress(int percent, const QString& speed) {
  if (IsCancelled()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (has_progress_emit_ && percent == last_percent_ && percent < 100) {
    return;
  }
  if (has_progress_emit_ && percent < 100) {
    const double elapsed_seconds = std::chrono::duration<double>(now - last_progress_emit_).count();
    if (elapsed_seconds < 0.1 && percent != last_percent_) {
      return;
    }
  }
  last_percent_ = percent;
  last_progress_emit_ = now;
  has_progress_emit_ = true;
  emit progress(percent, speed);
}

void DownloadWorker::run() {
  if (!task_)
    return;
  std::string error_msg;
  const bool ok = task_(&error_msg, &cancel_token_);
  if (!ok && !IsCancelled()) {
    emit error(QString::fromStdString(error_msg));
  }
}

} // namespace vinput::gui
