#include <cassert>
#include <fcitx-config/iniparser.h>
#include <iostream>
#include <sstream>

#include "common/config/vinput_config.h"

int main() {
  VinputConfig config;
  assert(config.holdActivationDelay.value() == 300);
  assert(config.triggerMode.value() == TriggerMode::Both);

  std::stringstream ss("TriggerMode=Hold\n"
                       "HoldActivationDelay=500\n");
  fcitx::readAsIni(config, ss);
  assert(config.holdActivationDelay.value() == 500);
  assert(config.triggerMode.value() == TriggerMode::Hold);

  std::cout << "All hotkey config tests passed!" << std::endl;
  return 0;
}
