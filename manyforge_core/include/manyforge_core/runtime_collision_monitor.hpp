#pragma once

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace manyforge::core {

struct RuntimeCollisionPolicyView {
  std::shared_ptr<const CollisionPolicySnapshot> snapshot;
  std::shared_ptr<const CollisionPolicyIndex> index;
};

/// Owns runtime-collision monitor scheduling, cancellation, and teardown
/// quiescence. Collision semantics remain with ExecutionKernel; this collaborator
/// only decides when ticks run and guarantees callbacks cannot outlive stop().
class RuntimeCollisionMonitor {
 public:
  struct Config {
    Duration period_ns = 50'000'000;
  };

  using TickCallback = std::function<Result<void>()>;
  /// Called from the monitor's scheduled callback while the monitor mutex is
  /// held. Implementations must not re-enter RuntimeCollisionMonitor
  /// start/stop, and scheduler cancel/drain implementations must not wait on a
  /// callback that is blocked trying to reacquire the same monitor mutex.
  using ShouldContinueCallback = std::function<bool()>;

  RuntimeCollisionMonitor(IClock& clock,
                          IScheduler& scheduler,
                          Config config,
                          TickCallback tick_callback,
                          ShouldContinueCallback should_continue_callback);
  ~RuntimeCollisionMonitor();

  RuntimeCollisionMonitor(const RuntimeCollisionMonitor&) = delete;
  RuntimeCollisionMonitor& operator=(const RuntimeCollisionMonitor&) = delete;
  RuntimeCollisionMonitor(RuntimeCollisionMonitor&&) = delete;
  RuntimeCollisionMonitor& operator=(RuntimeCollisionMonitor&&) = delete;

  void start();
  void stop(bool drain_callbacks = false);

  void set_active_contact_policy_id(std::string contact_policy_id);
  void clear_active_contact_policy_id();
  std::string active_contact_policy_id() const;

  /// Runtime ticks are single observed states, not full trajectories. Return a
  /// policy snapshot adjusted for runtime semantics while preserving the
  /// authoritatively configured policy data.
  RuntimeCollisionPolicyView runtime_policy_snapshot(
      std::shared_ptr<const CollisionPolicySnapshot> policy_snapshot) const;

  /// Apply the currently armed contact_grasp policy to the observed-state
  /// validation bundle. Returns a fault details map only for configuration
  /// errors such as unknown/invalid active policy. Contact violations remain in
  /// the bundle as blocking collision diagnostics.
  std::optional<std::map<std::string, std::string>>
  apply_active_contact_grasp_policy(
      const CollisionPolicySnapshot& policy_snapshot,
      CollisionValidationDecisionBundle& bundle) const;

  void reset_contact_release_state();
  std::optional<std::map<std::string, std::string>>
  update_contact_release_state(
      const CollisionPolicySnapshot& policy_snapshot,
      const CollisionValidationDecisionBundle& bundle);

 private:
  struct ContactReleasePairState {
    double initial_penetration_m = 0.0;
    double last_penetration_m = 0.0;
    bool separated = false;
  };

  void schedule_next_locked(std::uint64_t generation);

  IClock& clock_;
  IScheduler& scheduler_;
  Config config_;
  TickCallback tick_callback_;
  ShouldContinueCallback should_continue_callback_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  ScheduleHandle handle_{0};
  std::uint64_t generation_ = 0;
  bool active_ = false;
  std::uint64_t callbacks_in_flight_ = 0;

  mutable std::mutex contact_policy_mutex_;
  std::string active_contact_policy_id_;
  std::map<std::string, ContactReleasePairState> contact_release_state_;

  mutable std::mutex runtime_policy_cache_mutex_;
  mutable std::optional<CollisionPolicyRevision> runtime_policy_cache_revision_;
  mutable RuntimeCollisionPolicyView runtime_policy_cache_view_;
};

}  // namespace manyforge::core
