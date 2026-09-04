#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/config/core_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/llm/adapter_manager.h"
#include "common/utils/debug_log.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/postprocess/post_processor.h"
#include "daemon/remote/remote_text_service.h"
#include "daemon/runtime/daemon_runtime_controller.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/recognition_pipeline.h"

namespace {

std::atomic<bool> g_running{true};

void signal_handler(int sig) {
  (void)sig;
  g_running = false;
}

std::string ReadAvailableText(int fd) {
  std::string text;
  char buffer[4096];
  while (true) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      text.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    break;
  }
  return text;
}

constexpr auto kAdapterGracefulStopTimeout = std::chrono::seconds(2);
constexpr auto kAdapterForceKillTimeout = std::chrono::seconds(3);
constexpr auto kAdapterPollInterval = std::chrono::milliseconds(100);

bool WaitForProcessExit(pid_t pid, std::chrono::milliseconds timeout, int* status_out = nullptr) {
  if (pid <= 0) {
    return true;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t rc = waitpid(pid, &status, WNOHANG);
    if (rc == pid) {
      if (status_out) {
        *status_out = status;
      }
      return true;
    }
    if (rc < 0 && errno == ECHILD) {
      return true;
    }
    if (rc < 0 && errno != EINTR) {
      return false;
    }
    std::this_thread::sleep_for(kAdapterPollInterval);
  }

  return false;
}

void TerminateProcessBounded(pid_t pid) {
  if (pid <= 0) {
    return;
  }

  kill(pid, SIGTERM);
  if (WaitForProcessExit(pid, kAdapterGracefulStopTimeout)) {
    return;
  }

  kill(pid, SIGKILL);
  (void)WaitForProcessExit(pid, kAdapterForceKillTimeout);
}

class AdapterSupervisor {
public:
  explicit AdapterSupervisor(DbusService* dbus) : dbus_(dbus) {}
  AdapterSupervisor(const AdapterSupervisor&) = delete;
  AdapterSupervisor& operator=(const AdapterSupervisor&) = delete;
  AdapterSupervisor(AdapterSupervisor&&) = delete;
  AdapterSupervisor& operator=(AdapterSupervisor&&) = delete;

  ~AdapterSupervisor() {
    Shutdown();
    if (wake_fd_ >= 0) {
      close(wake_fd_);
    }
  }

  bool Start(std::string* error) {
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
      if (error) {
        *error = std::string("failed to create adapter wake fd: ") + strerror(errno);
      }
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = true;
      stop_requested_ = false;
    }

    try {
      thread_ = std::thread([this]() { Run(); });
    } catch (const std::exception& e) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        stop_requested_ = true;
      }
      if (error) {
        *error = std::string("failed to start adapter supervisor thread: ") + e.what();
      }
      close(wake_fd_);
      wake_fd_ = -1;
      return false;
    }

    if (error) {
      error->clear();
    }
    return true;
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      running_ = false;
      stop_requested_ = true;
    }
    Wake();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  DbusService::MethodResult StartAdapter(const std::string& adapter_id) {
    return SubmitRequest(Request::Type::Start, adapter_id);
  }

  DbusService::MethodResult StopAdapter(const std::string& adapter_id) {
    return SubmitRequest(Request::Type::Stop, adapter_id);
  }

  void AutoStart(const CoreConfig& config) {
    for (const auto& adapter : config.llm.adapters) {
      if (!adapter.autoStart) {
        continue;
      }
      auto result = StartAdapter(adapter.id);
      if (!result.ok) {
        fprintf(stderr, "vinput-daemon: failed to autostart adapter '%s': %s\n", adapter.id.c_str(),
                result.error.c_str());
      } else {
        vinput::debug::Log("autostarted adapter '%s'\n", adapter.id.c_str());
      }
    }
  }

