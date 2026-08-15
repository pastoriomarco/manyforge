#pragma once

#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::ros2_control_execution::detail {

inline constexpr std::string_view kRos2ControlSoftwareHardStopInvocationLaneId =
    "manyforge.ros2_control_execution_host.focused_software_hard.v1";
inline constexpr std::string_view
    kRos2ControlSoftwareHardProviderContractRoleId = "software_hard_stop";
inline constexpr std::string_view kRos2ControlSoftwareHardContractSchemaId =
    "manyforge.value_schema.provider_contract.empty.v1";
inline constexpr std::string_view
    kRos2ControlSoftwareHardContractSchemaRevision =
        "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270";

/// Internal observation points used only by the explicit offline Stop runner.
///
/// The installed backend contains these null-checked points. Normal runtime
/// construction never supplies a probe, and the software-hard issue path can
/// only be observed, never blocked or replaced, through this interface.
enum class Ros2ControlStopQualificationEvent : std::uint8_t {
  kNominalProviderEntry = 0,
  kNominalAdapterLockAttempt,
  kNominalResultWaitEnter,
  kSoftwareHardIssueAccepted,
};

class IRos2ControlStopQualificationProbe {
public:
  virtual ~IRos2ControlStopQualificationProbe() = default;

  virtual void observe(Ros2ControlStopQualificationEvent event,
                       std::string_view detail) = 0;
};

/// Uninstalled friend seam for the build-owned qualification executable.
///
/// It can install the dormant observer, hold the real nominal state-adapter
/// lock, and seed the exact already-sealed dispatch membership consumed by the
/// production Stop translator. It cannot replace the software-hard command or
/// its generation-fenced terminal callback.
class Ros2ControlStopQualificationAccess final {
public:
  static void install_probe(Ros2ControlExecutionBackend &backend,
                            IRos2ControlStopQualificationProbe &probe) {
    backend.stop_qualification_probe_.store(&probe,
                                            std::memory_order_release);
  }

  [[nodiscard]] static std::unique_lock<std::mutex>
  lock_nominal_state_adapter(Ros2ControlExecutionBackend &backend) {
    return std::unique_lock<std::mutex>(backend.state_mutex_);
  }

  static void seed_observed_joint_sample(
      Ros2ControlExecutionBackend &backend,
      std::vector<std::string> joint_names, std::vector<double> positions,
      std::vector<double> velocities) {
    backend.testing_store_latest_joint_sample_impl(
        std::move(joint_names), std::move(positions),
        std::chrono::steady_clock::now(), std::move(velocities),
        backend.clock_.now());
  }

  [[nodiscard]] static bool
  managed_replacement_owns_goal(Ros2ControlExecutionBackend &backend) {
    {
      std::lock_guard<std::mutex> lock(backend.execution_mutex_);
      if (!backend.controlled_stop_preempt_in_progress_ ||
          backend.active_goal_handle_) {
        return false;
      }
    }
    std::lock_guard<std::mutex> lock(backend.managed_fjt_mutex_);
    return static_cast<bool>(backend.managed_fjt_goal_);
  }

  static void install_prepared_goal_membership(
      Ros2ControlExecutionBackend &backend,
      const core::ResolvedParticipantStopStageDTO &stage,
      core::ContentDigest prepared_contract_digest) {
    const auto execution_generation =
        backend.running_generation_.load(std::memory_order_acquire);
    core::ExecutionHandle handle{
        [&backend](std::int64_t timeout_ns) {
          std::unique_lock<std::mutex> lock(backend.execution_mutex_);
          if (timeout_ns > 0) {
            return backend.execution_cv_.wait_for(
                lock, std::chrono::nanoseconds(timeout_ns),
                [&backend]() { return backend.terminal_; });
          }
          return backend.terminal_;
        },
        [&backend]() { return backend.get_execution_snapshot(); },
        execution_generation,
        stage.controller_goal_id,
        stage.controller_goal_generation,
        prepared_contract_digest};
    std::lock_guard<std::mutex> lock(backend.prepared_execution_mutex_);
    backend.prepared_goals_.erase(stage.controller_goal_id);
    backend.prepared_goals_.emplace(
        stage.controller_goal_id,
        Ros2ControlExecutionBackend::PreparedGoalRecord{
            stage.controller_goal_generation,
            std::move(prepared_contract_digest),
            stage.operation_id,
            stage.operation_revision,
            stage.participant_id,
            stage.controllable_component_id,
            stage.command_target_id,
            stage.capability_id,
            stage.control_domain_id,
            stage.control_domain_revision,
            stage.control_binding_id,
            stage.control_binding_revision,
            stage.controller_topology_id,
            stage.topology_revision,
            stage.control_route_id,
            stage.active_control_configuration_id,
            stage.active_control_configuration_generation,
            stage.provider_runtime_epoch,
            stage.resources,
            stage.stop_evidence_policy_id,
            stage.stop_evidence_policy_revision,
            {stage.participant_stop_plan},
            std::move(handle)});
  }

  static void arm_forced_managed_terminal_for_test(
      Ros2ControlExecutionBackend &backend,
      std::uint64_t execution_generation,
      std::uint64_t managed_generation) {
    std::lock_guard<std::mutex> lock(backend.managed_fjt_mutex_);
    backend.forced_managed_stop_execution_generation_ = execution_generation;
    backend.forced_managed_stop_generation_ = managed_generation;
  }
};

} // namespace manyforge::ros2_control_execution::detail
