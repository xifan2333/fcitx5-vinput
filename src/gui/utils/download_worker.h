#pragma once

#include <QString>
#include <QThread>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace vinput::gui {

class DownloadWorker : public QThread {
  Q_OBJECT
public:
  using Task = std::function<bool(std::string*)>;
  using TaskWithToken = std::function<bool(std::string*, const std::atomic<bool>*)>;

  explicit DownloadWorker(QObject* parent = nullptr);
  ~DownloadWorker() override;

  // The task should return true on success, or set error and return false.
  void SetTask(Task task);
  void SetTask(TaskWithToken task);

  // For tasks that report progress
  void ReportProgress(int percent, const QString& speed);

  // Cooperative cancellation
  void Cancel() noexcept;
  [[nodiscard]] bool IsCancelled() const noexcept;
  [[nodiscard]] const std::atomic<bool>* CancelToken() const noexcept;

signals:
  void progress(int percent, QString speed);
  void error(QString message);

protected:
  void run() override;

private:
  TaskWithToken task_;
  std::atomic<bool> cancel_token_{false};
  int last_percent_ = -1;
  std::chrono::steady_clock::time_point last_progress_emit_{};
  bool has_progress_emit_ = false;
};

} // namespace vinput::gui
