#include "cli/command_device.h"
#include "cli/i18n.h"
#include "common/core_config.h"

#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>

#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace {

struct DeviceInfo {
  uint32_t id;
  std::string name;
  std::string description;
};

struct PwData {
  pw_main_loop *loop;
  pw_context *context;
  pw_core *core;
  pw_registry *registry;
  spa_hook registry_listener;
  spa_hook core_listener;
  int pending_sync;
  std::vector<DeviceInfo> devices;
};

void on_core_done(void *data, uint32_t id, int seq) {
  PwData *d = static_cast<PwData *>(data);
  if (id == PW_ID_CORE && d->pending_sync == seq) {
    pw_main_loop_quit(d->loop);
  }
}

const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    nullptr, // info
    on_core_done,
    nullptr, // ping
    nullptr, // error
    nullptr, // remove_id
    nullptr, // bound_id
    nullptr, // add_mem
    nullptr  // remove_mem
};

void registry_event_global(void *data, uint32_t id, uint32_t permissions,
                           const char *type, uint32_t version,
                           const struct spa_dict *props) {
  if (std::string(type) == PW_TYPE_INTERFACE_Node && props) {
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class && std::string(media_class) == "Audio/Source") {
      const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

      PwData *d = static_cast<PwData *>(data);
      DeviceInfo info;
      info.id = id;
      if (name)
        info.name = name;
      if (desc)
        info.description = desc;
      d->devices.push_back(info);
    }
  }
}

void registry_event_global_remove(void *data, uint32_t id) {
  // Ignored
}

const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    registry_event_global,
    registry_event_global_remove,
};

std::vector<DeviceInfo> EnumerateAudioSources() {
  pw_init(nullptr, nullptr);

  PwData data = {0};
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
  pw_registry_add_listener(data.registry, &data.registry_listener,
                           &registry_events, &data);

  data.pending_sync = pw_core_sync(data.core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(data.registry));
  pw_core_disconnect(data.core);
  pw_context_destroy(data.context);
  pw_main_loop_destroy(data.loop);
  pw_deinit();

  return data.devices;
}

} // namespace

int RunDeviceList(Formatter &fmt, const CliContext &ctx) {
  CoreConfig config = LoadCoreConfig();
  std::string active_device = config.captureDevice;

  auto devices = EnumerateAudioSources();
  if (devices.empty()) {
    fmt.PrintInfo("No audio capture devices found or PipeWire is not running.");
    return 0;
  }

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &d : devices) {
      bool is_active = (d.name == active_device) ||
                       (active_device == "default" && d.name == "default");
      arr.push_back({{"id", d.id},
                     {"name", d.name},
                     {"description", d.description},
                     {"active", is_active}});
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {"NAME", "DESCRIPTION", "STATUS"};
  std::vector<std::vector<std::string>> rows;

  {
    std::string status =
        (active_device == "default" || active_device.empty())
            ? std::string("[*] ") + _(msgs::kActive, ctx.use_chinese)
            : "[ ]";
    rows.push_back({"default", "System Default", status});
  }

  for (const auto &d : devices) {
    std::string status =
        (active_device == d.name)
            ? std::string("[*] ") + _(msgs::kActive, ctx.use_chinese)
            : "[ ]";
    rows.push_back({d.name, d.description, status});
  }

  fmt.PrintTable(headers, rows);
  return 0;
}

int RunDeviceUse(const std::string &name, Formatter &fmt,
                 const CliContext &ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  if (name != "default") {
    auto devices = EnumerateAudioSources();
    bool found = false;
    for (const auto &d : devices) {
      if (d.name == name) {
        found = true;
        break;
      }
    }
    if (!found) {
      fmt.PrintWarning("Device '" + name +
                       "' not found in PipeWire. Setting it anyway.");
    }
  }

  config.captureDevice = name;

  if (!SaveCoreConfig(config)) {
    fmt.PrintError("Failed to save config.");
    return 1;
  }

  fmt.PrintSuccess("Capture device set to '" + name +
                   "'. Restart daemon to apply changes.");
  return 0;
}
