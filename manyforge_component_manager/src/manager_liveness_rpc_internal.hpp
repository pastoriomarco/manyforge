#pragma once

#include <manyforge_component_manager/manager_liveness_transport.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::component_manager::detail {

class ManagerLivenessRpcRejection final : public std::invalid_argument {
 public:
  explicit ManagerLivenessRpcRejection(std::string reason);

  [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

 private:
  std::string reason_;
};

inline constexpr std::string_view kManagerLivenessRequestTag =
    "manyforge.manager-liveness.request.v1";
inline constexpr std::string_view kManagerLivenessResponseTag =
    "manyforge.manager-liveness.response.v1";
inline constexpr std::string_view kManagerLivenessRejectionTag =
    "manyforge.manager-liveness.rejected.v1";
inline constexpr std::size_t kMaximumManagerLivenessRpcPacketBytes = 16U * 1024U;

// Private child-exit projection on the existing dedicated channel. A response
// proves servicing; only changed real phase/milestone facts prove progress.
inline constexpr std::string_view kNativeShutdownRequestTag =
    "manyforge.native-shutdown.request.v1";
inline constexpr std::string_view kNativeShutdownResponseTag =
    "manyforge.native-shutdown.response.v1";
enum class NativeShutdownPhase : unsigned {
  kRunning,
  kGroupRetirement,
  kExecutionDrain,
  kCurrentBuilderDrain,
  kManagedBuilderDrain,
  kProjectionDrain,
  kKernelDestruction,
  kPreparedCompositionDestruction,
  kRetiredCompositionDestruction,
  kActiveCompositionDestruction,
  kFailedCompositionDestruction,
  kProjectionDestruction,
  kLivenessReceiverDestruction,
  kSchedulerOwnerRelease,
  kFenceOwnerDestruction,
  kServicingLaneDrain
};
struct NativeShutdownProgress final {
  std::string runtime_epoch;
  NativeShutdownPhase phase{NativeShutdownPhase::kRunning};
  std::size_t completed_workers{};
  std::size_t total_groups{};
  std::size_t pending_groups{};
  std::size_t signalled_groups{};
  std::size_t escalated_groups{};
  std::size_t reaped_groups{};
  std::size_t drained_groups{};
  std::size_t unknown_groups{};
  int pending_pid{-1};
  bool operator==(const NativeShutdownProgress &) const = default;
};
[[nodiscard]] std::string_view
NativeShutdownPhaseName(NativeShutdownPhase phase);
[[nodiscard]] bool IsNativeShutdownRpcPacket(std::string_view bytes) noexcept;
[[nodiscard]] std::string
EncodeNativeShutdownRpcRequest(std::string_view epoch);
[[nodiscard]] std::string
DecodeNativeShutdownRpcRequest(std::string_view bytes);
[[nodiscard]] std::string
EncodeNativeShutdownRpcResponse(const NativeShutdownProgress &progress);
[[nodiscard]] NativeShutdownProgress
DecodeNativeShutdownRpcResponse(std::string_view bytes);

// Sole projection of the existing teardown caller's facts; no status poll
// increments a counter, drains a process or accesses a borrowed runtime.
class NativeShutdownProjection final {
public:
  explicit NativeShutdownProjection(std::string epoch) {
    value_.runtime_epoch = std::move(epoch);
  }
  void Enter(NativeShutdownPhase phase) {
    std::lock_guard lock(mutex_);
    value_.phase = phase;
  }
  void WorkerJoined() {
    std::lock_guard lock(mutex_);
    ++value_.completed_workers;
  }
  void Groups(std::size_t total, std::size_t pending, std::size_t signalled,
              std::size_t escalated, std::size_t reaped, std::size_t drained,
              std::size_t unknown, int pid) {
    std::lock_guard lock(mutex_);
    value_.total_groups = total;
    value_.pending_groups = pending;
    value_.signalled_groups = signalled;
    value_.escalated_groups = escalated;
    value_.reaped_groups = reaped;
    value_.drained_groups = drained;
    value_.unknown_groups = unknown;
    value_.pending_pid = pid;
  }
  [[nodiscard]] NativeShutdownProgress Read() const {
    std::lock_guard lock(mutex_);
    return value_;
  }
  void EnterDuringShutdown(NativeShutdownPhase phase) {
    std::lock_guard lock(mutex_);
    if (value_.phase != NativeShutdownPhase::kRunning)
      value_.phase = phase;
  }

private:
  mutable std::mutex mutex_;
  NativeShutdownProgress value_;
};
// Declared immediately after its actual owner: reports entry before that
// owner's destructor. A later phase comes only after actual return.
template <typename Owner> struct NativeShutdownOwnerPhase final {
  NativeShutdownProjection &projection;
  NativeShutdownPhase phase;
  Owner &owner;
  ~NativeShutdownOwnerPhase() {
    if constexpr (requires { static_cast<bool>(owner); }) {
      if (!owner)
        return; // An absent owner has no destructor work to report.
    }
    projection.EnterDuringShutdown(phase);
  }
};

enum class ManagerLivenessCommandClass {
  kEffectful,
  kStop,
  kReadOnly,
};

/// Child-local, initially closed ordinary-command admission.  A generation
/// snapshot prevents a reconciliation racing a loss callback from reopening
/// ingress after the callback has synchronously closed it.
class ManagerLivenessCommandAdmission final {
 public:
  [[nodiscard]] std::uint64_t generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
  }

