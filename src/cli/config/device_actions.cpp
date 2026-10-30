#include "cli/config/device_actions.h"

#include <glaze/json/json_t.hpp>
#include <string>
#include <utility>
#include <vector>

#include "common/audio/pipewire_device.h"
#include "common/config/core_config.h"
#include "common/config/core_config_types.h"
#include "common/i18n.h"
#include "common/utils/string_utils.h"

#include "cli/utils/cli_context.h"
#include "cli/utils/cli_helpers.h"
#include "cli/utils/formatter.h"

namespace {

bool DeviceMatchesResolved(const vinput::pw::DeviceInfo& device,
                           const vinput::pw::ResolvedCaptureTarget& resolved) {
  if (resolved.node_name.empty()) {
    return false;
  }
  if (resolved.is_sink_capture) {
    return device.is_sink_monitor && device.name == resolved.node_name + ".monitor";
  }
  return !device.is_sink_monitor && device.name == resolved.node_name;
}

std::string ActiveMark(bool active) {
  return active ? std::string("[*] ") + _("Active") : "[ ]";
}

} // namespace

int RunDeviceConfigList(Formatter& fmt, const CliContext& ctx) {
  const CoreConfig config = LoadCoreConfig();
  const std::string active_device = config.global.captureDevice;
  const bool auto_active =
      active_device == vinput::pw::kAutoCaptureTarget ||
      (vinput::pw::IsCaptureTargetPattern(active_device) && active_device != "default");

  auto devices = vinput::pw::EnumerateAudioSources();
  if (devices.empty()) {
    fmt.PrintInfo(_("No audio capture devices found or PipeWire is not running."));
    return 0;
  }

  const auto resolved = vinput::pw::ResolveCaptureTarget(active_device, devices);

  if (ctx.json_output) {
    glz::json_t::array_t arr;
    arr.reserve(devices.size());
    for (const auto& d : devices) {
      const bool is_active = DeviceMatchesResolved(d, resolved);
      glz::json_t json_entry = {{"id", d.id},
                                {"name", d.name},
                                {"description", d.description},
                                {"active", is_active},
                                {"recording_keys", d.recording_keys}};
      arr.emplace_back(std::move(json_entry));
    }
    fmt.PrintJson(glz::json_t(std::move(arr)));
    return 0;
  }

  const std::vector<std::string> headers = {_("NAME"), _("DESCRIPTION"), _("STATUS")};
  std::vector<std::vector<std::string>> rows;

  rows.push_back({"default", _("System Default"),
                  ActiveMark(active_device.empty() || active_device == "default")});
  rows.push_back({"auto", _("First matching source (atvv-*, default)"), ActiveMark(auto_active)});

  for (const auto& d : devices) {
    rows.push_back({d.name, d.description, ActiveMark(DeviceMatchesResolved(d, resolved))});
  }

  fmt.PrintTable(headers, rows);
  return 0;
}

int RunDeviceConfigUse(const std::string& name, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  const std::string target_to_save(vinput::str::Trim(name));
  if (target_to_save.empty()) {
    fmt.PrintError(_("Capture device must not be empty."));
    return 1;
  }

  const auto devices = vinput::pw::EnumerateAudioSources();
  const vinput::pw::DeviceInfo* exact_device = nullptr;
  for (const auto& device : devices) {
    if (device.name == target_to_save) {
      exact_device = &device;
      break;
    }
  }
  const bool pattern_target =
      exact_device == nullptr && vinput::pw::IsCaptureTargetPattern(target_to_save);

  if (exact_device == nullptr && !pattern_target && target_to_save != "default") {
    fmt.PrintWarning(vinput::str::FmtStr(_("Device '%s' not found in PipeWire. Setting it anyway."),
                                         target_to_save));
  }

  config.global.captureDevice = target_to_save;

  if (!SaveConfigOrFail(config, fmt)) {
    return 1;
  }

  const auto resolved = vinput::pw::ResolveCaptureTarget(target_to_save, devices);
  if (target_to_save == "default") {
    fmt.PrintSuccess(_("Capture device set to 'default' (system default)."));
  } else if (resolved.node_name.empty()) {
    fmt.PrintSuccess(vinput::str::FmtStr(
        _("Capture device set to '%s'. No matching source; the system default will be used."),
        target_to_save));
  } else if (exact_device != nullptr) {
    fmt.PrintSuccess(vinput::str::FmtStr(
        _("Capture device set to '%s'. The next recording will use it."), target_to_save));
  } else if (pattern_target) {
    fmt.PrintSuccess(
        vinput::str::FmtStr(_("Capture device set to '%s'. The next recording will use '%s'."),
                            target_to_save, resolved.node_name));
  } else {
    fmt.PrintSuccess(vinput::str::FmtStr(
        _("Capture device set to '%s'. The next recording will use it when it appears."),
        target_to_save));
  }
  return 0;
}
