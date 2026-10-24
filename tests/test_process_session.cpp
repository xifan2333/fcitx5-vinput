#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/utils/process_session.h"
#include "common/utils/process_utils.h"

namespace {

using Clock = std::chrono::steady_clock;
using vinput::process::ChildEvent;
using vinput::process::ChildEventKind;
using vinput::process::ProcessSession;

constexpr auto kEventWait = std::chrono::milliseconds(50);
// Well below the scripts' own sleeps, so a bounded failure is reported as a failure instead of the
// suite waiting for the child.
constexpr auto kTestTimeout = std::chrono::milliseconds(5000);

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

std::string ToText(const std::vector<std::byte>& data) {
  std::string text;
  text.reserve(data.size());
  for (const std::byte value : data) {
    text.push_back(static_cast<char>(value));
  }
  return text;
}

ProcessSession MustStart(const std::string& script) {
  vinput::process::CommandSpec spec;
  spec.command = "sh";
  spec.args = {"-c", script};
  auto session = ProcessSession::Start(spec);
  ExpectTrue(session.has_value(), "ProcessSession::Start should succeed");
  return std::move(*session);
}

// Drains events until the child is reaped, so a session that never finishes fails the test instead
// of hanging the suite.
ChildEvent AwaitExit(ProcessSession& session, std::chrono::milliseconds timeout) {
  const Clock::time_point deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::Exited) {
      return event;
    }
  }
  ExpectTrue(false, "the session should report Exited within the timeout");
  return {};
}

// Drains whatever arrives until the child exits, so a test can look at the whole conversation.
std::string DrainUntilExit(ProcessSession& session, ChildEvent* terminal,
                           std::chrono::milliseconds timeout) {
  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
    if (event.kind == ChildEventKind::Exited) {
      if (terminal != nullptr) {
        *terminal = std::move(event);
      }
      break;
    }
  }
  return stdout_text;
}

// Waits for a helper the provider script forked to disappear, tolerating the short window in which
// the kernel still lists a process that has just been killed.
bool WaitUntilHelperGone(int helper_pid, std::uint64_t helper_start_time) {
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(2000);
  while (Clock::now() < deadline) {
    if (!vinput::process::ProcessIdentityMatches(helper_pid, helper_start_time)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

// Reads the pid the child prints on its first line, so a test can track a helper the provider
// script forked.
int ReadHelperPid(ProcessSession& session) {
  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find('\n') == std::string::npos && Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
  }
  const std::size_t newline = stdout_text.find('\n');
  ExpectTrue(newline != std::string::npos, "the child should report its helper's pid");
  return std::atoi(stdout_text.substr(0, newline).c_str());
}

std::span<const std::byte> AsBytes(const std::string& text) {
  return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

/**
 * The session is a conversation: what the caller writes reaches the child, and what the child
 * answers comes back as events in the order it was produced.
 */
void TestRoundTrip() {
  auto session = MustStart(R"(while IFS= read -r line; do printf 'ack:%s\n' "$line"; done)");

  const std::string payload = "hello_session\n";
  ExpectTrue(session.PushStdin(AsBytes(payload)), "the session should accept stdin");

  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find("ack:hello_session") == std::string::npos && Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
    if (event.kind == ChildEventKind::Exited) {
      break;
    }
  }
  ExpectTrue(stdout_text.find("ack:hello_session") != std::string::npos,
             "the child's answer should come back as stdout events");

  session.CloseStdin();
  const ChildEvent exit = AwaitExit(session, kTestTimeout);
  ExpectTrue(exit.exit_code == 0, "a child that finished on its own should exit 0");
  ExpectTrue(!exit.killed_by_us, "a child that finished on its own was not killed by the session");
}

/**
 * Speech recognition runs the other way round: the provider talks on its own schedule, so output
 * has to be observed even while the caller pushes no audio at all.
 */
void TestOutputArrivesWithoutStdin() {
  auto session = MustStart("for i in 1 2 3; do echo tick$i; sleep 0.1; done");

  const std::string stdout_text = DrainUntilExit(session, nullptr, kTestTimeout);
  ExpectTrue(stdout_text.find("tick1") != std::string::npos,
             "the first line the child printed on its own should be delivered");
  ExpectTrue(stdout_text.find("tick3") != std::string::npos,
             "every line the child printed on its own should be delivered");
}

/**
 * Cancelling has to take effect while the caller is waiting for output, and it has to be reported
 * as something this session caused rather than as a provider failure.
 */
void TestCancelInterruptsBlockedWait() {
  auto session = MustStart("echo ready; sleep 30");

  std::string stdout_text;
  const Clock::time_point ready_deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find("ready") == std::string::npos && Clock::now() < ready_deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
  }
  ExpectTrue(stdout_text.find("ready") != std::string::npos, "the child should announce itself");

  const Clock::time_point cancel_start = Clock::now();
  session.Cancel();
  const ChildEvent exit = AwaitExit(session, kTestTimeout);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - cancel_start);
  ExpectTrue(elapsed < std::chrono::milliseconds(2000),
             "cancelling must end the session long before the child's own sleep does (elapsed=" +
                 std::to_string(elapsed.count()) + "ms)");
  ExpectTrue(exit.killed_by_us, "a cancelled session must report that it killed the child");
}

