#include "common/audio/pipewire_device.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/main-loop.h>
#include <pipewire/pipewire.h>
#include <spa/support/loop.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

void registry_event_global(void* data, uint32_t id, uint32_t permissions, const char* type,
                           uint32_t version, const struct spa_dict* props) {
  (void)permissions;
  (void)version;
  if (std::string(type) == PW_TYPE_INTERFACE_Node && props) {
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class == nullptr) {
      return;
    }
    const std::string_view cls(media_class);
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

    auto* d = static_cast<PwData*>(data);
    if (cls == "Audio/Source") {
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
    } else if (cls == "Audio/Sink") {
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

struct PipeWireSession {
  PwData data{};
  bool core_listener_added = false;
  bool registry_listener_added = false;

  PipeWireSession() {
    pw_init(nullptr, nullptr);
    spa_zero(data.core_listener);
    spa_zero(data.registry_listener);
  }

  ~PipeWireSession() {
    if (registry_listener_added) {
      spa_hook_remove(&data.registry_listener);
    }
    if (data.registry != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(data.registry));
    }
    if (core_listener_added) {
      spa_hook_remove(&data.core_listener);
    }
    if (data.core != nullptr) {
      pw_core_disconnect(data.core);
    }
    if (data.context != nullptr) {
      pw_context_destroy(data.context);
    }
    if (data.loop != nullptr) {
      pw_main_loop_destroy(data.loop);
    }
    pw_deinit();
  }

  PipeWireSession(const PipeWireSession&) = delete;
  PipeWireSession& operator=(const PipeWireSession&) = delete;
  PipeWireSession(PipeWireSession&&) = delete;
  PipeWireSession& operator=(PipeWireSession&&) = delete;
};

} // namespace

std::vector<DeviceInfo> EnumerateAudioSources() {
  PipeWireSession session;

  session.data.loop = pw_main_loop_new(nullptr);
  if (session.data.loop == nullptr) {
    return {};
  }

  session.data.context = pw_context_new(pw_main_loop_get_loop(session.data.loop), nullptr, 0);
  if (session.data.context == nullptr) {
    return {};
  }

  session.data.core = pw_context_connect(session.data.context, nullptr, 0);
  if (session.data.core == nullptr) {
    return {};
  }

  pw_core_add_listener(session.data.core, &session.data.core_listener, &core_events, &session.data);
  session.core_listener_added = true;

  session.data.registry = pw_core_get_registry(session.data.core, PW_VERSION_REGISTRY, 0);
  if (session.data.registry == nullptr) {
    return {};
  }

  pw_registry_add_listener(session.data.registry, &session.data.registry_listener, &registry_events,
                           &session.data);
  session.registry_listener_added = true;

  session.data.pending_sync = pw_core_sync(session.data.core, PW_ID_CORE, 0);
  if (session.data.pending_sync >= 0) {
    // Arm a 250ms bounded timer so enumeration never hangs if PipeWire is unresponsive.
    pw_loop* const loop = pw_main_loop_get_loop(session.data.loop);
    spa_source* timer = nullptr;
    const spa_loop_utils_methods* utils = nullptr;
    void* u_data = nullptr;
    bool timer_armed = false;

    if (loop != nullptr && loop->utils != nullptr && loop->utils->iface.cb.funcs != nullptr) {
      utils = static_cast<const spa_loop_utils_methods*>(loop->utils->iface.cb.funcs);
      u_data = loop->utils->iface.cb.data;
      if (utils->add_timer != nullptr && utils->update_timer != nullptr) {
        timer = utils->add_timer(u_data, on_enumeration_timeout, &session.data);
        if (timer != nullptr) {
          timespec value{};
          value.tv_sec = 0;
          value.tv_nsec = 250'000'000L;
          if (utils->update_timer(u_data, timer, &value, nullptr, false) >= 0) {
            timer_armed = true;
          }
        }
      }
    }

    // Only enter event loop if timeout protection is armed to prevent unbounded hang.
    if (timer_armed) {
      pw_main_loop_run(session.data.loop);
    }

    if (timer != nullptr && utils != nullptr && utils->destroy_source != nullptr) {
      utils->destroy_source(u_data, timer);
    }
  }

  std::stable_sort(session.data.devices.begin(), session.data.devices.end(),
                   [](const DeviceInfo& a, const DeviceInfo& b) {
                     if (a.is_sink_monitor != b.is_sink_monitor) {
                       return !a.is_sink_monitor && b.is_sink_monitor;
                     }
                     return a.id < b.id;
                   });

  return session.data.devices;
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
      if (target.size() > kMonitorSuffix.size() &&
          target.substr(target.size() - kMonitorSuffix.size()) == kMonitorSuffix) {
        resolved.node_name = std::string(target.substr(0, target.size() - kMonitorSuffix.size()));
      } else {
        resolved.node_name = dev.name;
      }
      resolved.is_sink_capture = true;
      return resolved;
    }
  }

  // 2. Fallback heuristic when device is offline or unknown.
  if (target.size() > kMonitorSuffix.size() &&
      target.substr(target.size() - kMonitorSuffix.size()) == kMonitorSuffix) {
    resolved.node_name = std::string(target.substr(0, target.size() - kMonitorSuffix.size()));
    resolved.is_sink_capture = true;
    return resolved;
  }

  resolved.node_name = std::string(target);
  resolved.is_sink_capture = false;
  return resolved;
}

} // namespace vinput::pw
