#include <chrono>
#include <exception>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/globalconfig.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

#include "core/vinput.h"

namespace {

void expect(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
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
    std::error_code ec;
    dir = std::filesystem::temp_directory_path() /
          ("vinput_cfg_test_" + std::to_string(static_cast<long>(getpid())));
    std::filesystem::create_directories(dir, ec);
  }
  ~ScopedConfigDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
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
  expect(engine.isPendingStart(), "Alt_R arms pending start timer");

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Pressing Trigger 2 (F8) cancels Trigger 1 without ambiguity
  fcitx::KeyEvent multi_f8_p(&ic, fcitx::Key(FcitxKey_F8), false);
  engine.handleKeyEvent(multi_f8_p);
  expect(multi_f8_p.filtered() && multi_f8_p.accepted(), "F8 press is consumed");

  // Releasing F8
  fcitx::KeyEvent multi_f8_r(&ic, fcitx::Key(FcitxKey_F8), true);
  engine.handleKeyEvent(multi_f8_r);
  expect(multi_f8_r.filtered() && multi_f8_r.accepted(), "F8 release is consumed");
  expect(!engine.isPendingStart(), "Pending start timer is cleared after F8 interaction");

  std::cout << "--- 5. Testing Menu Key (Shift_R) Release-Based Trigger & Combination ---\n";
  // 5a. Shift_R + A combination: Shift_R press is consumed, A interrupts and passes through,
  // Shift_R release is consumed without triggering palette
  fcitx::KeyEvent shift_p(&ic, fcitx::Key(FcitxKey_Shift_R), false);
  engine.handleKeyEvent(shift_p);
  expect(shift_p.filtered() && shift_p.accepted(), "Shift_R press is consumed");

  fcitx::KeyEvent key_a_p(&ic, fcitx::Key(FcitxKey_a), false);
  engine.handleKeyEvent(key_a_p);
  expect(!key_a_p.filtered() && !key_a_p.accepted(), "Key A passes through to client");

  fcitx::KeyEvent shift_r(&ic, fcitx::Key(FcitxKey_Shift_R), true);
  engine.handleKeyEvent(shift_r);
  expect(shift_r.filtered() && shift_r.accepted(),
         "Interrupted Shift_R release is consumed without triggering palette");

  // 5b. Solo Shift_R tap: press and release are both consumed, release toggles palette
  fcitx::KeyEvent solo_shift_p(&ic, fcitx::Key(FcitxKey_Shift_R), false);
  engine.handleKeyEvent(solo_shift_p);
  expect(solo_shift_p.filtered() && solo_shift_p.accepted(), "Solo Shift_R press is consumed");

  fcitx::KeyEvent solo_shift_r(&ic, fcitx::Key(FcitxKey_Shift_R), true);
  engine.handleKeyEvent(solo_shift_r);
  expect(solo_shift_r.filtered() && solo_shift_r.accepted(),
         "Solo Shift_R release is consumed and triggers palette");

  // 5c. Second solo Shift_R tap: press and release are consumed, toggles palette closed
  fcitx::KeyEvent close_shift_p(&ic, fcitx::Key(FcitxKey_Shift_R), false);
  engine.handleKeyEvent(close_shift_p);
  expect(close_shift_p.filtered() && close_shift_p.accepted(), "Close Shift_R press is consumed");

  fcitx::KeyEvent close_shift_r(&ic, fcitx::Key(FcitxKey_Shift_R), true);
  engine.handleKeyEvent(close_shift_r);
  expect(close_shift_r.filtered() && close_shift_r.accepted(),
         "Close Shift_R release is consumed and closes palette");

