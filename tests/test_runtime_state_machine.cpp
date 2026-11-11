#include <cstdlib>
#include <iostream>
#include <string>

#include "common/dbus/dbus_interface.h"

#include "daemon/runtime/runtime_state_machine.h"

namespace {

using vinput::daemon::runtime::PostprocessingState;
using vinput::daemon::runtime::RuntimeStateMachine;
using vinput::daemon::runtime::TransitionAction;
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
  Expect(sm.IsIdle(), "Initial state must be Idle via intent check");
  Expect(!sm.IsRecording(), "Initial state must not be Recording");
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
  Expect(res.action == TransitionAction::StartCapture, "Action must be StartCapture");
  Expect(sm.CurrentPhase() == Status::Recording, "Phase must be Recording");
  Expect(sm.IsRecording(), "Must be recording via intent check");
  Expect(!sm.IsIdle(), "Must not be idle while recording");
  Expect(!sm.IsCommand(), "is_command must be false");
  Expect(sm.SelectedText() == "selection", "Selected text must be preserved");

  // 2. Reject double start
  auto res_busy = sm.OnStartRecording(false, "");
  Expect(!res_busy.accepted, "Double start must be rejected");
  Expect(!res_busy.rejection_reason.empty(), "Rejection reason must be provided");
  Expect(res_busy.action == TransitionAction::None, "Rejected action must be None");

  // 3. Stop recording with sufficient samples
  auto res_stop = sm.OnStopRecording(/*sample_count=*/16000, /*min_samples=*/8000);
  Expect(res_stop.accepted, "StopRecording must be accepted");
  Expect(res_stop.new_phase == Status::Inferring, "New phase must be Inferring");
  Expect(res_stop.action == TransitionAction::SubmitInference, "Action must be SubmitInference");
  Expect(sm.CurrentPhase() == Status::Inferring, "Phase must be Inferring");
  Expect(!sm.IsRecording(), "Must no longer be recording");

  // 4. Inference complete with scene
  auto res_infer = sm.OnInferenceComplete(/*has_scene_to_postprocess=*/true);
  Expect(res_infer.accepted, "InferenceComplete must be accepted");
  Expect(res_infer.new_phase == Status::Postprocessing, "New phase must be Postprocessing");
  Expect(res_infer.action == TransitionAction::EnterPostprocessing,
         "Action must be EnterPostprocessing");
  Expect(sm.CurrentPhase() == Status::Postprocessing, "Phase must be Postprocessing");
  Expect(sm.CurrentPostprocessingState() == PostprocessingState::Dictation,
         "State must be Dictation");

  // 5. Postprocess complete
  auto res_post = sm.OnPostprocessingComplete();
  Expect(res_post.accepted, "PostprocessingComplete must be accepted");
  Expect(res_post.new_phase == Status::Idle, "New phase must be Idle");
  Expect(res_post.action == TransitionAction::CommitResult, "Action must be CommitResult");
  Expect(sm.CurrentPhase() == Status::Idle, "Phase must be back to Idle");
  Expect(sm.IsIdle(), "Must be Idle via intent check");
  Expect(sm.CurrentPostprocessingState() == PostprocessingState::Inactive,
         "State must be Inactive");
}

void TestStopTooShort() {
  RuntimeStateMachine sm;
  sm.OnStartRecording(false, "");

  auto res = sm.OnStopRecording(/*sample_count=*/1000, /*min_samples=*/8000);
  Expect(res.accepted, "StopRecording must be accepted");
  Expect(res.new_phase == Status::Idle, "Short recording must return to Idle");
  Expect(res.action == TransitionAction::DiscardTooShort, "Action must be DiscardTooShort");
  Expect(sm.CurrentPhase() == Status::Idle, "Phase must be Idle");
  Expect(sm.IsIdle(), "Must be idle");
}

void TestCancellationFlows() {
  // Cancel during recording
  {
    RuntimeStateMachine sm;
    sm.OnStartRecording(false, "");
    auto res = sm.OnCancel(/*commit_raw_text=*/false);
    Expect(res.accepted, "Cancel during recording must be accepted");
    Expect(res.new_phase == Status::Idle, "Phase must be Idle");
    Expect(res.action == TransitionAction::AbortCapture, "Action must be AbortCapture");
    Expect(sm.CurrentPhase() == Status::Idle, "Phase must be Idle");
    Expect(sm.IsIdle(), "Must be idle");
  }

  // Cancel during postprocessing - Dictation
  {
    RuntimeStateMachine sm;
    sm.OnStartRecording(false, "");
    sm.OnStopRecording(16000, 8000);
    sm.OnInferenceComplete(true);

    auto res = sm.OnCancel(/*commit_raw_text=*/true);
    Expect(res.accepted, "Cancel with commit raw in dictation must be accepted");
    Expect(res.action == TransitionAction::RequestCommitRaw, "Action must be RequestCommitRaw");
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
    Expect(res_valid.action == TransitionAction::RequestDiscard, "Action must be RequestDiscard");
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
    Expect(res.action == TransitionAction::None, "Action must be None");
  }
}

void TestAudioErrorAndReset() {
  RuntimeStateMachine sm;
  sm.OnStartRecording(false, "");

  auto res = sm.OnAudioError();
  Expect(res.accepted, "AudioError must be accepted during recording");
  Expect(res.new_phase == Status::Error, "Phase must be Error");
  Expect(res.action == TransitionAction::AbortCapture, "Action must be AbortCapture");
  Expect(sm.CurrentPhase() == Status::Error, "Phase must be Error");

  // Reset to idle
  auto res_reset = sm.OnResetToIdle();
  Expect(res_reset.accepted, "ResetToIdle must be accepted");
  Expect(res_reset.action == TransitionAction::None, "Action must be None");
  Expect(sm.CurrentPhase() == Status::Idle, "Phase must be Idle");
  Expect(sm.IsIdle(), "Must be Idle");
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