/**
 * A provider script is usually a wrapper: cancelling only its pid would leave the helpers it forked
 * running with the session's pipes still open.
 */
void TestCancelTerminatesTheProcessGroup() {
  auto session = MustStart("sleep 30 & echo $!; wait");

  const int helper_pid = ReadHelperPid(session);
  ExpectTrue(helper_pid > 0, "the reported helper pid should be usable");

  const std::uint64_t helper_start_time = vinput::process::ProcessStartTime(helper_pid);
  ExpectTrue(helper_start_time != 0, "the helper should be running before the cancel");

  session.Cancel();
  (void)AwaitExit(session, kTestTimeout);

  bool helper_gone = false;
  const Clock::time_point gone_deadline = Clock::now() + std::chrono::milliseconds(2000);
  while (Clock::now() < gone_deadline) {
    if (!vinput::process::ProcessIdentityMatches(helper_pid, helper_start_time)) {
      helper_gone = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ExpectTrue(helper_gone, "cancelling must not leave a helper the provider forked behind");
}

/**
 * Cancelling is a promise about the whole process group: a helper that ignores SIGTERM must still
 * be killed once the leader is gone, and the wait for it must stay bounded.
 */
void TestHelperIgnoringSigtermIsKilled() {
  auto session = MustStart("( trap '' TERM; exec sleep 30 ) & sleep 0.2; echo $!; sleep 30");
  const int helper_pid = ReadHelperPid(session);
  ExpectTrue(helper_pid > 0, "the reported helper pid should be usable");
  const std::uint64_t helper_start_time = vinput::process::ProcessStartTime(helper_pid);
  ExpectTrue(helper_start_time != 0, "the helper should be running before the cancel");

  const Clock::time_point cancel_start = Clock::now();
  session.Cancel();
  const ChildEvent exit = AwaitExit(session, kTestTimeout);
  const auto cancel_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - cancel_start);

  ExpectTrue(exit.killed_by_us, "a cancelled session must report that it killed the child");
  ExpectTrue(cancel_elapsed < std::chrono::milliseconds(3000),
             "cancelling must stay bounded even when a helper ignores SIGTERM (elapsed=" +
                 std::to_string(cancel_elapsed.count()) + "ms)");
  ExpectTrue(WaitUntilHelperGone(helper_pid, helper_start_time),
             "a helper that ignores SIGTERM must not outlive the session");
}

/**
 * Destruction is the last owner of the group: it must stop every member and return, without waiting
 * on one that ignores SIGTERM.
 */
void TestDestructorStopsTheWholeGroup() {
  int helper_pid = -1;
  std::uint64_t helper_start_time = 0;
  const Clock::time_point teardown_start = Clock::now();
  {
    auto session = MustStart("( trap '' TERM; exec sleep 30 ) & sleep 0.2; echo $!; sleep 30");
    helper_pid = ReadHelperPid(session);
    ExpectTrue(helper_pid > 0, "the reported helper pid should be usable");
    helper_start_time = vinput::process::ProcessStartTime(helper_pid);
    ExpectTrue(helper_start_time != 0, "the helper should be running before the teardown");
  }
  const auto teardown_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - teardown_start);

  ExpectTrue(teardown_elapsed < std::chrono::milliseconds(3000),
             "destroying the session must be bounded (elapsed=" +
                 std::to_string(teardown_elapsed.count()) + "ms)");
  ExpectTrue(WaitUntilHelperGone(helper_pid, helper_start_time),
             "destroying the session must stop the whole process group");
}

