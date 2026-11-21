#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/asr/recognition_result.h"
#include "common/client/client_command.h"
#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"
#include "common/notify/notification_bridge.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/audio/audio_input_source.h"
#include "daemon/audio/mock_audio_input_source.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/runtime/lane_event_sink.h"
#include "daemon/runtime/recognition_pipeline.h"
#include "daemon/runtime/session_lane.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

using vinput::daemon::asr::AudioDeliveryMode;
using vinput::daemon::asr::BackendDescriptor;
using vinput::daemon::asr::RecognitionEvent;
using vinput::daemon::asr::RecognitionEventKind;
using vinput::daemon::asr::RecognitionSession;
using vinput::daemon::audio::FakeVolumeAdapter;
using vinput::daemon::audio::MockAudioInputSource;
using vinput::daemon::audio::OutputDucker;
using vinput::daemon::runtime::LaneEventSink;
using vinput::daemon::runtime::LanePhase;
using vinput::daemon::runtime::PostprocessRequest;
using vinput::daemon::runtime::RecognitionPipelineResult;
using vinput::daemon::runtime::SessionLane;
using vinput::daemon::runtime::SessionLaneDependencies;

class RecordingSink final : public LaneEventSink {
public:
  void OnStatusChanged(vinput::dbus::Status status, bool ime) override {
    const std::scoped_lock lock(mutex_);
    statuses_.emplace_back(status, ime);
  }

  void OnPartialText(std::string_view text, bool ime) override {
    const std::scoped_lock lock(mutex_);
    partials_.emplace_back(std::string(text), ime);
  }

  void OnRecognitionResult(const vinput::result::Payload& payload, bool ime) override {
    const std::scoped_lock lock(mutex_);
    results_.emplace_back(payload, ime);
  }

  void OnNotification(const vinput::notify::Notification& notification) override {
    const std::scoped_lock lock(mutex_);
    notifications_.push_back(notification);
  }

  [[nodiscard]] std::vector<std::pair<vinput::dbus::Status, bool>> Statuses() const {
    const std::scoped_lock lock(mutex_);
    return statuses_;
  }

  [[nodiscard]] std::vector<std::pair<vinput::result::Payload, bool>> Results() const {
    const std::scoped_lock lock(mutex_);
    return results_;
  }

  [[nodiscard]] std::vector<vinput::notify::Notification> Notifications() const {
    const std::scoped_lock lock(mutex_);
    return notifications_;
  }

  [[nodiscard]] bool HasStatus(vinput::dbus::Status target) const {
    const std::scoped_lock lock(mutex_);
    return std::ranges::any_of(statuses_,
                               [target](const auto& entry) { return entry.first == target; });
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::pair<vinput::dbus::Status, bool>> statuses_;
  std::vector<std::pair<std::string, bool>> partials_;
  std::vector<std::pair<vinput::result::Payload, bool>> results_;
  std::vector<vinput::notify::Notification> notifications_;
};

class ScriptedSession final : public RecognitionSession {
public:
  struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int16_t> received_pcm;
    bool cancelled = false;
    bool interrupted = false;
    bool finished = false;
    bool block_finish_until_interrupt = false;
    bool finish_entered = false;
    std::string finish_text = "ok";
  };

  explicit ScriptedSession(std::shared_ptr<SharedState> state) : state_(std::move(state)) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    const std::scoped_lock lock(state_->mutex);
    if (state_->cancelled) {
      if (error != nullptr) {
        *error = "cancelled";
      }
      return false;
    }
    state_->received_pcm.insert(state_->received_pcm.end(), pcm.begin(), pcm.end());
    return true;
  }

  bool Finish(std::string* error) override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->block_finish_until_interrupt) {
      state_->finish_entered = true;
      state_->cv.notify_all();
      state_->cv.wait(lock, [this] { return state_->interrupted || state_->cancelled; });
      if (error != nullptr) {
        *error = "interrupted";
      }
      return false;
    }
    if (state_->cancelled) {
      if (error != nullptr) {
        *error = "cancelled";
      }
      return false;
    }
    state_->finished = true;
    events_.push_back({RecognitionEventKind::FinalText, state_->finish_text, {}});
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    return true;
  }

  void Cancel() override {
    {
      const std::scoped_lock lock(state_->mutex);
      state_->cancelled = true;
    }
    state_->cv.notify_all();
  }

  void Interrupt() noexcept override {
    {
      const std::scoped_lock lock(state_->mutex);
      state_->interrupted = true;
    }
    state_->cv.notify_all();
  }

  std::vector<RecognitionEvent> PollEvents() override {
    auto out = std::move(events_);
    events_.clear();
    return out;
  }

