#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/dbus/dbus_interface.h"

namespace vinput::daemon::runtime {

enum class PostprocessingState : std::uint8_t { Inactive, Dictation, Command, Discard, CommitRaw };

enum class TransitionAction : std::uint8_t {
  None,
  StartCapture,
  SubmitInference,
  DiscardTooShort,
  AbortCapture,
  EnterPostprocessing,
  CommitResult,
  RequestCommitRaw,
  RequestDiscard,
};

struct StateTransitionResult {
  bool accepted = false;
  vinput::dbus::Status previous_phase = vinput::dbus::Status::Idle;
  vinput::dbus::Status new_phase = vinput::dbus::Status::Idle;
  TransitionAction action = TransitionAction::None;
  std::string rejection_reason;
};

class RuntimeStateMachine {
public:
  RuntimeStateMachine() = default;

  [[nodiscard]] vinput::dbus::Status CurrentPhase() const noexcept;
  [[nodiscard]] bool IsIdle() const noexcept;
  [[nodiscard]] bool IsRecording() const noexcept;
  [[nodiscard]] PostprocessingState CurrentPostprocessingState() const noexcept;
  [[nodiscard]] bool IsCommand() const noexcept;
  [[nodiscard]] const std::string& SelectedText() const noexcept;

  StateTransitionResult OnStartRecording(bool is_command, const std::string& selected_text);
  StateTransitionResult OnStopRecording(std::size_t sample_count, std::size_t min_samples);
  StateTransitionResult OnCancel(bool commit_raw_text);
  StateTransitionResult OnAudioError();

  StateTransitionResult OnInferenceComplete(bool has_scene_to_postprocess);
  StateTransitionResult OnInferenceError();

  StateTransitionResult OnPostprocessingComplete();

  StateTransitionResult OnResetToIdle();

  PostprocessingState TakePostprocessingState() noexcept;

  void Clear() noexcept;

private:
  StateTransitionResult AbortRecordingToIdle(vinput::dbus::Status prev, TransitionAction action);
  StateTransitionResult CompleteToIdle(vinput::dbus::Status prev);

  vinput::dbus::Status phase_ = vinput::dbus::Status::Idle;
  PostprocessingState postprocessing_state_ = PostprocessingState::Inactive;
  bool is_command_ = false;
  std::string selected_text_;
};

} // namespace vinput::daemon::runtime
