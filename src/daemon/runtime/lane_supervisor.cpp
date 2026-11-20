#include "daemon/runtime/lane_supervisor.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <utility>
#include <variant>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/client/client_command.h"
#include "common/client/client_event.h"
#include "common/client/client_manager.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"
#include "common/notify/notification_bridge.h"
#include "common/utils/debug_log.h"
#include "common/utils/unique_fd.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_capture.h"
#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/fd_stream_audio_source.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/notify/desktop_notification_sender.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/lane_event_sink.h"
#include "daemon/runtime/recognition_pipeline.h"
#include "daemon/runtime/session_lane.h"

namespace vinput::daemon::runtime {

namespace {

template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

class DbusLaneSinkBase : public LaneEventSink {
public:
  explicit DbusLaneSinkBase(DbusService* dbus) : dbus_(dbus) {}

  void OnNotification(const vinput::notify::Notification& notification) final {
    if (dbus_ != nullptr) {
      dbus_->EmitDesktopNotification(notification);
    }
  }

protected:
  void EmitImeStatus(vinput::dbus::Status status, bool ime) const {
    if (ime && dbus_ != nullptr) {
      dbus_->EmitStatusChanged(vinput::dbus::StatusToString(status));
    }
  }

  void EmitImePartial(std::string_view text, bool ime) const {
    if (ime && dbus_ != nullptr) {
      dbus_->EmitRecognitionPartial(std::string(text));
    }
  }

  void EmitImeResult(const vinput::result::Payload& payload, bool ime) const {
    if (ime && dbus_ != nullptr) {
      dbus_->EmitRecognitionResult(vinput::result::Serialize(payload));
    }
  }

  [[nodiscard]] DbusService* Dbus() const noexcept { return dbus_; }

private:
  DbusService* dbus_ = nullptr;
};

class PrimaryImeSink final : public DbusLaneSinkBase {
public:
  PrimaryImeSink(DbusService* dbus, LaneSupervisorOptions::ClientBroadcastFn broadcast)
      : DbusLaneSinkBase(dbus), broadcast_(std::move(broadcast)) {}

  void OnStatusChanged(vinput::dbus::Status status, bool ime) override {
    EmitImeStatus(status, ime);
    if (broadcast_) {
      broadcast_(vinput::client::SerializeStatusEvent(vinput::dbus::StatusToString(status), ime));
    }
  }

  void OnPartialText(std::string_view text, bool ime) override {
    EmitImePartial(text, ime);
    if (broadcast_) {
      broadcast_(vinput::client::SerializePartialEvent(text, ime));
    }
  }

  void OnRecognitionResult(const vinput::result::Payload& payload, bool ime) override {
    EmitImeResult(payload, ime);
    if (!payload.commitText.empty() && broadcast_) {
      broadcast_(vinput::client::SerializeCommittedEvent(payload, ime));
    }
  }

private:
  LaneSupervisorOptions::ClientBroadcastFn broadcast_;
};

class HeadlessClientSink final : public DbusLaneSinkBase {
public:
  HeadlessClientSink(std::string client_id, DbusService* dbus,
                     LaneSupervisorOptions::ClientSendToFn send_to)
      : DbusLaneSinkBase(dbus), client_id_(std::move(client_id)), send_to_(std::move(send_to)) {}

  void OnStatusChanged(vinput::dbus::Status status, bool ime) override {
    if (send_to_) {
      send_to_(client_id_,
               vinput::client::SerializeStatusEvent(vinput::dbus::StatusToString(status), ime));
    }
  }

  void OnPartialText(std::string_view text, bool ime) override {
    if (send_to_) {
      send_to_(client_id_, vinput::client::SerializePartialEvent(text, ime));
    }
  }

  void OnRecognitionResult(const vinput::result::Payload& payload, bool ime) override {
    if (!payload.commitText.empty() && send_to_) {
      send_to_(client_id_, vinput::client::SerializeCommittedEvent(payload, ime));
    }
  }

private:
  std::string client_id_;
  LaneSupervisorOptions::ClientSendToFn send_to_;
};

class StreamLaneSink final : public DbusLaneSinkBase {
public:
  StreamLaneSink(std::string client_id, DbusService* dbus,
                 LaneSupervisorOptions::ClientSendToFn send_to)
      : DbusLaneSinkBase(dbus), client_id_(std::move(client_id)), send_to_(std::move(send_to)) {}