private:
  std::shared_ptr<SharedState> state_;
  std::vector<RecognitionEvent> events_;
};

CoreConfig MakeTestConfig(bool duck = false) {
  CoreConfig cfg;
  cfg.asr.inputGain = 1.0;
  cfg.asr.normalizeAudio = false;
  cfg.global.duckOutputWhileRecording = duck;
  cfg.global.duckOutputVolume = 0.2;
  return cfg;
}

void PumpUntilIdle(SessionLane& lane, int max_iters = 100) {
  for (int i = 0; i < max_iters && !lane.IsIdle(); ++i) {
    lane.ProcessPendingEvents();
    if (!lane.IsIdle()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
}

void TestReadinessGatingAndSampleOrdering() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto session_state = std::make_shared<ScriptedSession::SharedState>();

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool allow_asr_ready = false;

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .output_ducker = nullptr,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&] { return allow_asr_ready; });
            desc->provider_id = "mock";
            desc->backend_id = "mock-chunked";
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Chunked;
            return std::make_unique<ScriptedSession>(session_state);
          },
      .run_postprocess =
          [](const PostprocessRequest& req, const CoreConfig&, const std::function<void()>&,
             const std::atomic<bool>*) {
            RecognitionPipelineResult res;
            res.payload.commitText = req.text;
            return res;
          },
      .warn_on_clipped_head = false,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Starting);
  TEST_ASSERT(lane.GetStatus() == vinput::dbus::kStatusStarting);

  // Push 801 tagged samples while ASR session creation is still blocked.
  std::vector<int16_t> startup(801);
  for (std::size_t i = 0; i < startup.size(); ++i) {
    startup[i] = static_cast<int16_t>(i + 1);
  }
  source.PushSamples(startup);
  lane.ProcessPendingEvents();

  // Must remain Starting (never Recording) until ASR is also ready.
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Starting);
  TEST_ASSERT(!sink.HasStatus(vinput::dbus::Status::Recording));

  // Unblock ASR session creation.
  {
    const std::scoped_lock lock(gate_mutex);
    allow_asr_ready = true;
  }
  gate_cv.notify_all();

  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);
  TEST_ASSERT(sink.HasStatus(vinput::dbus::Status::Recording));

  // Push 799 + 800 + 6000 = 7599 more samples (total 8400 >= 8000), crossing 799/800/801
  // boundaries.
  std::vector<int16_t> live1(799);
  for (std::size_t i = 0; i < live1.size(); ++i) {
    live1[i] = static_cast<int16_t>(802 + i);
  }
  source.PushSamples(live1);
  lane.ProcessPendingEvents();

  std::vector<int16_t> live2(6800);
  for (std::size_t i = 0; i < live2.size(); ++i) {
    live2[i] = static_cast<int16_t>(1601 + i);
  }
  source.PushSamples(live2);
  lane.ProcessPendingEvents();

  // Push a tail chunk without draining first so DrainTailOnStop() delivers it at Stop().
  std::vector<int16_t> tail(250);
  for (std::size_t i = 0; i < tail.size(); ++i) {
    tail[i] = static_cast<int16_t>(8401 + i);
  }
  source.PushSamples(tail);

  TEST_ASSERT(lane.Stop(true, "").ok);
  TEST_ASSERT(!source.IsRecording());
  PumpUntilIdle(lane);
  TEST_ASSERT(lane.IsIdle());

  const std::scoped_lock lock(session_state->mutex);
  TEST_ASSERT(session_state->finished);
  TEST_ASSERT(session_state->received_pcm.size() == 8650);
  for (std::size_t i = 0; i < 8650; ++i) {
    TEST_ASSERT(session_state->received_pcm[i] == static_cast<int16_t>(i + 1));
  }
  TEST_ASSERT(sink.Results().size() == 1);
  TEST_ASSERT(sink.Results()[0].first.commitText == "ok");
}

