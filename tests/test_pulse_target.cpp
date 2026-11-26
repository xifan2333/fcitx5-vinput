#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <pulse/def.h>
#include <string>
#include <string_view>
#include <vector>

#include "common/audio/pulse_device.h"

namespace {

class ScopedEnvVar {
public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    const char* prev = std::getenv(name);
    if (prev != nullptr) {
      old_value_ = std::string(prev);
    }
    if (value != nullptr) {
      (void)::setenv(name, value, 1);
    } else {
      (void)::unsetenv(name);
    }
  }

  ~ScopedEnvVar() {
    if (old_value_.has_value()) {
      (void)::setenv(name_.c_str(), old_value_->c_str(), 1);
    } else {
      (void)::unsetenv(name_.c_str());
    }
  }

  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
  ScopedEnvVar(ScopedEnvVar&&) = delete;
  ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
  std::string name_;
  std::optional<std::string> old_value_;
};

bool Check(bool cond, std::string_view expr, const char* file, int line) {
  if (!cond) {
    std::cerr << "Test failure: " << expr << " at " << file << ":" << line << '\n';
    return false;
  }
  return true;
}

#define CHECK(expr)                                                                                \
  if (!Check((expr), #expr, __FILE__, __LINE__)) {                                                 \
    return false;                                                                                  \
  }

bool TestBasicTargets() {
  using vinput::pulse::ResolveCaptureTarget;

  {
    const auto res = ResolveCaptureTarget("");
    CHECK(res.source_name.empty());
    CHECK(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("default");
    CHECK(res.source_name.empty());
    CHECK(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("  @DEFAULT_SOURCE@  ");
    CHECK(res.source_name.empty());
    CHECK(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo");
    CHECK(res.source_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
    CHECK(!res.is_sink_monitor);
  }

  // PulseAudio / pipewire-pulse exposes monitor sources directly as "<sink>.monitor".
  {
    const auto res = ResolveCaptureTarget("probe.monitor");
    CHECK(res.source_name == "probe.monitor");
    CHECK(res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget(
        "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    CHECK(res.source_name ==
          "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    CHECK(res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget(".monitor");
    CHECK(res.source_name == ".monitor");
    CHECK(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("monitor");
    CHECK(res.source_name == "monitor");
    CHECK(!res.is_sink_monitor);
  }

  return true;
}

// Regression tests for Issue #232 & #701: disambiguate real sources named "*.monitor" from
// genuine sink monitors via known_devices metadata while keeping the exact Pulse source name.
bool TestSourceMonitorDisambiguation() {
  using vinput::pulse::DeviceInfo;
  using vinput::pulse::ResolveCaptureTarget;

  const std::vector<DeviceInfo> known_devices = {
      {
          .id = 100,
          .name = "clean.monitor",
          .description = "Remapped microphone source",
          .is_sink_monitor = false,
      },
      {
          .id = 101,
          .name = "probe.monitor",
          .description = "Monitor of Null Output",
          .is_sink_monitor = true,
      },
      {
          .id = 102,
          .name = "alsa_input.pci-0000_00_1b.0.analog-stereo",
          .description = "Built-in microphone",
          .is_sink_monitor = false,
      },
      {
          .id = 103,
          .name = "loopback_capture_source",
          .description = "Custom monitor without suffix",
          .is_sink_monitor = true,
      },
  };

  const auto res_source = ResolveCaptureTarget("clean.monitor", known_devices);
  CHECK(res_source.source_name == "clean.monitor");
  CHECK(!res_source.is_sink_monitor);

  const auto res_sink = ResolveCaptureTarget("probe.monitor", known_devices);
  CHECK(res_sink.source_name == "probe.monitor");
  CHECK(res_sink.is_sink_monitor);

  const auto res_mic =
      ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo", known_devices);
  CHECK(res_mic.source_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
  CHECK(!res_mic.is_sink_monitor);

  const auto res_custom_mon = ResolveCaptureTarget("loopback_capture_source", known_devices);
  CHECK(res_custom_mon.source_name == "loopback_capture_source");
  CHECK(res_custom_mon.is_sink_monitor);

  const auto res_fallback = ResolveCaptureTarget("offline.monitor", known_devices);
  CHECK(res_fallback.source_name == "offline.monitor");
  CHECK(res_fallback.is_sink_monitor);

  return true;
}

bool TestDefaultSourceResolution() {
  using vinput::pulse::DeviceInfo;
  using vinput::pulse::ResolveCaptureTarget;

  const std::vector<DeviceInfo> known_devices = {
      {
          .id = 38,
          .name = "rnnoise_source",
          .description = "Noise Canceling source",
          .is_sink_monitor = false,
      },
      {
          .id = 69,
          .name = "alsa_output.usb.monitor",
          .description = "USB Sink (Monitor)",
          .is_sink_monitor = true,
      },
      {
          .id = 70,
          .name = "alsa_input.usb",
          .description = "USB Mic",
          .is_sink_monitor = false,
      },
  };

  const auto live_default = ResolveCaptureTarget("default", known_devices, "alsa_input.usb");
  CHECK(live_default.source_name == "alsa_input.usb");
  CHECK(!live_default.is_sink_monitor);

  const auto monitor_default = ResolveCaptureTarget("", known_devices, "alsa_output.usb.monitor");
  CHECK(monitor_default.source_name == "alsa_output.usb.monitor");
  CHECK(monitor_default.is_sink_monitor);

  const auto explicit_override =
      ResolveCaptureTarget("rnnoise_source", known_devices, "alsa_input.usb");
  CHECK(explicit_override.source_name == "rnnoise_source");
  CHECK(!explicit_override.is_sink_monitor);

  const auto alias_default = ResolveCaptureTarget("@DEFAULT_SOURCE@", known_devices, "default");
  CHECK(alias_default.source_name.empty());
  CHECK(!alias_default.is_sink_monitor);

  return true;
}

bool TestFormatConnectionError() {
  using vinput::pulse::FormatConnectionError;

  const std::string refused = FormatConnectionError(PA_ERR_CONNECTIONREFUSED);
  CHECK(refused.find("pipewire-pulse") != std::string::npos);

  const std::string generic = FormatConnectionError(0);
  CHECK(generic.find("connection failed") != std::string::npos);
  CHECK(generic.find("pipewire-pulse") != std::string::npos);

  return true;
}

// Regression test for Issue #301 & #596: hermetic RAII lifecycle when sound server is unreachable,
// plus zero-latency instant default target resolution.
bool TestHermeticEnumerationAndInstantDefault() {
  const ScopedEnvVar pulse_env("PULSE_SERVER", "unix:/nonexistent/vinput-pulse-test-sock");

  const auto snapshot = vinput::pulse::QueryAudioServerSnapshot(80);
  CHECK(!snapshot.connected);
  CHECK(snapshot.sources.empty());

  const auto sources = vinput::pulse::EnumerateAudioSources(80);
  CHECK(sources.empty());

  const auto instant = vinput::pulse::ResolveCaptureTarget("default");
  CHECK(instant.source_name.empty());
  CHECK(!instant.is_sink_monitor);

  return true;
}

#undef CHECK

} // namespace

int main() {
  if (!TestBasicTargets() || !TestSourceMonitorDisambiguation() || !TestDefaultSourceResolution() ||
      !TestFormatConnectionError() || !TestHermeticEnumerationAndInstantDefault()) {
    return 1;
  }

  std::cout << "All pulse capture target tests passed!\n";
  return 0;
}
