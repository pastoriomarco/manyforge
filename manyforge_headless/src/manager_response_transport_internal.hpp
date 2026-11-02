#pragma once

#include <sys/socket.h>

#include <algorithm>
#include <cstddef>

namespace manyforge::headless::detail {

/// Largest Manager response one send carries atomically on this connected
/// SOCK_SEQPACKET client, at most `maximum`. Raises the client's send buffer
/// toward `maximum` first: an accepted socket does not inherit the listener's,
/// and the kernel caps it at twice net.core.wmem_max. Linux refuses a unix
/// datagram longer than the send buffer minus 32 bytes.
inline std::size_t AtomicResponseCapacity(const int client,
                                          const std::size_t maximum) noexcept {
  const int requested = static_cast<int>(maximum);
  static_cast<void>(::setsockopt(client, SOL_SOCKET, SO_SNDBUF, &requested,
                                 sizeof(requested)));
  int configured = 0;
  socklen_t configured_size = sizeof(configured);
  if (::getsockopt(client, SOL_SOCKET, SO_SNDBUF, &configured,
                   &configured_size) != 0) {
    return 0U;
  }
  return std::min(maximum,
                  static_cast<std::size_t>(std::max(configured - 32, 0)));
}

}  // namespace manyforge::headless::detail
