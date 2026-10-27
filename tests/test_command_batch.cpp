#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "common/config/core_config_types.h"
#include "common/notify/notification_bridge.h"

#include "daemon/asr/backends/command_batch_backend.h"
#include "daemon/asr/runtime/recognition_contract.h"

namespace {

void ExpectTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ExpectTrue failed: " << message << "\n";
    std::abort();
  }
}

void TestEmptyCommandRejected() {
  CommandAsrProvider provider;
  provider.id = "test-empty";
  provider.command = "";

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error);
  ExpectTrue(backend == nullptr, "Empty command must fail creation");
  ExpectTrue(!error.empty(), "Error message must be populated on failure");
}

void TestNormalExecution() {
  CommandAsrProvider provider;
  provider.id = "test-batch-echo";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo 'recognized text from batch'"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandBatchBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  const std::vector<int16_t> pcm = {100, 200, 300};
  ExpectTrue(session->PushAudio(pcm, &error), "PushAudio failed: " + error);
  ExpectTrue(session->Finish(&error), "Finish failed: " + error);

  const auto events = session->PollEvents();
  ExpectTrue(events.size() == 2, "Expected 2 events, got " + std::to_string(events.size()));
  ExpectTrue(events[0].kind == vinput::daemon::asr::RecognitionEventKind::FinalText,
             "First event must be FinalText");
  ExpectTrue(events[0].text == "recognized text from batch", "Text mismatch: " + events[0].text);
  ExpectTrue(events[1].kind == vinput::daemon::asr::RecognitionEventKind::Completed,
             "Second event must be Completed");
}

void TestStderrPipedToNotificationBridge() {
  std::vector<vinput::notify::Notification> received;
  vinput::notify::NotificationBridge bridge([&](const auto& n) { received.push_back(n); });

  CommandAsrProvider provider;
  provider.id = "test-batch-stderr";
  provider.command = "/bin/sh";
  provider.args = {
      "-c",
      "echo '{\"title\": \"Batch Warning\", \"body\": \"Token low\"}' >&2\n"
      "echo 'Verbatim batch notice' >&2\n"
      "echo 'hello batch'\n",
  };
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error, &bridge);
  ExpectTrue(backend != nullptr, "CreateCommandBatchBackend failed: " + error);

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed: " + error);

  ExpectTrue(session->Finish(&error), "Finish failed: " + error);

  const auto events = session->PollEvents();
  ExpectTrue(!events.empty() &&
                 events[0].kind == vinput::daemon::asr::RecognitionEventKind::FinalText,
             "Must produce final text");
  ExpectTrue(events[0].text == "hello batch", "Output mismatch");

  ExpectTrue(received.size() == 2,
             "Expected 2 notifications, got " + std::to_string(received.size()));
  ExpectTrue(received[0].title == "Batch Warning", "JSON notification title mismatch");
  ExpectTrue(received[0].body == "Token low", "JSON notification body mismatch");
  ExpectTrue(received[1].body == "Verbatim batch notice", "Plain text notification body mismatch");
}

void TestCommandFailure() {
  CommandAsrProvider provider;
  provider.id = "test-batch-fail";
  provider.command = "/bin/sh";
  provider.args = {"-c", "exit 42"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandBatchBackend failed");

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");

  const bool ok = session->Finish(&error);
  ExpectTrue(!ok, "Finish must fail on exit code != 0");
  ExpectTrue(!error.empty(), "Error message must be set");

  const auto events = session->PollEvents();
  ExpectTrue(events.size() == 2, "Expected Error and Completed events");
  ExpectTrue(events[0].kind == vinput::daemon::asr::RecognitionEventKind::Error,
             "First event must be Error");
  ExpectTrue(events[1].kind == vinput::daemon::asr::RecognitionEventKind::Completed,
             "Second event must be Completed");
}

void TestCommandTimeout() {
  CommandAsrProvider provider;
  provider.id = "test-batch-timeout";
  provider.command = "/bin/sh";
  provider.args = {"-c", "sleep 5"};
  provider.timeoutMs = 100;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandBatchBackend failed");

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");

  const bool ok = session->Finish(&error);
  ExpectTrue(!ok, "Finish must fail on timeout");
  ExpectTrue(!error.empty(), "Error message must be set");

  const auto events = session->PollEvents();
  ExpectTrue(!events.empty() && events[0].kind == vinput::daemon::asr::RecognitionEventKind::Error,
             "First event must be Error");
}

void TestEmptyOutputIsError() {
  CommandAsrProvider provider;
  provider.id = "test-batch-empty-output";
  provider.command = "/bin/sh";
  provider.args = {"-c", "true"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandBatchBackend failed");

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");

  const bool ok = session->Finish(&error);
  ExpectTrue(!ok, "Finish must fail on empty output");

  const auto events = session->PollEvents();
  ExpectTrue(!events.empty() && events[0].kind == vinput::daemon::asr::RecognitionEventKind::Error,
             "First event must be Error");
}

void TestCancel() {
  CommandAsrProvider provider;
  provider.id = "test-batch-cancel";
  provider.command = "/bin/sh";
  provider.args = {"-c", "echo 'should not matter'"};
  provider.timeoutMs = 5000;

  std::string error;
  auto backend = vinput::daemon::asr::CreateCommandBatchBackend(provider, &error);
  ExpectTrue(backend != nullptr, "CreateCommandBatchBackend failed");

  auto session = backend->CreateSession(&error);
  ExpectTrue(session != nullptr, "CreateSession failed");

  session->Cancel();
  const auto events = session->PollEvents();
  ExpectTrue(events.size() == 1, "Expected 1 event after cancel");
  ExpectTrue(events[0].kind == vinput::daemon::asr::RecognitionEventKind::Completed,
             "Event must be Completed");

  // Subsequent Finish should be a no-op
  ExpectTrue(session->Finish(&error), "Finish after cancel should succeed as no-op");
}

} // namespace

int main() {
  std::cout << "Running command batch tests...\n";
  TestEmptyCommandRejected();
  TestNormalExecution();
  TestStderrPipedToNotificationBridge();
  TestCommandFailure();
  TestCommandTimeout();
  TestEmptyOutputIsError();
  TestCancel();
  std::cout << "All command batch tests passed successfully!\n";
  return 0;
}
