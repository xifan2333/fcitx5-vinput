#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdlib.h>
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

void AssertTrue(bool cond, const char* expr, const char* file, int line) {
  if (!cond) {
    std::fprintf(stderr, "Test failure: %s at %s:%d\n", expr, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(expr) AssertTrue((expr), #expr, __FILE__, __LINE__)

void TestBasicTargets() {
  using vinput::pulse::ResolveCaptureTarget;

  {
    const auto res = ResolveCaptureTarget("");
    TEST_ASSERT(res.source_name.empty());
    TEST_ASSERT(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("default");
    TEST_ASSERT(res.source_name.empty());
    TEST_ASSERT(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("  @DEFAULT_SOURCE@  ");
    TEST_ASSERT(res.source_name.empty());
    TEST_ASSERT(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo");
    TEST_ASSERT(res.source_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
    TEST_ASSERT(!res.is_sink_monitor);
  }

  // PulseAudio / pipewire-pulse exposes monitor sources directly as "<sink>.monitor".
  {
    const auto res = ResolveCaptureTarget("probe.monitor");
    TEST_ASSERT(res.source_name == "probe.monitor");
    TEST_ASSERT(res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget(
        "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    TEST_ASSERT(res.source_name ==
                "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    TEST_ASSERT(res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget(".monitor");
    TEST_ASSERT(res.source_name == ".monitor");
    TEST_ASSERT(!res.is_sink_monitor);
  }

  {
    const auto res = ResolveCaptureTarget("monitor");
    TEST_ASSERT(res.source_name == "monitor");
    TEST_ASSERT(!res.is_sink_monitor);
  }
}

// Regression tests for Issue #232 & #701: disambiguate real sources named "*.monitor" from
// genuine sink monitors via known_devices metadata while keeping the exact Pulse source name.
void TestSourceMonitorDisambiguation() {
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
  TEST_ASSERT(res_source.source_name == "clean.monitor");
  TEST_ASSERT(!res_source.is_sink_monitor);

  const auto res_sink = ResolveCaptureTarget("probe.monitor", known_devices);
  TEST_ASSERT(res_sink.source_name == "probe.monitor");
  TEST_ASSERT(res_sink.is_sink_monitor);

  const auto res_mic =
      ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo", known_devices);
  TEST_ASSERT(res_mic.source_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
  TEST_ASSERT(!res_mic.is_sink_monitor);

  const auto res_custom_mon = ResolveCaptureTarget("loopback_capture_source", known_devices);
  TEST_ASSERT(res_custom_mon.source_name == "loopback_capture_source");
  TEST_ASSERT(res_custom_mon.is_sink_monitor);

  const auto res_fallback = ResolveCaptureTarget("offline.monitor", known_devices);
  TEST_ASSERT(res_fallback.source_name == "offline.monitor");
  TEST_ASSERT(res_fallback.is_sink_monitor);
}

void TestDefaultSourceResolution() {
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
  TEST_ASSERT(live_default.source_name == "alsa_input.usb");
  TEST_ASSERT(!live_default.is_sink_monitor);

  const auto monitor_default = ResolveCaptureTarget("", known_devices, "alsa_output.usb.monitor");
  TEST_ASSERT(monitor_default.source_name == "alsa_output.usb.monitor");
  TEST_ASSERT(monitor_default.is_sink_monitor);

  const auto explicit_override =
      ResolveCaptureTarget("rnnoise_source", known_devices, "alsa_input.usb");
  TEST_ASSERT(explicit_override.source_name == "rnnoise_source");
  TEST_ASSERT(!explicit_override.is_sink_monitor);

  const auto alias_default = ResolveCaptureTarget("@DEFAULT_SOURCE@", known_devices, "default");
  TEST_ASSERT(alias_default.source_name.empty());
  TEST_ASSERT(!alias_default.is_sink_monitor);
}

void TestFormatConnectionError() {
  using vinput::pulse::FormatConnectionError;

  const std::string refused = FormatConnectionError(PA_ERR_CONNECTIONREFUSED);
  TEST_ASSERT(refused.find("pipewire-pulse") != std::string::npos);

  const std::string generic = FormatConnectionError(0);
  TEST_ASSERT(generic.find("connection failed") != std::string::npos);
  TEST_ASSERT(generic.find("pipewire-pulse") != std::string::npos);
}

// Regression test for Issue #301 & #596: hermetic RAII lifecycle when sound server is unreachable,
// plus zero-latency instant default target resolution.
void TestHermeticEnumerationAndInstantDefault() {
  const ScopedEnvVar pulse_env("PULSE_SERVER", "unix:/nonexistent/vinput-pulse-test-sock");

  const auto snapshot = vinput::pulse::QueryAudioServerSnapshot(80);
  TEST_ASSERT(!snapshot.connected);
  TEST_ASSERT(snapshot.sources.empty());

  const auto sources = vinput::pulse::EnumerateAudioSources(80);
  TEST_ASSERT(sources.empty());

  const auto instant = vinput::pulse::ResolveCaptureTarget("default");
  TEST_ASSERT(instant.source_name.empty());
  TEST_ASSERT(!instant.is_sink_monitor);
}

} // namespace

int main() {
  TestBasicTargets();
  TestSourceMonitorDisambiguation();
  TestDefaultSourceResolution();
  TestFormatConnectionError();
  TestHermeticEnumerationAndInstantDefault();
  std::printf("All pulse capture target tests passed!\n");
  return 0;
}
