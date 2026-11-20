#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "common/client/client_command.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/runtime/dbus_service.h"
#include "daemon/runtime/lane_event_sink.h"
#include "daemon/runtime/recognition_pipeline.h"
#include "daemon/runtime/session_lane.h"

namespace vinput::daemon::runtime {

struct LaneSupervisorOptions {
  using HeadlessCaptureFactory =
      std::function<std::unique_ptr<vinput::daemon::audio::AudioInputSource>(std::string* error)>;
  using ClientBroadcastFn = std::function<void(std::string_view line)>;
  using ClientSendToFn = std::function<bool(std::string_view client_id, std::string_view line)>;

  SessionLaneDependencies::ConfigLoader load_config;
  SessionLaneDependencies::AsrSessionFactory create_asr_session;
  SessionLaneDependencies::PostprocessRunner run_postprocess;
  HeadlessCaptureFactory create_headless_capture;
  ClientBroadcastFn client_broadcast;
  ClientSendToFn client_send_to;
  std::shared_ptr<vinput::daemon::audio::VolumeControlAdapter> volume_adapter;
  std::chrono::milliseconds first_buffer_ready_timeout{1500};
};

// Owns and coordinates the primary IME SessionLane, per-client headless SessionLanes,
// and TranscribeStream SessionLanes on the daemon main event loop.
// ClientManager pump threads enqueue commands via EnqueueClientLine() and wake notify_fd_;
// all lane creation, command dispatch, event draining, and reclamation run on the main thread.
class LaneSupervisor {
public:
  LaneSupervisor(vinput::daemon::audio::AudioInputSource* primary_capture, DbusService* dbus,
                 vinput::daemon::asr::RecognitionSessionManager* recognition_manager,
                 RecognitionPipeline* pipeline, LaneSupervisorOptions options = {});
  ~LaneSupervisor();

  LaneSupervisor(const LaneSupervisor&) = delete;
  LaneSupervisor& operator=(const LaneSupervisor&) = delete;
  LaneSupervisor(LaneSupervisor&&) = delete;
  LaneSupervisor& operator=(LaneSupervisor&&) = delete;

  // Primary IME D-Bus control methods (main thread).
  DbusService::MethodResult StartRecording();
  DbusService::MethodResult StartCommandRecording(const std::string& selected_text);
  DbusService::MethodResult StopRecording(const std::string& scene_id);
  DbusService::MethodResult CancelOperation(bool commit_raw_text);
  DbusService::MethodResult TranscribeStream(int fd, const std::string& scene_id, bool ime);
  DbusService::MethodResult ReloadAsrBackend();
  [[nodiscard]] std::string GetStatus() const;
  [[nodiscard]] vinput::dbus::AsrBackendState GetAsrBackendState() const;

  // Thread-safe entry point for ClientManager pump threads.
  void EnqueueClientLine(std::string_view client_id, std::string_view line);
  void EnqueueClientCommand(std::string_view client_id,
                            const vinput::client::ClientCommand& command);

  // Main event-loop integration.
  [[nodiscard]] int GetNotifyFd() const noexcept;
  [[nodiscard]] int NextTimeoutMs() const;
  void FlushDeferredActions();
  void ReclaimLane(std::string_view client_id);
  void Shutdown();

  // Inspection helpers for tests.
  [[nodiscard]] std::size_t HeadlessLaneCount() const noexcept;
  [[nodiscard]] std::size_t StreamLaneCount() const noexcept;
  [[nodiscard]] std::string GetHeadlessLaneStatus(std::string_view client_id) const;

private:
  struct QueuedClientCommand {
    std::string client_id;
    vinput::client::ClientCommand command;
  };

  struct ManagedLane {
    std::string client_id;
    std::unique_ptr<vinput::daemon::audio::AudioInputSource> owned_capture;
    std::unique_ptr<LaneEventSink> sink;
    std::unique_ptr<SessionLane> lane;
  };

  SessionLaneDependencies MakeLaneDependencies(vinput::daemon::audio::AudioInputSource* source,
                                               LaneEventSink* sink,
                                               vinput::daemon::audio::OutputDucker* ducker,
                                               bool warn_on_clipped_head) const;
  void WakeMainThread() const noexcept;
  void DrainNotifyFd() const noexcept;
  void DrainClientCommands();
  void DispatchClientCommand(std::string_view client_id,
                             const vinput::client::ClientCommand& command);
  void DispatchCommandToLane(SessionLane& lane, const vinput::client::ClientCommand& command);
  SessionLane* GetOrCreateHeadlessLane(std::string_view client_id);
  void ReclaimIdleStreamLanes();

  DbusService* dbus_ = nullptr;
  vinput::daemon::asr::RecognitionSessionManager* recognition_manager_ = nullptr;
  RecognitionPipeline* pipeline_ = nullptr;
  LaneSupervisorOptions options_;

  int notify_fd_ = -1;
  std::atomic<bool> shutting_down_{false};
  std::atomic<std::uint64_t> stream_seq_{0};

  vinput::daemon::audio::OutputDucker primary_ducker_;
  std::unique_ptr<LaneEventSink> primary_sink_;
  std::unique_ptr<SessionLane> primary_lane_;

  std::map<std::string, std::unique_ptr<ManagedLane>, std::less<>> headless_lanes_;
  std::map<std::string, std::unique_ptr<ManagedLane>, std::less<>> stream_lanes_;

  std::mutex command_mutex_;
  std::deque<QueuedClientCommand> command_queue_;
};

} // namespace vinput::daemon::runtime
