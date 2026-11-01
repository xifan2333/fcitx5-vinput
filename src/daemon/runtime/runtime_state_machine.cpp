#include "daemon/runtime/runtime_state_machine.h"

#include <cstddef>
#include <string>
#include <utility>

#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"

namespace vinput::daemon::runtime {

namespace {

StateTransitionResult Reject(vinput::dbus::Status phase, std::string reason = {}) {
  return StateTransitionResult{
      .accepted = false,
      .previous_phase = phase,
      .new_phase = phase,
      .rejection_reason = std::move(reason),
  };
}

} // namespace

vinput::dbus::Status RuntimeStateMachine::CurrentPhase() const noexcept {
  return phase_;
}

PostprocessingState RuntimeStateMachine::CurrentPostprocessingState() const noexcept {
  return postprocessing_state_;
}

bool RuntimeStateMachine::IsCommand() const noexcept {
  return is_command_;
}

const std::string& RuntimeStateMachine::SelectedText() const noexcept {
  return selected_text_;
}

StateTransitionResult RuntimeStateMachine::OnStartRecording(bool is_command,
                                                            const std::string& selected_text) {
  if (phase_ != vinput::dbus::Status::Idle) {
    return Reject(phase_,
                  _("Voice input daemon is busy. Please wait for the current request to finish."));
  }

  const auto prev = phase_;
  phase_ = vinput::dbus::Status::Recording;
  postprocessing_state_ = PostprocessingState::Inactive;
  is_command_ = is_command;
  selected_text_ = selected_text;

  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = phase_,
      .should_start_capture = true,
  };
}

StateTransitionResult RuntimeStateMachine::OnStopRecording(std::size_t sample_count,
                                                           std::size_t min_samples) {
  if (phase_ != vinput::dbus::Status::Recording) {
    return Reject(phase_, _("Recording is not active."));
  }

  const auto prev = phase_;
  if (sample_count < min_samples) {
    return AbortRecordingToIdle(prev, /*is_too_short=*/true);
  }

  phase_ = vinput::dbus::Status::Inferring;
  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = phase_,
      .should_stop_capture = true,
      .should_submit_inference = true,
  };
}

StateTransitionResult RuntimeStateMachine::OnCancel(bool commit_raw_text) {
  const auto prev = phase_;
  if (phase_ == vinput::dbus::Status::Idle) {
    return StateTransitionResult{
        .accepted = true,
        .previous_phase = prev,
        .new_phase = prev,
    };
  }

  if (phase_ == vinput::dbus::Status::Recording) {
    return AbortRecordingToIdle(prev, /*is_too_short=*/false);
  }

  if (phase_ == vinput::dbus::Status::Postprocessing) {
    const bool accepted =
        postprocessing_state_ == PostprocessingState::Dictation ||
        (!commit_raw_text && postprocessing_state_ == PostprocessingState::Command);
    if (!accepted) {
      return Reject(prev, _("Requested post-processing action is not available."));
    }

    postprocessing_state_ =
        commit_raw_text ? PostprocessingState::CommitRaw : PostprocessingState::Discard;

    return StateTransitionResult{
        .accepted = true,
        .previous_phase = prev,
        .new_phase = prev,
        .should_commit_raw_text = commit_raw_text,
        .should_discard_text = !commit_raw_text,
    };
  }

  return Reject(prev, _("No cancellable operation in current phase."));
}

StateTransitionResult RuntimeStateMachine::OnAudioError() {
  if (phase_ != vinput::dbus::Status::Recording) {
    return Reject(phase_);
  }

  const auto prev = phase_;
  phase_ = vinput::dbus::Status::Error;
  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = phase_,
      .should_stop_capture = true,
      .should_cancel_session = true,
  };
}

StateTransitionResult RuntimeStateMachine::OnInferenceComplete(bool has_scene_to_postprocess) {
  if (phase_ != vinput::dbus::Status::Inferring) {
    return Reject(phase_);
  }

  const auto prev = phase_;
  if (has_scene_to_postprocess) {
    phase_ = vinput::dbus::Status::Postprocessing;
    postprocessing_state_ =
        is_command_ ? PostprocessingState::Command : PostprocessingState::Dictation;
    return StateTransitionResult{
        .accepted = true,
        .previous_phase = prev,
        .new_phase = phase_,
        .should_enter_postprocessing = true,
    };
  }

  return CompleteToIdle(prev);
}

StateTransitionResult RuntimeStateMachine::OnInferenceError() {
  if (phase_ != vinput::dbus::Status::Inferring) {
    return Reject(phase_);
  }

  const auto prev = phase_;
  phase_ = vinput::dbus::Status::Error;
  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = phase_,
  };
}

StateTransitionResult RuntimeStateMachine::OnPostprocessingComplete() {
  if (phase_ != vinput::dbus::Status::Postprocessing) {
    return Reject(phase_);
  }

  return CompleteToIdle(phase_);
}

StateTransitionResult RuntimeStateMachine::OnResetToIdle() {
  const auto prev = phase_;
  Clear();
  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = vinput::dbus::Status::Idle,
  };
}

StateTransitionResult RuntimeStateMachine::AbortRecordingToIdle(vinput::dbus::Status prev,
                                                                bool is_too_short) {
  phase_ = vinput::dbus::Status::Idle;
  is_command_ = false;
  selected_text_.clear();
  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = phase_,
      .should_stop_capture = true,
      .should_cancel_session = true,
      .is_too_short = is_too_short,
  };
}

StateTransitionResult RuntimeStateMachine::CompleteToIdle(vinput::dbus::Status prev) {
  phase_ = vinput::dbus::Status::Idle;
  postprocessing_state_ = PostprocessingState::Inactive;
  is_command_ = false;
  selected_text_.clear();
  return StateTransitionResult{
      .accepted = true,
      .previous_phase = prev,
      .new_phase = phase_,
      .should_commit_result = true,
  };
}

PostprocessingState RuntimeStateMachine::TakePostprocessingState() noexcept {
  const auto state = postprocessing_state_;
  postprocessing_state_ = PostprocessingState::Inactive;
  return state;
}

void RuntimeStateMachine::Clear() noexcept {
  phase_ = vinput::dbus::Status::Idle;
  postprocessing_state_ = PostprocessingState::Inactive;
  is_command_ = false;
  selected_text_.clear();
}

} // namespace vinput::daemon::runtime