/**
 * Closing stdin is an explicit end of input: the child must see every byte handed over before it,
 * and then EOF.
 */
void TestCloseStdinDeliversQueuedBytesBeforeEof() {
  auto session = MustStart("cat > /dev/null; echo done_after_eof");

  const std::string payload = "payload_without_newline";
  ExpectTrue(session.PushStdin(AsBytes(payload)), "the session should accept stdin");
  session.CloseStdin();
  ExpectTrue(!session.PushStdin(AsBytes(payload)), "a closed session must refuse further stdin");

  const std::string stdout_text = DrainUntilExit(session, nullptr, kTestTimeout);
  ExpectTrue(stdout_text.find("done_after_eof") != std::string::npos,
             "the child should reach EOF only after the queued bytes were written");
}

/**
 * Backpressure is a byte budget, not an event count: a provider that prints many small lines while
 * the caller is still busy must not lose the overflow - and above all must not lose the terminal
 * event, which is how the caller learns the child is gone.
 */
void TestManySmallEventsSurviveASlowCaller() {
  constexpr int kLineCount = 120;
  auto session =
      MustStart("i=0; while [ $i -lt 120 ]; do echo burst$i; sleep 0.001; i=$((i+1)); done");

  // Let the pump run ahead of the caller on purpose: a caller that has not asked for events yet is
  // exactly the situation a fixed-size event queue cannot survive.
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  const std::string stdout_text = DrainUntilExit(session, nullptr, kTestTimeout);
  std::string missing;
  for (int i = 0; i < kLineCount; ++i) {
    if (stdout_text.find("burst" + std::to_string(i) + "\n") == std::string::npos) {
      missing += " " + std::to_string(i);
    }
  }
  ExpectTrue(missing.empty(),
             "every line the provider printed must be delivered, missing:" + missing);
}

/**
 * A provider that closes its stdin before the caller writes must produce a failed write, not a
 * SIGPIPE that ends the host process: the session is one small part of the daemon, and losing the
 * whole daemon to a provider quirk is not an acceptable failure mode.
 */
void TestProviderClosingStdinDoesNotKillTheHost() {
  auto session = MustStart("exec 0<&-; echo ready; sleep 30");

  std::string stdout_text;
  const Clock::time_point deadline = Clock::now() + kTestTimeout;
  while (stdout_text.find("ready") == std::string::npos && Clock::now() < deadline) {
    ChildEvent event;
    if (!session.NextEvent(event, kEventWait)) {
      continue;
    }
    if (event.kind == ChildEventKind::StdoutChunk) {
      stdout_text.append(ToText(event.data));
    }
  }
  ExpectTrue(stdout_text.find("ready") != std::string::npos, "the child should announce itself");

  ExpectTrue(session.PushStdin(AsBytes("payload\n")), "the session should still accept the bytes");
  // The write happens on the pump thread; give it a moment to fail and be survived.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  session.Cancel();
  const ChildEvent exit = AwaitExit(session, kTestTimeout);
  ExpectTrue(exit.killed_by_us, "the session should still be cancellable after the failed write");
}

/**
 * The destructor is the last owner of the child: it has to stop it and reap it, because a zombie
 * out lives the session otherwise.
 */
void TestDestructorReapsTheChild() {
  int child_pid = -1;
  {
    auto session = MustStart("sleep 30");
    child_pid = session.Pid();
    ExpectTrue(child_pid > 0, "the session should report a usable pid");
    ExpectTrue(vinput::process::ProcessStartTime(child_pid) != 0, "the child should be running");
  }
  // A reaped child leaves no /proc entry behind; a zombie would still be listed there.
  ExpectTrue(vinput::process::ProcessStartTime(child_pid) == 0,
             "the destructor must reap the child instead of leaving a zombie");
}

} // namespace

int main() {
  std::cout << "Running test_process_session...\n";
  TestRoundTrip();
  TestOutputArrivesWithoutStdin();
  TestCancelInterruptsBlockedWait();
  TestCancelTerminatesTheProcessGroup();
  TestHelperIgnoringSigtermIsKilled();
  TestDestructorStopsTheWholeGroup();
  TestCloseStdinDeliversQueuedBytesBeforeEof();
  TestManySmallEventsSurviveASlowCaller();
  TestProviderClosingStdinDoesNotKillTheHost();
  TestDestructorReapsTheChild();
  std::cout << "All test_process_session passed!\n";
  return 0;
}
