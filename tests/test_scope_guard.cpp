#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include "common/utils/scope_guard.h"

inline void AssertTrue(bool condition, const char* message, const char* file, int line) {
  if (!condition) {
    std::fprintf(stderr, "Assertion failed: %s at %s:%d\n", message, file, line);
    std::abort();
  }
}

#define TEST_ASSERT(cond) AssertTrue((cond), #cond, __FILE__, __LINE__)

void TestScopeExitNormal() {
  bool executed = false;
  {
    auto guard = vinput::make_scope_exit([&] { executed = true; });
    TEST_ASSERT(!executed);
  }
  TEST_ASSERT(executed);
}

void TestScopeExitDismiss() {
  bool executed = false;
  {
    auto guard = vinput::make_scope_exit([&] { executed = true; });
    guard.dismiss();
  }
  TEST_ASSERT(!executed);
}

void TestScopeExitMove() {
  int count = 0;
  {
    auto guard1 = vinput::make_scope_exit([&] { ++count; });
    {
      auto guard2 = std::move(guard1);
      TEST_ASSERT(count == 0);
    }
    TEST_ASSERT(count == 1);
  }
  TEST_ASSERT(count == 1);
}

void TestUniqueFd() {
  int pipefd[2] = {-1, -1};
  TEST_ASSERT(pipe(pipefd) == 0);

  int read_fd = pipefd[0];
  int write_fd = pipefd[1];

  {
    vinput::UniqueFd ufd(read_fd);
    TEST_ASSERT(ufd.is_valid());
    TEST_ASSERT(ufd.get() == read_fd);
    TEST_ASSERT(static_cast<bool>(ufd));

    // Move
    vinput::UniqueFd ufd2 = std::move(ufd);
    TEST_ASSERT(!ufd.is_valid());
    TEST_ASSERT(ufd2.is_valid());
    TEST_ASSERT(ufd2.get() == read_fd);
  }
  // ufd2 destroyed, read_fd should now be closed.
  TEST_ASSERT(fcntl(read_fd, F_GETFD) == -1);

  // Close write_fd manually
  close(write_fd);
}

int main() {
  TestScopeExitNormal();
  TestScopeExitDismiss();
  TestScopeExitMove();
  TestUniqueFd();
  std::printf("test_scope_guard passed.\n");
  return 0;
}
