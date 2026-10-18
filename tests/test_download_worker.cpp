#include <QtCore/qcoreapplication.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qobject.h>
#include <QtCore/qthread.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "gui/utils/download_worker.h"

namespace {

void AssertTrue(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void ProcessEventsUntil(const std::function<bool()>& predicate,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto start = std::chrono::steady_clock::now();
  while (!predicate()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
      std::cerr << "Timed out waiting for predicate\n";
      std::abort();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void TestSuccessTask(QCoreApplication& app) {
  auto* worker = new vinput::gui::DownloadWorker();
  bool finished_emitted = false;
  bool error_emitted = false;

  QObject::connect(worker, &QThread::finished, &app, [&] { finished_emitted = true; });
  QObject::connect(worker, &vinput::gui::DownloadWorker::error, &app,
                   [&](const QString&) { error_emitted = true; });

  worker->SetTask([](std::string* /*err*/) -> bool { return true; });
  worker->start();

  ProcessEventsUntil([&] { return finished_emitted; });
  AssertTrue(!error_emitted, "success task must not emit error");
  AssertTrue(!worker->IsCancelled(), "worker should not be cancelled");

  worker->deleteLater();
  ProcessEventsUntil([&] { return true; }, std::chrono::milliseconds(50));
}

void TestFailureTaskEmitsError(QCoreApplication& app) {
  auto* worker = new vinput::gui::DownloadWorker();
  bool finished_emitted = false;
  QString received_error;

  QObject::connect(worker, &QThread::finished, &app, [&] { finished_emitted = true; });
  QObject::connect(worker, &vinput::gui::DownloadWorker::error, &app,
                   [&](const QString& msg) { received_error = msg; });

  worker->SetTask([](std::string* err) -> bool {
    if (err != nullptr) {
      *err = "test failure message";
    }
    return false;
  });
  worker->start();

  ProcessEventsUntil([&] { return finished_emitted; });
  AssertTrue(received_error == "test failure message", "failure task must emit expected error");
  AssertTrue(!worker->IsCancelled(), "worker should not be cancelled");

  worker->deleteLater();
}

void TestCooperativeCancellation(QCoreApplication& app) {
  auto* worker = new vinput::gui::DownloadWorker();
  bool finished_emitted = false;
  bool error_emitted = false;
  std::atomic<bool> task_started{false};

  QObject::connect(worker, &QThread::finished, &app, [&] { finished_emitted = true; });
  QObject::connect(worker, &vinput::gui::DownloadWorker::error, &app,
                   [&](const QString&) { error_emitted = true; });

  worker->SetTask([&](std::string* err, const std::atomic<bool>* cancel_token) -> bool {
    task_started.store(true);
    while (cancel_token != nullptr && !cancel_token->load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (err != nullptr) {
      *err = "task aborted";
    }
    return false;
  });
  worker->start();

  ProcessEventsUntil([&] { return task_started.load(); });
  AssertTrue(!worker->IsCancelled(), "worker not cancelled yet");

  worker->Cancel();
  AssertTrue(worker->IsCancelled(), "worker must be cancelled after Cancel()");

  ProcessEventsUntil([&] { return finished_emitted; });
  AssertTrue(!error_emitted, "cancelled task must not emit error");

  worker->deleteLater();
}

void TestDestructorCancelsAndWaits() {
  std::atomic<bool> task_running{true};
  {
    vinput::gui::DownloadWorker worker;
    worker.SetTask([&](std::string*, const std::atomic<bool>* cancel_token) -> bool {
      while (task_running.load() &&
             (cancel_token == nullptr || !cancel_token->load(std::memory_order_relaxed))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return false;
    });
    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  task_running.store(false);
}

void TestProgressSuppressionOnCancel(QCoreApplication& app) {
  vinput::gui::DownloadWorker worker;
  int progress_count = 0;
  QObject::connect(&worker, &vinput::gui::DownloadWorker::progress, &app,
                   [&](int, const QString&) { ++progress_count; });

  worker.ReportProgress(10, "1 MB/s");
  QCoreApplication::processEvents();
  AssertTrue(progress_count == 1, "progress emitted when not cancelled");

  worker.Cancel();
  worker.ReportProgress(20, "2 MB/s");
  QCoreApplication::processEvents();
  AssertTrue(progress_count == 1, "progress dropped after Cancel()");
}

} // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);

  TestSuccessTask(app);
  TestFailureTaskEmitsError(app);
  TestCooperativeCancellation(app);
  TestDestructorCancelsAndWaits();
  TestProgressSuppressionOnCancel(app);

  std::cout << "All DownloadWorker tests passed!\n";
  return 0;
}
