#include <algorithm>
#include <iostream>
#include <string_view>
#include <vector>

#include "common/audio/pipewire_device.h"

namespace {

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
  using vinput::pw::ResolveCaptureTarget;

  {
    const auto res = ResolveCaptureTarget("");
    CHECK(res.node_name.empty());
    CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("default");
    CHECK(res.node_name.empty());
    CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo");
    CHECK(res.node_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
    CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("probe.monitor");
    CHECK(res.node_name == "probe");
    CHECK(res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget(
        "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    CHECK(res.node_name == "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo");
    CHECK(res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget(".monitor");
    CHECK(res.node_name == ".monitor");
    CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("monitor");
    CHECK(res.node_name == "monitor");
    CHECK(!res.is_sink_capture);
  }

  return true;
}

// Regression tests for Issue #232 & #701: real Audio/Source named with .monitor suffix and
// propagation of cached object_serial.
bool TestSourceMonitorDisambiguation() {
  using vinput::pw::ResolveCaptureTarget;

  std::vector<vinput::pw::DeviceInfo> known_devices;
  known_devices.push_back({
      .id = 100,
      .name = "clean.monitor",
      .description = "Remapped source",
      .is_sink_monitor = false,
      .object_serial = "200",
  });
  known_devices.push_back({
      .id = 101,
      .name = "probe.monitor",
      .description = "Null sink (Monitor)",
      .is_sink_monitor = true,
      .object_serial = "201",
  });
  known_devices.push_back({
      .id = 102,
      .name = "alsa_input.pci-0000_00_1b.0.analog-stereo",
      .description = "Built-in microphone",
      .is_sink_monitor = false,
      .object_serial = "202",
  });

  // 1. Real Audio/Source named clean.monitor must NOT have its suffix stripped or be marked sink.
  const auto res_source = ResolveCaptureTarget("clean.monitor", known_devices);
  CHECK(res_source.node_name == "clean.monitor");
  CHECK(!res_source.is_sink_capture);
  CHECK(res_source.object_serial == "200");

  // 2. Real Audio/Sink monitor must have suffix stripped, be marked sink, and carry sink serial.
  const auto res_sink = ResolveCaptureTarget("probe.monitor", known_devices);
  CHECK(res_sink.node_name == "probe");
  CHECK(res_sink.is_sink_capture);
  CHECK(res_sink.object_serial == "201");

  // 3. Regular source must be preserved with its serial.
  const auto res_mic =
      ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo", known_devices);
  CHECK(res_mic.node_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
  CHECK(!res_mic.is_sink_capture);
  CHECK(res_mic.object_serial == "202");

  // 4. Unknown/offline device ending in .monitor falls back to stripping without serial.
  const auto res_fallback = ResolveCaptureTarget("offline.monitor", known_devices);
  CHECK(res_fallback.node_name == "offline");
  CHECK(res_fallback.is_sink_capture);
  CHECK(res_fallback.object_serial.empty());

  return true;
}

bool TestDefaultMetadataParsing() {
  using vinput::pw::ParseDefaultAudioSourceMetadata;

  CHECK(ParseDefaultAudioSourceMetadata(R"({"name":"rnnoise_source"})") == "rnnoise_source");
  CHECK(ParseDefaultAudioSourceMetadata(R"({"name":"alsa_input.usb","extra":1})") ==
        "alsa_input.usb");
  CHECK(ParseDefaultAudioSourceMetadata(R"({"name":123})").empty());
  CHECK(ParseDefaultAudioSourceMetadata(R"([])").empty());
  CHECK(ParseDefaultAudioSourceMetadata(R"({"other":"alsa_input.usb"})").empty());
  CHECK(ParseDefaultAudioSourceMetadata("").empty());
  CHECK(ParseDefaultAudioSourceMetadata("invalid-json").empty());

  return true;
}

bool TestLiveTargetResolution() {
  using vinput::pw::ResolveLiveCaptureTarget;

  const std::vector<vinput::pw::DeviceInfo> known_devices = {
      {
          .id = 38,
          .name = "rnnoise_source",
          .description = "Noise Canceling source",
          .is_sink_monitor = false,
          .object_serial = "38",
      },
      {
          .id = 69,
          .name = "alsa_output.usb.monitor",
          .description = "USB Sink (Monitor)",
          .is_sink_monitor = true,
          .object_serial = "70",
      },
      {
          .id = 70,
          .name = "alsa_input.usb",
          .description = "USB Mic",
          .is_sink_monitor = false,
          .object_serial = "71",
      },
  };

  // Default target resolves via live default.audio.source to its cached object.serial.
  const auto live_default = ResolveLiveCaptureTarget("default", known_devices, "alsa_input.usb");
  CHECK(live_default.node_name == "alsa_input.usb");
  CHECK(live_default.object_serial == "71");
  CHECK(!live_default.is_sink_capture);

  // Explicit config / Trigger target always overrides default.audio.source.
  const auto live_explicit =
      ResolveLiveCaptureTarget("rnnoise_source", known_devices, "alsa_input.usb");
  CHECK(live_explicit.node_name == "rnnoise_source");
  CHECK(live_explicit.object_serial == "38");
  CHECK(!live_explicit.is_sink_capture);

  const auto live_sink =
      ResolveLiveCaptureTarget("alsa_output.usb.monitor", known_devices, "alsa_input.usb");
  CHECK(live_sink.node_name == "alsa_output.usb");
  CHECK(live_sink.object_serial == "70");
  CHECK(live_sink.is_sink_capture);

  // Missing or uncached default source returns empty so PW_KEY_TARGET_OBJECT is omitted.
  const auto unknown_default = ResolveLiveCaptureTarget("", known_devices, "missing_source");
  CHECK(unknown_default.node_name.empty());
  CHECK(unknown_default.object_serial.empty());
  CHECK(!unknown_default.is_sink_capture);

  return true;
}

// Regression test for Issue #301: EnumerateAudioSources RAII initialization and teardown lifecycle.
bool TestEnumerateAudioSources() {
  const auto sources = vinput::pw::EnumerateAudioSources();
  CHECK(std::ranges::all_of(sources, [](const auto& dev) { return dev.id > 0; }));
  return true;
}

// Regression test for Issue #596: Default capture target must resolve instantly without
// requiring known_devices enumeration to avoid start latency.
bool TestDefaultTargetInstantResolution() {
  using vinput::pw::ResolveCaptureTarget;

  const auto res_empty = ResolveCaptureTarget("");
  CHECK(res_empty.node_name.empty());
  CHECK(!res_empty.is_sink_capture);

  const auto res_default = ResolveCaptureTarget("default");
  CHECK(res_default.node_name.empty());
  CHECK(!res_default.is_sink_capture);

  return true;
}

#undef CHECK

} // namespace

int main() {
  if (!TestBasicTargets() || !TestSourceMonitorDisambiguation() || !TestDefaultMetadataParsing() ||
      !TestLiveTargetResolution() || !TestEnumerateAudioSources() ||
      !TestDefaultTargetInstantResolution()) {
    return 1;
  }

  std::cout << "All pipewire capture target tests passed!\n";
  return 0;
}
