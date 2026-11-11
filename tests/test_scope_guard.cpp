#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

#include "common/utils/scope_guard.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestScopeExitExecutes() {
  int count = 0;
  {
    auto guard = vinput::MakeScopeExit([&] { ++count; });
    Expect(count == 0, "Guard must not execute before scope exit");
  }
  Expect(count == 1, "Guard must execute exactly once on scope exit");
}

void TestScopeExitDismiss() {
  int count = 0;
  {
    auto guard = vinput::MakeScopeExit([&] { ++count; });
    guard.Dismiss();
  }
  Expect(count == 0, "Dismissed guard must not execute on scope exit");
}

void TestScopeExitMoveConstruct() {
  int count = 0;
  {
    auto guard1 = vinput::MakeScopeExit([&] { ++count; });
    {
      auto guard2 = std::move(guard1);
      Expect(count == 0, "Move construction must not execute callback");
    }
    Expect(count == 1, "Target guard must execute callback on scope exit");
  }
  Expect(count == 1, "Source guard must not execute callback after move");
}

void TestScopeExitMoveDismissed() {
  int count = 0;
  {
    auto guard1 = vinput::MakeScopeExit([&] { ++count; });
    guard1.Dismiss();
    {
      auto guard2 = std::move(guard1);
      Expect(count == 0, "Moving dismissed guard must not execute");
    }
    Expect(count == 0, "Dismissed moved guard must not execute on scope exit");
  }
  Expect(count == 0, "Source of dismissed moved guard must not execute");
}

void TestScopeExitMoveOnlyCallable() {
  auto ptr = std::make_unique<int>(42);
  int result = 0;
  {
    auto guard = vinput::MakeScopeExit([p = std::move(ptr), &result] { result = *p; });
    auto moved_guard = std::move(guard);
    Expect(result == 0, "Move-only callable guard must not execute before exit");
  }
  Expect(result == 42, "Move-only callable guard must execute on exit");
}

} // namespace

int main() {
  TestScopeExitExecutes();
  TestScopeExitDismiss();
  TestScopeExitMoveConstruct();
  TestScopeExitMoveDismissed();
  TestScopeExitMoveOnlyCallable();
  std::cout << "All scope guard tests passed!\n";
  return 0;
}
