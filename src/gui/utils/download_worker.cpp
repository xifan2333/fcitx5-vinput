#include "gui/utils/download_worker.h"

namespace vinput::gui {

DownloadWorker::DownloadWorker(QObject* parent) : QThread(parent) {}

DownloadWorker::~DownloadWorker() {
    wait();
}

void DownloadWorker::SetTask(std::function<bool(std::string*)> task) {
    task_ = std::move(task);
    last_percent_ = -1;
    has_progress_emit_ = false;
}

void DownloadWorker::ReportProgress(int percent, const QString& speed) {
    const auto now = std::chrono::steady_clock::now();
    if (has_progress_emit_ && percent == last_percent_ && percent < 100) {
        return;
    }
    if (has_progress_emit_ && percent < 100) {
        const double elapsed_seconds =
            std::chrono::duration<double>(now - last_progress_emit_).count();
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
    if (!task_) return;
    std::string error_msg;
    if (!task_(&error_msg)) {
        emit error(QString::fromStdString(error_msg));
    }
}

} // namespace vinput::gui
