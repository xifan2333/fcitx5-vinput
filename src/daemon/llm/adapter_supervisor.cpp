#include "daemon/llm/adapter_supervisor.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <sched.h> // pid_t (include-cleaner provider in this translation unit)
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/types.h> // ssize_t
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/config/core_config.h"
#include "common/dbus/error_info.h"
#include "common/llm/adapter_manager.h"
#include "common/utils/debug_log.h"
#include "common/utils/process_utils.h"
#include "common/utils/string_utils.h"

#include "daemon/runtime/dbus_service.h"

namespace vinput::daemon::llm {

namespace {

constexpr auto kAdapterGracefulStopTimeout = std::chrono::seconds(2);
constexpr auto kAdapterForceKillTimeout = std::chrono::seconds(3);

std::string ReadAvailableText(int fd) {
  std::string text;
  char buffer[4096];
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      text.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    break;
  }
  return text;
}

} // namespace

class AdapterSupervisor::Impl {
public:
  explicit Impl(DbusService* dbus) : dbus_(dbus) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() {
    Shutdown();
    if (wake_fd_ >= 0) {
      close(wake_fd_);
    }
  }

  bool Start(std::string* error) {
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
      if (error != nullptr) {
        *error = std::string("failed to create adapter wake fd: ") + strerror(errno);
      }
      return false;
    }

    {
      const std::scoped_lock lock(mutex_);
      running_ = true;
      stop_requested_ = false;
    }

