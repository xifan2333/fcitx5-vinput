#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/dbus/dbus_interface.h"

#include "daemon/asr/runtime/recognition_contract.h"
#include "daemon/asr/runtime/recognition_session_manager.h"
#include "daemon/audio/mock_audio_input_source.h"
#include "daemon/audio/output_ducker.h"
#include "daemon/runtime/lane_supervisor.h"
#include "daemon/runtime/recognition_pipeline.h"

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
using vinput::daemon::asr::RecognitionSessionManager;
using vinput::daemon::audio::FakeVolumeAdapter;
using vinput::daemon::audio::MockAudioInputSource;
using vinput::daemon::runtime::LaneSupervisor;
using vinput::daemon::runtime::LaneSupervisorOptions;
using vinput::daemon::runtime::PostprocessRequest;
using vinput::daemon::runtime::RecognitionPipelineResult;

class FakeSession final : public RecognitionSession {
public:
  explicit FakeSession(std::string text) : text_(std::move(text)) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string*) override {
    sample_count_ += pcm.size();
    return true;
  }

  bool Finish(std::string*) override {
    events_.push_back({RecognitionEventKind::FinalText, text_, {}});
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    return true;
  }

  void Cancel() override { cancelled_ = true; }

  std::vector<RecognitionEvent> PollEvents() override {
    auto out = std::move(events_);
    events_.clear();
    return out;
  }

private:
  std::string text_;
  std::size_t sample_count_ = 0;
  bool cancelled_ = false;
  std::vector<RecognitionEvent> events_;
};

CoreConfig MakeConfig(bool duck) {
  CoreConfig cfg;
  cfg.asr.inputGain = 1.0;
  cfg.asr.normalizeAudio = false;
  cfg.global.duckOutputWhileRecording = duck;
  cfg.global.duckOutputVolume = 0.25;
  return cfg;
}