  void OnStatusChanged(vinput::dbus::Status status, bool ime) override {
    EmitImeStatus(status, ime);
    EmitStreamLine(vinput::client::SerializeStatusEvent(vinput::dbus::StatusToString(status), ime));
  }

  void OnPartialText(std::string_view text, bool ime) override {
    EmitImePartial(text, ime);
    EmitStreamLine(vinput::client::SerializePartialEvent(text, ime));
  }

  void OnRecognitionResult(const vinput::result::Payload& payload, bool ime) override {
    EmitImeResult(payload, ime);
    if (!payload.commitText.empty()) {
      EmitStreamLine(vinput::client::SerializeCommittedEvent(payload, ime));
    }
  }

private:
  void EmitStreamLine(const std::string& line) const {
    if (Dbus() != nullptr) {
      Dbus()->EmitStreamEvent(client_id_, line);
    }
    if (send_to_) {
      send_to_(client_id_, line);
    }
  }

  std::string client_id_;
  LaneSupervisorOptions::ClientSendToFn send_to_;
};

CoreConfig LoadNormalizedConfig() {
  auto settings = LoadCoreConfig();
  NormalizeCoreConfig(&settings);
  return settings;
}

std::unique_ptr<vinput::daemon::audio::AudioInputSource>
CreateDefaultAudioCapture(std::string* error) {
  auto cap = std::make_unique<AudioCapture>();
  if (!cap->Start(error)) {
    return nullptr;
  }
  return cap;
}

void PopulateDefaultOptions(LaneSupervisorOptions& options,
                            vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
                            RecognitionPipeline* pipeline) {
  if (!options.load_config) {
    options.load_config = LoadNormalizedConfig;
  }
  if (!options.create_asr_session && recognition_manager != nullptr) {
    options.create_asr_session =
        [recognition_manager](const CoreConfig& settings,
                              vinput::daemon::asr::BackendDescriptor* descriptor,
                              std::string* error, const std::function<void()>& wake_callback) {
          return recognition_manager->CreateSession(settings, descriptor, error, wake_callback);
        };
  }
  if (!options.run_postprocess && pipeline != nullptr) {
    options.run_postprocess = [pipeline](const PostprocessRequest& request,
                                         const CoreConfig& settings,
                                         const std::function<void()>& on_enter_postprocessing,
                                         const std::atomic<bool>* cancel_flag) {
      return pipeline->Process(request, settings, on_enter_postprocessing, cancel_flag);
    };
  }
  if (!options.create_headless_capture) {
    options.create_headless_capture = CreateDefaultAudioCapture;
  }
  if (!options.client_broadcast) {
    options.client_broadcast = [](std::string_view line) { vinput::client::Broadcast(line); };
  }
  if (!options.client_send_to) {
    options.client_send_to = [](std::string_view client_id, std::string_view line) {
      return vinput::client::SendTo(client_id, line);
    };
  }
}

int MinPositiveTimeout(int current_min, int candidate) noexcept {
  if (candidate < 0) {
    return current_min;
  }
  if (current_min < 0) {
    return candidate;
  }
  return std::min(current_min, candidate);
}

} // namespace

LaneSupervisor::LaneSupervisor(vinput::daemon::audio::AudioInputSource* primary_capture,
                               DbusService* dbus,
                               vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
                               RecognitionPipeline* pipeline, LaneSupervisorOptions options)
    : dbus_(dbus), recognition_manager_(recognition_manager), options_(std::move(options)),
      primary_ducker_(options_.volume_adapter) {
  PopulateDefaultOptions(options_, recognition_manager_, pipeline);

  notify_fd_.Reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!notify_fd_.IsValid()) {
    VLOG_ERROR("failed to create lane supervisor notify fd: {}", std::strerror(errno));
  }

  if (recognition_manager_ != nullptr) {
    recognition_manager_->SetReloadResultCallback([this](bool success, const std::string& message) {
      if (success || dbus_ == nullptr || message.empty()) {
        return;
      }
      dbus_->EmitDesktopNotification(vinput::daemon::notify::MakeDaemonErrorNotification(
          std::string(_("ASR backend reload failed. The previous backend may still be active.")) +
          "\n" + message));
    });
  }

