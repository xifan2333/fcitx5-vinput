#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "input/modifier_gesture.h"

using G = ModifierGesture;
using Mode = G::Mode;
using Action = G::Action;
using enum G::Event;
using namespace std::chrono_literals;

// Each step defaults to no event. Expected events and filtering are specified
// at the point they should happen; elapse() advances time without firing timers.
struct Step {
  enum Kind { Press, Release, Tick, Elapse, Reset, Reload, NoTimer } kind;
  fcitx::Key key{};
  G::Duration elapsed{};
  G::Event event = None;
  std::size_t binding = 0;
  std::optional<bool> filter = std::nullopt;
};

Step down(const std::string& key, G::Event event = None, std::optional<bool> filter = {}) {
  return {Step::Press, fcitx::Key(key), {}, event, 0, filter};
}
Step up(const std::string& key, G::Event event = None, std::size_t binding = 0) {
  return {Step::Release, fcitx::Key(key), {}, event, binding, {}};
}
Step tick(G::Duration elapsed = 0ms, G::Event event = None, std::size_t binding = 0) {
  return {Step::Tick, fcitx::Key{}, elapsed, event, binding, {}};
}
Step elapse(G::Duration elapsed) {
  return {Step::Elapse, fcitx::Key{}, elapsed, None, 0, {}};
}

G::Binding binding(const char* key, Mode mode = Mode::Both, Action action = Action::Command,
                   G::Duration delay = 300ms) {
  return {fcitx::Key(key), action, mode, delay,
          action == Action::Palette ? std::optional(250ms) : std::nullopt};
}

struct Scenario {
  std::string name;
  std::vector<G::Binding> config;
  std::vector<Step> sequence;
  bool toggle_recording = false;
};

std::size_t scenarios_run = 0;

void run(const Scenario& test) {
  G gesture;
  gesture.setBindings(test.config);
  std::vector<fcitx::Key> pressed;
  std::vector<fcitx::Key> consumed;
  G::Clock::time_point now{};
  std::string trace = test.name;
  for (const auto& config : test.config) {
    trace += " [" + config.key.toString() +
             ", mode=" + std::to_string(static_cast<int>(config.mode)) + "]";
  }
  const auto expect = [&](bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << trace << "\nFAIL: " << message << '\n';
      std::exit(1);
    }
  };
  constexpr const char* kinds[] = {"down ", "up ", "tick", "elapse", "reset", "reload", "no timer"};
  constexpr const char* events[] = {"None", "Tap", "HoldStart", "HoldRelease", "HoldCancel"};
  for (const auto& step : test.sequence) {
    now += step.elapsed;
    trace += " / " + std::to_string(step.elapsed.count()) + "ms " + kinds[step.kind] +
             step.key.toString();
    G::Result result;
    if (step.kind == Step::Press || step.kind == Step::Release) {
      auto states = step.key.states();
      for (const auto& key : pressed)
        states |= fcitx::Key::keySymToStates(key.sym());
      const auto same_key = [&](const auto& key) {
        return key.code() && step.key.code() ? key.code() == step.key.code()
                                             : key.sym() == step.key.sym();
      };
      const auto found = std::find_if(pressed.begin(), pressed.end(), same_key);
      result = gesture.keyEvent(fcitx::Key(step.key.sym(), states, step.key.code()),
                                step.kind == Step::Release, now, test.toggle_recording);
      const auto owned = std::find_if(consumed.begin(), consumed.end(), same_key);
      if (step.kind == Step::Release || found != pressed.end()) {
        expect(result.filter == (owned != consumed.end()),
               "presses, repeats, and releases must have consistent application delivery");
      }
      if (step.kind == Step::Release && owned != consumed.end()) {
        consumed.erase(owned);
      } else if (result.filter && owned == consumed.end()) {
        consumed.push_back(step.key);
      }
      if (step.kind == Step::Release && found != pressed.end()) {
        pressed.erase(found);
      } else if (step.kind == Step::Press && found == pressed.end() &&
                 !states.test(fcitx::KeyState::Repeat)) {
        pressed.push_back(step.key);
      }
    } else if (step.kind == Step::Tick) {
      result = gesture.timeout(now);
    } else if (step.kind == Step::Reset) {
      gesture.cancel();
    } else if (step.kind == Step::Reload) {
      gesture.setBindings(test.config);
    } else if (step.kind == Step::NoTimer) {
      expect(!gesture.deadline(), "timer must be disarmed");
    }
    expect(result.event == step.event, std::string("expected ") +
                                           events[static_cast<int>(step.event)] + ", got " +
                                           events[static_cast<int>(result.event)]);
    expect(result.binding.has_value() == (step.event != None), "only events carry a binding");
    if (result.binding) {
      const auto& expected = test.config.at(step.binding);
      expect(result.binding->key == expected.key && result.binding->action == expected.action,
             "event must route to the expected binding and action");
    }
    expect(!step.filter || result.filter == *step.filter, "unexpected key filtering");
  }
  expect(!gesture.deadline(), "completed sequence must leave no timer");
  ++scenarios_run;
}

