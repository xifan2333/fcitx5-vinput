#include <cstdlib>
#include <iostream>

#include "common/utils/scope_guard.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "Assertion failed: " << message << '\n';
    std::abort();
  }
}

void TestScopeExitExecutes() {
  bool executed = false;
  {
    auto guard = vinput::MakeScopeExit([&] { executed = true; });
    Expect(!executed, "Guard must not execute before scope exit");
  }
  Expect(executed, "Guard must execute on scope exit");
}

void TestScopeExitDismiss() {
  bool executed = false;
  {
    auto guard = vinput::MakeScopeExit([&] { executed = true; });
    guard.Dismiss();
  }
  Expect(!executed, "Dismissed guard must not execute on scope exit");
}

} // namespace

int main() {
  TestScopeExitExecutes();
  TestScopeExitDismiss();
  std::cout << "All scope guard tests passed!\n";
  return 0;
}
