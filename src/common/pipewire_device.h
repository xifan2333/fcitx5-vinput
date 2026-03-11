#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vinput::pw {

struct DeviceInfo {
  uint32_t id;
  std::string name;
  std::string description;
};

std::vector<DeviceInfo> EnumerateAudioSources();

} // namespace vinput::pw