void examples() {
  const fcitx::Key locked_control(FcitxKey_Control_L, fcitx::KeyStates(fcitx::KeyState::CapsLock) |
                                                          fcitx::KeyState::NumLock);
  for (auto mode : {Mode::Tap, Mode::Hold, Mode::Both}) {
    const auto tap = mode == Mode::Hold ? None : Tap;
    const auto start = mode == Mode::Tap ? None : HoldStart;
    const auto stop = mode == Mode::Tap ? Tap : HoldRelease;
    run({"tap and hold",
         {binding("Control_L", mode)},
         {down("Control_L"),
          elapse(100ms),
          up("Control_L", tap),
          {Step::NoTimer},
          down("Control_L"),
          tick(300ms, start),
          tick(100ms),
          up("Control_L", stop)}});
    for (bool modifier_first : {false, true}) {
      run({"Ctrl+C cancels until all keys are up",
           {binding("Control+Control_L", mode)},
           {down("Control_L"),
            elapse(50ms),
            down("c", None, false),
            {Step::NoTimer},
            tick(500ms),
            modifier_first ? up("Control_L") : up("c"),
            modifier_first ? up("c") : down("Control_L"),
            modifier_first ? tick() : up("Control_L"),
            down("Control_L"),
            elapse(50ms),
            up("Control_L", tap)}});
    }
  }
  for (auto held : {299ms, 300ms, 301ms, 400ms}) {
    run({"release before delayed timer delivery",
         {binding("Control_L")},
         {down("Control_L"), elapse(held), up("Control_L", held < 300ms ? Tap : None), tick()}});
  }
  for (auto mode : {Mode::Hold, Mode::Both}) {
    run({"hold threshold",
         {binding("Control_L", mode)},
         {down("Control_L"), tick(299ms), tick(1ms, HoldStart), up("Control_L", HoldRelease),
          tick()}});
  }
  for (auto gap : {199ms, 200ms, 201ms}) {
    run({"chord release deadline",
         {binding("Control+Shift_L")},
         {down("Control_L"),
          down("Shift_L"),
          elapse(299ms),
          up("Shift_L"),
          {Step::NoTimer},
          tick(gap),
          up("Control_L", gap <= 200ms ? Tap : None)}});
  }
  for (auto reset : {Step::Reset, Step::Reload}) {
    run({"context/config reset",
         {binding("Control_L")},
         {down("Control_L"), {reset}, {Step::NoTimer}, up("Control_L")}});
  }
  for (auto control : {"Control_L", "Control_R"}) {
    for (auto shift : {"Shift_L", "Shift_R"}) {
      run({"explicit modifier side",
           {binding("Control+Shift_L")},
           {down(control), down(shift), up(control),
            up(shift, std::string_view(shift) == "Shift_L" ? Tap : None)}});
    }
  }
  for (const auto& test : std::vector<Scenario>{
           {"key held before modifier",
            {binding("Control_L")},
            {down("c"), down("Control_L"), up("c"), up("Control_L")}},
           {"reset recovers when an ordinary release was lost",
            {binding("Control_L")},
            {down("c"), {Step::Reset}, down("Control_L"), up("Control_L", Tap)}},
           {"single binding does not reserve the opposite side",
            {binding("Control_L", Mode::Hold)},
            {down("Control_L", None, true), down("Control_R", None, false), up("Control_L"),
             up("Control_R")}},
           {"slow three-modifier release",
            {binding("Control+Alt+Shift_L")},
            {down("Control_L"), down("Alt_L"), down("Shift_L"), elapse(50ms), up("Shift_L"),
             elapse(201ms), up("Alt_L"), up("Control_L")}},
           {"typing during release",
            {binding("Control+Shift_L")},
            {down("Control_L"), down("Shift_L"), up("Shift_L"), down("c"), up("c"),
             up("Control_L")}},
           {"extra modifier suppresses subset",
            {binding("Control_L")},
            {down("Control_L"), down("Alt_L"), up("Alt_L"), up("Control_L")}},
           {"custom delay and unflagged repeat",
            {binding("Control_L", Mode::Both, Action::Command, 600ms)},
            {down("Control_L"), tick(300ms), down("Control_L"), tick(300ms, HoldStart),
             down("c", HoldCancel, false), up("c"), up("Control_L")}},
           {"tap stops toggle",
            {binding("Control_L")},
            {down("Control_L"), tick(800ms), up("Control_L", Tap)},
            true},
           {"Ctrl+C does not stop toggle",
            {binding("Control_L")},
            {down("Control_L"), down("c", None, false), up("c"), up("Control_L")},
            true},
           {"palette tap boundary and interruption",
            {binding("Shift_R", Mode::Tap, Action::Palette), binding("Control_L")},
            {down("Shift_R"), elapse(250ms), up("Shift_R", Tap), down("Shift_R"), elapse(251ms),
             up("Shift_R"), down("Shift_R"), down("Control_L"), up("Control_L"), up("Shift_R"),
             down("Control_R"), up("Control_R")}},
           {"physical identity across layout change",
            {binding("Alt_L", Mode::Tap)},
            {{Step::Press, fcitx::Key(FcitxKey_Alt_L, fcitx::KeyStates(), 64)},
             {Step::Release, fcitx::Key(FcitxKey_Meta_L, fcitx::KeyStates(), 64), {}, Tap},
             down("F8", None, false)}},
           {"flagged repeat preserves deadline",
            {binding("Control_L")},
            {down("Control_L"),
             elapse(299ms),
             {Step::Press, fcitx::Key(FcitxKey_Control_L, fcitx::KeyState::Repeat)},
             tick(1ms, HoldStart),
             up("Control_L", HoldRelease)}},
           {"locks ignored",
            {binding("Control_L")},
            {{Step::Press, locked_control}, {Step::Release, locked_control, {}, Tap}}},
           {"larger chord takes over pending tap",
            {binding("Control_L"), binding("Control+Alt+Shift_L", Mode::Both, Action::Dictation)},
            {down("Control_L"), down("Alt_L"), tick(400ms), down("Shift_L"), elapse(50ms),
             up("Control_L"), up("Alt_L"), up("Shift_L", Tap, 1)}},
           {"larger chord restarts hold delay",
            {binding("Control_L"), binding("Control+Alt+Shift_L", Mode::Hold, Action::Dictation)},
            {down("Control_L"), elapse(250ms), down("Alt_L"), tick(500ms), down("Shift_L"),
             tick(299ms), tick(1ms, HoldStart, 1), up("Control_L", HoldRelease, 1), up("Alt_L"),
             up("Shift_L")}},
       }) {
    run(test);
  }
  for (auto action : {Action::Dictation, Action::Command, Action::Palette}) {
    run({"shared tap routing and recovery",
         {binding("Control+Alt+Shift_L", Mode::Tap, action)},
         {down("Shift_L"), down("Control_L"), down("Alt_L"), down("c"), up("c"), elapse(50ms),
          up("Control_L"), up("Shift_L"), up("Alt_L"), down("Shift_L"), down("Control_L"),
          down("Alt_L"), elapse(50ms), up("Control_L"), up("Shift_L"), up("Alt_L", Tap)}});
    if (action != Action::Palette) {
      for (auto mode : {Mode::Hold, Mode::Both}) {
        run({"shared hold routing",
             {binding("Control+Alt+Shift_L", mode, action)},
             {down("Alt_L"), down("Shift_L"), down("Control_L"), tick(300ms, HoldStart),
              up("Shift_L", HoldRelease), up("Alt_L"), up("Control_L")}});
      }
    }
  }
}

