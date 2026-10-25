#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/notify/notification_bridge.h"

namespace {

using vinput::notify::Notification;
using vinput::notify::NotificationBridge;

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

std::span<const std::byte> AsBytes(std::string_view str) {
  return std::as_bytes(std::span(str.data(), str.size()));
}

void TestPlainTextNotification() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  bridge.DeliverLine("provider1", "Hello from helper");
  ExpectTrue(delivered.size() == 1, "plain text delivered");
  ExpectTrue(delivered[0].body == "Hello from helper", "plain text body matches");
  ExpectTrue(!delivered[0].title.empty(), "plain text default title present");

  bridge.DeliverLine("provider2", "Another alert", "Custom Provider");
  ExpectTrue(delivered.size() == 2, "second alert delivered");
  ExpectTrue(delivered[1].body == "Another alert", "custom title body matches");
  ExpectTrue(delivered[1].title == "Custom Provider", "custom title matches");

  // Verbatim contract: whitespace in non-empty lines must be preserved
  bridge.DeliverLine("provider3", "  indented message  ");
  ExpectTrue(delivered.size() == 3, "whitespace line delivered");
  ExpectTrue(delivered[2].body == "  indented message  ", "verbatim whitespace preserved");

  // Blank lines should be ignored
  bridge.DeliverLine("provider1", "   \t  ");
  ExpectTrue(delivered.size() == 3, "blank line ignored");
}

void TestJsonNotification() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  // Valid JSON with both title and body
  bridge.DeliverLine("adapter1", R"({"title": "Bailian ASR", "body": "Token expired"})");
  ExpectTrue(delivered.size() == 1, "json delivered");
  ExpectTrue(delivered[0].title == "Bailian ASR", "json title matches");
  ExpectTrue(delivered[0].body == "Token expired", "json body matches");

  // Explicit empty body in JSON should be preserved
  bridge.DeliverLine("adapter2", R"({"title": "Bailian ASR", "body": ""})");
  ExpectTrue(delivered.size() == 2, "json with empty body delivered");
  ExpectTrue(delivered[1].title == "Bailian ASR", "json empty body title matches");
  ExpectTrue(delivered[1].body.empty(), "json empty body preserved");

  // JSON missing body field falls back to verbatim plain text
  bridge.DeliverLine("adapter3", R"({"title": "Incomplete"})");
  ExpectTrue(delivered.size() == 3, "incomplete json delivered as plain text");
  ExpectTrue(delivered[2].body == R"({"title": "Incomplete"})", "incomplete json verbatim body");

  // Broken JSON falls back to verbatim body
  bridge.DeliverLine("adapter4", R"({"title": broken json)");
  ExpectTrue(delivered.size() == 4, "broken json delivered as plain text");
  ExpectTrue(delivered[3].body == R"({"title": broken json)", "broken json verbatim body");
}

void TestChunkedStreamingAndFlush() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  // Chunk 1: incomplete line
  bridge.FeedStderr("proc1", AsBytes("Part 1 "));
  ExpectTrue(delivered.empty(), "incomplete chunk buffered");

  // Chunk 2: completes first line and has a second line
  bridge.FeedStderr("proc1", AsBytes("Part 2\nSecond line\n"));
  ExpectTrue(delivered.size() == 2, "two lines delivered from chunk 2");
  ExpectTrue(delivered[0].body == "Part 1 Part 2", "chunked line concatenated");
  ExpectTrue(delivered[1].body == "Second line", "second line delivered");

  // Chunk 3: CRLF line
  bridge.FeedStderr("proc1", AsBytes("Third CRLF line\r\n"));
  ExpectTrue(delivered.size() == 3, "crlf line delivered");
  ExpectTrue(delivered[2].body == "Third CRLF line", "crlf line stripped carriage return");

  // Chunk 4: trailing unterminated line flushed on process exit
  bridge.FeedStderr("proc1", AsBytes("Final trailing text"));
  ExpectTrue(delivered.size() == 3, "trailing text buffered until flush");
  bridge.Flush("proc1");
  ExpectTrue(delivered.size() == 4, "flushed trailing text delivered");
  ExpectTrue(delivered[3].body == "Final trailing text", "flushed body matches");
}

