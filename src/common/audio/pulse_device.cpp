#include "common/audio/pulse_device.h"

#include <algorithm>
#include <chrono>
#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/operation.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/audio/pulse_raii.h"
#include "common/utils/string_utils.h"

namespace vinput::pulse {

namespace {

constexpr std::string_view kMonitorSuffix = ".monitor";

bool IsDefaultAlias(std::string_view target) {
  return target.empty() || target == "default" || target == "@DEFAULT_SOURCE@";
}

bool HasMonitorSuffix(std::string_view name) {
  return name.size() > kMonitorSuffix.size() && name.ends_with(kMonitorSuffix);
}

void OnServerInfo(pa_context* /*ctx*/, const pa_server_info* info, void* userdata) noexcept {
  if (info == nullptr || userdata == nullptr) {
    return;
  }
  try {
    auto* snapshot = static_cast<AudioServerSnapshot*>(userdata);
    if (info->default_source_name != nullptr) {
      snapshot->default_source_name = info->default_source_name;
    }
    if (info->default_sink_name != nullptr) {
      snapshot->default_sink_name = info->default_sink_name;
    }
  } catch (...) {
    auto* snapshot = static_cast<AudioServerSnapshot*>(userdata);
    snapshot->default_source_name.clear();
    snapshot->default_sink_name.clear();
  }
}

void OnSourceInfo(pa_context* /*ctx*/, const pa_source_info* info, int eol,
                  void* userdata) noexcept {
  if (eol != 0 || info == nullptr || userdata == nullptr || info->name == nullptr) {
    return;
  }
  auto* devices = static_cast<std::vector<DeviceInfo>*>(userdata);
  try {
    const std::string name(info->name);
    if (name.empty()) {
      return;
    }
    const std::string desc = (info->description != nullptr && info->description[0] != '\0')
                                 ? std::string(info->description)
                                 : name;
    devices->push_back(DeviceInfo{
        .id = info->index,
        .name = name,
        .description = desc,
        .is_sink_monitor = (info->monitor_of_sink != PA_INVALID_INDEX),
    });
  } catch (...) {
    devices->clear();
  }
}

} // namespace

bool PulseSyncSession::Connect(const char* app_name,
                               std::chrono::steady_clock::time_point deadline) {
  last_error_ = 0;
  if (!loop_) {
    last_error_ = PA_ERR_INTERNAL;
    return false;
  }

  ctx_.Reset(
      pa_context_new(pa_mainloop_get_api(loop_.Get()), app_name != nullptr ? app_name : "vinput"));
  if (!ctx_) {
    last_error_ = PA_ERR_INTERNAL;
    return false;
  }

  if (pa_context_connect(ctx_.Get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    last_error_ = pa_context_errno(ctx_.Get());
    return false;
  }

  const bool finished = StepUntil(
      [this]() {
        const pa_context_state_t st = pa_context_get_state(ctx_.Get());
        return st == PA_CONTEXT_READY || st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED;
      },
      deadline);

  if (!finished) {
    last_error_ = PA_ERR_TIMEOUT;
    return false;
  }

  if (pa_context_get_state(ctx_.Get()) != PA_CONTEXT_READY) {
    last_error_ = pa_context_errno(ctx_.Get());
    return false;
  }
  return true;
}

bool PulseSyncSession::RunOperation(pa_operation* raw_op,
                                    std::chrono::steady_clock::time_point deadline) {
  ScopedOperation op(raw_op);
  if (!op) {
    last_error_ = ctx_ ? pa_context_errno(ctx_.Get()) : PA_ERR_INTERNAL;
    return false;
  }

  const bool finished = StepUntil(
      [this, &op]() {
        const pa_context_state_t cst = pa_context_get_state(ctx_.Get());
        if (cst != PA_CONTEXT_READY) {
          return true;
        }
        return pa_operation_get_state(op.Get()) != PA_OPERATION_RUNNING;
      },
      deadline);

  if (!finished) {
    last_error_ = PA_ERR_TIMEOUT;
    return false;
  }
  if (pa_context_get_state(ctx_.Get()) != PA_CONTEXT_READY) {
    last_error_ = pa_context_errno(ctx_.Get());
    return false;
  }
  return pa_operation_get_state(op.Get()) == PA_OPERATION_DONE;
}

ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           std::span<const DeviceInfo> known_devices,
                                           std::string_view default_source_name) {
  std::string_view effective = vinput::str::Trim(target);
  if (IsDefaultAlias(effective)) {
    const std::string_view trimmed_default = vinput::str::Trim(default_source_name);
    if (IsDefaultAlias(trimmed_default)) {
      return {};
    }
    effective = trimmed_default;
  }

  const auto it = std::ranges::find_if(
      known_devices, [effective](const DeviceInfo& dev) { return dev.name == effective; });
  if (it != known_devices.end()) {
    return ResolvedCaptureTarget{
        .source_name = it->name,
        .is_sink_monitor = it->is_sink_monitor,
    };
  }

  return ResolvedCaptureTarget{
      .source_name = std::string(effective),
      .is_sink_monitor = HasMonitorSuffix(effective),
  };
}

std::string FormatConnectionError(int pa_err) {
  const char* detail = pa_err != 0 ? pa_strerror(pa_err) : nullptr;
  std::string msg = (detail != nullptr && detail[0] != '\0') ? std::string(detail)
                                                             : std::string("connection failed");
  msg += " (ensure PulseAudio or pipewire-pulse is running)";
  return msg;
}

AudioServerSnapshot QueryAudioServerSnapshot(int timeout_ms) {
  AudioServerSnapshot snapshot;
  const int clamped_ms = std::max(timeout_ms, 50);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(clamped_ms);

  PulseSyncSession session;
  if (!session.Connect("vinput-device-probe", deadline)) {
    return snapshot;
  }
  snapshot.connected = true;

  (void)session.RunOperation(
      pa_context_get_server_info(session.Context(), &OnServerInfo, &snapshot), deadline);
  (void)session.RunOperation(
      pa_context_get_source_info_list(session.Context(), &OnSourceInfo, &snapshot.sources),
      deadline);

  std::ranges::sort(snapshot.sources, [](const DeviceInfo& a, const DeviceInfo& b) {
    if (a.is_sink_monitor != b.is_sink_monitor) {
      return !a.is_sink_monitor;
    }
    return a.id < b.id;
  });

  return snapshot;
}

std::vector<DeviceInfo> EnumerateAudioSources(int timeout_ms) {
  return QueryAudioServerSnapshot(timeout_ms).sources;
}

} // namespace vinput::pulse
