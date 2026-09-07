#include <cassert>
#include <cstdio>
#include <fcitx-config/iniparser.h>
#include <fstream>
#include <iostream>
#include <string>

#include "common/config/vinput_config.h"

int main() {
  const VinputConfig default_config;
  assert(default_config.holdActivationDelay.value() == 300);
  assert(default_config.triggerMode.value() == TriggerMode::Both);

  const std::string tmp_path = "/tmp/vinput_test_config.conf";
  {
    std::ofstream ofs(tmp_path);
    ofs << "TriggerMode=Hold\n"
        << "HoldActivationDelay=500\n";
  }

  VinputConfig custom_config;
  fcitx::readAsIni(custom_config, tmp_path);
  std::remove(tmp_path.c_str());

  assert(custom_config.holdActivationDelay.value() == 500);
  assert(custom_config.triggerMode.value() == TriggerMode::Hold);

  std::cout << "All hotkey config tests passed!\n";
  return 0;
}