void TestFastReleaseBeforeFirstBuffer() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto session_state = std::make_shared<ScriptedSession::SharedState>();

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            return std::make_unique<ScriptedSession>(session_state);
          },
      .run_postprocess =
          [](const PostprocessRequest& req, const CoreConfig&, const std::function<void()>&,
             const std::atomic<bool>*) {
            RecognitionPipelineResult res;
            res.payload.commitText = req.text;
            return res;
          },
      .warn_on_clipped_head = false,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);
  TEST_ASSERT(lane.Stop(true, "").ok);
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Stopping);

  // Deliver 8500 silent samples after Stop was already requested.
  const std::vector<int16_t> pcm(8500, 0);
  source.PushSamples(pcm);
  PumpUntilIdle(lane);

  TEST_ASSERT(lane.IsIdle());
  TEST_ASSERT(!sink.HasStatus(vinput::dbus::Status::Recording));
  TEST_ASSERT(sink.HasStatus(vinput::dbus::Status::Stopping));
  TEST_ASSERT(sink.HasStatus(vinput::dbus::Status::Inferring));
  TEST_ASSERT(sink.Results().size() == 1);
}

void TestReadinessTimeoutFallback() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto session_state = std::make_shared<ScriptedSession::SharedState>();

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            return std::make_unique<ScriptedSession>(session_state);
          },
      .run_postprocess = nullptr,
      .warn_on_clipped_head = true,
      .first_buffer_ready_timeout = std::chrono::milliseconds(25),
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);
  // Boundedly wait for both the readiness timeout and the ASR-ready event instead of
  // assuming worker scheduling completes within a fixed sleep.
  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);
  TEST_ASSERT(sink.HasStatus(vinput::dbus::Status::Recording));

  // Even when the first chunk arrives after timeout transitioned to Recording,
  // speech-level energy on that first chunk must still trigger the clipped-head warning.
  const std::vector<int16_t> first_speech(160, 2000);
  source.PushSamples(first_speech);
  lane.ProcessPendingEvents();
  bool saw_warning = false;
  for (const auto& n : sink.Notifications()) {
    if (n.level == vinput::notify::NotificationLevel::Warning) {
      saw_warning = true;
    }
  }
  TEST_ASSERT(saw_warning);

  TEST_ASSERT(lane.Cancel(true, false).ok);
  TEST_ASSERT(!source.IsRecording());
  TEST_ASSERT(lane.IsIdle());
}

void TestClippedHeadWarningAndShortDiscardFeedback() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto session_state = std::make_shared<ScriptedSession::SharedState>();

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            return std::make_unique<ScriptedSession>(session_state);
          },
      .run_postprocess = nullptr,
      .warn_on_clipped_head = true,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);

  // First buffer already contains speech-level energy (2000) and is < 8000 samples.
  const std::vector<int16_t> short_speech(799, 2000);
  source.PushSamples(short_speech);
  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);
  TEST_ASSERT(lane.Stop(true, "").ok);
  PumpUntilIdle(lane);

  const auto notes = sink.Notifications();
  bool saw_warning = false;
  bool saw_short_info = false;
  for (const auto& n : notes) {
    if (n.level == vinput::notify::NotificationLevel::Warning) {
      saw_warning = true;
    }
    if (n.level == vinput::notify::NotificationLevel::Information) {
      saw_short_info = true;
    }
  }
  TEST_ASSERT(saw_warning);
  TEST_ASSERT(saw_short_info);
  TEST_ASSERT(session_state->cancelled);
}

