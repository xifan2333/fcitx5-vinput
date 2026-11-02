#include <cstdlib>
#include <iostream>
#include <string>

#include "common/dbus/dbus_interface.h"

#include "daemon/runtime/runtime_state_machine.h"

namespace {

using vinput::daemon::runtime::PostprocessingState;
using vinput::daemon::runtime::RuntimeStateMachine;
using vinput::dbus::Status;

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestInitialState() {
  const RuntimeStateMachine sm;
  Expect(sm.CurrentPhase() == Status::Idle, "Initial phase must be Idle");
  Expect(sm.CurrentPostprocessingState() == PostprocessingState::Inactive,
         "Initial postprocessing state must be Inactive");
  Expect(!sm.IsCommand(), "Initial is_command must be false");
  Expect(sm.SelectedText().empty(), "Initial selected text must be empty");
}

void TestHappyPathDictation() {
  RuntimeStateMachine sm;

  // 1. Start recording
  auto res = sm.OnStartRecording(/*is_command=*/false, "selection");
  Expect(res.accepted, "StartRecording must be accepted in Idle");
  Expect(res.previous_phase == Status::Idle, "Prev phase must be Idle");
  Expect(res.new_phase == Status::Recording, "New phase must be Recording");
  Expect(res.should_start_capture, "Must flag should_start_capture");
  Expect(sm.CurrentPhase() == Status::Recording, "Phase must be Recording");
  Expect(!sm.IsCommand(), "is_command must be false");
  Expect(sm.SelectedText() == "selection", "Selected text must be preserved");

  // 2. Reject double start
  auto res_busy = sm.OnStartRecording(false, "");
  Expect(!res_busy.accepted, "Double start must be rejected");
  Expect(!res_busy.rejection_reason.empty(), "Rejection reason must be provided");

  // 3. Stop recording with sufficient samples
  auto res_stop = sm.OnStopRecording(/*sample_count=*/16000, /*min_samples=*/8000);
  Expect(res_stop.accepted, "StopRecording must be accepted");
  Expect(res_stop.new_phase == Status::Inferring, "New phase must be Inferring");
  Expect(res_stop.should_stop_capture, "Must flag should_stop_capture");
  Expect(res_stop.should_submit_inference, "Must flag should_submit_inference");
  Expect(!res_stop.is_too_short, "Should not be too short");
  Expect(sm.CurrentPhase() == Status::Inferring, "Phase must be Inferring");

  // 4. Inference complete with scene
  auto res_infer = sm.OnInferenceComplete(/*has_scene_to_postprocess=*/true);
  Expect(res_infer.accepted, "InferenceComplete must be accepted");
  Expect(res_infer.new_phase == Status::Postprocessing, "New phase must be Postprocessing");
  Expect(res_infer.should_enter_postprocessing, "Must flag should_enter_postprocessing");
  Expect(sm.CurrentPhase() == Status::Postprocessing, "Phase must be Postprocessing");
  Expect(sm.CurrentPostprocessingState() == PostprocessingState::Dictation,
         "State must be Dictation");

  // 5. Postprocess complete
  auto res_post = sm.OnPostprocessingComplete();
  Expect(res_post.accepted, "PostprocessingComplete must be accepted");
  Expect(res_post.new_phase == Status::Idle, "New phase must be Idle");
  Expect(res_post.should_commit_result, "Must flag should_commit_result");
  Expect(sm.CurrentPhase() == Status::Idle, "Phase must be back to Idle");
  Expect(sm.CurrentPostprocessingState() == PostprocessingState::Inactive,
         "State must be Inactive");
}

void TestStopTooShort() {
  RuntimeStateMachine sm;
  sm.OnStartRecording(false, "");

  auto res = sm.OnStopRecording(/*sample_count=*/1000, /*min_samples=*/8000);
  Expect(res.accepted, "StopRecording must be accepted");
  Expect(res.new_phase == Status::Idle, "Short recording must return to Idle");
  Expect(res.should_stop_capture, "Must stop capture");
  Expect(res.should_cancel_session, "Must cancel session");
  Expect(res.is_too_short, "Must flag is_too_short");
  Expect(sm.CurrentPhase() == Status::Idle, "Phase must be Idle");
}

void TestCancellationFlows() {
  // Cancel during recording
  {
    RuntimeStateMachine sm;
    sm.OnStartRecording(false, "");
    auto res = sm.OnCancel(/*commit_raw_text=*/false);
    Expect(res.accepted, "Cancel during recording must be accepted");
    Expect(res.new_phase == Status::Idle, "Phase must be Idle");
    Expect(res.should_stop_capture, "Must stop capture");
    Expect(res.should_cancel_session, "Must cancel session");
    Expect(sm.CurrentPhase() == Status::Idle, "Phase must be Idle");
  }

  // Cancel during postprocessing - Dictation
  {
    RuntimeStateMachine sm;
    sm.OnStartRecording(false, "");
    sm.OnStopRecording(16000, 8000);
    sm.OnInferenceComplete(true);

    auto res = sm.OnCancel(/*commit_raw_text=*/true);
    Expect(res.accepted, "Cancel with commit raw in dictation must be accepted");
    Expect(sm.CurrentPostprocessingState() == PostprocessingState::CommitRaw,
           "State must be CommitRaw");

    auto taken = sm.TakePostprocessingState();
    Expect(taken == PostprocessingState::CommitRaw, "Taken state must be CommitRaw");
    Expect(sm.CurrentPostprocessingState() == PostprocessingState::Inactive,
           "State must reset to Inactive");
  }

  // Cancel during postprocessing - Command
  {
    RuntimeStateMachine sm;
    sm.OnStartRecording(/*is_command=*/true, "");
    sm.OnStopRecording(16000, 8000);
    sm.OnInferenceComplete(true);

    Expect(sm.CurrentPostprocessingState() == PostprocessingState::Command,
           "State must be Command");
    // Command mode rejects commit_raw_text
    auto res_invalid = sm.OnCancel(/*commit_raw_text=*/true);
    Expect(!res_invalid.accepted, "Cancel commit_raw in Command mode must be rejected");

    // Command mode accepts discard
    auto res_valid = sm.OnCancel(/*commit_raw_text=*/false);
    Expect(res_valid.accepted, "Cancel discard in Command mode must be accepted");
    Expect(sm.CurrentPostprocessingState() == PostprocessingState::Discard,
           "State must be Discard");
  }

  // Cancel during inferring must be rejected
  {
    RuntimeStateMachine sm;
    sm.OnStartRecording(false, "");
    sm.OnStopRecording(16000, 8000);
    auto res = sm.OnCancel(false);
    Expect(!res.accepted, "Cancel during inferring must be rejected");
  }
}

void TestAudioErrorAndReset() {
  RuntimeStateMachine sm;
  sm.OnStartRecording(false, "");

  auto res = sm.OnAudioError();
  Expect(res.accepted, "AudioError must be accepted during recording");
  Expect(res.new_phase == Status::Error, "Phase must be Error");
  Expect(res.should_stop_capture, "Must stop capture");
  Expect(res.should_cancel_session, "Must cancel session");
  Expect(sm.CurrentPhase() == Status::Error, "Phase must be Error");

  // Reset to idle
  auto res_reset = sm.OnResetToIdle();
  Expect(res_reset.accepted, "ResetToIdle must be accepted");
  Expect(sm.CurrentPhase() == Status::Idle, "Phase must be Idle");
}

} // namespace

int main() {
  TestInitialState();
  TestHappyPathDictation();
  TestStopTooShort();
  TestCancellationFlows();
  TestAudioErrorAndReset();

  std::cout << "All RuntimeStateMachine tests passed!\n";
  return 0;
}
