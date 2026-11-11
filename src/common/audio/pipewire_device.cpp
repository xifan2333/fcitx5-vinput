#include "common/audio/pipewire_device.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <ctime>
#include <optional>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <spa/support/loop.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/utils/string_utils.h"

namespace vinput::pw {

namespace {

struct PwData {
  pw_main_loop* loop = nullptr;
  pw_context* context = nullptr;
  pw_core* core = nullptr;
  pw_registry* registry = nullptr;
  spa_hook registry_listener;
  spa_hook core_listener;
  int pending_sync = -1;
  std::vector<DeviceInfo> devices;
};

void on_core_done(void* data, uint32_t id, int seq) {
  auto* d = static_cast<PwData*>(data);
  if (id == PW_ID_CORE && d->pending_sync == seq) {
    pw_main_loop_quit(d->loop);
  }
}

void on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
  (void)id;
  (void)seq;
  (void)res;
  (void)message;
  auto* d = static_cast<PwData*>(data);
  pw_main_loop_quit(d->loop);
}

void on_enumeration_timeout(void* data, uint64_t expirations) {
  (void)expirations;
  auto* d = static_cast<PwData*>(data);
  pw_main_loop_quit(d->loop);
}

const struct pw_core_events core_events = []() {
  struct pw_core_events ev;
  spa_zero(ev);
  ev.version = PW_VERSION_CORE_EVENTS;
  ev.done = on_core_done;
  ev.error = on_core_error;
  return ev;
}();

void AppendSourceDevice(PwData* d, uint32_t id, const char* name, const char* desc) {
  DeviceInfo info;
  info.id = id;
  if (name != nullptr) {
    info.name = name;
  }
  if (desc != nullptr) {
    info.description = desc;
  }
  info.is_sink_monitor = false;
  d->devices.push_back(std::move(info));
}

void AppendSinkMonitorDevice(PwData* d, uint32_t id, const char* name, const char* desc) {
  DeviceInfo info;
  info.id = id;
  if (name != nullptr) {
    info.name = std::string(name) + ".monitor";
  }
  if (desc != nullptr) {
    info.description = std::string(desc) + " (Monitor)";
  } else if (name != nullptr) {
    info.description = std::string(name) + " (Monitor)";
  }
  info.is_sink_monitor = true;
  d->devices.push_back(std::move(info));
}

void registry_event_global(void* data, uint32_t id, uint32_t permissions, const char* type,
                           uint32_t version, const struct spa_dict* props) {
  (void)permissions;
  (void)version;
  if (data == nullptr || type == nullptr || props == nullptr) {
    return;
  }
  if (std::string_view(type) != PW_TYPE_INTERFACE_Node) {
    return;
  }

  const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (media_class == nullptr) {
    return;
  }

  const std::string_view cls(media_class);
  auto* d = static_cast<PwData*>(data);
  const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

  if (cls == "Audio/Source") {
    AppendSourceDevice(d, id, name, desc);
  } else if (cls == "Audio/Sink") {
    AppendSinkMonitorDevice(d, id, name, desc);
  }
}

void registry_event_global_remove(void* data, uint32_t id) {
  (void)data;
  (void)id;
}

const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    registry_event_global,
    registry_event_global_remove,
};

struct PwResourceGuard {
  PwData* d = nullptr;

  explicit PwResourceGuard(PwData* data_ptr) : d(data_ptr) {}
  ~PwResourceGuard() {
    if (d == nullptr) {
      return;
    }
    if (d->registry != nullptr) {
      spa_hook_remove(&d->registry_listener);
      pw_proxy_destroy(std::bit_cast<pw_proxy*>(d->registry));
    }
    if (d->core != nullptr) {
      spa_hook_remove(&d->core_listener);
      pw_core_disconnect(d->core);
    }
    if (d->context != nullptr) {
      pw_context_destroy(d->context);
    }
    if (d->loop != nullptr) {
      pw_main_loop_destroy(d->loop);
    }
    pw_deinit();
  }

  PwResourceGuard(const PwResourceGuard&) = delete;
  PwResourceGuard& operator=(const PwResourceGuard&) = delete;
  PwResourceGuard(PwResourceGuard&&) = delete;
  PwResourceGuard& operator=(PwResourceGuard&&) = delete;
};

struct ScopedEnumerationTimer {
  spa_source* timer = nullptr;
  const spa_loop_utils_methods* utils = nullptr;
  void* u_data = nullptr;

  ScopedEnumerationTimer(spa_source* t, const spa_loop_utils_methods* u, void* ud) noexcept
      : timer(t), utils(u), u_data(ud) {}

  ~ScopedEnumerationTimer() {
    if (timer != nullptr && utils != nullptr && utils->destroy_source != nullptr) {
      utils->destroy_source(u_data, timer);
    }
  }

