#include <cassert>
#include <cstdio>
#include <fcitx-config/iniparser.h>
#include <fstream>
#include <iostream>
#include <string>

#include "common/config/vinput_config.h"

int main() {
  VinputConfig config;
  assert(config.holdActivationDelay.value() == 300);
  assert(config.triggerMode.value() == TriggerMode::Both);
  assert(config.holdActivationDelay.constrain().min() == 100);
  assert(config.holdActivationDelay.constrain().max() == 2000);

  const std::string tmp_path = "/tmp/vinput_test_config.conf";
  {
    std::ofstream ofs(tmp_path);
    ofs << "[Config]\n"
        << "TriggerMode=Hold\n"
        << "HoldActivationDelay=500\n";
  }

  fcitx::readAsIni(config, tmp_path);
  std::remove(tmp_path.c_str());

  assert(config.holdActivationDelay.value() == 500);
  assert(config.triggerMode.value() == TriggerMode::Hold);

  std::cout << "All hotkey config tests passed!\n";
  return 0;
}
