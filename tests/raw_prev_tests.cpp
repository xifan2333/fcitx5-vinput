#include <cstdlib>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <iostream>
#include <optional>
#include <string>

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

} // namespace

class VinputTestAccessor {
public:
  static void enterRecordingState(VinputEngine& engine, fcitx::InputContext* ic) {
    engine.enterRecordingState(ic, fcitx::Key(FcitxKey_Alt_R), false);
  }
  static void enterBusyState(VinputEngine& engine, fcitx::InputContext* ic,
                             const std::string& preedit, bool postprocessing = false,
                             std::optional<bool> raw_prev = std::nullopt) {
    engine.enterBusyState(ic, false, preedit, postprocessing, raw_prev);
  }
  static bool getSessionRawPrev(const VinputEngine& engine) {
    return engine.session_ ? engine.session_->raw_prev : false;
  }
  static void setSessionRawPrev(VinputEngine& engine, bool raw_prev) {
    if (engine.session_) {
      engine.session_->raw_prev = raw_prev;
    }
  }
  static void setSessionTranscript(VinputEngine& engine, const std::string& text) {
    if (engine.session_) {
      engine.session_->transcript_text = text;
    }
  }
  static std::string getSessionTranscript(const VinputEngine& engine) {
    return engine.session_ ? engine.session_->transcript_text : std::string{};
  }
  static void handleRecognitionPartial(VinputEngine& engine, const std::string& text) {
    engine.handleRecognitionPartial(text);
  }
  static void finishFrontendSession(VinputEngine& engine, fcitx::InputContext* ic) {
    engine.finishFrontendSession(ic);
  }
};

namespace {

void testRawPrevDisabledPresentation(VinputEngine& engine, TestInputContext& ic) {
  // 1. Enter recording state and configure session with raw_prev=false
  VinputTestAccessor::enterRecordingState(engine, &ic);
  VinputTestAccessor::setSessionRawPrev(engine, false);
  VinputTestAccessor::setSessionTranscript(engine, "Raw speech transcript that should not preview");

  // 2. Transition to Inferring state without explicit raw_prev argument
  VinputTestAccessor::enterBusyState(engine, &ic, "... Recognizing ...", false);
  expect(!VinputTestAccessor::getSessionRawPrev(engine),
         "session raw_prev=false must not be clobbered during Inferring transition");
  expect(ic.inputPanel().auxDown().toString().empty(),
         "aux_down must remain empty during Inferring when raw_prev is false");
  expect(ic.inputPanel().preedit().toString() == "... Recognizing ...",
         "preedit must show recognizing status instead of transcript when raw_prev is false");

  // 3. Receive partial transcript during Inferring when raw_prev is false
  VinputTestAccessor::handleRecognitionPartial(engine,
                                               "Late partial transcript after recording stopped");
  expect(VinputTestAccessor::getSessionTranscript(engine) ==
             "Late partial transcript after recording stopped",
         "session transcript must be updated in memory");
  expect(ic.inputPanel().auxDown().toString().empty(),
         "aux_down must remain empty when partial arrives during Inferring with raw_prev=false");
  expect(ic.inputPanel().preedit().toString() == "... Recognizing ...",
         "preedit must remain in recognizing status instead of displaying late transcript");

  // 4. Transition to Postprocessing state
  VinputTestAccessor::enterBusyState(engine, &ic, "... Postprocessing ...", true);
  expect(!VinputTestAccessor::getSessionRawPrev(engine),
         "session raw_prev=false must not be clobbered during Postprocessing transition");
  expect(ic.inputPanel().auxDown().toString().empty(),
         "aux_down must remain empty during Postprocessing when raw_prev is false");
  expect(ic.inputPanel().preedit().toString() == "... Postprocessing ...",
         "preedit must show compact postprocessing status when raw_prev is false");

  VinputTestAccessor::finishFrontendSession(engine, &ic);
}

void testRawPrevEnabledPresentation(VinputEngine& engine, TestInputContext& ic) {
  // 1. Enter recording state and configure session with raw_prev=true
  VinputTestAccessor::enterRecordingState(engine, &ic);
  VinputTestAccessor::setSessionRawPrev(engine, true);
  VinputTestAccessor::setSessionTranscript(engine, "Raw speech transcript preview");

  // 2. Transition to Postprocessing state with raw_prev=true
  VinputTestAccessor::enterBusyState(engine, &ic, "... Postprocessing ...", true, true);
  expect(VinputTestAccessor::getSessionRawPrev(engine), "session raw_prev=true must be preserved");
  expect(ic.inputPanel().auxDown().toString() == "Raw speech transcript preview",
         "aux_down must display transcript preview during Postprocessing when raw_prev is true");

  VinputTestAccessor::finishFrontendSession(engine, &ic);
}

} // namespace

int main() {
  char arg0[] = "vinput-raw-prev-test";
  char arg1[] = "--disable=all";
  char* argv[] = {arg0, arg1, nullptr};

  fcitx::Instance instance(2, argv);
  TestInputContext ic(instance);
  VinputEngine engine(&instance);

  testRawPrevDisabledPresentation(engine, ic);
  testRawPrevEnabledPresentation(engine, ic);

  std::cout << "All raw_prev presentation tests passed!\n";
  return 0;
}