void TestMultiSourceIsolation() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  bridge.FeedStderr("srcA", AsBytes("A_part1 "));
  bridge.FeedStderr("srcB", AsBytes("B_part1 "));
  bridge.FeedStderr("srcA", AsBytes("A_part2\n"));
  bridge.FeedStderr("srcB", AsBytes("B_part2\n"));

  ExpectTrue(delivered.size() == 2, "multi-source lines delivered");
  ExpectTrue(delivered[0].body == "A_part1 A_part2", "srcA line isolated");
  ExpectTrue(delivered[1].body == "B_part1 B_part2", "srcB line isolated");

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
  ExpectTrue(delivered.size() == 1, "initial message delivered");

  // Immediate repeat: suppressed
  bridge.DeliverLine("proc", "Connection error");
  bridge.DeliverLine("proc", "Connection error");
  ExpectTrue(delivered.size() == 1, "repeated messages suppressed");

  // Advance time by 1000ms: still within 3000ms window, suppressed
  simulated_time += std::chrono::milliseconds(1000);
  bridge.DeliverLine("proc", "Connection error");
  ExpectTrue(delivered.size() == 1, "repeat within window suppressed");

  // Different message: not suppressed
  bridge.DeliverLine("proc", "Different warning");
  ExpectTrue(delivered.size() == 2, "different message delivered");
  ExpectTrue(delivered[1].body == "Different warning", "different message body matches");

  // Advance time past 3000ms window: original message allowed again
  simulated_time += std::chrono::milliseconds(3001);
  bridge.DeliverLine("proc", "Connection error");
  ExpectTrue(delivered.size() == 3, "message after window delivered");
  ExpectTrue(delivered[2].body == "Connection error", "message body matches");

  // ResetDedup clears cache
  bridge.DeliverLine("proc", "Connection error");
  ExpectTrue(delivered.size() == 3, "repeat suppressed before reset");
  bridge.ResetDedup();
  bridge.DeliverLine("proc", "Connection error");
  ExpectTrue(delivered.size() == 4, "delivered immediately after ResetDedup");
}

void TestOversizedBufferProtection() {
  std::vector<Notification> delivered;
  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); });

  // Write a 70 KiB string without any newline to test buffer overflow protection
  const std::string huge_chunk(std::size_t{70} * 1024, 'X');
  bridge.FeedStderr("runaway_helper", AsBytes(huge_chunk));

  // Should have triggered force-split / drain to protect buffer memory
  ExpectTrue(!delivered.empty(), "oversized chunk without newlines force-drained");
  ExpectTrue(delivered[0].body.size() == std::size_t{64} * 1024, "force-drained line size bounded");

  bridge.Flush("runaway_helper");
  ExpectTrue(delivered.size() == 2, "tail chunk delivered on flush");
  ExpectTrue(delivered[1].body.size() == std::size_t{6} * 1024, "tail size matches");
}

void TestDedupCacheHardCapEviction() {
  std::vector<Notification> delivered;
  auto simulated_time = std::chrono::steady_clock::now();

  NotificationBridge bridge([&](const Notification& n) { delivered.push_back(n); },
                            std::chrono::milliseconds(10000), [&]() { return simulated_time; });

  // Deliver 300 unique notifications at the same timestamp
  for (int i = 0; i < 300; ++i) {
    bridge.DeliverLine("flooder", "unique_msg_" + std::to_string(i));
  }
  ExpectTrue(delivered.size() == 300, "all 300 unique messages delivered");
}

} // namespace

int main() {
  TestPlainTextNotification();
  TestJsonNotification();
  TestChunkedStreamingAndFlush();
  TestMultiSourceIsolation();
  TestSlidingWindowDeduplication();
  TestOversizedBufferProtection();
  TestDedupCacheHardCapEviction();
  return 0;
}
