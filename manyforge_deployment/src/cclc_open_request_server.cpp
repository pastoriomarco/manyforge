#include <manyforge_deployment/cclc_open_request_server.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr int kSocketBacklog = 64;
constexpr int kPollMilliseconds = 100;
constexpr int kClientDeadlineMilliseconds = 2'000;

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value = -1) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        static_cast<void>(::close(value_));
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_;
};

class BoundSocketPathGuard final {
public:
  explicit BoundSocketPathGuard(std::filesystem::path path)
      : path_(std::move(path)) {}

  BoundSocketPathGuard(const BoundSocketPathGuard &) = delete;
  BoundSocketPathGuard &operator=(const BoundSocketPathGuard &) = delete;

  ~BoundSocketPathGuard() {
    if (!armed_) {
      return;
    }
    struct stat current {};
    if (::lstat(path_.c_str(), &current) == 0 && current.st_dev == device_ &&
        current.st_ino == inode_ && S_ISSOCK(current.st_mode) &&
        !S_ISLNK(current.st_mode)) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

  void Arm(const struct stat &status) noexcept {
    device_ = status.st_dev;
    inode_ = status.st_ino;
    armed_ = true;
  }

  void Release() noexcept { armed_ = false; }

private:
  std::filesystem::path path_;
  dev_t device_{};
  ino_t inode_{};
  bool armed_{false};
};

[[nodiscard]] core::ErrorDTO server_error(std::string reason,
                                          std::string message) {
  return {"cclc_open_transport_unavailable",
          std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool socket_deadline_ready(const int descriptor,
                                         const short events) noexcept {
  pollfd row{descriptor, events, 0};
  while (true) {
    const int result = ::poll(&row, 1U, kClientDeadlineMilliseconds);
    if (result > 0) {
      return (row.revents & events) != 0 &&
             (row.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
    }
    if (result == 0) {
      return false;
    }
    if (errno != EINTR) {
      return false;
    }
  }
}

} // namespace

class CCLCOpenRequestUnixServer::Impl final {
public:
  Impl(std::filesystem::path state_root, core::ICCLCOpenRequestPort &port,
       const core::RequestTransportDTO transport)
      : state_root_(std::move(state_root)),
        socket_path_(state_root_ /
                     std::string(transport == core::RequestTransportDTO::kHttp
                                     ? kCCLCOpenRequestSocketFilename
                                     : kCCLCOpenRequestRosSocketFilename)),
        port_(port), transport_(transport) {
    if (transport_ != core::RequestTransportDTO::kHttp &&
        transport_ != core::RequestTransportDTO::kRos) {
      throw std::invalid_argument(
          "CCLC open transport must be exactly HTTP or ROS");
    }
    struct stat root_status {};
    if (::lstat(state_root_.c_str(), &root_status) != 0 ||
        !S_ISDIR(root_status.st_mode) || S_ISLNK(root_status.st_mode)) {
      throw std::invalid_argument(
          "CCLC transport state root is not a pre-existing real directory");
    }
    const auto spelling = socket_path_.string();
    if (spelling.size() >= sizeof(sockaddr_un::sun_path)) {
      throw std::invalid_argument("CCLC transport socket path is too long");
    }

    struct stat existing {};
    if (::lstat(socket_path_.c_str(), &existing) == 0) {
      if (!S_ISSOCK(existing.st_mode) || S_ISLNK(existing.st_mode) ||
          existing.st_uid != ::geteuid()) {
        throw std::invalid_argument(
            "CCLC transport path exists and is not a same-user socket");
      }
      if (::unlink(socket_path_.c_str()) != 0) {
        throw std::runtime_error("cannot remove stale CCLC transport socket");
      }
    } else if (errno != ENOENT) {
      throw std::runtime_error("cannot inspect CCLC transport socket path");
    }

    socket_ = FileDescriptor(
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (socket_.get() < 0) {
      throw std::runtime_error("cannot create CCLC transport socket");
    }
    int buffer_bytes = static_cast<int>(core::kMaximumCCLCOpenResponseBytes);
    static_cast<void>(::setsockopt(socket_.get(), SOL_SOCKET, SO_SNDBUF,
                                   &buffer_bytes, sizeof(buffer_bytes)));
    buffer_bytes = static_cast<int>(core::kMaximumCCLCOpenRequestBytes);
    static_cast<void>(::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVBUF,
                                   &buffer_bytes, sizeof(buffer_bytes)));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
    if (::bind(socket_.get(), reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0) {
      throw std::runtime_error("cannot bind CCLC transport socket");
    }
    BoundSocketPathGuard bound_path_guard(socket_path_);
    struct stat bound {};
    if (::lstat(socket_path_.c_str(), &bound) != 0 ||
        !S_ISSOCK(bound.st_mode) || S_ISLNK(bound.st_mode) ||
        bound.st_uid != ::geteuid()) {
      static_cast<void>(::unlink(socket_path_.c_str()));
      throw std::runtime_error("CCLC transport socket readback failed");
    }
    bound_path_guard.Arm(bound);
    if (::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
      throw std::runtime_error("cannot restrict CCLC transport socket");
    }
    if (::lstat(socket_path_.c_str(), &bound) != 0 ||
        !S_ISSOCK(bound.st_mode) || S_ISLNK(bound.st_mode) ||
        bound.st_uid != ::geteuid()) {
      throw std::runtime_error("CCLC transport socket readback failed");
    }
    socket_device_ = bound.st_dev;
    socket_inode_ = bound.st_ino;
    if (::listen(socket_.get(), kSocketBacklog) != 0) {
      throw std::runtime_error("cannot listen on CCLC transport socket");
    }
    worker_ = std::jthread([this](std::stop_token token) { Run(token); });
    bound_path_guard.Release();
  }

  ~Impl() {
    worker_.request_stop();
    if (socket_.get() >= 0) {
      static_cast<void>(::shutdown(socket_.get(), SHUT_RDWR));
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    struct stat current {};
    if (::lstat(socket_path_.c_str(), &current) == 0 &&
        current.st_dev == socket_device_ && current.st_ino == socket_inode_ &&
        S_ISSOCK(current.st_mode) && !S_ISLNK(current.st_mode)) {
      static_cast<void>(::unlink(socket_path_.c_str()));
    }
  }

  [[nodiscard]] const std::filesystem::path &socket_path() const noexcept {
    return socket_path_;
  }

private:
  void Run(const std::stop_token token) noexcept {
    std::vector<char> request(core::kMaximumCCLCOpenRequestBytes);
    while (!token.stop_requested()) {
      pollfd row{socket_.get(), POLLIN, 0};
      const int ready = ::poll(&row, 1U, kPollMilliseconds);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        failed_.store(true, std::memory_order_release);
        return;
      }
      if (ready == 0) {
        continue;
      }
      if ((row.revents & POLLIN) == 0) {
        if (!token.stop_requested()) {
          failed_.store(true, std::memory_order_release);
        }
        return;
      }
      FileDescriptor client(::accept4(socket_.get(), nullptr, nullptr,
                                      SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (client.get() < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        failed_.store(true, std::memory_order_release);
        return;
      }
      Handle(client.get(), request);
    }
  }

  void Handle(const int client, std::vector<char> &request) noexcept {
    if (!socket_deadline_ready(client, POLLIN)) {
      return;
    }
    iovec vector{request.data(), request.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1U;
    const ssize_t count = ::recvmsg(client, &message, 0);
    if (count <= 0 || (message.msg_flags & MSG_TRUNC) != 0) {
      return;
    }
    const auto response = core::ExecuteCanonicalCCLCOpenRequest(
        std::string_view(request.data(), static_cast<std::size_t>(count)),
        port_,
        {transport_, core::RequestAssuranceDTO::kUnverified, std::nullopt});
    if (!response.ok() || response.value().empty() ||
        response.value().size() > core::kMaximumCCLCOpenResponseBytes ||
        !socket_deadline_ready(client, POLLOUT)) {
      return;
    }
    static_cast<void>(::send(client, response.value().data(),
                             response.value().size(), MSG_NOSIGNAL));
  }

  std::filesystem::path state_root_;
  std::filesystem::path socket_path_;
  core::ICCLCOpenRequestPort &port_;
  core::RequestTransportDTO transport_;
  FileDescriptor socket_;
  dev_t socket_device_{};
  ino_t socket_inode_{};
  std::atomic<bool> failed_{false};
  std::jthread worker_;
};

core::Result<std::unique_ptr<CCLCOpenRequestUnixServer>>
CCLCOpenRequestUnixServer::CreateAndStart(
    const std::filesystem::path &trusted_state_root,
    core::ICCLCOpenRequestPort &port,
    const core::RequestTransportDTO transport) {
  try {
    return core::Result<std::unique_ptr<CCLCOpenRequestUnixServer>>::Ok(
        std::unique_ptr<CCLCOpenRequestUnixServer>(
            new CCLCOpenRequestUnixServer(
                std::make_unique<Impl>(trusted_state_root, port, transport))));
  } catch (const std::exception &exception) {
    return core::Result<std::unique_ptr<CCLCOpenRequestUnixServer>>::Err(
        server_error("construction_failed", exception.what()));
  } catch (...) {
    return core::Result<std::unique_ptr<CCLCOpenRequestUnixServer>>::Err(
        server_error("construction_failed",
                     "CCLC transport construction failed"));
  }
}

CCLCOpenRequestUnixServer::CCLCOpenRequestUnixServer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

CCLCOpenRequestUnixServer::~CCLCOpenRequestUnixServer() = default;

const std::filesystem::path &
CCLCOpenRequestUnixServer::socket_path() const noexcept {
  return impl_->socket_path();
}

} // namespace manyforge::deployment