private:
  struct ManagedAdapter {
    std::string id;
    pid_t pid = -1;
    int stderr_fd = -1;
    std::string stderr_buffer;
  };

  struct Request {
    enum class Type { Start, Stop };

    Type type = Type::Start;
    std::string adapter_id;
    DbusService::MethodResult result;
    bool done = false;
    std::condition_variable cv;
  };

  DbusService::MethodResult SubmitRequest(Request::Type type, const std::string& adapter_id) {
    auto request = std::make_shared<Request>();
    request->type = type;
    request->adapter_id = adapter_id;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || stop_requested_) {
        return DbusService::MethodResult::Failure("adapter supervisor is not running");
      }
      pending_requests_.push_back(request);
    }

    Wake();

    std::unique_lock<std::mutex> lock(mutex_);
    request->cv.wait(lock, [&]() { return request->done; });
    return request->result;
  }

  void Wake() {
    if (wake_fd_ < 0) {
      return;
    }
    uint64_t value = 1;
    (void)write(wake_fd_, &value, sizeof(value));
  }

  void DrainWakeFd() {
    if (wake_fd_ < 0) {
      return;
    }
    uint64_t value = 0;
    while (read(wake_fd_, &value, sizeof(value)) == sizeof(value)) {
    }
  }

  void EmitNotification(std::string_view text) {
    const std::string notification = vinput::str::TrimAsciiWhitespace(text);
    if (notification.empty()) {
      return;
    }
    dbus_->EmitNotification(vinput::dbus::MakeRawError(notification));
  }

  void FlushAdapterBuffer(ManagedAdapter& adapter, bool flush_partial) {
    size_t start = 0;
    while (true) {
      const size_t end = adapter.stderr_buffer.find('\n', start);
      if (end == std::string::npos) {
        break;
      }
      std::string line = adapter.stderr_buffer.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty()) {
        vinput::debug::Log("adapter[%s] stderr: %s\n", adapter.id.c_str(), line.c_str());
        EmitNotification(line);
      }
      start = end + 1;
    }

    adapter.stderr_buffer.erase(0, start);
    if (flush_partial) {
      std::string line = vinput::str::TrimAsciiWhitespace(adapter.stderr_buffer);
      if (!line.empty()) {
        vinput::debug::Log("adapter[%s] stderr: %s\n", adapter.id.c_str(), line.c_str());
        EmitNotification(line);
      }
      adapter.stderr_buffer.clear();
    }
  }

  DbusService::MethodResult HandleStartRequest(const std::string& adapter_id) {
    auto runtime_settings = LoadCoreConfig();
    NormalizeCoreConfig(&runtime_settings);
    const auto* adapter = ResolveLlmAdapter(runtime_settings, adapter_id);
    if (!adapter) {
      return DbusService::MethodResult::Failure("adapter not found");
    }

    if (adapters_.find(adapter_id) != adapters_.end() || vinput::adapter::IsRunning(adapter_id)) {
      return DbusService::MethodResult::Failure("adapter is already running");
    }

    if (adapter->command.empty()) {
      return DbusService::MethodResult::Failure("adapter command is not configured");
    }

    std::string error;
    const auto spec = vinput::adapter::BuildCommandSpec(*adapter);
    const auto working_dir = vinput::adapter::ResolveWorkingDir(*adapter);
    vinput::process::SpawnedProcess process;
    if (!vinput::process::SpawnForMonitoring(spec, working_dir, &process, &error)) {
      return DbusService::MethodResult::Failure(error.empty() ? "failed to start adapter" : error);
    }

    usleep(250000);
    int status = 0;
    if (waitpid(process.pid, &status, WNOHANG) == process.pid) {
      std::string stderr_text = ReadAvailableText(process.stderr_fd);
      close(process.stderr_fd);
      process.stderr_fd = -1;
      stderr_text = vinput::str::TrimAsciiWhitespace(stderr_text);
      if (stderr_text.empty()) {
        stderr_text = "adapter exited immediately";
      }
      return DbusService::MethodResult::Failure(stderr_text);
    }

    if (!vinput::adapter::WritePidFile(adapter_id, process.pid, &error)) {
      TerminateProcessBounded(process.pid);
      close(process.stderr_fd);
      process.stderr_fd = -1;
      return DbusService::MethodResult::Failure(error.empty() ? "failed to persist adapter pid"
                                                              : error);
    }

    adapters_.emplace(adapter_id, ManagedAdapter{
                                      .id = adapter_id,
                                      .pid = process.pid,
                                      .stderr_fd = process.stderr_fd,
                                      .stderr_buffer = {},
                                  });
    vinput::debug::Log("adapter started id=%s pid=%d\n", adapter_id.c_str(),
                       static_cast<int>(process.pid));
    return DbusService::MethodResult::Success();
  }

  DbusService::MethodResult HandleStopRequest(const std::string& adapter_id) {
    auto runtime_settings = LoadCoreConfig();
    NormalizeCoreConfig(&runtime_settings);
    if (!ResolveLlmAdapter(runtime_settings, adapter_id)) {
      return DbusService::MethodResult::Failure("adapter not found");
    }

    auto it = adapters_.find(adapter_id);
    pid_t tracked_pid = -1;
    if (it != adapters_.end()) {
      tracked_pid = it->second.pid;
      if (it->second.stderr_fd >= 0) {
        close(it->second.stderr_fd);
        it->second.stderr_fd = -1;
      }
      adapters_.erase(it);
    }

    if (tracked_pid > 0) {
      TerminateProcessBounded(tracked_pid);
      vinput::adapter::RemovePidFile(adapter_id);
      vinput::debug::Log("adapter stopped id=%s\n", adapter_id.c_str());
      return DbusService::MethodResult::Success();
    }

    std::string error;
    if (!vinput::adapter::Stop(adapter_id, &error)) {
      return DbusService::MethodResult::Failure(error.empty() ? "failed to stop adapter" : error);
    }

    vinput::debug::Log("adapter stopped id=%s\n", adapter_id.c_str());
    return DbusService::MethodResult::Success();
  }

  void ProcessPendingRequests() {
    std::vector<std::shared_ptr<Request>> requests;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests.swap(pending_requests_);
    }

    for (const auto& request : requests) {
      DbusService::MethodResult result;
      if (request->type == Request::Type::Start) {
        result = HandleStartRequest(request->adapter_id);
      } else {
        result = HandleStopRequest(request->adapter_id);
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        request->result = std::move(result);
        request->done = true;
      }
      request->cv.notify_one();
    }
  }

  void ReapExitedAdapters() {
    std::vector<std::string> exited_ids;
    for (auto& [id, adapter] : adapters_) {
      int status = 0;
      if (waitpid(adapter.pid, &status, WNOHANG) != adapter.pid) {
        continue;
      }

      if (adapter.stderr_fd >= 0) {
        adapter.stderr_buffer += ReadAvailableText(adapter.stderr_fd);
      }
      FlushAdapterBuffer(adapter, true);
      if (adapter.stderr_fd >= 0) {
        close(adapter.stderr_fd);
        adapter.stderr_fd = -1;
      }

      vinput::debug::Log("adapter exited id=%s code=%d\n", adapter.id.c_str(),
                         WIFEXITED(status) ? WEXITSTATUS(status)
                                           : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1));
      exited_ids.push_back(id);
    }

    for (const auto& id : exited_ids) {
      vinput::adapter::RemovePidFile(id);
      adapters_.erase(id);
    }
  }

  void PollAdaptersOnce() {
    std::vector<pollfd> fds;
    fds.reserve(1 + adapters_.size());
    fds.push_back({.fd = wake_fd_, .events = POLLIN, .revents = 0});

    std::vector<std::string> adapter_ids;
    adapter_ids.reserve(adapters_.size());
    for (const auto& [id, adapter] : adapters_) {
      if (adapter.stderr_fd < 0) {
        continue;
      }
      fds.push_back({.fd = adapter.stderr_fd,
                     .events = static_cast<short>(POLLIN | POLLHUP | POLLERR),
                     .revents = 0});
      adapter_ids.push_back(id);
    }

    const int ret = poll(fds.data(), static_cast<nfds_t>(fds.size()), 1000);
    if (ret < 0) {
      if (errno != EINTR) {
        fprintf(stderr, "vinput-daemon: adapter poll error: %s\n", strerror(errno));
      }
      return;
    }

    if (ret > 0 && (fds[0].revents & POLLIN)) {
      DrainWakeFd();
      ProcessPendingRequests();
    }

    for (size_t i = 0; i < adapter_ids.size(); ++i) {
      auto it = adapters_.find(adapter_ids[i]);
      if (it == adapters_.end()) {
        continue;
      }

      ManagedAdapter& adapter = it->second;
      const pollfd& fd = fds[i + 1];
      if ((fd.revents & (POLLIN | POLLHUP | POLLERR)) == 0 || adapter.stderr_fd < 0) {
        continue;
      }

      adapter.stderr_buffer += ReadAvailableText(adapter.stderr_fd);
      const bool closing_stderr = (fd.revents & (POLLHUP | POLLERR)) != 0;
      FlushAdapterBuffer(adapter, closing_stderr);
      if (closing_stderr) {
        close(adapter.stderr_fd);
        adapter.stderr_fd = -1;
      }
    }

    ReapExitedAdapters();
  }

  void ShutdownAdapters() {
    for (auto& [id, adapter] : adapters_) {
      if (adapter.stderr_fd >= 0) {
        close(adapter.stderr_fd);
        adapter.stderr_fd = -1;
      }
      if (adapter.pid > 0) {
        TerminateProcessBounded(adapter.pid);
      }
      vinput::adapter::RemovePidFile(id);
    }
    adapters_.clear();
  }

  void Run() {
    while (true) {
      ProcessPendingRequests();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
          break;
        }
      }
      PollAdaptersOnce();
    }

    ProcessPendingRequests();
    ShutdownAdapters();
  }

  DbusService* dbus_ = nullptr;
  int wake_fd_ = -1;
  std::thread thread_;
  std::mutex mutex_;
  bool running_ = false;
  bool stop_requested_ = false;
  std::vector<std::shared_ptr<Request>> pending_requests_;
  std::map<std::string, ManagedAdapter> adapters_;
};

} // namespace

