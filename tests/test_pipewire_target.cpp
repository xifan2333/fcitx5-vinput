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

  // Regression test for Issue #301: EnumerateAudioSources RAII initialization and teardown
  // lifecycle.
  {
    const auto sources = vinput::pw::EnumerateAudioSources();
    for (const auto& dev : sources) {
      CHECK(dev.id > 0);
    }
  }

  // Auto selection and priority lists.
  {
    std::vector<vinput::pw::DeviceInfo> devices;
    devices.push_back({
        .id = 200,
        .name = "alsa_input.usb",
        .description = "USB microphone",
        .is_sink_monitor = false,
    });
    devices.push_back({
        .id = 201,
        .name = "atvv-miremote-rc003",
        .description = "ATVV source",
        .is_sink_monitor = false,
    });
    devices.push_back({
        .id = 202,
        .name = "probe.monitor",
        .description = "Null sink (Monitor)",
        .is_sink_monitor = true,
    });

    const auto auto_res = ResolveCaptureTarget("auto", devices);
    CHECK(auto_res.node_name == "atvv-miremote-rc003");
    CHECK(!auto_res.is_sink_capture);

    const auto auto_missing =
        ResolveCaptureTarget("auto", std::vector<vinput::pw::DeviceInfo>{devices[0]});
    CHECK(auto_missing.node_name.empty());
    CHECK(!auto_missing.is_sink_capture);

    const auto priority = ResolveCaptureTarget("atvv-*,alsa_input.*", devices);
    CHECK(priority.node_name == "atvv-miremote-rc003");
    CHECK(!priority.is_sink_capture);

    const auto fallback = ResolveCaptureTarget("missing-*,alsa_input.*", devices);
    CHECK(fallback.node_name == "alsa_input.usb");
    CHECK(!fallback.is_sink_capture);

    const auto no_match = ResolveCaptureTarget("missing-*", devices);
    CHECK(no_match.node_name.empty());
    CHECK(!no_match.is_sink_capture);

    const auto default_first = ResolveCaptureTarget("default,atvv-*", devices);
    CHECK(default_first.node_name.empty());
    CHECK(!default_first.is_sink_capture);

    const auto wildcard_sink = ResolveCaptureTarget("probe.*", devices);
    CHECK(wildcard_sink.node_name == "probe");
    CHECK(wildcard_sink.is_sink_capture);

    const auto wildcard_any = ResolveCaptureTarget("*", devices);
    CHECK(wildcard_any.node_name == "alsa_input.usb");
    CHECK(!wildcard_any.is_sink_capture);

    std::vector<vinput::pw::DeviceInfo> comma_device;
    comma_device.push_back({
        .id = 203,
        .name = "weird,name",
        .description = "Source with a comma",
        .is_sink_monitor = false,
    });
    const auto exact_with_comma = ResolveCaptureTarget("weird,name", comma_device);
    CHECK(exact_with_comma.node_name == "weird,name");
    CHECK(!exact_with_comma.is_sink_capture);

    CHECK(vinput::pw::IsCaptureTargetPattern("auto"));
    CHECK(vinput::pw::IsCaptureTargetPattern("atvv-*"));
    CHECK(vinput::pw::IsCaptureTargetPattern("a,b"));
    CHECK(!vinput::pw::IsCaptureTargetPattern("default"));
    CHECK(!vinput::pw::IsCaptureTargetPattern("alsa_input.usb"));
  }

  std::cout << "All pipewire capture target tests passed!\n";
  return 0;
}
