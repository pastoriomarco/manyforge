#pragma once

#include <poll.h>

#include <array>
#include <cerrno>
#include <system_error>

namespace manyforge::headless::detail {

/// How a wait of one Manager local exchange ended (OD-296 (a), OD-306 (a)).
enum class LocalPeerWaitEnd {
  kReady,       ///< the awaited events are ready
  kPeerClosed,  ///< the peer closed or failed the connection
  kStopped,     ///< the caller's application stop came
};

/// The poll slice between two `stop_requested` checks. A slice never ends a
/// wait by itself.
inline constexpr int kLocalPeerStopRecheckMilliseconds = 20;

/// Waits for `events` on `fd` with no time limit (OD-296 (a), OD-306 (a)):
/// it ends on the events, on the peer closing or failing the connection, or
/// on the caller's stop. The stop is `stop_fd` becoming readable (an eventfd
/// that stays readable once written), or, where that signal is already
/// written for the whole stop (the application-stop Halt), `stop_requested`
/// returning true, checked between short poll slices. The stop is checked
/// first. A failed poll throws.
inline LocalPeerWaitEnd
WaitForLocalPeer(const int fd, const short events, const int stop_fd,
                 bool (*const stop_requested)() noexcept = nullptr) {
  std::array<pollfd, 2U> descriptors{{{fd, events, 0}, {stop_fd, POLLIN, 0}}};
  const nfds_t count = stop_fd >= 0 ? 2U : 1U;
  const int slice =
      stop_requested != nullptr ? kLocalPeerStopRecheckMilliseconds : -1;
  for (;;) {
    if (stop_requested != nullptr && stop_requested())
      return LocalPeerWaitEnd::kStopped;
    descriptors[0].revents = 0;
    descriptors[1].revents = 0;
    if (::poll(descriptors.data(), count, slice) < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(),
                              "local exchange wait failed");
    }
    if (count == 2U && (descriptors[1].revents & POLLIN) != 0)
      return LocalPeerWaitEnd::kStopped;
    if ((descriptors[0].revents & events) != 0) return LocalPeerWaitEnd::kReady;
    if ((descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
      return LocalPeerWaitEnd::kPeerClosed;
  }
}

}  // namespace manyforge::headless::detail