  std::cout << "--- 6. Testing Palette Menu Pagination & Selection (pageSize = 2, #401) ---\n";
  {
    fcitx::RawConfig global_config;
    global_config.setValueByPath("Behavior/DefaultPageSize", "2");
    instance.globalConfig().load(global_config);
    expect(instance.globalConfig().defaultPageSize() == 2,
           "defaultPageSize must be configured to 2");

    // 6a. Open palette menu via Shift_R tap
    fcitx::KeyEvent open_p(&ic, fcitx::Key(FcitxKey_Shift_R), false);
    engine.handleKeyEvent(open_p);
    fcitx::KeyEvent open_r(&ic, fcitx::Key(FcitxKey_Shift_R), true);
    engine.handleKeyEvent(open_r);

    auto candidate_list = ic.inputPanel().candidateList();
    expect(candidate_list != nullptr, "Candidate list must be present after opening palette");
    auto* pageable = candidate_list->toPageable();
    expect(pageable != nullptr, "Candidate list must implement PageableCandidateList");
    expect(pageable->totalPages() == 2, "4 command entries with pageSize 2 must form 2 pages");
    expect(pageable->currentPage() == 0, "Initial page must be page 0");
    expect(candidate_list->size() == 2, "First page must contain 2 candidates");
    expect(candidate_list->candidate(0).text().toString() == "/model (/m)",
           "First candidate must be /model (/m)");
    expect(candidate_list->candidate(1).text().toString() == "/asr (/a)",
           "Second candidate must be /asr (/a)");

    // 6b. Page down to page 1
    fcitx::KeyEvent pgdn(&ic, fcitx::Key(FcitxKey_Page_Down), false);
    engine.handleKeyEvent(pgdn);
    expect(pgdn.filtered() && pgdn.accepted(), "Page_Down must be filtered and accepted");
    expect(pageable->currentPage() == 1, "Pageable current page must now be 1");
    expect(candidate_list->size() == 2, "Second page must contain remaining 2 candidates");
    expect(candidate_list->candidate(0).text().toString() == "/scene (/s)",
           "Page 1 first candidate must be /scene (/s)");
    expect(candidate_list->candidate(1).text().toString() == "/proc (/p)",
           "Page 1 second candidate must be /proc (/p)");

    // 6c. Select first candidate on page 1 with digit key '1' (digitSelection() == 0)
    // Previously, DigitSelectionIndex used current_page * 10 + 0 = 10, which overflowed (10 >= 4).
    // Now it uses candidate_list->candidate(digit).select(...) and correctly selects /scene.
    fcitx::KeyEvent digit_1_p(&ic, fcitx::Key(FcitxKey_1), false);
    engine.handleKeyEvent(digit_1_p);
    expect(digit_1_p.filtered() && digit_1_p.accepted(),
           "Digit 1 on page 1 must select candidate 0 on page 1 (/scene)");

    fcitx::KeyEvent digit_1_r(&ic, fcitx::Key(FcitxKey_1), true);
    engine.handleKeyEvent(digit_1_r);
    expect(digit_1_r.filtered() && digit_1_r.accepted(), "Digit 1 release must be consumed");

    // Verify /scene was actually selected: menu remains open and title updates to reflect Scene
    candidate_list = ic.inputPanel().candidateList();
    expect(candidate_list != nullptr, "Menu must stay open after command entry selection");
    expect(ic.inputPanel().auxUp().toString().find(_("Scene")) != std::string::npos,
           "Menu title must reflect Scene after selecting /scene with digit 1");

    // Close menu with Escape keys
    fcitx::KeyEvent esc_p1(&ic, fcitx::Key(FcitxKey_Escape), false);
    engine.handleKeyEvent(esc_p1);
    fcitx::KeyEvent esc_r1(&ic, fcitx::Key(FcitxKey_Escape), true);
    engine.handleKeyEvent(esc_r1);
    fcitx::KeyEvent esc_p2(&ic, fcitx::Key(FcitxKey_Escape), false);
    engine.handleKeyEvent(esc_p2);
    fcitx::KeyEvent esc_r2(&ic, fcitx::Key(FcitxKey_Escape), true);
    engine.handleKeyEvent(esc_r2);
    expect(ic.inputPanel().candidateList() == nullptr || ic.inputPanel().candidateList()->empty(),
           "Escape must dismiss the palette menu");

    // 6d. Test Enter key selection on page 1 with Down key navigation
    fcitx::KeyEvent open2_p(&ic, fcitx::Key(FcitxKey_Shift_R), false);
    engine.handleKeyEvent(open2_p);
    fcitx::KeyEvent open2_r(&ic, fcitx::Key(FcitxKey_Shift_R), true);
    engine.handleKeyEvent(open2_r);

    fcitx::KeyEvent pgdn2(&ic, fcitx::Key(FcitxKey_Page_Down), false);
    engine.handleKeyEvent(pgdn2);

    fcitx::KeyEvent down_k(&ic, fcitx::Key(FcitxKey_Down), false);
    engine.handleKeyEvent(down_k);
    expect(down_k.filtered() && down_k.accepted(), "Down key must move cursor");

    fcitx::KeyEvent enter_k(&ic, fcitx::Key(FcitxKey_Return), false);
    engine.handleKeyEvent(enter_k);
    expect(enter_k.filtered() && enter_k.accepted(),
           "Enter key on page 1 must select the candidate at cursor");

    // Verify /proc was actually selected by checking title reflects Proc
    candidate_list = ic.inputPanel().candidateList();
    expect(candidate_list != nullptr, "Menu must stay open after command entry selection");
    expect(ic.inputPanel().auxUp().toString().find(_("Proc")) != std::string::npos,
           "Menu title must reflect Proc after selecting /proc with Enter");

    // Close menu again
    fcitx::KeyEvent esc2_p1(&ic, fcitx::Key(FcitxKey_Escape), false);
    engine.handleKeyEvent(esc2_p1);
    fcitx::KeyEvent esc2_r1(&ic, fcitx::Key(FcitxKey_Escape), true);
    engine.handleKeyEvent(esc2_r1);
    fcitx::KeyEvent esc2_p2(&ic, fcitx::Key(FcitxKey_Escape), false);
    engine.handleKeyEvent(esc2_p2);
    fcitx::KeyEvent esc2_r2(&ic, fcitx::Key(FcitxKey_Escape), true);
    engine.handleKeyEvent(esc2_r2);

    // 6e. Test out-of-range digit selection on page 1 (2 candidates on page 1)
    // Pressing digit '3' (digitSelection() == 2) must not access out of bounds
    fcitx::KeyEvent open3_p(&ic, fcitx::Key(FcitxKey_Shift_R), false);
    engine.handleKeyEvent(open3_p);
    fcitx::KeyEvent open3_r(&ic, fcitx::Key(FcitxKey_Shift_R), true);
    engine.handleKeyEvent(open3_r);

    fcitx::KeyEvent pgdn3(&ic, fcitx::Key(FcitxKey_Page_Down), false);
    engine.handleKeyEvent(pgdn3);

    fcitx::KeyEvent digit_3_p(&ic, fcitx::Key(FcitxKey_3), false);
    engine.handleKeyEvent(digit_3_p);
    // An out-of-range digit candidate selection dismisses the menu safely
    expect(ic.inputPanel().candidateList() == nullptr || ic.inputPanel().candidateList()->empty(),
           "Out-of-range digit key on page 1 must dismiss menu without out-of-bounds access");
  }

  std::cout << "--- 7. Testing VinputEngine lifetime_token_ UAF Guard ---\n";
  std::weak_ptr<bool> token;
  {
    const VinputEngine scoped_engine(&instance);
    token = scoped_engine.lifetimeToken();
    expect(!token.expired(), "lifetimeToken must be valid while engine is alive");
    auto shared = token.lock();
    expect(shared != nullptr && *shared, "lifetimeToken payload must be true");
  }
  expect(token.expired(), "lifetimeToken must expire immediately upon engine destruction");

  std::cout << "\n✅ ALL TRIGGER TESTS PASSED CLEANLY!\n";
}

} // namespace

int main() {
  try {
    runAllTests();
  } catch (const std::exception& e) {
    std::cerr << "TEST FAILED: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
