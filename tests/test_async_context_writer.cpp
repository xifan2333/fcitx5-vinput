#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "core/context_writer.h"

namespace {
void Check(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << "Async context journal test failed: " << reason << '\n';
    std::abort();
  }
}

std::vector<std::string> ReadLines(const std::filesystem::path& file) {
  std::vector<std::string> result;
  std::ifstream stream(file);
  std::string line;
  while (std::getline(stream, line)) {
    result.push_back(std::move(line));
  }
  return result;
}

void TestOrderedPersistenceAndRotation(const std::filesystem::path& path) {
  vinput::addon::AsyncContextWriter writer(path);
  for (int i = 0; i < 105; ++i) {
    const auto text = std::string(R"({"source":"user","text":")") + std::to_string(i) + R"("}")";
    Check(writer.Enqueue(text, 10), "normal queue must accept each row");
  }
  Check(writer.WaitUntilIdle(std::chrono::seconds(5)), "worker drained queued writes");
  const auto lines = ReadLines(path);
  // Identical to the previous policy: trim on the 100th append, then five
  // more records may accumulate before the next trim.
  Check(lines.size() == 15, "100-entry trim cycle preserved");
  Check(lines.front().find("\"90\"") != std::string::npos, "retained oldest recent row");
  Check(lines.back().find("\"104\"") != std::string::npos, "most recent row available");
}

void TestShutdownDrainsAndBoundedQueue(const std::filesystem::path& path) {
  {
    vinput::addon::AsyncContextWriter writer(path);
    const std::string too_large(5ULL * 1024ULL * 1024ULL, 'x');
    Check(!writer.Enqueue(too_large, 30), "oversized entry rejected without blocking");
    Check(writer.DroppedEntries() == 1, "overflow explicitly counted");
    Check(writer.Enqueue(R"({"source":"asr","text":"last"})", 30),
          "normal entry still accepted after overflow");
  }
  const auto lines = ReadLines(path);
  Check(lines.size() == 1, "destructor must finish the pending write");
  Check(lines[0].find("last") != std::string::npos, "ASR entry persisted after teardown");
}
} // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("vinput-context-writer-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  TestOrderedPersistenceAndRotation(dir / "history.jsonl");
  TestShutdownDrainsAndBoundedQueue(dir / "shutdown.jsonl");
  std::filesystem::remove_all(dir);
  std::cout << "Async context writer tests passed\n";
}