void TestMultiLaneIsolationAndConcurrentEnqueue() {
  MockAudioInputSource primary_source(15, 16000);
  auto volume_adapter = std::make_shared<FakeVolumeAdapter>(1.0);

  std::mutex sources_mutex;
  std::vector<MockAudioInputSource*> headless_sources;

  std::mutex events_mutex;
  std::vector<std::string> broadcasts;
  std::map<std::string, std::vector<std::string>, std::less<>> unicasts;

  LaneSupervisorOptions opts{
      .load_config = [] { return MakeConfig(/*duck=*/true); },
      .create_asr_session =
          [](const CoreConfig&, BackendDescriptor* desc, std::string*,
             const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            return std::make_unique<FakeSession>("hello");
          },
      .run_postprocess =
          [](const PostprocessRequest& req, const CoreConfig&, const std::function<void()>&,
             const std::atomic<bool>*) {
            RecognitionPipelineResult res;
            res.payload.commitText = req.text;
            return res;
          },
      .create_headless_capture =
          [&](std::string*) {
            auto src = std::make_unique<MockAudioInputSource>(15, 16000);
            const std::scoped_lock lock(sources_mutex);
            headless_sources.push_back(src.get());
            return src;
          },
      .client_broadcast =
          [&](std::string_view line) {
            const std::scoped_lock lock(events_mutex);
            broadcasts.emplace_back(line);
          },
      .client_send_to =
          [&](std::string_view cid, std::string_view line) {
            const std::scoped_lock lock(events_mutex);
            unicasts[std::string(cid)].emplace_back(line);
            return true;
          },
      .volume_adapter = volume_adapter,
  };

  LaneSupervisor supervisor(&primary_source, nullptr, nullptr, nullptr, std::move(opts));

  // Enqueue commands concurrently from 4 threads (two headless lanes + one IME inject + invalid).
  std::thread t1(
      [&] { supervisor.EnqueueClientLine("client-a", R"({"cmd":"start","ime":false})"); });
  std::thread t2(
      [&] { supervisor.EnqueueClientLine("client-b", R"({"cmd":"start","ime":false})"); });
  std::thread t3([&] {
    supervisor.EnqueueClientLine("client-c",
                                 R"({"cmd":"inject","ime":true,"stage":"partial","text":"live"})");
  });
  std::thread t4([&] { supervisor.EnqueueClientLine("client-d", "not-json"); });
  t1.join();
  t2.join();
  t3.join();
  t4.join();

  supervisor.FlushDeferredActions();
  TEST_ASSERT(supervisor.HeadlessLaneCount() == 2);
  TEST_ASSERT(supervisor.GetStatus() == vinput::dbus::kStatusIdle);
  TEST_ASSERT(supervisor.GetHeadlessLaneStatus("client-a") == vinput::dbus::kStatusStarting);
  TEST_ASSERT(supervisor.GetHeadlessLaneStatus("client-b") == vinput::dbus::kStatusStarting);

  // Headless lanes must NOT duck output volume.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  TEST_ASSERT(volume_adapter->WriteCount() == 0);

  // Push audio to both headless sources and stop them concurrently.
  {
    const std::scoped_lock lock(sources_mutex);
    TEST_ASSERT(headless_sources.size() == 2);
    const std::vector<int16_t> pcm(8500, 0);
    headless_sources[0]->PushSamples(pcm);
    headless_sources[1]->PushSamples(pcm);
  }
  for (int i = 0;
       i < 50 && (supervisor.GetHeadlessLaneStatus("client-a") != vinput::dbus::kStatusRecording ||
                  supervisor.GetHeadlessLaneStatus("client-b") != vinput::dbus::kStatusRecording);
       ++i) {
    supervisor.FlushDeferredActions();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(supervisor.GetHeadlessLaneStatus("client-a") == vinput::dbus::kStatusRecording);
  TEST_ASSERT(supervisor.GetHeadlessLaneStatus("client-b") == vinput::dbus::kStatusRecording);

  supervisor.EnqueueClientLine("client-a", R"({"cmd":"stop","ime":false})");
  supervisor.EnqueueClientLine("client-b", R"({"cmd":"stop","ime":false})");
  for (int i = 0;
       i < 100 && (supervisor.GetHeadlessLaneStatus("client-a") != vinput::dbus::kStatusIdle ||
                   supervisor.GetHeadlessLaneStatus("client-b") != vinput::dbus::kStatusIdle);
       ++i) {
    supervisor.FlushDeferredActions();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(supervisor.GetHeadlessLaneStatus("client-a") == vinput::dbus::kStatusIdle);
  TEST_ASSERT(supervisor.GetHeadlessLaneStatus("client-b") == vinput::dbus::kStatusIdle);

  {
    const std::scoped_lock lock(events_mutex);
    TEST_ASSERT(!broadcasts.empty());
    TEST_ASSERT(!unicasts["client-a"].empty());
    TEST_ASSERT(!unicasts["client-b"].empty());
  }

  supervisor.ReclaimLane("client-a");
  TEST_ASSERT(supervisor.HeadlessLaneCount() == 1);
  supervisor.Shutdown();
  TEST_ASSERT(supervisor.HeadlessLaneCount() == 0);
}

void TestTranscribeStreamAndAutoReclaim() {
  MockAudioInputSource primary_source(15, 16000);
  auto volume_adapter = std::make_shared<FakeVolumeAdapter>(1.0);

  std::mutex events_mutex;
  std::vector<std::string> stream_events;

  LaneSupervisorOptions opts{
      .load_config = [] { return MakeConfig(/*duck=*/true); },
      .create_asr_session =
          [](const CoreConfig&, BackendDescriptor* desc, std::string*,
             const std::function<void()>&) {
            desc->capabilities.audio_delivery_mode = AudioDeliveryMode::Buffered;
            return std::make_unique<FakeSession>("streamed");
          },
      .run_postprocess =
          [](const PostprocessRequest& req, const CoreConfig&, const std::function<void()>&,
             const std::atomic<bool>*) {
            RecognitionPipelineResult res;
            res.payload.commitText = req.text;
            return res;
          },
      .client_send_to =
          [&](std::string_view, std::string_view line) {
            const std::scoped_lock lock(events_mutex);
            stream_events.emplace_back(line);
            return true;
          },
      .volume_adapter = volume_adapter,
  };

  LaneSupervisor supervisor(&primary_source, nullptr, nullptr, nullptr, std::move(opts));

  std::array<int, 2> pipe_fds{-1, -1};
  TEST_ASSERT(pipe(pipe_fds.data()) == 0);
  const std::vector<int16_t> pcm(8500, 100);
  const auto* raw = reinterpret_cast<const uint8_t*>(pcm.data());
  const std::size_t total_bytes = pcm.size() * sizeof(int16_t);
  std::size_t written = 0;
  while (written < total_bytes) {
    const ssize_t n = write(pipe_fds[1], raw + written, total_bytes - written);
    TEST_ASSERT(n > 0);
    written += static_cast<std::size_t>(n);
  }
  close(pipe_fds[1]);

  const auto res = supervisor.TranscribeStream(pipe_fds[0], "", /*ime=*/false);
  TEST_ASSERT(res.ok);
  TEST_ASSERT(res.payload == "stream-1");
  TEST_ASSERT(supervisor.StreamLaneCount() == 1);

  for (int i = 0; i < 100 && supervisor.StreamLaneCount() > 0; ++i) {
    supervisor.FlushDeferredActions();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT(supervisor.StreamLaneCount() == 0);
  TEST_ASSERT(volume_adapter->WriteCount() == 0);

  bool saw_committed = false;
  {
    const std::scoped_lock lock(events_mutex);
    for (const auto& line : stream_events) {
      if (line.find(R"("type":"committed")") != std::string::npos &&
          line.find("streamed") != std::string::npos) {
        saw_committed = true;
      }
    }
  }
  TEST_ASSERT(saw_committed);
}

void TestReloadAsrBackendDelegation() {
  MockAudioInputSource primary_source(15, 16000);
  RecognitionSessionManager manager;
  LaneSupervisorOptions opts{
      .load_config = [] { return MakeConfig(/*duck=*/false); },
  };
  LaneSupervisor supervisor(&primary_source, nullptr, &manager, nullptr, std::move(opts));
  const auto res = supervisor.ReloadAsrBackend();
  TEST_ASSERT(res.ok);
  const auto state = supervisor.GetAsrBackendState();
  TEST_ASSERT(!state.has_effective_backend);
}

} // namespace

int main() {
  std::printf("Running test_lane_supervisor...\n");
  TestMultiLaneIsolationAndConcurrentEnqueue();
  TestTranscribeStreamAndAutoReclaim();
  TestReloadAsrBackendDelegation();
  std::printf("All test_lane_supervisor passed!\n");
  return 0;
}
