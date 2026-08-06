#include <manyforge_roboplan/process_contained_candidate_client.hpp>

#include "roboplan_worker_protocol.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

extern char** environ;

namespace manyforge::roboplan_backend {
namespace {

using Clock = std::chrono::steady_clock;
using worker_protocol::Frame;
using worker_protocol::MessageType;

constexpr std::size_t kHeaderBytes = 16U;
constexpr std::chrono::milliseconds kPollSlice{20};

[[nodiscard]] core::ErrorDTO error(
    core::ErrorCode code, std::string message, std::string reason,
    std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  return {std::string(core::to_string(code)), std::move(message),
          std::move(details)};
}

class UniqueFd final {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int descriptor) : descriptor_(descriptor) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.descriptor_, -1));
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return descriptor_ >= 0;
  }
  [[nodiscard]] int release() noexcept {
    return std::exchange(descriptor_, -1);
  }
  void reset(int replacement = -1) noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    descriptor_ = replacement;
  }

 private:
  int descriptor_{-1};
};

enum class RequestFailureKind {
  kCancelled,
  kTimeout,
  kTransport,
  kProtocol,
};

class RequestFailure final : public std::runtime_error {
 public:
  RequestFailure(RequestFailureKind kind, std::string detail)
      : std::runtime_error(std::move(detail)), kind(kind) {}
  RequestFailureKind kind;
};

class ProcessSignalHandle final {
 public:
  explicit ProcessSignalHandle(UniqueFd pidfd) : pidfd_(std::move(pidfd)) {}

  void kill() const noexcept {
#if defined(SYS_pidfd_send_signal)
    if (pidfd_) {
      static_cast<void>(
          ::syscall(SYS_pidfd_send_signal, pidfd_.get(), SIGKILL, nullptr, 0));
    }
#endif
  }

 private:
  UniqueFd pidfd_;
};

struct ActiveRequest final {
  std::atomic_bool cancelled{false};
  std::atomic<std::shared_ptr<ProcessSignalHandle>> process;
};

struct WorkerProcess final {
  pid_t pid{-1};
  UniqueFd socket;
  std::shared_ptr<ProcessSignalHandle> signal;
  bool reaped{false};
};

[[nodiscard]] UniqueFd open_pidfd(pid_t pid) {
#if defined(SYS_pidfd_open)
  const auto descriptor = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
  if (descriptor < 0) {
    throw std::runtime_error("pidfd_open failed: " +
                             std::string(std::strerror(errno)));
  }
  const int flags = ::fcntl(descriptor, F_GETFD);
  if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0) {
    const int saved = errno;
    static_cast<void>(::close(descriptor));
    throw std::runtime_error("could not seal pidfd: " +
                             std::string(std::strerror(saved)));
  }
  return UniqueFd(descriptor);
#else
  static_cast<void>(pid);
  throw std::runtime_error("pidfd_open is unavailable on this platform");
#endif
}

void make_nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    throw std::runtime_error("could not make worker transport nonblocking: " +
                             std::string(std::strerror(errno)));
  }
}

[[nodiscard]] int bounded_poll_timeout(Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(
      std::max<std::int64_t>(1, std::min<std::int64_t>(
                                    kPollSlice.count(), remaining.count())));
}

void check_request(const std::shared_ptr<ActiveRequest>& request,
                   Clock::time_point deadline) {
  if (request && request->cancelled.load(std::memory_order_acquire)) {
    throw RequestFailure(RequestFailureKind::kCancelled,
                         "worker request was cancelled");
  }
  if (Clock::now() >= deadline) {
    throw RequestFailure(RequestFailureKind::kTimeout,
                         "worker request exceeded its deadline");
  }
}

