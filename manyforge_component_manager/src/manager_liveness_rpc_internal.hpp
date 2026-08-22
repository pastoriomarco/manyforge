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

inline constexpr std::string_view kManagerLivenessRequestTag =
    "manyforge.manager-liveness.request.v1";
inline constexpr std::string_view kManagerLivenessResponseTag =
    "manyforge.manager-liveness.response.v1";
inline constexpr std::size_t kMaximumManagerLivenessRpcPacketBytes = 16U * 1024U;

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

}  // namespace manyforge::component_manager::detail
