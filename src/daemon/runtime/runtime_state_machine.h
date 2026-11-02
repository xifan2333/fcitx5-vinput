#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/dbus/dbus_interface.h"

namespace vinput::daemon::runtime {

enum class PostprocessingState : std::uint8_t { Inactive, Dictation, Command, Discard, CommitRaw };

struct StateTransitionResult {
  bool accepted = false;
  vinput::dbus::Status previous_phase = vinput::dbus::Status::Idle;
  vinput::dbus::Status new_phase = vinput::dbus::Status::Idle;
  std::string rejection_reason;

  bool should_start_capture = false;
  bool should_stop_capture = false;
  bool should_submit_inference = false;
  bool should_cancel_session = false;
  bool should_enter_postprocessing = false;
  bool should_commit_result = false;
  bool should_commit_raw_text = false;
  bool should_discard_text = false;
  bool is_too_short = false;
};

class RuntimeStateMachine {
public:
  RuntimeStateMachine() = default;

  [[nodiscard]] vinput::dbus::Status CurrentPhase() const noexcept;
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
  StateTransitionResult AbortRecordingToIdle(vinput::dbus::Status prev, bool is_too_short);
  StateTransitionResult CompleteToIdle(vinput::dbus::Status prev);

  vinput::dbus::Status phase_ = vinput::dbus::Status::Idle;
  PostprocessingState postprocessing_state_ = PostprocessingState::Inactive;
  bool is_command_ = false;
  std::string selected_text_;
};

} // namespace vinput::daemon::runtime