void permutations() {
  // Keep exhaustive two-/three-/four-modifier orders and all active-hold cases.
  const std::vector<std::pair<const char*, std::vector<std::string>>> chords = {
      {"Control_L", {"Control_L"}},
      {"Control+Shift_L", {"Control_L", "Shift_L"}},
      {"Control+Alt+Shift_L", {"Control_L", "Alt_L", "Shift_L"}},
      {"Control+Alt+Super+Shift_L", {"Control_L", "Alt_L", "Shift_L", "Super_L"}}};
  for (const auto& [name, keys] : chords) {
    auto press = keys;
    std::sort(press.begin(), press.end());
    do {
      auto release = keys;
      std::sort(release.begin(), release.end());
      do {
        for (auto mode : {Mode::Tap, Mode::Hold, Mode::Both}) {
          for (bool held : {false, true}) {
            Scenario test{"modifier orders", {binding(name, mode)}, {}};
            for (auto key : press) {
              test.sequence.push_back(down(key));
              if (key != press.back())
                test.sequence.push_back(tick(400ms));
            }
            const bool started = held && mode != Mode::Tap;
            test.sequence.push_back(tick(held ? 300ms : 50ms, started ? HoldStart : None));
            for (auto key : release) {
              const auto event = started && key == release.front() ? HoldRelease
                                 : !started && mode != Mode::Hold && key == release.back() ? Tap
                                                                                           : None;
              test.sequence.push_back(up(key, event));
              test.sequence.push_back(elapse(30ms));
            }
            run(test);
          }
        }
      } while (std::next_permutation(release.begin(), release.end()));
    } while (std::next_permutation(press.begin(), press.end()));
    for (auto mode : {Mode::Hold, Mode::Both}) {
      for (auto action : {Action::Dictation, Action::Command}) {
        for (auto extra : {"c", "F8", "Control_R"}) {
          Scenario test{"active hold cancellation and recovery", {binding(name, mode, action)}, {}};
          for (auto key : keys)
            test.sequence.push_back(down(key));
          for (auto step : {tick(300ms, HoldStart), down(keys.back()),
                            down(extra, HoldCancel, false), down(extra)})
            test.sequence.push_back(step);
          for (auto key : keys)
            test.sequence.push_back(up(key));
          test.sequence.push_back(tick(500ms));
          test.sequence.push_back(up(extra));
          for (auto key : keys)
            test.sequence.push_back(down(key));
          for (auto step :
               {tick(300ms, HoldStart), up(keys.front(), HoldRelease), down(extra), up(extra)})
            test.sequence.push_back(step);
          for (std::size_t i = 1; i < keys.size(); ++i)
            test.sequence.push_back(up(keys[i]));
          run(test);
        }
      }
    }
  }
}

int main(int argc, char** argv) {
  const std::string_view group = argc == 2 ? argv[1] : "";
  if (group.empty() || group == "scenarios")
    examples();
  if (group.empty() || group == "permutations")
    permutations();
  if (!scenarios_run)
    return 1;
  std::cout << "PASS " << scenarios_run << " modifier scenarios\n";
}