  primary_sink_ = std::make_unique<PrimaryImeSink>(dbus_, options_.client_broadcast);
  primary_lane_ = std::make_unique<SessionLane>(MakeLaneDependencies(
      primary_capture, primary_sink_.get(), &primary_ducker_, /*warn_on_clipped_head=*/true));
}

LaneSupervisor::~LaneSupervisor() {
  Shutdown();
}

SessionLaneDependencies LaneSupervisor::MakeLaneDependencies(
    vinput::daemon::audio::AudioInputSource* source, LaneEventSink* sink,
    vinput::daemon::audio::OutputDucker* ducker, bool warn_on_clipped_head) const {
  return SessionLaneDependencies{
      .audio_source = source,
      .event_sink = sink,
      .output_ducker = ducker,
      .load_config = options_.load_config,
      .create_asr_session = options_.create_asr_session,
      .run_postprocess = options_.run_postprocess,
      .shared_notify_fd = notify_fd_.Get(),
      .warn_on_clipped_head = warn_on_clipped_head,
      .first_buffer_ready_timeout = options_.first_buffer_ready_timeout,
  };
}

DbusService::MethodResult LaneSupervisor::StartRecording() {
  return primary_lane_->Start(/*ime=*/true, /*is_command=*/false);
}

DbusService::MethodResult LaneSupervisor::StartCommandRecording(const std::string& selected_text) {
  return primary_lane_->Start(/*ime=*/true, /*is_command=*/true, selected_text);
}

DbusService::MethodResult LaneSupervisor::StopRecording(const std::string& scene_id) {
  return primary_lane_->Stop(/*ime=*/true, scene_id);
}

DbusService::MethodResult LaneSupervisor::CancelOperation(bool commit_raw_text) {
  return primary_lane_->Cancel(/*ime=*/true, commit_raw_text);
}

DbusService::MethodResult LaneSupervisor::TranscribeStream(int fd, const std::string& scene_id,
                                                           bool ime) {
  vinput::file::UniqueFd owned_fd(fd);
  if (shutting_down_.load(std::memory_order_acquire)) {
    return DbusService::MethodResult::Failure("daemon is shutting down");
  }

  const std::string client_id = "stream-" + std::to_string(++stream_seq_);
  auto managed = std::make_unique<ManagedLane>();
  managed->client_id = client_id;
  auto stream_source =
      std::make_unique<vinput::daemon::audio::FdStreamAudioSource>(owned_fd.Release());
  auto* stream_source_ptr = stream_source.get();
  managed->owned_capture = std::move(stream_source);
  managed->sink = std::make_unique<StreamLaneSink>(client_id, dbus_, options_.client_send_to);
  managed->lane = std::make_unique<SessionLane>(
      MakeLaneDependencies(managed->owned_capture.get(), managed->sink.get(),
                           /*ducker=*/nullptr, /*warn_on_clipped_head=*/false));

  auto* lane_ptr = managed->lane.get();
  stream_source_ptr->SetEofCallback(
      [lane_ptr] { lane_ptr->NotifySourceEof(lane_ptr->CurrentToken()); });

  const auto start_result =
      managed->lane->Start(ime, /*is_command=*/false, /*selected_text=*/"",
                           /*capture_device=*/"", /*default_scene_id=*/scene_id);
  if (!start_result.ok) {
    managed->lane->Shutdown();
    return start_result;
  }

  stream_lanes_.emplace(client_id, std::move(managed));
  return DbusService::MethodResult::Success(client_id);
}

DbusService::MethodResult LaneSupervisor::ReloadAsrBackend() {
  if (recognition_manager_ == nullptr) {
    return DbusService::MethodResult::Failure("ASR session manager is not initialized.");
  }
  const CoreConfig settings =
      options_.load_config ? options_.load_config() : LoadNormalizedConfig();
  std::string error;
  if (!recognition_manager_->SynchronizeBackend(settings, &error)) {
    std::string message = _("ASR backend reload failed. The previous backend may still be active.");
    if (!error.empty()) {
      message += "\n" + error;
    }
    VLOG_WARN("{}", message);
    return DbusService::MethodResult::Failure(message);
  }
  return DbusService::MethodResult::Success();
}

