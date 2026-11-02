#include "../src/manager_response_transport_internal.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>

namespace manyforge::headless::detail {
namespace {

// The Manager raised SO_SNDBUF only on its listener, which an accepted socket
// never inherits, so a response above the default 212992-byte send buffer
// failed to send and the client saw the connection close. The capacity is
// configured and measured on the accepted client itself, and a response of
// exactly that size crosses the real socket in one packet.
TEST(ManagerResponseTransportTest, AcceptedClientCarriesItsAtomicResponseCapacity) {
  constexpr std::size_t kMaximum = 4U * 1024U * 1024U;
  const auto root = std::filesystem::temp_directory_path() /
                    ("mf-manager-capacity-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  const auto path = (root / "manager.sock").string();
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  const int listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  ASSERT_GE(client, 0);
  const int requested = static_cast<int>(kMaximum);
  ASSERT_EQ(::setsockopt(listener, SOL_SOCKET, SO_SNDBUF, &requested,
                         sizeof(requested)), 0);
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 1), 0);
  ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&address),
                      sizeof(address)), 0);
  const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
  ASSERT_GE(accepted, 0);
  int initial = 0;
  socklen_t initial_size = sizeof(initial);
  ASSERT_EQ(::getsockopt(accepted, SOL_SOCKET, SO_SNDBUF, &initial,
                         &initial_size), 0);

  const auto capacity = AtomicResponseCapacity(accepted, kMaximum);
  EXPECT_GT(capacity, static_cast<std::size_t>(initial) - 32U);
  EXPECT_LE(capacity, kMaximum);
  const std::string payload(capacity, 'x');
  EXPECT_EQ(::send(accepted, payload.data(), payload.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(payload.size()));
  std::string received(kMaximum, '\0');
  EXPECT_EQ(::recv(client, received.data(), received.size(), 0),
            static_cast<ssize_t>(payload.size()));
  if (capacity < kMaximum) {
    const std::string oversized(capacity + 1U, 'x');
    EXPECT_EQ(::send(accepted, oversized.data(), oversized.size(), MSG_NOSIGNAL), -1);
    EXPECT_EQ(errno, EMSGSIZE);
  }
  EXPECT_EQ(AtomicResponseCapacity(accepted, 1024U), 1024U);
  ::close(accepted);
  ::close(client);
  ::close(listener);
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace manyforge::headless::detail
