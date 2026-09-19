#include <iostream>

#include "common/audio/pipewire_device.h"

#define TEST_CHECK(cond)                                                                           \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "Test failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n';            \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

int main() {
  using vinput::pw::ResolveCaptureTarget;

  {
    const auto res = ResolveCaptureTarget("");
    TEST_CHECK(res.node_name.empty());
    TEST_CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("default");
    TEST_CHECK(res.node_name.empty());
    TEST_CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo");
    TEST_CHECK(res.node_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
    TEST_CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("probe.monitor");
    TEST_CHECK(res.node_name == "probe");
    TEST_CHECK(res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget(
        "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    TEST_CHECK(res.node_name ==
               "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo");
    TEST_CHECK(res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget(".monitor");
    TEST_CHECK(res.node_name == ".monitor");
    TEST_CHECK(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("monitor");
    TEST_CHECK(res.node_name == "monitor");
    TEST_CHECK(!res.is_sink_capture);
  }

  std::cout << "All pipewire capture target tests passed!\n";
  return 0;
}