std::string LaneSupervisor::GetStatus() const {
  return primary_lane_->GetStatus();
}

vinput::dbus::AsrBackendState LaneSupervisor::GetAsrBackendState() const {
  if (recognition_manager_ == nullptr) {
    return {};
  }
  const auto snapshot = recognition_manager_->GetReloadSnapshot();
  return vinput::dbus::AsrBackendState{
      .target_provider_id = snapshot.target_provider_id,
      .target_model_id = snapshot.target_model_id,
      .effective_provider_id = snapshot.effective_provider_id,
      .effective_model_id = snapshot.effective_model_id,
      .last_error = snapshot.last_error,
      .reload_in_progress = snapshot.reload_in_progress,
      .has_effective_backend = snapshot.has_effective_backend,
      .remote_endpoints = {},
  };
}

void LaneSupervisor::EnqueueClientLine(std::string_view client_id, std::string_view line) {
  std::string err;
  const auto cmd = vinput::client::ParseClientCommand(line, &err);
  if (!cmd.has_value()) {
    VLOG_WARN("invalid client command: {}", err);
    return;
  }
  EnqueueClientCommand(client_id, *cmd);
}

void LaneSupervisor::EnqueueClientCommand(std::string_view client_id,
                                          const vinput::client::ClientCommand& command) {
  if (shutting_down_.load(std::memory_order_acquire)) {
    return;
  }
  {
    const std::scoped_lock lock(command_mutex_);
    command_queue_.push_back(
        QueuedClientCommand{.client_id = std::string(client_id), .command = command});
  }
  vinput::file::SignalEventFd(notify_fd_.Get());
}

int LaneSupervisor::GetNotifyFd() const noexcept {
  return notify_fd_.Get();
}

int LaneSupervisor::NextTimeoutMs() const {
  int timeout_ms = primary_lane_->NextTimeoutMs();
  for (const auto& [id, managed] : headless_lanes_) {
    (void)id;
    if (managed && managed->lane) {
      timeout_ms = MinPositiveTimeout(timeout_ms, managed->lane->NextTimeoutMs());
    }
  }
  for (const auto& [id, managed] : stream_lanes_) {
    (void)id;
    if (managed && managed->lane) {
      timeout_ms = MinPositiveTimeout(timeout_ms, managed->lane->NextTimeoutMs());
    }
  }
  return timeout_ms < 0 ? 1000 : timeout_ms;
}

void LaneSupervisor::DrainClientCommands() {
  std::deque<QueuedClientCommand> local;
  {
    const std::scoped_lock lock(command_mutex_);
    if (command_queue_.empty()) {
      return;
    }
    local.swap(command_queue_);
  }
  for (const auto& item : local) {
    DispatchClientCommand(item.client_id, item.command);
  }
}

void LaneSupervisor::DispatchCommandToLane(SessionLane& lane,
                                           const vinput::client::ClientCommand& command) {
  const auto result = std::visit(
      Overloaded{
          [&lane](const vinput::client::StartCommand& cmd) {
            const bool is_command = (cmd.mode == "command");
            return lane.Start(cmd.ime, is_command, cmd.selected_text, cmd.capture_device);
          },
          [&lane](const vinput::client::StopCommand& cmd) { return lane.Stop(cmd.ime, cmd.scene); },
          [&lane](const vinput::client::CancelCommand& cmd) {
            return lane.Cancel(cmd.ime, cmd.commit_raw);
          },
          [&lane](const vinput::client::InjectCommand& cmd) {
            return lane.InjectText(cmd.ime, cmd.stage, cmd.text, cmd.scene);
          },
      },
      command);
  if (!result.ok) {
    VLOG_WARN("client command rejected: {}", result.message);
  }
}

void LaneSupervisor::DispatchClientCommand(std::string_view client_id,
                                           const vinput::client::ClientCommand& command) {
  const bool is_ime = std::visit([](const auto& cmd) { return cmd.ime; }, command);
  if (is_ime || client_id.empty()) {
    DispatchCommandToLane(*primary_lane_, command);
    return;
  }

  SessionLane* lane = GetOrCreateHeadlessLane(client_id);
  if (lane != nullptr) {
    DispatchCommandToLane(*lane, command);
  }
}

