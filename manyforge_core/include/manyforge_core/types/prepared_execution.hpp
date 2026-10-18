#pragma once

#include <cstdint>
#include <manyforge_core/execution_progress_publication.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

class RuntimeParticipantBinding;

/// Exact resident predecessor captured by the existing execution owner. This
/// process-local fact is not caller-authored planning or dispatch authority.
struct PreparedMotionReplacementSourceDTO final {
  std::uint64_t execution_id{};
  std::uint64_t execution_generation{};
  ExecutionGoalKey goal;
  ExecutionBindingEpoch binding_epoch{0U};
  ContentDigest prepared_contract_digest{std::string{}};
  ExecutionControllerSampleDTO controller_sample;
  /// Existing canonical prepared-trajectory cursor, joined to the exact
  /// controller sample. Raw controller time can have a delivery-origin offset.
  ExecutionProgressCursorSnapshotDTO progress;
};

/// Computed observation of the existing candidate membership and goal facts;
/// not an independently advanced backend state machine.
enum class PreparedMotionReplacementPhaseDTO : std::uint8_t {
  kPreparing = 0,
  kPrepared,
  kNoEffect,
  kPossiblySent,
  kReadyToCommit,
  kCommitted,
  kEffectUncertain,
};

struct PreparedMotionReplacementDispatchDTO final {
  ExecutionPortCommandResultDTO command;
  std::optional<ExecutionHandle> handle{};
  /// Existing host clock at the actual Replace call. Watchdog accounting
  /// only; never a trajectory cursor or proof of controller acceptance.
  std::optional<std::int64_t> submitted_at_ns{};
};

struct PreparedMotionReplacementObservationDTO final {
  PreparedMotionReplacementPhaseDTO phase{PreparedMotionReplacementPhaseDTO::kPreparing};
  std::optional<ExecutionGoalObservationDTO> predecessor{};
  std::optional<ExecutionGoalObservationDTO> successor{};
  /// Existing own-goal association evidence. Absence never proves recognition.
  std::optional<std::int64_t> recognition_delay_ns{};
  std::optional<ErrorDTO> error{};
};

/// Called under the participant mutex with the addressed state lock already
/// held. The callback must not wait or acquire that lock. All preparation and
/// validation precede it; successful owner CAS is the last fallible action.
/// After success, backend and worker adoption perform prebuilt noexcept swaps.
struct PreparedMotionReplacementCommitDTO final {
  void* context{};
  Result<void> (*owner_commit)(void*) noexcept {};
};

enum class PreparedParticipantPayloadKindDTO : std::uint8_t {
  kMotionExecution = 0,
  kCapabilityCommand,
};

[[nodiscard]] constexpr std::string_view to_string(
    PreparedParticipantPayloadKindDTO value) noexcept {
  switch (value) {
    case PreparedParticipantPayloadKindDTO::kMotionExecution:
      return "motion_execution";
    case PreparedParticipantPayloadKindDTO::kCapabilityCommand:
      return "capability_command";
  }
  return {};
}

enum class PrepareParticipantResultStateDTO : std::uint8_t {
  kPrepared = 0,
  kRejected,
};

[[nodiscard]] constexpr std::string_view to_string(
    PrepareParticipantResultStateDTO value) noexcept {
  switch (value) {
    case PrepareParticipantResultStateDTO::kPrepared:
      return "prepared";
    case PrepareParticipantResultStateDTO::kRejected:
      return "rejected";
  }
  return {};
}

/// Backend-local result for the side-effect-free participant Prepare gate.
/// Every identity is copied from one already sealed prepared participant;
/// adapters cannot use this value to alter Kernel preparation authority.
struct PrepareParticipantResultDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  std::optional<CapabilityProviderBindingId> capability_provider_binding_id;
  std::optional<CapabilityProviderBindingRevision> capability_provider_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  RuntimeEpoch provider_runtime_epoch;
  PreparedParticipantPayloadKindDTO payload_kind{
      PreparedParticipantPayloadKindDTO::kMotionExecution};
  PrepareParticipantResultStateDTO state{PrepareParticipantResultStateDTO::kRejected};
  std::optional<ContentDigest> prepared_contract_digest;
  std::optional<ErrorDTO> error;

  friend bool operator==(const PrepareParticipantResultDTO&,
                         const PrepareParticipantResultDTO&) = default;
};

enum class ExecutionBackendStateDTO : std::uint8_t {
  kAccepted = 0,
  kExecuting,
  kCompleted,
  kFailed,
  kCancelled,
};

