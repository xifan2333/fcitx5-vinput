#include <cassert>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/notify/notification_bridge.h"

namespace {

using vinput::notify::Notification;
using vinput::notify::NotificationBridge;

std::span<const std::byte> AsBytes(std::string_view str) {
  return std::as_bytes(std::span(str.data(), str.size()));
}

void TestPlainTextNotification() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  bridge.DeliverLine("provider1", "Hello from helper");
  assert(delivered.size() == 1);
  assert(delivered[0].body == "Hello from helper");
  assert(!delivered[0].title.empty());

  bridge.DeliverLine("provider2", "Another alert", "Custom Provider");
  assert(delivered.size() == 2);
  assert(delivered[1].body == "Another alert");
  assert(delivered[1].title == "Custom Provider");

  // Blank lines should be ignored
  bridge.DeliverLine("provider1", "   \t  ");
  assert(delivered.size() == 2);
}

void TestJsonNotification() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  bridge.DeliverLine("adapter1", R"({"title": "Bailian ASR", "body": "Token expired"})");
  assert(delivered.size() == 1);
  assert(delivered[0].title == "Bailian ASR");
  assert(delivered[0].body == "Token expired");

  // Broken JSON falls back to verbatim body
  bridge.DeliverLine("adapter1", R"({"title": incomplete json)");
  assert(delivered.size() == 2);
  assert(delivered[1].body == R"({"title": incomplete json)");
}

void TestChunkedStreamingAndFlush() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  // Chunk 1: incomplete line
  bridge.FeedStderr("proc1", AsBytes("Part 1 "));
  assert(delivered.empty());

  // Chunk 2: completes first line and has a second line
  bridge.FeedStderr("proc1", AsBytes("Part 2\nSecond line\n"));
  assert(delivered.size() == 2);
  assert(delivered[0].body == "Part 1 Part 2");
  assert(delivered[1].body == "Second line");

  // Chunk 3: CRLF line
  bridge.FeedStderr("proc1", AsBytes("Third CRLF line\r\n"));
  assert(delivered.size() == 3);
  assert(delivered[2].body == "Third CRLF line");

  // Chunk 4: trailing unterminated line flushed on process exit
  bridge.FeedStderr("proc1", AsBytes("Final trailing text"));
  assert(delivered.size() == 3);
  bridge.Flush("proc1");
  assert(delivered.size() == 4);
  assert(delivered[3].body == "Final trailing text");
}

void TestMultiSourceIsolation() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  bridge.FeedStderr("srcA", AsBytes("A_part1 "));
  bridge.FeedStderr("srcB", AsBytes("B_part1 "));
  bridge.FeedStderr("srcA", AsBytes("A_part2\n"));
  bridge.FeedStderr("srcB", AsBytes("B_part2\n"));

  assert(delivered.size() == 2);
  assert(delivered[0].body == "A_part1 A_part2");
  assert(delivered[1].body == "B_part1 B_part2");

  bridge.RetireSource("srcA");
  bridge.RetireSource("srcB");
}

void TestSlidingWindowDeduplication() {
  std::vector<Notification> delivered;
  auto simulated_time = std::chrono::steady_clock::now();

  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); },
                            std::chrono::milliseconds(3000), [&]() { return simulated_time; });

  // First delivery: succeeds
  bridge.DeliverLine("proc", "Connection error");
  assert(delivered.size() == 1);

  // Immediate repeat: suppressed
  bridge.DeliverLine("proc", "Connection error");
  bridge.DeliverLine("proc", "Connection error");
  assert(delivered.size() == 1);

  // Advance time by 1000ms: still within 3000ms window, suppressed
  simulated_time += std::chrono::milliseconds(1000);
  bridge.DeliverLine("proc", "Connection error");
  assert(delivered.size() == 1);

  // Different message: not suppressed
  bridge.DeliverLine("proc", "Different warning");
  assert(delivered.size() == 2);
  assert(delivered[1].body == "Different warning");

  // Advance time past 3000ms window: original message allowed again
  simulated_time += std::chrono::milliseconds(3001);
  bridge.DeliverLine("proc", "Connection error");
  assert(delivered.size() == 3);
  assert(delivered[2].body == "Connection error");

  // ResetDedup clears cache
  bridge.DeliverLine("proc", "Connection error");
  assert(delivered.size() == 3); // suppressed
  bridge.ResetDedup();
  bridge.DeliverLine("proc", "Connection error");
  assert(delivered.size() == 4); // delivered after reset
}

} // namespace

int main() {
  TestPlainTextNotification();
  TestJsonNotification();
  TestChunkedStreamingAndFlush();
  TestMultiSourceIsolation();
  TestSlidingWindowDeduplication();
  return 0;
}