void TestCancelImmediateStartAndDuckerIsolation() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto volume_adapter = std::make_shared<FakeVolumeAdapter>(1.0);
  OutputDucker ducker(volume_adapter);

  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool release_gen1 = false;
  bool gen1_entered = false;
  std::atomic<int> create_calls{0};
  auto gen1_state = std::make_shared<ScriptedSession::SharedState>();
  auto gen2_state = std::make_shared<ScriptedSession::SharedState>();

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .output_ducker = &ducker,
      .load_config = [] { return MakeTestConfig(/*duck=*/true); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            const int call = ++create_calls;
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            if (call == 1) {
              {
                const std::scoped_lock lock(gate_mutex);
                gen1_entered = true;
              }
              gate_cv.notify_all();
              std::unique_lock<std::mutex> lock(gate_mutex);
              gate_cv.wait(lock, [&] { return release_gen1; });
              return std::make_unique<ScriptedSession>(gen1_state);
            }
            return std::make_unique<ScriptedSession>(gen2_state);
          },
      .run_postprocess = nullptr,
      .warn_on_clipped_head = false,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);

  // Wait deterministically until the worker has claimed generation 1's CreateSession and
  // is blocked inside the factory; otherwise generation 2's task could replace generation
  // 1's unclaimed task and generation 1 would never be created (SubmitWorkerTask overwrites
  // pending_task_).
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return gen1_entered; });
  }
  TEST_ASSERT(gen1_entered);

  TEST_ASSERT(lane.Cancel(true, false).ok);
  TEST_ASSERT(lane.IsIdle());

  // Immediately start generation 2 while generation 1's CreateSession is still blocked.
  TEST_ASSERT(lane.Start(true, false).ok);
  const std::vector<int16_t> silence(160, 0);
  source.PushSamples(silence);

  // Release the generation-1 factory first: the single worker must finish the blocked
  // call before it can process generation 2's task and let it reach Recording.
  {
    const std::scoped_lock lock(gate_mutex);
    release_gen1 = true;
  }
  gate_cv.notify_all();
  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);

  // Generation 1's late session is retired without restoring generation 2's ducked volume.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  lane.ProcessPendingEvents();

  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);
  TEST_ASSERT(gen1_state->cancelled);
  TEST_ASSERT(!gen2_state->cancelled);
  TEST_ASSERT(volume_adapter->CurrentVolume() < 0.5);

  TEST_ASSERT(lane.Cancel(true, false).ok);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  TEST_ASSERT(volume_adapter->CurrentVolume() > 0.9);
}

void TestInjectTextConcurrencyAndPostprocessCancel() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session = nullptr,
      .run_postprocess =
          [](const PostprocessRequest& req, const CoreConfig&,
             const std::function<void()>& on_enter, const std::atomic<bool>* cancel_flag) {
            if (on_enter) {
              on_enter();
            }
            while (cancel_flag != nullptr && !cancel_flag->load(std::memory_order_acquire)) {
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            RecognitionPipelineResult res;
            res.payload.commitText = req.text + " (llm)";
            return res;
          },
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.InjectText(true, vinput::client::InjectStage::Final, "raw text", "polish").ok);
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Postprocessing);

  // Concurrent Start must be rejected while InjectText postprocessing is active.
  TEST_ASSERT(!lane.Start(true, false).ok);

  // Cancel with commit_raw_text=true must commit raw text and return to Idle.
  TEST_ASSERT(lane.Cancel(true, true).ok);
  PumpUntilIdle(lane);
  TEST_ASSERT(lane.IsIdle());
  TEST_ASSERT(sink.Results().size() == 1);
  TEST_ASSERT(sink.Results()[0].first.commitText == "raw text");
}

void TestStreamErrorAndShutdownInterrupt() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto gen1_state = std::make_shared<ScriptedSession::SharedState>();
  auto gen2_state = std::make_shared<ScriptedSession::SharedState>();
  gen2_state->block_finish_until_interrupt = true;

  std::atomic<int> create_calls{0};
  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            const int call = create_calls.fetch_add(1);
            return std::make_unique<ScriptedSession>(call == 0 ? gen1_state : gen2_state);
          },
      .run_postprocess = nullptr,
      .warn_on_clipped_head = false,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);
  source.SimulateError("pipewire node vanished");
  PumpUntilIdle(lane);
  TEST_ASSERT(!source.IsRecording());
  TEST_ASSERT(lane.IsIdle());
  TEST_ASSERT(sink.HasStatus(vinput::dbus::Status::Error));

  // Now start a recording that enters Inferring and blocks in Finish(), then call Shutdown().
  TEST_ASSERT(lane.Start(true, false).ok);
  const std::vector<int16_t> pcm(8500, 0);
  source.PushSamples(pcm);
  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(lane.Stop(true, "").ok);
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Inferring);

  // Wait until the worker is actually parked inside Finish() before interrupting it.
  {
    std::unique_lock<std::mutex> lock(gen2_state->mutex);
    gen2_state->cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return gen2_state->finish_entered; });
  }
  TEST_ASSERT(gen2_state->finish_entered);
  lane.Shutdown();
  TEST_ASSERT(gen2_state->interrupted);
  TEST_ASSERT(lane.IsIdle());
}