void wait_ready(int descriptor, short events,
                const std::shared_ptr<ActiveRequest>& request,
                Clock::time_point deadline) {
  for (;;) {
    check_request(request, deadline);
    pollfd item{descriptor, events, 0};
    int status = 0;
    do {
      status = ::poll(&item, 1U, bounded_poll_timeout(deadline));
    } while (status < 0 && errno == EINTR);
    if (status < 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker transport poll failed");
    }
    if (status == 0) {
      continue;
    }
    // request_cancel() publishes cancellation before signalling the exact
    // pidfd. Prefer that causal state over the resulting socket HUP so the
    // public outcome remains deterministic across scheduler interleavings.
    check_request(request, deadline);
    if ((item.revents & (POLLERR | POLLNVAL)) != 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker transport became invalid");
    }
    if ((item.revents & events) != 0) {
      return;
    }
    if ((item.revents & POLLHUP) != 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker process closed its transport");
    }
  }
}

void write_controlled(int descriptor, const void* input, std::size_t size,
                      const std::shared_ptr<ActiveRequest>& request,
                      Clock::time_point deadline) {
  const auto* bytes = static_cast<const char*>(input);
  std::size_t offset = 0U;
  while (offset < size) {
    wait_ready(descriptor, POLLOUT, request, deadline);
    const auto count = ::send(descriptor, bytes + offset, size - offset,
                              MSG_NOSIGNAL | MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                      errno == EWOULDBLOCK)) {
      continue;
    }
    if (count < 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker transport write failed");
    }
    if (count == 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker transport write made no progress");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void read_controlled(int descriptor, void* output, std::size_t size,
                     const std::shared_ptr<ActiveRequest>& request,
                     Clock::time_point deadline) {
  auto* bytes = static_cast<char*>(output);
  std::size_t offset = 0U;
  while (offset < size) {
    wait_ready(descriptor, POLLIN, request, deadline);
    const auto count =
        ::recv(descriptor, bytes + offset, size - offset, MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                      errno == EWOULDBLOCK)) {
      continue;
    }
    if (count < 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker transport read failed");
    }
    if (count == 0) {
      throw RequestFailure(RequestFailureKind::kTransport,
                           "worker process exited without a response");
    }
    offset += static_cast<std::size_t>(count);
  }
}

void put_u16(std::array<unsigned char, kHeaderBytes>& header,
             std::size_t offset, std::uint16_t value) {
  header[offset] = static_cast<unsigned char>(value & 0xffU);
  header[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void put_u32(std::array<unsigned char, kHeaderBytes>& header,
             std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    header[offset++] = static_cast<unsigned char>((value >> shift) & 0xffU);
  }
}

void put_u64(std::array<unsigned char, kHeaderBytes>& header,
             std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    header[offset++] = static_cast<unsigned char>((value >> shift) & 0xffU);
  }
}

