# Hotkey regression tests

Gesture tests declare binding configuration and a timed sequence. Each step
expects no event unless one is specified; the runner also checks routing and
optional key filtering. `tick()` advances time and delivers the timer, while
`elapse()` advances time without delivering it.

```cpp
run({"Ctrl+C during a hold", {binding("Control_L")},
     {down("Control_L"), tick(300ms, HoldStart),
      down("c", HoldCancel, false), up("c"), up("Control_L")}});
```

The `scenarios` group contains named regressions; `permutations` generates the
modifier-order and cancellation matrices. Separate tests cover configuration
serialization and real addon/daemon cancellation on a private D-Bus with a fake
ASR session. The latter require `dbus-run-session`. All assertions run in Release.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target modifier-gesture-tests hotkey-config-tests cancellation-tests -j 2
ctest --test-dir build --output-on-failure
```

Real desktop event delivery, application shortcut propagation, and microphone
capture still need manual testing.
