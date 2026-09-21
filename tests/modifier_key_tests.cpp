#include <chrono>
#include <cstdlib>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#include "core/vinput.h"

namespace {

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "TEST FAILED: " << message << '\n';
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

struct ScopedConfigDir {
  std::filesystem::path dir;
  ScopedConfigDir() {
    char tpl[] = "/tmp/vinput_test_cfg_XXXXXX";
    char* const res = mkdtemp(tpl);
    if (res != nullptr) {
      dir = res;
      setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    }
  }
  ~ScopedConfigDir() {
    if (!dir.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  }
  ScopedConfigDir(const ScopedConfigDir&) = delete;
  ScopedConfigDir& operator=(const ScopedConfigDir&) = delete;
  ScopedConfigDir(ScopedConfigDir&&) = delete;
  ScopedConfigDir& operator=(ScopedConfigDir&&) = delete;
};

void runAllTests() {
  const ScopedConfigDir sandbox;

  char arg0[] = "vinput-modifier-test";
  char arg1[] = "--disable=all";
  char* argv[] = {arg0, arg1, nullptr};

  fcitx::Instance instance(2, argv);
  TestInputContext ic(instance);
  VinputEngine engine(&instance);

  fcitx::RawConfig test_config;
  test_config.setValueByPath("TriggerKey/0", "Alt_R");
  test_config.setValueByPath("TriggerKey/1", "F8");
  test_config.setValueByPath("CommandKeys/0", "Control_R");
  test_config.setValueByPath("MenuKey/0", "Shift_R");
  engine.setConfig(test_config);

  std::cout << "--- 1. Testing Single-Modifier Tap (Start & Stop) ---\n";
  // Press Alt_R -> arms pending timer, exclusively consumed
  fcitx::KeyEvent press_alt(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(press_alt);
  expect(press_alt.filtered() && press_alt.accepted(),
         "Trigger press must be exclusively consumed");
  expect(engine.isPendingStart(), "Pressing trigger should arm pending start timer");

  // Release Alt_R quickly (< 300ms) -> Clean Tap triggers voice recording!
  fcitx::KeyEvent release_alt(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(release_alt);
  expect(release_alt.filtered() && release_alt.accepted(),
         "Trigger release must be exclusively consumed");
  expect(!engine.isPendingStart(), "Pending start timer should be cleared on tap release");

  std::cout << "--- 2. Testing Alt_R + T Shortcut Combination (Chord Interruption) ---\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // Press Alt_R
  fcitx::KeyEvent chord_alt_p(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(chord_alt_p);
  expect(chord_alt_p.filtered() && chord_alt_p.accepted(), "Alt_R press is consumed");
  expect(engine.isPendingStart(), "Pending start timer armed");

  // While Alt_R is held, user presses 'T' (e.g. Firefox Tools menu shortcut)
  fcitx::KeyEvent chord_t_p(&ic, fcitx::Key(FcitxKey_t, fcitx::KeyState::Alt), false);
  engine.handleKeyEvent(chord_t_p);
  expect(!chord_t_p.filtered() && !chord_t_p.accepted(),
         "T key must pass untouched to application");
  expect(!engine.isPendingStart(), "Intervening non-trigger key must cancel pending start timer");

  // User releases 'T'
  fcitx::KeyEvent chord_t_r(&ic, fcitx::Key(FcitxKey_t, fcitx::KeyState::Alt), true);
  engine.handleKeyEvent(chord_t_r);
  expect(!chord_t_r.filtered() && !chord_t_r.accepted(), "T release passes untouched");

  // User releases Alt_R -> Because it was interrupted, it must NEVER trigger voice recording!
  fcitx::KeyEvent chord_alt_r(&ic, fcitx::Key(FcitxKey_Alt_R), true);
  engine.handleKeyEvent(chord_alt_r);
  expect(chord_alt_r.filtered() && chord_alt_r.accepted(),
         "Interrupted Alt_R release is consumed without trigger");
  expect(!engine.isRecordingActive(), "Chord release must not start recording");

  std::cout << "--- 3. Testing Non-Trigger Regular Keys (Alt_L + T) Pass-through ---\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // Left Alt (not in TriggerKey list) must pass through untouched
  fcitx::KeyEvent left_alt_p(&ic, fcitx::Key(FcitxKey_Alt_L), false);
  engine.handleKeyEvent(left_alt_p);
  expect(!left_alt_p.filtered() && !left_alt_p.accepted(), "Left Alt must pass through to app");

  fcitx::KeyEvent left_alt_r(&ic, fcitx::Key(FcitxKey_Alt_L), true);
  engine.handleKeyEvent(left_alt_r);
  expect(!left_alt_r.filtered() && !left_alt_r.accepted(),
         "Left Alt release must pass through to app");

  std::cout << "--- 4. Testing Multi-Trigger Isolation ---\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // Press Trigger 1 (Alt_R)
  fcitx::KeyEvent multi_alt_p(&ic, fcitx::Key(FcitxKey_Alt_R), false);
  engine.handleKeyEvent(multi_alt_p);

  // Pressing Trigger 2 (F8) cancels Trigger 1 without ambiguity
  fcitx::KeyEvent multi_f8_p(&ic, fcitx::Key(FcitxKey_F8), false);
  engine.handleKeyEvent(multi_f8_p);
  expect(multi_f8_p.filtered() && multi_f8_p.accepted(), "F8 press is consumed");

  // Releasing F8
  fcitx::KeyEvent multi_f8_r(&ic, fcitx::Key(FcitxKey_F8), true);
  engine.handleKeyEvent(multi_f8_r);
  expect(multi_f8_r.filtered() && multi_f8_r.accepted(), "F8 release is consumed");

  std::cout << "\n✅ ALL TRIGGER TESTS PASSED CLEANLY!\n";
}

} // namespace

int main() {
  runAllTests();
  return 0;
}
