#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <pulse/volume.h>
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
using vinput::daemon::audio::PulseVolumeAdapter;

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

void TestPulseVolumeAdapterMath() {
  pa_cvolume norm{};
  pa_cvolume_set(&norm, 2, PA_VOLUME_NORM);
  const auto norm_scalar = PulseVolumeAdapter::CvolumeToScalar(norm);
  TEST_ASSERT(norm_scalar.has_value() && std::abs(*norm_scalar - 1.0) < 1e-4);

  // Asymmetric stereo channel balance (L = 100%, R = 50%) must preserve 2:1 ratio on scale.
  pa_cvolume balanced{};
  balanced.channels = 2;
  balanced.values[0] = PA_VOLUME_NORM;
  balanced.values[1] = PA_VOLUME_NORM / 2U;
  const auto bal_scalar = PulseVolumeAdapter::CvolumeToScalar(balanced);
  TEST_ASSERT(bal_scalar.has_value() && std::abs(*bal_scalar - 1.0) < 1e-4);

  const pa_cvolume scaled_half = PulseVolumeAdapter::ScaleCvolume(balanced, 0.50);
  TEST_ASSERT(scaled_half.channels == 2);
  const auto half_scalar = PulseVolumeAdapter::CvolumeToScalar(scaled_half);
  TEST_ASSERT(half_scalar.has_value() && std::abs(*half_scalar - 0.50) < 1e-3);
  const double right_ratio =
      static_cast<double>(scaled_half.values[1]) / static_cast<double>(scaled_half.values[0]);
  TEST_ASSERT(std::abs(right_ratio - 0.50) < 1e-2);

  // Muted baseline scales cleanly to target scalar.
  pa_cvolume muted{};
  pa_cvolume_set(&muted, 2, PA_VOLUME_MUTED);
  const auto muted_scalar = PulseVolumeAdapter::CvolumeToScalar(muted);
  TEST_ASSERT(muted_scalar.has_value() && std::abs(*muted_scalar - 0.0) < 1e-4);
  const pa_cvolume unmuted = PulseVolumeAdapter::ScaleCvolume(muted, 0.40);
  const auto unmuted_scalar = PulseVolumeAdapter::CvolumeToScalar(unmuted);
  TEST_ASSERT(unmuted_scalar.has_value() && std::abs(*unmuted_scalar - 0.40) < 1e-3);

  // Out-of-range scalars clamp to [0.0, 1.5] and 0-channel structs fail gracefully.
  const pa_cvolume clamped_high = PulseVolumeAdapter::ScaleCvolume(norm, 3.0);
  const auto high_scalar = PulseVolumeAdapter::CvolumeToScalar(clamped_high);
  TEST_ASSERT(high_scalar.has_value() && std::abs(*high_scalar - 1.50) < 1e-3);

  const pa_cvolume clamped_low = PulseVolumeAdapter::ScaleCvolume(norm, -0.5);
  const auto low_scalar = PulseVolumeAdapter::CvolumeToScalar(clamped_low);
  TEST_ASSERT(low_scalar.has_value() && std::abs(*low_scalar - 0.0) < 1e-4);

  pa_cvolume empty{};
  empty.channels = 0;
  TEST_ASSERT(!PulseVolumeAdapter::CvolumeToScalar(empty).has_value());
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
  // Constructing and destroying with default PulseVolumeAdapter should be safe without
  // connecting to the host PulseAudio server until Duck/Restore is invoked.
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
  TestPulseVolumeAdapterMath();
  TestFakeVolumeAdapter();
  TestDuckerLifecycleAndIdempotence();
  TestDuckerRapidToggling();
  TestDuckerDestructorSafety();
  TestDuckerNullAdapter();
  TestDuckerAdapterFailure();
  std::printf("All test_output_ducker passed!\n");
  return 0;
}
