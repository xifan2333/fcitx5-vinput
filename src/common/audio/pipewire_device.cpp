#include "common/audio/pipewire_device.h"

#include <algorithm>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vinput::pw {

namespace {

struct PwData {
  pw_main_loop* loop;
  pw_context* context;
  pw_core* core;
  pw_registry* registry;
  spa_hook registry_listener;
  spa_hook core_listener;
  int pending_sync;
  std::vector<DeviceInfo> devices;
};

void on_core_done(void* data, uint32_t id, int seq) {
  auto* d = static_cast<PwData*>(data);
  if (id == PW_ID_CORE && d->pending_sync == seq) {
    pw_main_loop_quit(d->loop);
  }
}

const struct pw_core_events core_events = []() {
  struct pw_core_events ev;
  spa_zero(ev);
  ev.version = PW_VERSION_CORE_EVENTS;
  ev.done = on_core_done;
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
        const std::string_view raw_name(name);
        static constexpr std::string_view kMonitorSuffix = ".monitor";
        if (raw_name.size() > kMonitorSuffix.size() &&
            raw_name.substr(raw_name.size() - kMonitorSuffix.size()) == kMonitorSuffix) {
          info.name = "source:" + std::string(raw_name);
        } else {
          info.name = std::string(raw_name);
        }
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

} // namespace

std::vector<DeviceInfo> EnumerateAudioSources() {
  pw_init(nullptr, nullptr);

  PwData data{};
  data.loop = pw_main_loop_new(nullptr);
  if (!data.loop) {
    pw_deinit();
    return {};
  }

  data.context = pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (!data.context) {
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    return {};
  }

  data.core = pw_context_connect(data.context, nullptr, 0);
  if (!data.core) {
    pw_context_destroy(data.context);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    return {};
  }

  pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
  data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
  spa_zero(data.registry_listener);
  pw_registry_add_listener(data.registry, &data.registry_listener, &registry_events, &data);

  data.pending_sync = pw_core_sync(data.core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

  pw_proxy_destroy(reinterpret_cast<pw_proxy*>(data.registry));
  pw_core_disconnect(data.core);
  pw_context_destroy(data.context);
  pw_main_loop_destroy(data.loop);
  pw_deinit();

  std::stable_sort(data.devices.begin(), data.devices.end(),
                   [](const DeviceInfo& a, const DeviceInfo& b) {
                     if (a.is_sink_monitor != b.is_sink_monitor) {
                       return !a.is_sink_monitor && b.is_sink_monitor;
                     }
                     return a.id < b.id;
                   });

  return data.devices;
}

ResolvedCaptureTarget ResolveCaptureTarget(std::string_view target,
                                           const std::vector<DeviceInfo>& known_devices) {
  ResolvedCaptureTarget resolved;
  if (target.empty() || target == "default") {
    return resolved;
  }

  static constexpr std::string_view kSourcePrefix = "source:";
  static constexpr std::string_view kSinkPrefix = "sink:";
  static constexpr std::string_view kMonitorSuffix = ".monitor";

  // 1. Check known devices if provided
  for (const auto& dev : known_devices) {
    if (dev.name == target) {
      if (!dev.is_sink_monitor) {
        if (target.size() > kSourcePrefix.size() &&
            target.substr(0, kSourcePrefix.size()) == kSourcePrefix) {
          resolved.node_name = std::string(target.substr(kSourcePrefix.size()));
        } else {
          resolved.node_name = std::string(target);
        }
        resolved.is_sink_capture = false;
        return resolved;
      }

      if (target.size() > kMonitorSuffix.size() &&
          target.substr(target.size() - kMonitorSuffix.size()) == kMonitorSuffix) {
        resolved.node_name = std::string(target.substr(0, target.size() - kMonitorSuffix.size()));
      } else {
        resolved.node_name = std::string(target);
      }
      resolved.is_sink_capture = true;
      return resolved;
    }
  }

  // 2. Explicit source prefix (e.g. "source:mic.monitor" -> node "mic.monitor", source capture)
  if (target.size() > kSourcePrefix.size() &&
      target.substr(0, kSourcePrefix.size()) == kSourcePrefix) {
    resolved.node_name = std::string(target.substr(kSourcePrefix.size()));
    resolved.is_sink_capture = false;
    return resolved;
  }

  // 3. Explicit sink prefix (e.g. "sink:probe" or "sink:probe.monitor")
  if (target.size() > kSinkPrefix.size() && target.substr(0, kSinkPrefix.size()) == kSinkPrefix) {
    std::string node = std::string(target.substr(kSinkPrefix.size()));
    if (node.size() > kMonitorSuffix.size() &&
        node.substr(node.size() - kMonitorSuffix.size()) == kMonitorSuffix) {
      node = node.substr(0, node.size() - kMonitorSuffix.size());
    }
    resolved.node_name = std::move(node);
    resolved.is_sink_capture = true;
    return resolved;
  }

  // 4. Legacy backward compatibility: targets ending in ".monitor" without prefix are sinks
  if (target.size() > kMonitorSuffix.size() &&
      target.substr(target.size() - kMonitorSuffix.size()) == kMonitorSuffix) {
    resolved.node_name = std::string(target.substr(0, target.size() - kMonitorSuffix.size()));
    resolved.is_sink_capture = true;
    return resolved;
  }

  // 5. Default: regular source
  resolved.node_name = std::string(target);
  resolved.is_sink_capture = false;
  return resolved;
}

} // namespace vinput::pw