  ScopedEnumerationTimer(const ScopedEnumerationTimer&) = delete;
  ScopedEnumerationTimer& operator=(const ScopedEnumerationTimer&) = delete;
  ScopedEnumerationTimer(ScopedEnumerationTimer&& other) noexcept
      : timer(std::exchange(other.timer, nullptr)), utils(std::exchange(other.utils, nullptr)),
        u_data(std::exchange(other.u_data, nullptr)) {}
  ScopedEnumerationTimer& operator=(ScopedEnumerationTimer&& other) noexcept {
    if (this != &other) {
      if (timer != nullptr && utils != nullptr && utils->destroy_source != nullptr) {
        utils->destroy_source(u_data, timer);
      }
      timer = std::exchange(other.timer, nullptr);
      utils = std::exchange(other.utils, nullptr);
      u_data = std::exchange(other.u_data, nullptr);
    }
    return *this;
  }
};

std::optional<ScopedEnumerationTimer> ArmEnumerationTimeout(pw_loop* loop, PwData& data) {
  if (loop == nullptr || loop->utils == nullptr || loop->utils->iface.cb.funcs == nullptr) {
    return std::nullopt;
  }
  const auto* utils = static_cast<const spa_loop_utils_methods*>(loop->utils->iface.cb.funcs);
  void* u_data = loop->utils->iface.cb.data;
  if (utils->add_timer == nullptr || utils->update_timer == nullptr) {
    return std::nullopt;
  }

  spa_source* timer = utils->add_timer(u_data, on_enumeration_timeout, &data);
  if (timer == nullptr) {
    return std::nullopt;
  }

  timespec value{};
  value.tv_sec = 0;
  value.tv_nsec = 250'000'000L;
  if (utils->update_timer(u_data, timer, &value, nullptr, false) < 0) {
    if (utils->destroy_source != nullptr) {
      utils->destroy_source(u_data, timer);
    }
    return std::nullopt;
  }

  return ScopedEnumerationTimer(timer, utils, u_data);
}

bool CompareDevices(const DeviceInfo& a, const DeviceInfo& b) noexcept {
  if (a.is_sink_monitor != b.is_sink_monitor) {
    return !a.is_sink_monitor && b.is_sink_monitor;
  }
  return a.id < b.id;
}

} // namespace

std::vector<DeviceInfo> EnumerateAudioSources() {
  pw_init(nullptr, nullptr);

  PwData data{};
  const PwResourceGuard guard{&data};

  data.loop = pw_main_loop_new(nullptr);
  if (data.loop == nullptr) {
    return {};
  }

  data.context = pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (data.context == nullptr) {
    return {};
  }

  data.core = pw_context_connect(data.context, nullptr, 0);
  if (data.core == nullptr) {
    return {};
  }

  pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
  data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
  spa_zero(data.registry_listener);
  pw_registry_add_listener(data.registry, &data.registry_listener, &registry_events, &data);

  data.pending_sync = pw_core_sync(data.core, PW_ID_CORE, 0);
  if (data.pending_sync >= 0) {
    // Arm a 250ms bounded timer so enumeration never hangs if PipeWire is unresponsive.
    if (auto timer = ArmEnumerationTimeout(pw_main_loop_get_loop(data.loop), data);
        timer.has_value()) {
      pw_main_loop_run(data.loop);
    }
  }

  std::stable_sort(data.devices.begin(), data.devices.end(), CompareDevices);

  return data.devices;
}

ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           const std::vector<DeviceInfo>& known_devices) {
  ResolvedCaptureTarget resolved;
  if (target.empty() || target == "default") {
    return resolved;
  }

  static constexpr std::string_view kMonitorSuffix = ".monitor";

  // 1. If known devices are provided, prioritize matching against real device metadata.
  for (const auto& dev : known_devices) {
    if (dev.name == target) {
      if (!dev.is_sink_monitor) {
        // Real Audio/Source node, even if named with a ".monitor" suffix (e.g. clean.monitor).
        resolved.node_name = dev.name;
        resolved.is_sink_capture = false;
        return resolved;
      }

      // Real Audio/Sink monitor: strip the ".monitor" suffix to target the underlying sink node.
      const auto stripped = vinput::str::StripSuffix(target, kMonitorSuffix);
      resolved.node_name = stripped.empty() ? dev.name : std::string(stripped);
      resolved.is_sink_capture = true;
      return resolved;
    }
  }

  // 2. Fallback heuristic when device is offline or unknown.
  if (target.size() > kMonitorSuffix.size() && target.ends_with(kMonitorSuffix)) {
    resolved.node_name = std::string(vinput::str::StripSuffix(target, kMonitorSuffix));
    resolved.is_sink_capture = true;
    return resolved;
  }

  resolved.node_name = std::string(target);
  resolved.is_sink_capture = false;
  return resolved;
}

} // namespace vinput::pw
