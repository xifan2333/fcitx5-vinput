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
    return 1;                                                                                      \
  }

} // namespace

int main() {
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

  // Regression tests for Issue #232: real Audio/Source named with .monitor suffix.
  {
    std::vector<vinput::pw::DeviceInfo> known_devices;
    known_devices.push_back({
        .id = 100,
        .name = "clean.monitor",
        .description = "Remapped source",
        .is_sink_monitor = false,
    });
    known_devices.push_back({
        .id = 101,
        .name = "probe.monitor",
        .description = "Null sink (Monitor)",
        .is_sink_monitor = true,
    });
    known_devices.push_back({
        .id = 102,
        .name = "alsa_input.pci-0000_00_1b.0.analog-stereo",
        .description = "Built-in microphone",
        .is_sink_monitor = false,
    });

    // 1. Real Audio/Source named clean.monitor must NOT have its suffix stripped or be marked sink.
    const auto res_source = ResolveCaptureTarget("clean.monitor", known_devices);
    CHECK(res_source.node_name == "clean.monitor");
    CHECK(!res_source.is_sink_capture);

    // 2. Real Audio/Sink monitor must have suffix stripped and be marked sink.
    const auto res_sink = ResolveCaptureTarget("probe.monitor", known_devices);
    CHECK(res_sink.node_name == "probe");
    CHECK(res_sink.is_sink_capture);

    // 3. Regular source must be preserved.
    const auto res_mic =
        ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo", known_devices);
    CHECK(res_mic.node_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
    CHECK(!res_mic.is_sink_capture);

    // 4. Unknown/offline device ending in .monitor falls back to stripping.
    const auto res_fallback = ResolveCaptureTarget("offline.monitor", known_devices);
    CHECK(res_fallback.node_name == "offline");
    CHECK(res_fallback.is_sink_capture);
  }

  std::cout << "All pipewire capture target tests passed!\n";
  return 0;
}
