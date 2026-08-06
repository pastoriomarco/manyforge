#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/time/iclock.hpp>

namespace manyforge::core {

inline constexpr std::size_t kMaxI6OperationReplayEntries = 4096U;

enum class OperationStateKind {
  kQueued,
  kResolving,
  kPlanning,
  kValidating,
  kPrepared,
  kDispatching,
  kExecuting,
  kPaused,
  kStopping,
  kCompleted,
  kStopped,
  kFailed,
  kInterrupted,
  kInterventionRequired,
  kCancelledPreDispatch,
};

enum class OperationParticipantPayloadKind {
  kMotion,
  kCapabilityCommand,
};

struct OperationParticipantStateDTO final {
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  std::optional<CapabilityProviderBindingId> capability_provider_binding_id;
  std::optional<CapabilityProviderBindingRevision>
      capability_provider_binding_revision;
  OperationParticipantPayloadKind payload_kind{
      OperationParticipantPayloadKind::kMotion};
  OperationStateKind state{OperationStateKind::kResolving};
  double progress{};
  std::vector<ResourceId> resources;
  std::optional<ErrorDTO> terminal_error;
};

/// Bounded I6 observation for the one admitted operation. It intentionally
/// carries final addressed identities rather than exposing the subordinate
/// engine's legacy robot/group fields.
struct OperationStateDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
  OperationCoordinationPolicy coordination_policy{
      OperationCoordinationPolicy::kIndependent};
  OperationStateKind state{OperationStateKind::kResolving};
  std::vector<OperationParticipantStateDTO> participants;
  std::int64_t updated_at_ns{};
};

enum class OperationControlKind {
  kPause,
  kResume,
  kReset,
  kAcknowledgeCompleted,
};

struct OperationControlRequestDTO final {
  PublicRequestContextDTO request_context;
  OperationId operation_id;
  OperationRevision expected_operation_revision;
  std::optional<std::string> reason;

  friend bool operator==(const OperationControlRequestDTO&,
                         const OperationControlRequestDTO&) = default;
};

struct OperationControlAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  OperationId operation_id;
  OperationRevision accepted_against_revision;
  OperationControlKind control{OperationControlKind::kPause};
  OperationRevision operation_revision_after_acceptance;
  std::int64_t accepted_at_ns{};
};

struct RuntimeParticipantExecutionRegistration final {
  RuntimeParticipantBinding binding;

  /// Non-owning. The engine must remain alive and quiesce callbacks before
  /// this addressed owner is destroyed.
  IResolvedParticipantExecutionEngine* engine{};
};

struct ResourceAddressedExecutionKernelConfig final {
  RequestOwnerEpoch request_owner_epoch;
  std::size_t request_replay_capacity{1024U};
};

/// I6 resource-addressed Kernel owner.
///
/// The current restoration gate deliberately admits one operation with one
/// participant. Existing ExecutionKernel instances remain private subordinate
/// planning/execution mechanisms. I8 later expands operation coordination and
/// concurrency without changing the public component/target/resource identity.
class ResourceAddressedExecutionKernel final {
 public:
  ResourceAddressedExecutionKernel(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      IClock& clock, ResourceAddressedExecutionKernelConfig config);
  ~ResourceAddressedExecutionKernel();

  ResourceAddressedExecutionKernel(const ResourceAddressedExecutionKernel&) =
      delete;
  ResourceAddressedExecutionKernel& operator=(
      const ResourceAddressedExecutionKernel&) = delete;

  /// Resolve, reserve, and submit one portable operation. Exact replay returns
  /// the original owner-minted identities and performs no subordinate call.
  [[nodiscard]] Result<OperationAcceptedDTO> execute(
      const OperationIntentDTO& intent);

  [[nodiscard]] Result<OperationControlAcceptedDTO> pause(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> resume(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> reset(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> acknowledge_completed(
      const OperationControlRequestDTO& request);

  /// Read-only owner projection. Subordinate polling and state advancement are
  /// performed only by reconcile_subordinate_state(), never by this method.
  [[nodiscard]] Result<OperationStateDTO> get_operation_state(
      const OperationId& operation_id) const;

  /// NRT owner-scheduler reconciliation point. This closes event-loss windows
  /// and advances capability-command completion without making public reads
  /// effectful.
  [[nodiscard]] Result<void> reconcile_subordinate_state();

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> resource_snapshot()
      const;
  [[nodiscard]] const RequestOwnerEpoch& request_owner_epoch() const noexcept {
    return config_.request_owner_epoch;
  }

 private:
  struct ReplayEntry;
  struct ControlReplayEntry;
  struct ActiveOperation;
  struct EngineSubscription;

  [[nodiscard]] IResolvedParticipantExecutionEngine* engine_for(
      const RuntimeParticipantBinding& binding) const noexcept;
  void on_engine_event(IResolvedParticipantExecutionEngine* engine,
                       const KernelEvent& event);
  [[nodiscard]] Result<void> apply_subordinate_state(
      IResolvedParticipantExecutionEngine* engine,
      std::uint64_t subordinate_id, OperationStateKind state, double progress,
      std::optional<ErrorDTO> terminal_error);
  [[nodiscard]] Result<OperationControlAcceptedDTO> apply_control(
      const OperationControlRequestDTO& request,
      OperationControlKind control);

  RuntimeParticipantRegistry registry_;
  ResourceSlotTable resource_slots_;
  std::vector<RuntimeParticipantExecutionRegistration> registrations_;
  IClock& clock_;
  ResourceAddressedExecutionKernelConfig config_;

  // Serializes public Execute admission while permitting synchronous engine
  // callbacks to take state_mutex_. The subordinate execute call is required
  // to be prompt/non-blocking by its port contract.
  mutable std::mutex submission_mutex_;
  mutable std::mutex control_mutex_;
  mutable std::mutex state_mutex_;
  std::vector<ReplayEntry> replay_ledger_;
  std::vector<ControlReplayEntry> control_replay_ledger_;
  std::unique_ptr<ActiveOperation> active_operation_;
  std::vector<EngineSubscription> engine_subscriptions_;
  std::uint64_t next_operation_id_{1U};
};

}  // namespace manyforge::core
