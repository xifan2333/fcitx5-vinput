#include <cstdlib>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>
#include <filesystem>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <unistd.h>

#include "core/vinput.h"

namespace {

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

class TestInputContext : public fcitx::InputContext {
public:
  explicit TestInputContext(fcitx::Instance& instance)
      : fcitx::InputContext(instance.inputContextManager()) {
    created();
  }
  TestInputContext(const TestInputContext&) = delete;
  TestInputContext& operator=(const TestInputContext&) = delete;
  TestInputContext(TestInputContext&&) = delete;
  TestInputContext& operator=(TestInputContext&&) = delete;
  ~TestInputContext() override { destroy(); }
  [[nodiscard]] const char* frontend() const override { return "test"; }

protected:
  void commitStringImpl(const std::string&) override {}
  void deleteSurroundingTextImpl(int, unsigned int) override {}
  void forwardKeyImpl(const fcitx::ForwardKeyEvent&) override {}
  void updatePreeditImpl() override {}
};

struct TestEnvironment {
  std::filesystem::path directory;
  TestEnvironment() {
    char pattern[] = "/tmp/vinput-test-env-XXXXXX";
    char* path = mkdtemp(pattern);
    expect(path != nullptr, "failed to create temporary environment directory");
    directory = path;
    for (const auto* var :
         {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
      const auto p = directory / var;
      std::filesystem::create_directory(p);
      setenv(var, p.c_str(), 1);
    }
  }
  TestEnvironment(const TestEnvironment&) = delete;
  TestEnvironment& operator=(const TestEnvironment&) = delete;
  TestEnvironment(TestEnvironment&&) = delete;
  TestEnvironment& operator=(TestEnvironment&&) = delete;
  ~TestEnvironment() { std::filesystem::remove_all(directory); }
};

void testTapActivation(VinputEngine& engine, TestInputContext& ic) {
  // 1. Press Alt_R (default trigger key)
  fcitx::KeyEvent press_alt(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(press_alt);
  expect(press_alt.filtered() && !press_alt.accepted(),
         "Alt_R press should be filtered and passed through to app");

  // 2. Release Alt_R in < 300ms (Tap)
  fcitx::KeyEvent release_alt(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(release_alt);
  expect(release_alt.filtered() && release_alt.accepted(),
         "Alt_R release should be consumed (filterAndAccept) to protect Firefox menus");
}

void testComboInterruption(VinputEngine& engine, TestInputContext& ic) {
  // 1. Press Alt_R
  fcitx::KeyEvent press_alt(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(press_alt);
  expect(press_alt.filtered() && !press_alt.accepted(), "Alt_R press is passed through");

  // 2. Press Tab while Alt is held (simulating Alt+Tab)
  fcitx::KeyEvent press_tab(&ic, fcitx::Key(FcitxKey_Tab, fcitx::KeyState::Alt), false);
  engine.handleKeyEvent(press_tab);
  expect(!press_tab.accepted() && !press_tab.filtered(),
         "Tab key must NOT be consumed or filtered by vinput; must reach application");

  // 3. Release Alt_R
  fcitx::KeyEvent release_alt(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(release_alt);
  expect(!release_alt.accepted(),
         "Interrupted Alt_R release must not be accepted as a voice trigger");
}

void testCtrlCInterruption(VinputEngine& engine, TestInputContext& ic) {
  // 1. Press Control_R (command mode trigger)
  fcitx::KeyEvent press_ctrl(&ic, fcitx::Key(FcitxKey_Control_R), false);
  engine.handleKeyEvent(press_ctrl);
  expect(press_ctrl.filtered() && !press_ctrl.accepted(), "Control_R press is passed through");

  // 2. Press 'c' with Ctrl modifier (Ctrl+C copy)
  fcitx::KeyEvent press_c(&ic, fcitx::Key(FcitxKey_c, fcitx::KeyState::Ctrl), false);
  engine.handleKeyEvent(press_c);
  expect(!press_c.accepted() && !press_c.filtered(),
         "Ctrl+C must NOT be intercepted; must reach application");

  // 3. Release Control_R
  fcitx::KeyEvent release_ctrl(&ic, fcitx::Key(FcitxKey_Control_R), true);
  engine.handleKeyEvent(release_ctrl);
  expect(!release_ctrl.accepted(),
         "Interrupted Control_R release must not trigger command recording");
}

void testShiftAInterruption(VinputEngine& engine, TestInputContext& ic) {
  // 1. Press Shift_R (menu palette trigger)
  fcitx::KeyEvent press_shift(&ic, fcitx::Key(FcitxKey_Shift_R), false);
  engine.handleKeyEvent(press_shift);
  expect(press_shift.filtered() && !press_shift.accepted(), "Shift_R press is passed through");

  // 2. Press 'A' with Shift modifier (capital letter typing)
  fcitx::KeyEvent press_a(&ic, fcitx::Key(FcitxKey_A, fcitx::KeyState::Shift), false);
  engine.handleKeyEvent(press_a);
  expect(!press_a.accepted() && !press_a.filtered(),
         "Shift+A typing must NOT be intercepted; must reach application");

  // 3. Release Shift_R
  fcitx::KeyEvent release_shift(&ic, fcitx::Key(FcitxKey_Shift_R), true);
  engine.handleKeyEvent(release_shift);
  expect(!release_shift.accepted(), "Interrupted Shift_R release must not pop up command palette");
}

} // namespace

int main() {
  const TestEnvironment env;
  char arg0[] = "vinput-modifier-test";
  char arg1[] = "--disable=all";
  char* argv[] = {arg0, arg1, nullptr};

  fcitx::Instance instance(2, argv);
  TestInputContext ic(instance);
  VinputEngine engine(&instance);

  testTapActivation(engine, ic);
  testComboInterruption(engine, ic);
  testCtrlCInterruption(engine, ic);
  testShiftAInterruption(engine, ic);

  std::cout << "All modifier key behavior tests passed!\n";
  return 0;
}
