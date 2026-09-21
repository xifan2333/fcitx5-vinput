#include <cassert>
#include <cstdlib>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>
#include <iostream>
#include <string>

#define private public
#include "core/vinput.h"
#undef private

namespace {

class TestInputContext : public fcitx::InputContext {
public:
  explicit TestInputContext(fcitx::Instance& instance)
      : fcitx::InputContext(instance.inputContextManager()) {
    created();
  }
  ~TestInputContext() override { destroy(); }
  [[nodiscard]] const char* frontend() const override { return "test"; }

protected:
  void commitStringImpl(const std::string&) override {}
  void deleteSurroundingTextImpl(int, unsigned int) override {}
  void forwardKeyImpl(const fcitx::ForwardKeyEvent&) override {}
  void updatePreeditImpl() override {}
};

void testExclusiveTriggerConsumption() {
  char arg0[] = "vinput-modifier-test";
  char arg1[] = "--disable=all";
  char* argv[] = {arg0, arg1, nullptr};

  fcitx::Instance instance(2, argv);
  TestInputContext ic(instance);
  VinputEngine engine(&instance);

  fcitx::RawConfig test_config;
  test_config.setValueByPath("TriggerKey/0", "Alt_R");
  test_config.setValueByPath("TriggerMode", "Both");
  engine.setConfig(test_config);

  // 1. Trigger key (Alt_R) must be exclusively consumed on press and release
  fcitx::KeyEvent press_alt(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(press_alt);
  assert(press_alt.filtered() && press_alt.accepted());

  fcitx::KeyEvent release_alt(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(release_alt);
  assert(release_alt.filtered() && release_alt.accepted());

  // 2. Regular keys and non-trigger modifiers (Alt_L + T) must pass untouched to application
  fcitx::KeyEvent press_alt_l(&ic, fcitx::Key(FcitxKey_Alt_L), false);
  engine.handleKeyEvent(press_alt_l);
  assert(!press_alt_l.filtered() && !press_alt_l.accepted());

  fcitx::KeyEvent press_t(&ic, fcitx::Key(FcitxKey_t, fcitx::KeyState::Alt), false);
  engine.handleKeyEvent(press_t);
  assert(!press_t.filtered() && !press_t.accepted());

  fcitx::KeyEvent release_t(&ic, fcitx::Key(FcitxKey_t, fcitx::KeyState::Alt), true);
  engine.handleKeyEvent(release_t);
  assert(!release_t.filtered() && !release_t.accepted());

  fcitx::KeyEvent release_alt_l(&ic, fcitx::Key(FcitxKey_Alt_L), true);
  engine.handleKeyEvent(release_alt_l);
  assert(!release_alt_l.filtered() && !release_alt_l.accepted());

  // 3. Tap mode stop recording must cleanly consume both press and release
  test_config.setValueByPath("TriggerMode", "Tap");
  engine.setConfig(test_config);

  fcitx::KeyEvent tap_start_p(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(tap_start_p);
  assert(tap_start_p.filtered() && tap_start_p.accepted());

  fcitx::KeyEvent tap_start_r(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(tap_start_r);
  assert(tap_start_r.filtered() && tap_start_r.accepted());

  // Second tap to stop recording
  fcitx::KeyEvent tap_stop_p(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(tap_stop_p);
  assert(tap_stop_p.filtered() && tap_stop_p.accepted());

  fcitx::KeyEvent tap_stop_r(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(tap_stop_r);
  assert(tap_stop_r.filtered() && tap_stop_r.accepted());

  std::cout << "All exclusive trigger key tests passed successfully!\n";
}

} // namespace

int main() {
  testExclusiveTriggerConsumption();
  return 0;
}