[[nodiscard]] constexpr std::string_view to_string(ExecutionBackendStateDTO value) noexcept {
  switch (value) {
    case ExecutionBackendStateDTO::kAccepted:
      return "accepted";
    case ExecutionBackendStateDTO::kExecuting:
      return "executing";
    case ExecutionBackendStateDTO::kCompleted:
      return "completed";
    case ExecutionBackendStateDTO::kFailed:
      return "failed";
    case ExecutionBackendStateDTO::kCancelled:
      return "cancelled";
  }
  return {};
}

/// Observer-safe evidence for one exact prepared backend dispatch. It is not
/// Kernel operation terminality and cannot authorize a different goal,
/// binding, route, configuration, runtime epoch, or resource scope.
struct ExecutionBackendStatusDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  RuntimeEpoch provider_runtime_epoch;
  std::vector<ResourceId> resources;
  ExecutionBackendStateDTO state{ExecutionBackendStateDTO::kAccepted};
  double accepted_trajectory_time_s{};
  double progress{};
  std::uint64_t source_sequence{};
  std::int64_t receiver_local_receipt_timestamp_ns{};
  std::optional<ErrorDTO> error;

  friend bool operator==(const ExecutionBackendStatusDTO&,
                         const ExecutionBackendStatusDTO&) = default;
};

[[nodiscard]] PreparedParticipantPayloadKindDTO prepared_participant_payload_kind(
    const PreparedOperationParticipantDTO& participant) noexcept;

/// Validate the complete backend-owned binding projection without repeating
/// readiness or repository work. The caller still supplies only a participant
/// produced by the sealed Make/Verify boundary; this check prevents an adapter
/// instance from accepting a participant for another binding or payload kind.
[[nodiscard]] std::optional<std::string_view> prepared_participant_backend_binding_field_error(
    const PreparedOperationParticipantDTO& participant,
    const RuntimeParticipantBinding& backend_binding,
    PreparedParticipantPayloadKindDTO expected_payload_kind) noexcept;

/// Validate that an already sealed Program-scoped participant is contained by
/// one exact physical backend owner. Program and role identities may differ;
/// retained product, model/frame scope, resources, route, provider occurrence,
/// runtime epochs, and Stop evidence remain fail-closed. This is the adapter
/// counterpart of product-scoped startup plus operation-scoped Program Binding.
[[nodiscard]] std::optional<std::string_view>
prepared_participant_backend_physical_binding_field_error(
    const PreparedOperationParticipantDTO& participant,
    const RuntimeParticipantBinding& physical_backend_binding,
    PreparedParticipantPayloadKindDTO expected_payload_kind) noexcept;

/// Validate one backend result against the exact already-verified prepared
/// participant that was supplied to the backend.
[[nodiscard]] std::optional<std::string_view> prepare_participant_result_field_error(
    const PrepareParticipantResultDTO& result,
    const PreparedOperationParticipantDTO& participant) noexcept;

/// Construct an identity-preserving Prepare result and validate its closed
/// state/presence rules.
[[nodiscard]] Result<PrepareParticipantResultDTO> MakePrepareParticipantResult(
    const PreparedOperationParticipantDTO& participant, PrepareParticipantResultStateDTO state,
    std::optional<ErrorDTO> error = std::nullopt);

/// Validate the immediate handle returned by Dispatch.  A
/// prepared dispatch is not admitted into runtime observation unless the
/// backend echoes the exact Kernel goal generation and prepared-contract
/// digest and supplies both bounded observation callbacks.
[[nodiscard]] std::optional<std::string_view> prepared_execution_handle_field_error(
    const ExecutionHandle& handle, const PreparedOperationParticipantDTO& participant,
    const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation) noexcept;

/// Validate one status publication against the exact prepared participant and
/// Kernel-minted goal identity for that dispatch.
[[nodiscard]] std::optional<std::string_view> execution_backend_status_field_error(
    const ExecutionBackendStatusDTO& status, const PreparedOperationParticipantDTO& participant,
    const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation) noexcept;

/// Construct one exact backend status projection from the immutable prepared
/// participant and Kernel goal membership.
[[nodiscard]] Result<ExecutionBackendStatusDTO> MakeExecutionBackendStatus(
    const PreparedOperationParticipantDTO& participant, const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation, ExecutionBackendStateDTO state,
    double accepted_trajectory_time_s, double progress, std::uint64_t source_sequence,
    std::int64_t receiver_local_receipt_timestamp_ns, std::optional<ErrorDTO> error = std::nullopt);

}  // namespace manyforge::core
