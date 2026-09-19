#include <iostream>
#include <string_view>

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

  std::cout << "All pipewire capture target tests passed!\n";
  return 0;
}
