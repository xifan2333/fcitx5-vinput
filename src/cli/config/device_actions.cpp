#include "cli/config/device_actions.h"

#include <nlohmann/json.hpp>
#include <vector>

#include "common/audio/pipewire_device.h"
#include "common/config/core_config.h"
#include "common/i18n.h"
#include "common/utils/string_utils.h"

#include "cli/utils/cli_helpers.h"

int RunDeviceConfigList(Formatter& fmt, const CliContext& ctx) {
  CoreConfig config = LoadCoreConfig();
  std::string active_device = config.global.captureDevice;

  auto devices = vinput::pw::EnumerateAudioSources();
  if (devices.empty()) {
    fmt.PrintInfo(_("No audio capture devices found or PipeWire is not running."));
    return 0;
  }

  if (ctx.json_output) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& d : devices) {
      bool is_active =
          (d.name == active_device) || (active_device == "default" && d.name == "default");
      arr.push_back(
          {{"id", d.id}, {"name", d.name}, {"description", d.description}, {"active", is_active}});
    }
    fmt.PrintJson(arr);
    return 0;
  }

  std::vector<std::string> headers = {_("NAME"), _("DESCRIPTION"), _("STATUS")};
  std::vector<std::vector<std::string>> rows;

  {
    std::string status = (active_device == "default" || active_device.empty())
                             ? std::string("[*] ") + _("Active")
                             : "[ ]";
    rows.push_back({"default", _("System Default"), status});
  }

  for (const auto& d : devices) {
    std::string status = (active_device == d.name) ? std::string("[*] ") + _("Active") : "[ ]";
    rows.push_back({d.name, d.description, status});
  }

  fmt.PrintTable(headers, rows);
  return 0;
}

int RunDeviceConfigUse(const std::string& name, Formatter& fmt, const CliContext& ctx) {
  (void)ctx;
  CoreConfig config = LoadCoreConfig();

  std::string target_to_save = vinput::str::TrimAsciiWhitespace(name);
  if (target_to_save != "default") {
    auto devices = vinput::pw::EnumerateAudioSources();
    bool found = false;
    for (const auto& d : devices) {
      if (d.name == target_to_save) {
        found = true;
        break;
      }
    }
    if (!found) {
      fmt.PrintWarning(
          vinput::str::FmtStr(_("Device '%s' not found in PipeWire. Setting it anyway."), name));
    }
  }

  config.global.captureDevice = target_to_save;

  if (!SaveConfigOrFail(config, fmt))
    return 1;

  fmt.PrintSuccess(vinput::str::FmtStr(
      _("Capture device set to '%s'. Restart daemon to apply changes."), target_to_save));
  return 0;
}
