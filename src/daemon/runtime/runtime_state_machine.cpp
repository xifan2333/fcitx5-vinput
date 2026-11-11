#include "daemon/runtime/runtime_state_machine.h"

#include <cstddef>
#include <string>
#include <utility>

#include "common/dbus/dbus_interface.h"
#include "common/i18n.h"

namespace vinput::daemon::runtime {

namespace {

StateTransitionResult Reject(vinput::dbus::Status phase, std::string reason = {}) {
  StateTransitionResult res;
  res.accepted = false;
  res.previous_phase = phase;
  res.new_phase = phase;
  res.action = TransitionAction::None;
  res.rejection_reason = std::move(reason);
  return res;
}

} // namespace

vinput::dbus::Status RuntimeStateMachine::CurrentPhase() const noexcept {
  return phase_;
}

bool RuntimeStateMachine::IsIdle() const noexcept {
  return phase_ == vinput::dbus::Status::Idle;
}

bool RuntimeStateMachine::IsRecording() const noexcept {
  return phase_ == vinput::dbus::Status::Recording;
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

  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = phase_;
  res.action = TransitionAction::StartCapture;
  return res;
}

StateTransitionResult RuntimeStateMachine::OnStopRecording(std::size_t sample_count,
                                                           std::size_t min_samples) {
  if (phase_ != vinput::dbus::Status::Recording) {
    return Reject(phase_, _("Recording is not active."));
  }

  const auto prev = phase_;
  if (sample_count < min_samples) {
    return AbortRecordingToIdle(prev, TransitionAction::DiscardTooShort);
  }

  phase_ = vinput::dbus::Status::Inferring;
  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = phase_;
  res.action = TransitionAction::SubmitInference;
  return res;
}

StateTransitionResult RuntimeStateMachine::OnCancel(bool commit_raw_text) {
  const auto prev = phase_;
  if (phase_ == vinput::dbus::Status::Idle) {
    StateTransitionResult res;
    res.accepted = true;
    res.previous_phase = prev;
    res.new_phase = prev;
    res.action = TransitionAction::None;
    return res;
  }

  if (phase_ == vinput::dbus::Status::Recording) {
    return AbortRecordingToIdle(prev, TransitionAction::AbortCapture);
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

    StateTransitionResult res;
    res.accepted = true;
    res.previous_phase = prev;
    res.new_phase = prev;
    res.action =
        commit_raw_text ? TransitionAction::RequestCommitRaw : TransitionAction::RequestDiscard;
    return res;
  }

  return Reject(prev, _("No cancellable operation in current phase."));
}

StateTransitionResult RuntimeStateMachine::OnAudioError() {
  if (phase_ != vinput::dbus::Status::Recording) {
    return Reject(phase_);
  }

  const auto prev = phase_;
  phase_ = vinput::dbus::Status::Error;
  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = phase_;
  res.action = TransitionAction::AbortCapture;
  return res;
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
    StateTransitionResult res;
    res.accepted = true;
    res.previous_phase = prev;
    res.new_phase = phase_;
    res.action = TransitionAction::EnterPostprocessing;
    return res;
  }

  return CompleteToIdle(prev);
}

StateTransitionResult RuntimeStateMachine::OnInferenceError() {
  if (phase_ != vinput::dbus::Status::Inferring) {
    return Reject(phase_);
  }

  const auto prev = phase_;
  phase_ = vinput::dbus::Status::Error;
  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = phase_;
  res.action = TransitionAction::None;
  return res;
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
  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = vinput::dbus::Status::Idle;
  res.action = TransitionAction::None;
  return res;
}

StateTransitionResult RuntimeStateMachine::AbortRecordingToIdle(vinput::dbus::Status prev,
                                                                TransitionAction action) {
  phase_ = vinput::dbus::Status::Idle;
  is_command_ = false;
  selected_text_.clear();
  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = phase_;
  res.action = action;
  return res;
}

StateTransitionResult RuntimeStateMachine::CompleteToIdle(vinput::dbus::Status prev) {
  phase_ = vinput::dbus::Status::Idle;
  postprocessing_state_ = PostprocessingState::Inactive;
  is_command_ = false;
  selected_text_.clear();
  StateTransitionResult res;
  res.accepted = true;
  res.previous_phase = prev;
  res.new_phase = phase_;
  res.action = TransitionAction::CommitResult;
  return res;
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