  void OpenIfUnchanged(const std::uint64_t expected_generation) {
    std::lock_guard lock(mutex_);
    if (generation_ == expected_generation) {
      open_ = true;
    }
  }

  [[nodiscard]] bool open() const {
    std::lock_guard lock(mutex_);
    return open_;
  }

  [[nodiscard]] bool Allows(const ManagerLivenessCommandClass command) const {
    return command != ManagerLivenessCommandClass::kEffectful || open();
  }

  void CloseForRequiredSetRebind() {
    std::lock_guard lock(mutex_);
    open_ = false;
    ++generation_;
  }

  [[nodiscard]] manyforge::core::ContentDigest CloseAndAttest(
      const manyforge::core::ManagerLivenessLossIntentDTO& intent) {
    std::lock_guard lock(mutex_);
    open_ = false;
    ++generation_;
    std::string material(
        "manyforge.native-execution.manager-loss-local-admission-close.v1");
    for (const auto& value :
         {intent.cell_id.value(), intent.active_bundle_id.value(),
          intent.required_runtime_set_digest.value(),
          intent.receiver_runtime_unit_id.value(),
          intent.receiver_runtime_epoch.value(), intent.runtime_policy_id.value(),
          intent.runtime_policy_revision.value(),
          intent.lost_manager_runtime_unit_id.value(),
          intent.lost_manager_runtime_epoch.value(),
          intent.lost_manager_owner_epoch.value()}) {
      material.push_back('\0');
      material.append(value);
    }
    material.push_back('\0');
    material.append(std::to_string(intent.receiver_status_sequence));
    material.push_back('\0');
    material.append(std::to_string(static_cast<unsigned>(intent.kind)));
    return manyforge::core::ContentDigest(
        manyforge::content_identity::sha256_digest(material));
  }

 private:
  mutable std::mutex mutex_;
  bool open_{};
  std::uint64_t generation_{};
};

/// Owns watchdog timing dependencies for the full managed-child lifetime.
/// Member order intentionally destroys the receiver before its scheduler and
/// clock, independently of composition replacement.
class ChildOwnedManagerLivenessReceiver final {
 public:
  ChildOwnedManagerLivenessReceiver(
      std::shared_ptr<manyforge::core::IClock> clock,
      std::unique_ptr<manyforge::core::IScheduler> scheduler)
      : clock_(std::move(clock)), scheduler_(std::move(scheduler)) {
    if (clock_ == nullptr || scheduler_ == nullptr) {
      throw std::invalid_argument(
          "child-owned Manager liveness timing is incomplete");
    }
  }

  void Install(
      manyforge::core::ManagerLivenessReceiverConfig config,
      manyforge::core::ManagerLossLocalSafeCallback local_safe_callback,
      manyforge::core::IManagerLivenessFailSafePort& fail_safe_port,
      manyforge::core::IManagerLivenessObservationPort& observation_port) {
    if (receiver_ != nullptr) {
      throw std::logic_error("Manager liveness receiver is already installed");
    }
    receiver_ = std::make_unique<manyforge::core::ManagerLivenessReceiver>(
        std::move(config), *clock_, *scheduler_,
        std::move(local_safe_callback), fail_safe_port, observation_port);
  }

  [[nodiscard]] manyforge::core::ManagerLivenessReceiver* get() noexcept {
    return receiver_.get();
  }

 private:
  std::shared_ptr<manyforge::core::IClock> clock_;
  std::unique_ptr<manyforge::core::IScheduler> scheduler_;
  std::unique_ptr<manyforge::core::ManagerLivenessReceiver> receiver_;
};

class ManagerLivenessBatchDeadline final {
 public:
  using Clock = std::chrono::steady_clock;

  ManagerLivenessBatchDeadline(Clock::time_point started_at,
                               Clock::duration budget)
      : deadline_(started_at + budget) {}

  [[nodiscard]] Clock::time_point value() const noexcept { return deadline_; }

 private:
  Clock::time_point deadline_;
};

[[nodiscard]] bool IsManagerLivenessRpcPacket(std::string_view bytes) noexcept;
[[nodiscard]] std::string EncodeManagerLivenessRpcRequest(
    const ManagerLivenessTransportRequestDTO& request);
[[nodiscard]] ManagerLivenessTransportRequestDTO
DecodeManagerLivenessRpcRequest(std::string_view bytes);
[[nodiscard]] std::string EncodeManagerLivenessRpcResponse(
    const manyforge::core::ManagerLivenessObservationDTO& response);
[[nodiscard]] manyforge::core::ManagerLivenessObservationDTO
DecodeManagerLivenessRpcResponse(std::string_view bytes);
[[nodiscard]] std::string EncodeManagerLivenessRpcRejection(
    std::string_view reason);

}  // namespace manyforge::component_manager::detail