int main(int argc, char* argv[]) {
  vinput::i18n::Init();
  signal(SIGTERM, signal_handler);
  signal(SIGINT, signal_handler);

  bool disable_asr = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-asr") == 0) {
      disable_asr = true;
    }
  }

  auto startup_settings = LoadCoreConfig();
  NormalizeCoreConfig(&startup_settings);
  vinput::daemon::asr::RecognitionSessionManager recognition_manager(disable_asr);

  std::string asr_disabled_reason;
  if (!recognition_manager.Initialize(startup_settings, &asr_disabled_reason)) {
    fprintf(stderr, "vinput-daemon: running with ASR disabled");
    if (!asr_disabled_reason.empty()) {
      fprintf(stderr, " (%s)", asr_disabled_reason.c_str());
    }
    fprintf(stderr, "\n");
  }

  AudioCapture capture;
  std::string capture_error;
  if (!capture.Start(&capture_error)) {
    if (capture_error.empty()) {
      capture_error = "audio capture start failed";
    }
    fprintf(stderr, "vinput-daemon: %s\n", capture_error.c_str());
    return 1;
  }

  DbusService dbus;
  PostProcessor post_processor;

  using vinput::dbus::Status;
  using vinput::dbus::StatusToString;

  // --- Single-slot state (all protected by state_mutex) ---
  AdapterSupervisor adapter_supervisor(&dbus);
  vinput::daemon::remote::RemoteTextService remote_text_service;
  vinput::daemon::runtime::RecognitionPipeline recognition_pipeline(&post_processor);
  vinput::daemon::runtime::DaemonRuntimeController runtime_controller(
      &capture, &dbus, &recognition_manager, &recognition_pipeline, &remote_text_service);

  dbus.SetStartHandler([&]() { return runtime_controller.StartRecording(); });

  dbus.SetStartCommandHandler([&](const std::string& selected_text) {
    return runtime_controller.StartCommandRecording(selected_text);
  });

  dbus.SetStopHandler([&](const std::string& scene_id) -> DbusService::MethodResult {
    return runtime_controller.StopRecording(scene_id);
  });

  dbus.SetStatusHandler([&]() -> std::string { return runtime_controller.GetStatus(); });
  dbus.SetAsrBackendStateHandler(
      [&]() -> vinput::dbus::AsrBackendState { return runtime_controller.GetAsrBackendState(); });
  dbus.SetReloadAsrBackendHandler([&]() { return runtime_controller.ReloadAsrBackend(); });

  dbus.SetStartAdapterHandler([&](const std::string& adapter_id) -> DbusService::MethodResult {
    return adapter_supervisor.StartAdapter(adapter_id);
  });

  dbus.SetStopAdapterHandler([&](const std::string& adapter_id) -> DbusService::MethodResult {
    return adapter_supervisor.StopAdapter(adapter_id);
  });

  std::string dbus_error;
  if (!dbus.Start(&dbus_error)) {
    if (dbus_error.empty()) {
      dbus_error = "DBus service start failed";
    }
    fprintf(stderr, "vinput-daemon: %s\n", dbus_error.c_str());
    return 1;
  }

  std::string adapter_error;
  if (!adapter_supervisor.Start(&adapter_error)) {
    fprintf(stderr, "vinput-daemon: %s\n", adapter_error.c_str());
    return 1;
  }
  adapter_supervisor.AutoStart(startup_settings);

  runtime_controller.StartWorker();
  std::string remote_error;
  if (!remote_text_service.Synchronize(startup_settings, &remote_error)) {
    fprintf(stderr, "vinput-daemon: remote ASR service disabled");
    if (!remote_error.empty()) {
      fprintf(stderr, " (%s)", remote_error.c_str());
    }
    fprintf(stderr, "\n");
  }

  fprintf(stderr, "vinput-daemon: running\n");

  int dbus_fd = dbus.GetFd();
  int notify_fd = dbus.GetNotifyFd();
  int runtime_notify_fd = runtime_controller.GetNotifyFd();
  while (g_running) {
    pollfd fds[3] = {
        {.fd = dbus_fd, .events = POLLIN, .revents = 0},
        {.fd = notify_fd, .events = POLLIN, .revents = 0},
        {.fd = runtime_notify_fd, .events = POLLIN, .revents = 0},
    };

    int ret = poll(fds, 3, 1000);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "vinput-daemon: poll error: %s\n", strerror(errno));
      break;
    }

    // Bounded warm grace: destroy inactive reusable streams after idle timeout.
    capture.MaybeDestroyExpiredStream();

    if (ret > 0) {
      if (fds[1].revents & POLLIN) {
        dbus.FlushEmitQueue();
      }
      if (fds[2].revents & POLLIN) {
        runtime_controller.FlushDeferredActions();
      }
      if (fds[0].revents & POLLIN) {
        while (dbus.ProcessOnce()) {
          // process all pending messages
        }
      }
    }
  }

  fprintf(stderr, "vinput-daemon: shutting down\n");
  post_processor.Shutdown();
  remote_text_service.Shutdown();
  runtime_controller.Shutdown();
  recognition_manager.Shutdown();
  adapter_supervisor.Shutdown();
  return 0;
}