SessionLane* LaneSupervisor::GetOrCreateHeadlessLane(std::string_view client_id) {
  if (shutting_down_.load(std::memory_order_acquire) || client_id.empty()) {
    return nullptr;
  }
  if (const auto it = headless_lanes_.find(client_id); it != headless_lanes_.end()) {
    return it->second->lane.get();
  }

  const std::string key(client_id);
  std::string cap_err;
  auto capture =
      options_.create_headless_capture ? options_.create_headless_capture(&cap_err) : nullptr;
  if (!capture) {
    VLOG_ERROR("failed to start audio capture for headless lane '{}': {}", key, cap_err);
    return nullptr;
  }

  auto managed = std::make_unique<ManagedLane>();
  managed->client_id = key;
  managed->owned_capture = std::move(capture);
  managed->sink = std::make_unique<HeadlessClientSink>(key, dbus_, options_.client_send_to);
  managed->lane = std::make_unique<SessionLane>(
      MakeLaneDependencies(managed->owned_capture.get(), managed->sink.get(),
                           /*ducker=*/nullptr, /*warn_on_clipped_head=*/true));
  SessionLane* lane_ptr = managed->lane.get();
  headless_lanes_.emplace(key, std::move(managed));
  return lane_ptr;
}

void LaneSupervisor::ReclaimIdleStreamLanes() {
  std::vector<std::string> to_reclaim;
  for (const auto& [id, managed] : stream_lanes_) {
    if (managed && managed->lane && managed->lane->IsIdle()) {
      to_reclaim.push_back(id);
    }
  }
  for (const auto& id : to_reclaim) {
    ReclaimLane(id);
  }
}

void LaneSupervisor::FlushDeferredActions() {
  vinput::file::DrainEventFd(notify_fd_.Get());
  DrainClientCommands();

  primary_lane_->ProcessPendingEvents();
  for (const auto& [id, managed] : headless_lanes_) {
    (void)id;
    if (managed && managed->lane) {
      managed->lane->ProcessPendingEvents();
    }
  }
  for (const auto& [id, managed] : stream_lanes_) {
    (void)id;
    if (managed && managed->lane) {
      managed->lane->ProcessPendingEvents();
    }
  }

  ReclaimIdleStreamLanes();
}

void LaneSupervisor::ReclaimLane(std::string_view client_id) {
  if (client_id.empty()) {
    return;
  }
  if (const auto it = headless_lanes_.find(client_id); it != headless_lanes_.end()) {
    if (it->second && it->second->lane) {
      it->second->lane->Shutdown();
    }
    headless_lanes_.erase(it);
  }
  if (const auto it = stream_lanes_.find(client_id); it != stream_lanes_.end()) {
    if (it->second && it->second->lane) {
      it->second->lane->Shutdown();
    }
    stream_lanes_.erase(it);
  }
}

void LaneSupervisor::Shutdown() {
  shutting_down_.store(true, std::memory_order_release);
  {
    const std::scoped_lock lock(command_mutex_);
    command_queue_.clear();
  }
  for (auto& [id, managed] : stream_lanes_) {
    (void)id;
    if (managed && managed->lane) {
      managed->lane->Shutdown();
    }
  }
  stream_lanes_.clear();

  for (auto& [id, managed] : headless_lanes_) {
    (void)id;
    if (managed && managed->lane) {
      managed->lane->Shutdown();
    }
  }
  headless_lanes_.clear();

  if (primary_lane_) {
    primary_lane_->Shutdown();
  }
}

std::size_t LaneSupervisor::HeadlessLaneCount() const noexcept {
  return headless_lanes_.size();
}

std::size_t LaneSupervisor::StreamLaneCount() const noexcept {
  return stream_lanes_.size();
}

std::string LaneSupervisor::GetHeadlessLaneStatus(std::string_view client_id) const {
  if (const auto it = headless_lanes_.find(client_id);
      it != headless_lanes_.end() && it->second && it->second->lane) {
    return it->second->lane->GetStatus();
  }
  return vinput::dbus::kStatusIdle;
}

} // namespace vinput::daemon::runtime
