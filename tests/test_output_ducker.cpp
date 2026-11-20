#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

#include "daemon/audio/output_ducker.h"

namespace {

void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

using vinput::daemon::audio::FakeVolumeAdapter;
using vinput::daemon::audio::OutputDucker;
using vinput::daemon::audio::WpctlVolumeAdapter;

bool WaitForVolume(const std::shared_ptr<FakeVolumeAdapter>& adapter, double target,
                   int max_iterations = 200) {
  for (int i = 0; i < max_iterations; ++i) {
    if (std::abs(adapter->CurrentVolume() - target) < 1e-3) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::abs(adapter->CurrentVolume() - target) < 1e-3;
}

bool WaitForWriteCount(const std::shared_ptr<FakeVolumeAdapter>& adapter, int target_count,
                       int max_iterations = 200) {
  for (int i = 0; i < max_iterations; ++i) {
    if (adapter->WriteCount() >= target_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return adapter->WriteCount() >= target_count;
}

void TestWpctlVolumeAdapterParsing() {
  const auto valid = WpctlVolumeAdapter::ParseVolume("Volume: 0.85");
  TEST_ASSERT(valid.has_value() && std::abs(*valid - 0.85) < 1e-4);

  const auto muted = WpctlVolumeAdapter::ParseVolume("Volume: 0.15 [MUTED]");
  TEST_ASSERT(muted.has_value() && std::abs(*muted - 0.15) < 1e-4);

  const auto full = WpctlVolumeAdapter::ParseVolume("Volume: 1.00");
  TEST_ASSERT(full.has_value() && std::abs(*full - 1.00) < 1e-4);

  const auto zero = WpctlVolumeAdapter::ParseVolume("Volume: 0.00");
  TEST_ASSERT(zero.has_value() && std::abs(*zero - 0.00) < 1e-4);

  TEST_ASSERT(!WpctlVolumeAdapter::ParseVolume("").has_value());
  TEST_ASSERT(!WpctlVolumeAdapter::ParseVolume("Invalid output text").has_value());
  TEST_ASSERT(!WpctlVolumeAdapter::ParseVolume("Volume: ").has_value());
  TEST_ASSERT(!WpctlVolumeAdapter::ParseVolume("Volume: not-a-number").has_value());
}

void TestFakeVolumeAdapter() {
  FakeVolumeAdapter adapter(0.75);
  const auto initial = adapter.GetVolume();
  TEST_ASSERT(initial.has_value() && std::abs(*initial - 0.75) < 1e-4);
  TEST_ASSERT(std::abs(adapter.CurrentVolume() - 0.75) < 1e-4);

  TEST_ASSERT(adapter.SetVolume(0.30));
  const auto updated = adapter.GetVolume();
  TEST_ASSERT(updated.has_value() && std::abs(*updated - 0.30) < 1e-4);
  TEST_ASSERT(std::abs(adapter.CurrentVolume() - 0.30) < 1e-4);
}

void TestDuckerLifecycleAndIdempotence() {
  auto adapter = std::make_shared<FakeVolumeAdapter>(0.80);
  OutputDucker ducker(adapter);

  // 1. First duck call scales 0.80 * 0.5 = 0.40
  ducker.Duck(0.5);
  TEST_ASSERT(WaitForVolume(adapter, 0.40));

  // 2. Idempotent duplicate duck call should be ignored
  ducker.Duck(0.2);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  TEST_ASSERT(std::abs(adapter->CurrentVolume() - 0.40) < 1e-3);

  // 3. Restore call resets volume to 0.80
  ducker.Restore();
  TEST_ASSERT(WaitForVolume(adapter, 0.80));

  // 4. Idempotent duplicate restore call should be ignored
  ducker.Restore();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  TEST_ASSERT(std::abs(adapter->CurrentVolume() - 0.80) < 1e-3);
}

void TestDuckerRapidToggling() {
  auto adapter = std::make_shared<FakeVolumeAdapter>(1.0);
  OutputDucker ducker(adapter);

  for (int i = 0; i < 20; ++i) {
    ducker.Duck(0.5);
    ducker.Restore();
  }
  TEST_ASSERT(WaitForWriteCount(adapter, 40));
  TEST_ASSERT(WaitForVolume(adapter, 1.0));
}

void TestDuckerDestructorSafety() {
  auto adapter = std::make_shared<FakeVolumeAdapter>(0.60);
  {
    OutputDucker ducker(adapter);
    ducker.Duck(0.5);
    TEST_ASSERT(WaitForVolume(adapter, 0.30));
  }
  // Destructor must restore original volume 0.60
  TEST_ASSERT(std::abs(adapter->CurrentVolume() - 0.60) < 1e-3);

  auto adapter_rapid = std::make_shared<FakeVolumeAdapter>(0.60);
  {
    OutputDucker ducker(adapter_rapid);
    ducker.Duck(0.5);
  }
  // Immediate destruction after Duck should leave volume at 0.60
  TEST_ASSERT(std::abs(adapter_rapid->CurrentVolume() - 0.60) < 1e-3);
}

void TestDuckerNullAdapter() {
  // Constructing and destroying with default adapter should be safe.
  // We do not call Duck/Restore here to avoid invoking host wpctl in tests.
  const OutputDucker ducker(nullptr);
}

void TestDuckerAdapterFailure() {
  auto adapter = std::make_shared<FakeVolumeAdapter>(0.80);
  adapter->SetFailGet(true);
  OutputDucker ducker(adapter);
  ducker.Duck(0.5);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  TEST_ASSERT(std::abs(adapter->CurrentVolume() - 0.80) < 1e-3);
}

} // namespace

int main() {
  std::printf("Running test_output_ducker...\n");
  TestWpctlVolumeAdapterParsing();
  TestFakeVolumeAdapter();
  TestDuckerLifecycleAndIdempotence();
  TestDuckerRapidToggling();
  TestDuckerDestructorSafety();
  TestDuckerNullAdapter();
  TestDuckerAdapterFailure();
  std::printf("All test_output_ducker passed!\n");
  return 0;
}