void TestInjectTextDuringRecording() {
  MockAudioInputSource source(15, 16000);
  RecordingSink sink;
  auto session_state = std::make_shared<ScriptedSession::SharedState>();

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            return std::make_unique<ScriptedSession>(session_state);
          },
      .run_postprocess = nullptr,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);
  const std::vector<int16_t> silence(160, 0);
  source.PushSamples(silence);
  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);

  // Phone-style injection without a scene is synchronous and must not disturb the
  // active recording session.
  TEST_ASSERT(
      lane.InjectText(true, vinput::client::InjectStage::Final, "phone text", /*scene_id=*/"").ok);
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);
  {
    const auto results = sink.Results();
    TEST_ASSERT(results.size() == 1);
    TEST_ASSERT(results[0].first.commitText == "phone text");
  }

  // The recording still finishes normally and delivers its own result afterwards.
  const std::vector<int16_t> pcm(8500, 0);
  source.PushSamples(pcm);
  TEST_ASSERT(lane.Stop(true, "").ok);
  PumpUntilIdle(lane);
  TEST_ASSERT(lane.IsIdle());
  TEST_ASSERT(sink.Results().size() == 2);
}

void TestStreamingPushFailureDoesNotDeadlockConsumerLock() {
  class LockedDrainSource final : public vinput::daemon::audio::AudioInputSource {
  public:
    bool Begin(std::string_view, FirstBufferCallback on_first, ErrorCallback,
               std::string*) override {
      recording_ = true;
      on_first_ = std::move(on_first);
      return true;
    }
    std::vector<int16_t> StopAndDrain() override {
      const std::scoped_lock lock(consumer_mutex_);
      recording_ = false;
      return {};
    }
    void Abort() override {
      const std::scoped_lock lock(consumer_mutex_);
      recording_ = false;
    }
    std::size_t DrainAvailable(SampleConsumer consumer) override {
      const std::scoped_lock lock(consumer_mutex_);
      if (!pending_.empty()) {
        if (on_first_) {
          auto cb = std::move(on_first_);
          cb(1);
        }
        consumer(pending_);
        const std::size_t n = pending_.size();
        pending_.clear();
        return n;
      }
      return 0;
    }
    [[nodiscard]] bool IsRecording() const override { return recording_; }
    [[nodiscard]] std::size_t DroppedSamples() const noexcept override { return 0; }
    void ResetDropCounters() noexcept override {}
    [[nodiscard]] std::optional<long> FirstBufferLatencyMs() const override { return 1; }

    void QueueSamples(std::vector<int16_t> pcm) { pending_ = std::move(pcm); }

  private:
    std::mutex consumer_mutex_;
    bool recording_ = false;
    FirstBufferCallback on_first_;
    std::vector<int16_t> pending_;
  };

  LockedDrainSource source;
  RecordingSink sink;
  auto session_state = std::make_shared<ScriptedSession::SharedState>();

  SessionLaneDependencies deps{
      .audio_source = &source,
      .event_sink = &sink,
      .load_config = [] { return MakeTestConfig(); },
      .create_asr_session =
          [&](const CoreConfig&, BackendDescriptor* desc, std::string*,
              const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Chunked;
            return std::make_unique<ScriptedSession>(session_state);
          },
      .run_postprocess = nullptr,
      .warn_on_clipped_head = false,
  };

  SessionLane lane(std::move(deps));
  TEST_ASSERT(lane.Start(true, false).ok);
  source.QueueSamples(std::vector<int16_t>(160, 0));
  for (int i = 0; i < 50 && lane.CurrentPhase() != LanePhase::Recording; ++i) {
    lane.ProcessPendingEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(lane.CurrentPhase() == LanePhase::Recording);

  // Force PushAudio failure on the next chunk while DrainAvailable holds consumer_mutex_.
  {
    const std::scoped_lock lock(session_state->mutex);
    session_state->cancelled = true;
  }
  source.QueueSamples(std::vector<int16_t>(800, 0));
  lane.ProcessPendingEvents();

  TEST_ASSERT(!source.IsRecording());
  TEST_ASSERT(lane.IsIdle());
  TEST_ASSERT(sink.HasStatus(vinput::dbus::Status::Error));
}

} // namespace

int main() {
  std::printf("Running test_session_lane...\n");
  TestReadinessGatingAndSampleOrdering();
  TestFastReleaseBeforeFirstBuffer();
  TestReadinessTimeoutFallback();
  TestClippedHeadWarningAndShortDiscardFeedback();
  TestCancelImmediateStartAndDuckerIsolation();
  TestInjectTextConcurrencyAndPostprocessCancel();
  TestStreamErrorAndShutdownInterrupt();
  TestInjectTextDuringRecording();
  TestStreamingPushFailureDoesNotDeadlockConsumerLock();
  std::printf("All test_session_lane passed!\n");
  return 0;
}