    try {
      thread_ = std::thread([this]() { Run(); });
    } catch (const std::exception& e) {
      {
        const std::scoped_lock lock(mutex_);
        running_ = false;
        stop_requested_ = true;
      }
      if (error != nullptr) {
        *error = std::string("failed to start adapter supervisor thread: ") + e.what();
      }
      close(wake_fd_);
      wake_fd_ = -1;
      return false;
    }

    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  void Shutdown() {
    {
      const std::scoped_lock lock(mutex_);
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
                result.message.c_str());
      } else {
        VLOG_DEBUG("autostarted adapter '{}'", adapter.id);
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
    enum class Type : std::uint8_t { Start, Stop };

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
      const std::scoped_lock lock(mutex_);
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

  void Wake() const {
    if (wake_fd_ < 0) {
      return;
    }
    uint64_t value = 1;
    (void)write(wake_fd_, &value, sizeof(value));
  }

  void DrainWakeFd() const {
    if (wake_fd_ < 0) {
      return;
    }
    uint64_t value = 0;
    while (read(wake_fd_, &value, sizeof(value)) == sizeof(value)) {
    }
  }

  void EmitNotification(std::string_view text) {
    const std::string notification(vinput::str::Trim(text));
    if (notification.empty()) {
      return;
    }
    dbus_->EmitNotification(vinput::dbus::MakeRawError(notification));
  }

  void FlushAdapterBuffer(ManagedAdapter& adapter, bool flush_partial) {
    vinput::process::DrainLinesFromBuffer(
        adapter.stderr_buffer,
        [this, &adapter](std::string_view raw_line) {
          const std::string line(vinput::str::Trim(raw_line));
          if (!line.empty()) {
            VLOG_DEBUG("adapter[{}] stderr: {}", adapter.id, line);
            EmitNotification(line);
          }
        },
        flush_partial);
  }

  DbusService::MethodResult HandleStartRequest(const std::string& adapter_id) {
    auto runtime_settings = LoadCoreConfig();
    NormalizeCoreConfig(&runtime_settings);
    const auto* adapter = ResolveLlmAdapter(runtime_settings, adapter_id);
    if (adapter == nullptr) {
      return DbusService::MethodResult::Failure("adapter not found");
    }

    if (adapters_.contains(adapter_id) || vinput::adapter::IsRunning(adapter_id)) {
      return DbusService::MethodResult::Failure("adapter is already running");
    }

    if (adapter->command.empty()) {
      return DbusService::MethodResult::Failure("adapter command is not configured");
    }

    const auto spec = vinput::adapter::BuildCommandSpec(*adapter);
    const auto working_dir = vinput::adapter::ResolveWorkingDir(*adapter);
    auto process_res = vinput::process::SpawnForMonitoring(spec, working_dir);
    if (!process_res) {
      return DbusService::MethodResult::Failure(
          process_res.error().empty() ? "failed to start adapter" : process_res.error());
    }
    vinput::process::SpawnedProcess process = *process_res;

    usleep(250000);
    int exit_code = -1;
    if (vinput::process::TryReapProcess(process.pid, &exit_code)) {
      std::string stderr_text = ReadAvailableText(process.stderr_fd);
      close(process.stderr_fd);
      process.stderr_fd = -1;
      stderr_text = std::string(vinput::str::Trim(stderr_text));
      if (stderr_text.empty()) {
        stderr_text = "adapter exited immediately";
      }
      return DbusService::MethodResult::Failure(stderr_text);
    }

    std::string error;
    if (!vinput::adapter::WritePidFile(adapter_id, process.pid, &error)) {
      vinput::process::TerminateProcessBounded(process.pid, kAdapterGracefulStopTimeout,
                                               kAdapterForceKillTimeout);
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
    VLOG_DEBUG("adapter started id={} pid={}", adapter_id, static_cast<int>(process.pid));
    return DbusService::MethodResult::Success();
  }

  DbusService::MethodResult HandleStopRequest(const std::string& adapter_id) {
    auto runtime_settings = LoadCoreConfig();
    NormalizeCoreConfig(&runtime_settings);
    if (ResolveLlmAdapter(runtime_settings, adapter_id) == nullptr) {
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
      vinput::process::TerminateProcessBounded(tracked_pid, kAdapterGracefulStopTimeout,
                                               kAdapterForceKillTimeout);
      vinput::adapter::RemovePidFile(adapter_id);
      VLOG_DEBUG("adapter stopped id={}", adapter_id);
      return DbusService::MethodResult::Success();
    }

    std::string error;
    if (!vinput::adapter::Stop(adapter_id, &error)) {
      return DbusService::MethodResult::Failure(error.empty() ? "failed to stop adapter" : error);
    }

    VLOG_DEBUG("adapter stopped id={}", adapter_id);
    return DbusService::MethodResult::Success();
  }

  void ProcessPendingRequests() {
    std::vector<std::shared_ptr<Request>> requests;
    {
      const std::scoped_lock lock(mutex_);
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
        const std::scoped_lock lock(mutex_);
        request->result = std::move(result);
        request->done = true;
      }
      request->cv.notify_one();
    }
  }

  void ReapExitedAdapters() {
    std::vector<std::string> exited_ids;
    for (auto& [id, adapter] : adapters_) {
      int exit_code = -1;
      if (!vinput::process::TryReapProcess(adapter.pid, &exit_code)) {
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

      VLOG_DEBUG("adapter exited id={} code={}", adapter.id, exit_code);
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

    if (ret > 0 && ((fds[0].revents & POLLIN) != 0)) {
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
        vinput::process::TerminateProcessBounded(adapter.pid, kAdapterGracefulStopTimeout,
                                                 kAdapterForceKillTimeout);
      }
      vinput::adapter::RemovePidFile(id);
    }
    adapters_.clear();
  }

  void Run() {
    while (true) {
      ProcessPendingRequests();
      {
        const std::scoped_lock lock(mutex_);
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

AdapterSupervisor::AdapterSupervisor(DbusService* dbus) : impl_(std::make_unique<Impl>(dbus)) {}

AdapterSupervisor::~AdapterSupervisor() = default;

bool AdapterSupervisor::Start(std::string* error) {
  return impl_->Start(error);
}

void AdapterSupervisor::Shutdown() {
  impl_->Shutdown();
}

DbusService::MethodResult AdapterSupervisor::StartAdapter(const std::string& adapter_id) {
  return impl_->StartAdapter(adapter_id);
}

DbusService::MethodResult AdapterSupervisor::StopAdapter(const std::string& adapter_id) {
  return impl_->StopAdapter(adapter_id);
}

void AdapterSupervisor::AutoStart(const CoreConfig& config) {
  impl_->AutoStart(config);
}

} // namespace vinput::daemon::llm
