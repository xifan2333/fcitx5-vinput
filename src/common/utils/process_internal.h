#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "common/utils/process_utils.h"

// Shared internals of the process module: everything the one-shot facade (`Run`) and the streaming
// session (`ProcessSession`) must agree on. Nothing outside src/common/utils/ includes this header;
// it exists so the two entry points cannot drift into separate copies of the same descriptor
// plumbing.
namespace vinput::process::detail {

// Events that mean a read is worth attempting on a channel: data, a closed peer, a broken pipe, or
// a descriptor that died while the pump was running. POLLNVAL has to be here because it is the only
// way an invalid descriptor gets closed: the read that follows fails with EBADF and reports the
// channel closed. Without it the channel stays open and poll() returns at once on every iteration,
// burning the CPU until the deadline.
constexpr short kReadableChannelEvents = POLLIN | POLLHUP | POLLERR | POLLNVAL;

// Events that end the input channel without a successful write, including an invalid descriptor.
constexpr short kInputFailureEvents = POLLHUP | POLLERR | POLLNVAL;

// Per-channel read budget for one pump iteration. Every channel gets its own, so a child that
// floods one stream cannot hold the other back: starvation would not just be slow - once the
// starved pipe fills, the child blocks in write() and both directions stall until the deadline.
constexpr std::size_t kReadBudgetBytes = std::size_t{64} * 1024;

// The command line and environment a `CommandSpec` resolves to. The environment is the parent's,
// with the spec's entries interpolated and overriding it.
std::vector<std::string> BuildCmd(const CommandSpec& spec);
std::map<std::string, std::string> BuildEnvMap(const CommandSpec& spec);

// Makes a write to a closed pipe fail with EPIPE instead of raising SIGPIPE, whose default action
// would end the whole process. A child that closes its stdin is a normal provider behaviour, and
// the process that spawned it must survive it. Idempotent and process-wide, because the signal is.
void EnsureSigpipeIgnored();

// Switches `fd` to non-blocking mode. poll() only announces readiness, so nothing may block
// afterwards: a descriptor that refuses to switch must be reported instead of being used.
bool MakeNonBlocking(int fd);

struct DrainOutcome {
  std::size_t bytes_read = 0;
  bool closed = false;
};

// Reads at most `budget` bytes from `fd` and forwards every chunk to `emit`, reporting how much was
// read and whether the peer closed the channel.
template <typename Emit> DrainOutcome DrainBudgeted(int fd, std::size_t budget, const Emit& emit) {
  DrainOutcome outcome;
  std::array<char, 4096> buffer{};

  while (outcome.bytes_read < budget) {
    const std::size_t want = std::min(buffer.size(), budget - outcome.bytes_read);
    const ssize_t n = ::read(fd, buffer.data(), want);
    if (n > 0) {
      const auto count = static_cast<std::size_t>(n);
      outcome.bytes_read += count;
      emit(std::string_view(buffer.data(), count));
      continue;
    }
    if (n == 0) {
      // Peer closed the write end.
      outcome.closed = true;
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      break;
    }
    outcome.closed = true;
    break;
  }

  return outcome;
}

} // namespace vinput::process::detail
