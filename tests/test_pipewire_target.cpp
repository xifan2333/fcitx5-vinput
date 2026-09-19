#include <cassert>
#include <iostream>

#include "common/audio/pipewire_device.h"

int main() {
  using vinput::pw::ResolveCaptureTarget;

  {
    const auto res = ResolveCaptureTarget("");
    assert(res.node_name.empty());
    assert(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("default");
    assert(res.node_name.empty());
    assert(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("alsa_input.pci-0000_00_1b.0.analog-stereo");
    assert(res.node_name == "alsa_input.pci-0000_00_1b.0.analog-stereo");
    assert(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("probe.monitor");
    assert(res.node_name == "probe");
    assert(res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget(
        "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo.monitor");
    assert(res.node_name == "alsa_output.usb-Generic_HP_DHE-8008U_20210726905926-00.analog-stereo");
    assert(res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget(".monitor");
    assert(res.node_name == ".monitor");
    assert(!res.is_sink_capture);
  }

  {
    const auto res = ResolveCaptureTarget("monitor");
    assert(res.node_name == "monitor");
    assert(!res.is_sink_capture);
  }

  std::cout << "All pipewire capture target tests passed!" << std::endl;
  return 0;
}
