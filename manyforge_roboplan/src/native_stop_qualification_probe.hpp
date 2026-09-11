#pragma once

#include <cstdint>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend::detail {

inline constexpr std::string_view kNativeSoftwareHardStopInvocationLaneId =
    "manyforge.native_execution_host.focused_software_hard.v1";
inline constexpr std::string_view kNativeSoftwareHardProviderContractRoleId =
    "software_hard_stop";
inline constexpr std::string_view kNativeControlledStopProviderContractRoleId =
    "stop_contract";
inline constexpr std::string_view kNativeSoftwareHardContractSchemaId =
    "manyforge.value_schema.provider_contract.empty.v1";
inline constexpr std::string_view kNativeSoftwareHardContractSchemaRevision =
    "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270";

/// Internal observation points used only by the explicit offline Stop runner.
///
/// The installed backend contains the same null-checked observation points as
/// the qualified runner. Normal construction never supplies a probe, and the
/// software-hard issue path deliberately contains no probe call.
enum class NativeStopQualificationEvent : std::uint8_t {
  kNominalProviderEntry = 0,
  kExecutionLoopArmed,
  kExecutionSampleReady,
  kNominalAdapterLockAttempt,
  kNominalResultWaitEnter,
  kWorkerStopObservedBeforeTerminal,
  kNominalRearmAttempt,
  kSubstitutionSelected,
  kPhysicalDispatch,
};

class INativeStopQualificationProbe {
public:
  virtual ~INativeStopQualificationProbe() = default;

  virtual void observe(NativeStopQualificationEvent event,
                       std::string_view detail) = 0;
};

struct NativeStopQualificationScope final {
  std::string controllable_component_id;
  std::string command_target_id;
  std::vector<std::string> resource_ids;
  std::vector<std::string> compiled_resource_names;

  friend bool operator==(const NativeStopQualificationScope &,
                         const NativeStopQualificationScope &) = default;
};

/// Uninstalled friend seam for the build-owned qualification executable.
///
/// It can inject the dormant probe and deliberately hold the real nominal
/// adapter lock. It cannot replace the backend's software-hard implementation.
class NativeStopQualificationAccess final {
public:
  [[nodiscard]] static std::unique_ptr<RoboPlanExecutionBackend>
  construct(core::RuntimeParticipantBinding binding,
            std::shared_ptr<IRetainedObservedStateChannel> state_channel,
            core::IClock &clock, RoboPlanExecutionBackendConfig config,
            INativeStopQualificationProbe &probe,
            core::IScheduler *scheduler = nullptr) {
    return std::unique_ptr<RoboPlanExecutionBackend>(
        new RoboPlanExecutionBackend(std::move(binding),
                                     std::move(state_channel), clock,
                                     std::move(config), scheduler, &probe));
  }

  static std::uint64_t
  hard_stop_requests(const RoboPlanExecutionBackend &backend) {
    return backend.hard_stop_request_generation_.load(
        std::memory_order_acquire);
  }

  // Exercise the real worker rendezvous without claiming canonical prepared
  // product/admission qualification. The public path binds this same clock
  // only after verifying exact prepared membership.
  static core::ExecutionHandle execute_with_clock(
      RoboPlanExecutionBackend &backend, const core::TrajectoryDTO &trajectory,
      std::shared_ptr<core::detail::CoordinatedExecutionClock> clock,
      core::OperationParticipantId participant) {
    return backend.execute_with_clock(trajectory, std::move(clock), std::move(participant));
  }

  [[nodiscard]] static std::unique_lock<std::mutex>
  lock_nominal_adapter(RoboPlanExecutionBackend &backend) {
    return std::unique_lock<std::mutex>(backend.state_mutex_);
  }

  static void freeze_runtime_scale(RoboPlanExecutionBackend &backend) {
    backend.begin_controlled_stop_runtime_scale_freeze();
  }
  static void release_runtime_scale(RoboPlanExecutionBackend &backend) {
    (void)backend.end_controlled_stop_runtime_scale_freeze();
    backend.execution_cv_.notify_all();
  }

  [[nodiscard]] static NativeStopQualificationScope
  scope(const RoboPlanExecutionBackend &backend) {
    const auto retained_binding =
        backend.binding_.load(std::memory_order_acquire);
    const auto &binding = retained_binding->value();
    NativeStopQualificationScope result{
        binding.controllable_component_id.value(),
        binding.command_target_id.value(),
        {},
        {}};
    result.resource_ids.reserve(binding.resources.size());
    result.compiled_resource_names.reserve(binding.resources.size());
    for (const auto &resource : binding.resources) {
      result.resource_ids.push_back(resource.resource_id.value());
      result.compiled_resource_names.push_back(resource.compiled_resource_name);
    }
    return result;
  }

  static void install_prepared_goal_membership(
      RoboPlanExecutionBackend &backend,
      const core::ResolvedParticipantStopStageDTO &stage,
      core::ContentDigest prepared_contract_digest,
      core::ExecutionHandle handle) {
    std::lock_guard<std::mutex> lock(backend.prepared_execution_mutex_);
    backend.prepared_goal_ = RoboPlanExecutionBackend::PreparedGoalRecord{
        stage.controller_goal_id,
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
        std::move(handle)};
  }
};

} // namespace manyforge::roboplan_backend::detail
