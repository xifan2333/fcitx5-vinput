#include <cstdio>
#include <cstdlib>

#include "daemon/audio/output_ducker.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

namespace {

void TestDuckerLifecycleAndIdempotence() {
  using vinput::daemon::audio::OutputDucker;

  OutputDucker ducker;
  TEST_ASSERT(!ducker.IsDucked());

  // First duck call
  ducker.Duck(0.5);
  TEST_ASSERT(ducker.IsDucked());

  // Idempotent duplicate duck
  ducker.Duck(0.2);
  TEST_ASSERT(ducker.IsDucked());

  ducker.Flush();

  // Restore call
  ducker.Restore();
  TEST_ASSERT(!ducker.IsDucked());

  // Idempotent duplicate restore
  ducker.Restore();
  TEST_ASSERT(!ducker.IsDucked());

  ducker.Flush();
}

void TestDuckerRapidToggling() {
  using vinput::daemon::audio::OutputDucker;

  OutputDucker ducker;
  for (int i = 0; i < 10; ++i) {
    ducker.Duck(0.5);
    TEST_ASSERT(ducker.IsDucked());
    ducker.Restore();
    TEST_ASSERT(!ducker.IsDucked());
  }
  ducker.Flush();
  TEST_ASSERT(!ducker.IsDucked());
}

void TestDuckerDestructorSafety() {
  using vinput::daemon::audio::OutputDucker;

  {
    OutputDucker ducker;
    ducker.Duck(0.5);
    // Destroyed immediately while ducking may be queued or running
  }

  {
    OutputDucker ducker;
    ducker.Duck(0.5);
    ducker.Restore();
    // Destroyed immediately while restore may be queued or running
  }
}

} // namespace

int main() {
  std::printf("Running test_output_ducker...\n");
  TestDuckerLifecycleAndIdempotence();
  TestDuckerRapidToggling();
  TestDuckerDestructorSafety();
  std::printf("All test_output_ducker passed!\n");
  return 0;
}
