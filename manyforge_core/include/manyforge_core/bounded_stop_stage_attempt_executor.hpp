#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <optional>

namespace manyforge::core {

inline constexpr std::size_t kMaximumPrequalifiedStopStageAttemptLanes = 4096U;

struct StopStageAttemptHandle final {
  std::uint32_t slot{};
  std::uint64_t generation{};

  [[nodiscard]] bool locally_valid() const noexcept { return generation != 0U; }
  friend bool operator==(const StopStageAttemptHandle&, const StopStageAttemptHandle&) = default;
  friend bool operator<(const StopStageAttemptHandle& left,
                        const StopStageAttemptHandle& right) noexcept {
    return left.slot < right.slot ||
           (left.slot == right.slot && left.generation < right.generation);
  }
};

struct StopStageAttemptRequest final {
  IControlDomainExecutionPort* port{};
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const RemainingStopBudgetDTO> budget;
  /// Transient private selection; never changes the sealed stage command.
  bool force_spring_return{};
  /// Same Kernel clock; referents outlive the attempt's Release/Drain.
  const IClock* stop_ingress_clock{};
  TimePoint stop_ingress_at_ns{};
};

enum class StopStageAttemptStartStatus : std::uint8_t {
  kStarted = 0,
  kInvalidRequest,
  kClosed,
  kNotPrequalified,
  kCapacityExhausted,
};

struct StopStageAttemptStartResult final {
  StopStageAttemptStartStatus status{StopStageAttemptStartStatus::kInvalidRequest};
  std::optional<StopStageAttemptHandle> handle;
};

enum class StopStageAttemptTakeStatus : std::uint8_t {
  kPending = 0,
  kCompleted,
  kInfrastructureFailure,
  kAlreadyTaken,
  kStaleHandle,
};

struct StopStageAttemptTakeResult final {
  StopStageAttemptTakeStatus status{StopStageAttemptTakeStatus::kStaleHandle};
  std::optional<ParticipantStopStageResultDTO> result;
};

enum class StopStageAttemptReleaseStatus : std::uint8_t {
  kReleased = 0,
  kNotTaken,
  kStaleHandle,
  kReplenishmentFailed,
};

class BoundedStopStageAttemptExecutor;

class IStopStageAttemptCompletionReceiver {
 public:
  virtual ~IStopStageAttemptCompletionReceiver() = default;
  /// Also wakes on a first spring point; TryTake remains the completion truth.
  /// `source` is the executor that ran the attempt: an owner that replaces
  /// its executor settles a retired executor's late attempts on that
  /// executor, never on its successor.
  virtual void StageAttemptCompleted(BoundedStopStageAttemptExecutor& source,
                                     StopStageAttemptHandle handle) noexcept = 0;
};

/// Fixed-capacity isolation for already prepared Stop-stage attempts.
/// Construction allocates every lane, cancellation source, and worker. A
/// successful TryStart moves only prebuilt shared referents into a free lane;
/// it performs no allocation and cannot fail after publishing kStarted.
/// Handles are unique across every executor of the process: each executor
/// starts its lane generations in its own range, so a handle of a retired
/// executor is stale on its successor.
///
/// Each port and request referent must outlive Release(handle), or successful
/// CloseIngress()+DrainFor() during shutdown. Ports remain subject to the
/// bounded-return IControlDomainExecutionPort contract. A non-cooperative
/// in-process call can occupy only its own lane, but cannot be made safely
/// destructible; such providers require a supervised process boundary.
class BoundedStopStageAttemptExecutor final {
 public:
  BoundedStopStageAttemptExecutor(std::size_t capacity,
                                  std::size_t prequalified_max_live_participants,
                                  IStopStageAttemptCompletionReceiver& receiver);
  ~BoundedStopStageAttemptExecutor();

  BoundedStopStageAttemptExecutor(const BoundedStopStageAttemptExecutor&) = delete;
  BoundedStopStageAttemptExecutor& operator=(const BoundedStopStageAttemptExecutor&) = delete;
  BoundedStopStageAttemptExecutor(BoundedStopStageAttemptExecutor&&) = delete;
  BoundedStopStageAttemptExecutor& operator=(BoundedStopStageAttemptExecutor&&) = delete;

  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t prequalified_max_live_participants() const noexcept;
  [[nodiscard]] bool prequalified_ready() const noexcept;
  [[nodiscard]] StopStageAttemptStartResult TryStart(StopStageAttemptRequest request) noexcept;
  [[nodiscard]] bool RequestCancel(StopStageAttemptHandle handle) noexcept;
  [[nodiscard]] StopStageAttemptTakeResult TryTake(StopStageAttemptHandle handle) noexcept;
  /// Exact lane-generation progress, never a completion or dispatch permit.
  [[nodiscard]] std::shared_ptr<const SpringBackReturnPointDTO> ReadSpringReturnPoint(
      StopStageAttemptHandle handle) noexcept;
  [[nodiscard]] StopStageAttemptReleaseStatus Release(StopStageAttemptHandle handle) noexcept;
  void CloseIngress() noexcept;
  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