[[nodiscard]] std::uint16_t get_u16(
    const std::array<unsigned char, kHeaderBytes>& header,
    std::size_t offset) {
  return static_cast<std::uint16_t>(header[offset]) |
         (static_cast<std::uint16_t>(header[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t get_u32(
    const std::array<unsigned char, kHeaderBytes>& header,
    std::size_t offset) {
  std::uint32_t value = 0U;
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    value |= static_cast<std::uint32_t>(header[offset++]) << shift;
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(
    const std::array<unsigned char, kHeaderBytes>& header,
    std::size_t offset) {
  std::uint64_t value = 0U;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(header[offset++]) << shift;
  }
  return value;
}

void send_frame(int descriptor, MessageType type, std::string_view payload,
                std::size_t maximum_payload,
                const std::shared_ptr<ActiveRequest>& request,
                Clock::time_point deadline) {
  if (payload.size() > maximum_payload) {
    throw RequestFailure(RequestFailureKind::kProtocol,
                         "outgoing worker payload exceeds its configured bound");
  }
  std::array<unsigned char, kHeaderBytes> header{};
  put_u32(header, 0U, worker_protocol::kFrameMagic);
  put_u16(header, 4U, worker_protocol::kProtocolVersion);
  put_u16(header, 6U, static_cast<std::uint16_t>(type));
  put_u64(header, 8U, payload.size());
  write_controlled(descriptor, header.data(), header.size(), request, deadline);
  if (!payload.empty()) {
    write_controlled(descriptor, payload.data(), payload.size(), request,
                     deadline);
  }
}

[[nodiscard]] Frame receive_frame(
    int descriptor, std::size_t maximum_payload,
    const std::shared_ptr<ActiveRequest>& request,
    Clock::time_point deadline) {
  std::array<unsigned char, kHeaderBytes> header{};
  read_controlled(descriptor, header.data(), header.size(), request, deadline);
  if (get_u32(header, 0U) != worker_protocol::kFrameMagic ||
      get_u16(header, 4U) != worker_protocol::kProtocolVersion) {
    throw RequestFailure(RequestFailureKind::kProtocol,
                         "worker response has an invalid magic or version");
  }
  const auto raw_type = get_u16(header, 6U);
  if (raw_type < static_cast<std::uint16_t>(MessageType::kInitialize) ||
      raw_type > static_cast<std::uint16_t>(MessageType::kProtocolError)) {
    throw RequestFailure(RequestFailureKind::kProtocol,
                         "worker response has an invalid type");
  }
  const auto size = get_u64(header, 8U);
  if (size > maximum_payload ||
      size > std::numeric_limits<std::size_t>::max()) {
    throw RequestFailure(RequestFailureKind::kProtocol,
                         "worker response exceeds its configured byte bound");
  }
  std::string payload(static_cast<std::size_t>(size), '\0');
  if (!payload.empty()) {
    read_controlled(descriptor, payload.data(), payload.size(), request,
                    deadline);
  }
  return {static_cast<MessageType>(raw_type), std::move(payload)};
}

[[nodiscard]] Clock::time_point request_deadline(
    double timeout_s, std::chrono::milliseconds grace) {
  if (!std::isfinite(timeout_s) || timeout_s <= 0.0) {
    throw RequestFailure(RequestFailureKind::kProtocol,
                         "worker request timeout is invalid");
  }
  const auto duration = std::chrono::duration<double>(timeout_s) + grace;
  const auto bounded = std::chrono::duration_cast<Clock::duration>(duration);
  if (bounded <= Clock::duration::zero() ||
      bounded > std::chrono::hours(24)) {
    throw RequestFailure(RequestFailureKind::kProtocol,
                         "worker request timeout is outside its hard bound");
  }
  return Clock::now() + bounded;
}

[[nodiscard]] core::ErrorDTO request_failure_error(
    const RequestFailure& failure, bool ik) {
  switch (failure.kind) {
    case RequestFailureKind::kCancelled:
      return error(core::ErrorCode::kPlanningFailed,
                   ik ? "RoboPlan IK was cancelled"
                      : "RoboPlan planning was cancelled",
                   "planning_cancelled_by_interrupt");
    case RequestFailureKind::kTimeout:
      return error(core::ErrorCode::kPlanningTimeout,
                   ik ? "RoboPlan IK exceeded its deadline"
                      : "RoboPlan planning exceeded its deadline",
                   "planning_timeout");
    case RequestFailureKind::kProtocol:
      return error(core::ErrorCode::kInvariantViolation,
                   "RoboPlan worker protocol rejected a message",
                   ik ? "native_ik_worker_protocol_failure"
                      : "native_planner_worker_protocol_failure");
    case RequestFailureKind::kTransport:
      return error(ik ? core::ErrorCode::kIkFailed
                      : core::ErrorCode::kPlanningFailed,
                   "RoboPlan worker became unavailable",
                   ik ? "native_ik_worker_unavailable"
                      : "native_planner_worker_unavailable");
  }
  return error(core::ErrorCode::kInternalError,
               "RoboPlan worker failed", "native_worker_failure");
}

}  // namespace

struct ProcessContainedRoboPlanCandidateClient::Impl final {
  Impl(RoboPlanWorkerModelDTO model_value,
       RoboPlanCandidateBindingDTO binding_value,
       RoboPlanCandidateComputerConfig candidate_config_value,
       ProcessContainedCandidateClientConfig config_value)
      : model(std::move(model_value)),
        binding(std::move(binding_value)),
        candidate_config(std::move(candidate_config_value)),
        config(std::move(config_value)) {
    validate_config();
    start_worker(nullptr, Clock::now() + config.startup_timeout);
  }

  ~Impl() {
    auto active = active_request.exchange(nullptr, std::memory_order_acq_rel);
    if (active) {
      active->cancelled.store(true, std::memory_order_release);
      if (auto signal =
              active->process.load(std::memory_order_acquire)) {
        signal->kill();
      }
    }
    std::lock_guard lock(request_mutex);
    shutdown_worker();
  }

  void validate_config() const {
    if (!config.worker_executable.is_absolute() ||
        !std::filesystem::is_regular_file(config.worker_executable) ||
        ::access(config.worker_executable.c_str(), X_OK) != 0 ||
        config.startup_timeout <= std::chrono::milliseconds::zero() ||
        config.startup_timeout > std::chrono::minutes(5) ||
        config.request_transport_grace < std::chrono::milliseconds::zero() ||
        config.request_transport_grace > std::chrono::seconds(10) ||
        config.shutdown_timeout <= std::chrono::milliseconds::zero() ||
        config.shutdown_timeout > std::chrono::seconds(10) ||
        config.maximum_request_bytes == 0U ||
        config.maximum_request_bytes > 64U * 1024U * 1024U ||
        config.maximum_response_bytes == 0U ||
        config.maximum_response_bytes > 256U * 1024U * 1024U) {
      throw std::invalid_argument(
          "RoboPlan process-contained client configuration is invalid");
    }
  }

  [[nodiscard]] bool worker_alive() {
    if (!worker.has_value()) {
      return false;
    }
    int status = 0;
    const auto result = ::waitpid(worker->pid, &status, WNOHANG);
    if (result == 0) {
      return true;
    }
    if (result == worker->pid || (result < 0 && errno == ECHILD)) {
      worker->reaped = true;
      worker.reset();
      return false;
    }
    if (result < 0 && errno != EINTR) {
      worker.reset();
      return false;
    }
    return true;
  }

  void start_worker(const std::shared_ptr<ActiveRequest>& request,
                    Clock::time_point deadline) {
    if (worker_alive()) {
      if (request) {
        request->process.store(worker->signal, std::memory_order_release);
        check_request(request, deadline);
      }
      return;
    }

    int sockets[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
      throw std::runtime_error("could not create planner worker socket: " +
                               std::string(std::strerror(errno)));
    }
    UniqueFd parent_socket(sockets[0]);
    UniqueFd child_socket(sockets[1]);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
      throw std::runtime_error("could not initialize planner spawn actions");
    }
    struct ActionGuard final {
      posix_spawn_file_actions_t* actions;
      ~ActionGuard() { ::posix_spawn_file_actions_destroy(actions); }
    } guard{&actions};
    UniqueFd duplicated_child;
    int child_descriptor = child_socket.get();
    const int parent_descriptor = parent_socket.get();
    const int protocol_descriptor = worker_protocol::kProtocolDescriptor;
    if (child_descriptor == protocol_descriptor) {
      const int duplicate =
          ::fcntl(child_descriptor, F_DUPFD_CLOEXEC,
                  protocol_descriptor + 1);
      if (duplicate < 0) {
        throw std::runtime_error(
            "could not duplicate planner transport for worker executable");
      }
      duplicated_child.reset(duplicate);
      child_descriptor = duplicate;
    }
    if ((child_descriptor != protocol_descriptor &&
         ::posix_spawn_file_actions_adddup2(
             &actions, child_descriptor, protocol_descriptor) != 0) ||
        (parent_descriptor != protocol_descriptor &&
         ::posix_spawn_file_actions_addclose(&actions,
                                             parent_descriptor) != 0) ||
        (child_descriptor != protocol_descriptor &&
         ::posix_spawn_file_actions_addclose(&actions, child_descriptor) !=
             0)) {
      throw std::runtime_error("could not configure planner spawn actions");
    }

    const std::string executable = config.worker_executable.string();
    const std::string parent_pid = std::to_string(::getpid());
    std::array<char*, 4U> arguments{
        const_cast<char*>(executable.c_str()),
        const_cast<char*>("--parent-pid"),
        const_cast<char*>(parent_pid.c_str()), nullptr};
    pid_t child = -1;
    const int spawned = ::posix_spawn(
        &child, executable.c_str(), &actions, nullptr, arguments.data(),
        environ);
    if (spawned != 0) {
      throw std::runtime_error("could not spawn planner worker: " +
                               std::string(std::strerror(spawned)));
    }
    child_socket.reset();

    try {
      auto signal =
          std::make_shared<ProcessSignalHandle>(open_pidfd(child));
      make_nonblocking(parent_socket.get());
      worker.emplace(
          WorkerProcess{child, std::move(parent_socket), signal, false});
      if (request) {
        request->process.store(signal, std::memory_order_release);
        check_request(request, deadline);
      }
      const auto initialization = worker_protocol::encode_initialization(
          worker_protocol::WorkerInitializationDTO{
              model, binding, candidate_config});
      send_frame(worker->socket.get(), MessageType::kInitialize,
                 initialization, config.maximum_request_bytes, request,
                 deadline);
      auto ready = receive_frame(worker->socket.get(),
                                 config.maximum_response_bytes, request,
                                 deadline);
      if (ready.type == MessageType::kProtocolError) {
        static_cast<void>(
            worker_protocol::decode_protocol_error(ready.payload));
        throw RequestFailure(RequestFailureKind::kProtocol,
                             "worker rejected initialization");
      }
      if (ready.type != MessageType::kReady || !ready.payload.empty()) {
        throw RequestFailure(RequestFailureKind::kProtocol,
                             "worker readiness response is malformed");
      }
    } catch (...) {
      if (worker.has_value()) {
        terminate_worker();
      } else {
        static_cast<void>(::kill(child, SIGKILL));
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
      }
      throw;
    }
  }

  void reap_until(Clock::time_point deadline) noexcept {
    if (!worker.has_value() || worker->reaped) {
      return;
    }
    for (;;) {
      const auto result = ::waitpid(worker->pid, nullptr, WNOHANG);
      if (result == worker->pid || (result < 0 && errno == ECHILD)) {
        worker->reaped = true;
        return;
      }
      if (result < 0 && errno != EINTR) {
        return;
      }
      if (Clock::now() >= deadline) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void terminate_worker() noexcept {
    if (!worker.has_value()) {
      return;
    }
    worker->signal->kill();
    reap_until(Clock::now() + config.shutdown_timeout);
    worker.reset();
  }

  void shutdown_worker() noexcept {
    if (!worker_alive()) {
      return;
    }
    try {
      const auto deadline = Clock::now() + config.shutdown_timeout;
      send_frame(worker->socket.get(), MessageType::kShutdown, {},
                 config.maximum_request_bytes, nullptr, deadline);
      reap_until(deadline);
    } catch (...) {
    }
    if (worker.has_value() && !worker->reaped) {
      terminate_worker();
    } else {
      worker.reset();
    }
  }

  template <typename Value, typename Encode, typename Decode>
  [[nodiscard]] core::Result<Value> transact(
      MessageType request_type, MessageType response_type,
      double timeout_s, bool ik, Encode encode, Decode decode) {
    auto active = std::make_shared<ActiveRequest>();
    std::shared_ptr<ActiveRequest> expected;
    if (!active_request.compare_exchange_strong(
            expected, active, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return core::Result<Value>::Err(error(
          core::ErrorCode::kInvalidStateTransition,
          "RoboPlan candidate client already has an active request",
          "native_planner_concurrent_request_rejected"));
    }
    struct ActiveGuard final {
      std::atomic<std::shared_ptr<ActiveRequest>>& slot;
      std::shared_ptr<ActiveRequest> value;
      ~ActiveGuard() {
        value->process.store(nullptr, std::memory_order_release);
        auto expected = value;
        static_cast<void>(slot.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel,
            std::memory_order_acquire));
      }
    } active_guard{active_request, active};
    std::lock_guard lock(request_mutex);

    try {
      const auto deadline =
          request_deadline(timeout_s, config.request_transport_grace);
      start_worker(active, std::min(
                               deadline,
                               Clock::now() + config.startup_timeout));
      const std::string payload = encode();
      send_frame(worker->socket.get(), request_type, payload,
                 config.maximum_request_bytes, active, deadline);
      auto response = receive_frame(worker->socket.get(),
                                    config.maximum_response_bytes, active,
                                    deadline);
      check_request(active, deadline);
      if (response.type == MessageType::kProtocolError) {
        static_cast<void>(
            worker_protocol::decode_protocol_error(response.payload));
        throw RequestFailure(RequestFailureKind::kProtocol,
                             "worker rejected request");
      }
      if (response.type != response_type) {
        throw RequestFailure(RequestFailureKind::kProtocol,
                             "worker returned an unexpected response type");
      }
      try {
        return decode(response.payload);
      } catch (const std::invalid_argument&) {
        throw RequestFailure(RequestFailureKind::kProtocol,
                             "worker response payload is malformed");
      }
    } catch (const RequestFailure& failure) {
      terminate_worker();
      return core::Result<Value>::Err(request_failure_error(failure, ik));
    } catch (...) {
      terminate_worker();
      return core::Result<Value>::Err(error(
          ik ? core::ErrorCode::kIkFailed
             : core::ErrorCode::kPlanningFailed,
          "RoboPlan worker transport failed",
          ik ? "native_ik_worker_unavailable"
             : "native_planner_worker_unavailable"));
    }
  }

  RoboPlanWorkerModelDTO model;
  RoboPlanCandidateBindingDTO binding;
  RoboPlanCandidateComputerConfig candidate_config;
  ProcessContainedCandidateClientConfig config;
  std::mutex request_mutex;
  std::optional<WorkerProcess> worker;
  std::atomic<std::shared_ptr<ActiveRequest>> active_request;
};

ProcessContainedRoboPlanCandidateClient::
    ProcessContainedRoboPlanCandidateClient(
        RoboPlanWorkerModelDTO model,
        RoboPlanCandidateBindingDTO binding,
        RoboPlanCandidateComputerConfig candidate_config,
        ProcessContainedCandidateClientConfig config)
    : impl_(std::make_unique<Impl>(
          std::move(model), std::move(binding),
          std::move(candidate_config), std::move(config))) {}

ProcessContainedRoboPlanCandidateClient::
    ~ProcessContainedRoboPlanCandidateClient() = default;

const RoboPlanCandidateBindingDTO&
ProcessContainedRoboPlanCandidateClient::binding() const noexcept {
  return impl_->binding;
}

core::Result<core::TrajectoryDTO>
ProcessContainedRoboPlanCandidateClient::compute(
    const RoboPlanCandidateRequestDTO& request) {
  return impl_->transact<core::TrajectoryDTO>(
      MessageType::kPlan, MessageType::kPlanResult,
      request.planning_timeout_s, false,
      [&request] {
        return worker_protocol::encode_candidate_request(request);
      },
      [](std::string_view bytes) {
        return worker_protocol::decode_trajectory_result(bytes);
      });
}

core::Result<std::vector<double>>
ProcessContainedRoboPlanCandidateClient::solve_ik(
    const core::Pose3DTO& goal,
    const core::ResolvedFrameDTO& base_frame,
    const core::ResolvedFrameDTO& tcp_frame,
    const core::ControlledStateDTO& seed_state, double timeout_s) {
  return impl_->transact<std::vector<double>>(
      MessageType::kIk, MessageType::kIkResult, timeout_s, true,
      [&goal, &base_frame, &tcp_frame, &seed_state, timeout_s] {
        return worker_protocol::encode_ik_request(
            worker_protocol::WorkerIkRequestDTO{
                goal, base_frame, tcp_frame, seed_state, timeout_s});
      },
      [](std::string_view bytes) {
        return worker_protocol::decode_ik_result(bytes);
      });
}

void ProcessContainedRoboPlanCandidateClient::request_cancel() noexcept {
  auto request =
      impl_->active_request.load(std::memory_order_acquire);
  if (!request) {
    return;
  }
  request->cancelled.store(true, std::memory_order_release);
  if (auto process = request->process.load(std::memory_order_acquire)) {
    process->kill();
  }
}

}  // namespace manyforge::roboplan_backend
