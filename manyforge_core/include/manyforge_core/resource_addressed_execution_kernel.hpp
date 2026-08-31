#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_core/bounded_operation_control_executor.hpp>
#include <manyforge_core/bounded_stop_control_executor.hpp>
#include <manyforge_core/bounded_stop_stage_attempt_executor.hpp>
#include <manyforge_core/emergency_stop_acceptance_reserve.hpp>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <manyforge_core/orphan_resource_reconciliation.hpp>
#include <manyforge_core/ports/iaddressed_motion_preview.hpp>
#include <manyforge_core/ports/iaddressed_operation_owner.hpp>
#include <manyforge_core/ports/i_manager_liveness_fail_safe_port.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/ports/istop_surveillance_observation_port.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/route_selection_owner.hpp>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/stop_fault_recovery.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxI6OperationReplayEntries = 4096U;

struct RuntimeParticipantExecutionRegistration final {
  RuntimeParticipantBinding binding;

  /// Independently verified current lifecycle/configuration/base-plan facts.
  /// Registration without this value remains observable but cannot admit a
  /// new operation because the Kernel could not prepare a truthful Stop plan.
  std::optional<RuntimeParticipantStopReadiness> stop_readiness;

  /// Complete retained deployment defaults used only while preparing the
  /// immutable ordinary participant Stop plan before operation dispatch.
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;

  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};

  /// Non-owning. The engine must remain alive and quiesce callbacks before
  /// this addressed owner is destroyed.
  IResolvedParticipantExecutionEngine* engine{};

  /// Exact prepared-stage realization boundary. This is deliberately
  /// separate from the historical subordinate engine: StopControl never
  /// calls that engine's deployment-wide Stop surface.
  IControlDomainExecutionPort* stop_execution_port{};

  /// Exact retained runtime policy sealed against this participant binding.
  /// Absence remains observable but motion admission fails closed.
  std::optional<RuntimeParticipantPolicySelection> runtime_policy{};

  /// Exact two-phase motion preparation boundary. Motion registrations must
  /// provide it; capability-command registrations leave it absent.
  IResolvedPreparedParticipantExecutionEngine* prepared_motion_engine{};

  /// Optional deployment-owned operator profile prepared into every exact
  /// ordinary motion Stop plan. Non-motion participants leave it absent.
  std::optional<MotionInterruptStopProfileDTO> default_operator_stop_profile;

  /// Optional deployment-owned fault profile used to prepare a second exact
  /// Stop plan for runtime-collision containment. It is sealed with the same
  /// operation/goal as the ordinary plan and never selected by caller input.
  std::optional<MotionInterruptStopProfileDTO> default_fault_stop_profile;
};

using RuntimeParticipantAdmissionResolver =
    std::function<Result<RuntimeParticipantExecutionRegistration>(const OperationIntentDTO&,
                                                                  std::size_t)>;
using StopFaultRecoveryFactsResolver =
    std::function<Result<StopFaultRecoveryFactsDTO>(const StopStatusDTO&, std::int64_t)>;

/// One construction-fixed post-terminal observation owner. The observer is
/// non-owning and must outlive the addressed Kernel. Absence is
/// reserved for explicitly Stop-surveillance-unqualified compositions; such a
/// composition can contain a Stop but can never claim late-quiescence or clear
/// a fault that requires it.
struct StopSurveillanceExecutionConfig final {
  IStopSurveillanceObservationPort* observer{};
  StopSurveillancePolicyDTO policy;
};

/// Atomic read-only projection of one retained operation. `resident` is the
/// exact Kernel-owned distinction between the active set and retained history;
/// separate admission and recovery projections retain their own authority.
struct OperationReadProjectionDTO final {
  OperationStateDTO operation;
  bool resident{};

  friend bool operator==(const OperationReadProjectionDTO&,
                         const OperationReadProjectionDTO&) = default;
};

/// One coherent, read-only projection of the current addressed execution
/// truth needed by the durable Kernel Disable-handoff owner.  Active
/// publication and configuration-fence generation remain owned by that
/// durable owner and are deliberately not duplicated here.
struct KernelDisableHandoffRuntimeSnapshotDTO final {
  ControllableComponentId controllable_component_id;
  DisableHandoffScopeRequestKindDTO request_kind{
      DisableHandoffScopeRequestKindDTO::kFullStopRequired};
  RequestOwnerEpoch request_owner_epoch;
  DisableHandoffScopeResolutionDTO scope_resolution{
      DisableHandoffScopeResolutionDTO::kPromotedToFull};
  ConfigurationFenceScopeDTO current_scope;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::uint64_t operation_resource_generation{};
  std::vector<CurrentDomainActiveControlConfigurationDTO> current_active_control_configurations;
  std::optional<ComponentLifecycleGeneration> current_component_lifecycle_generation;
  StopGeneration current_cell_stop_generation{0U};
  std::vector<OperationId> affected_queued_operations;
  std::uint64_t affected_active_operation_count{};

  friend bool operator==(const KernelDisableHandoffRuntimeSnapshotDTO&,
                         const KernelDisableHandoffRuntimeSnapshotDTO&) = default;
};

/// Compact owner projection for the existing full-cell Stop command. Detailed
/// evidence remains in the Stop certificate/history APIs; this status value
/// carries only the exact action fence and the latest outcome summary needed
/// by operators.
struct CellStopStageSummaryDTO final {
  OperationId operation_id;
  OperationParticipantId participant_id;
  StopEscalationStageId stage_id;
  std::size_t stage_index{};
  StopActionClass action_class{StopActionClass::kControlled};
  ParticipantStopStageOutcomeDTO outcome{ParticipantStopStageOutcomeDTO::kFailed};
  std::optional<MotionControlledStopMode> executed_mode;
  std::optional<std::string> substitution_reason;

  friend bool operator==(const CellStopStageSummaryDTO&,
                         const CellStopStageSummaryDTO&) = default;
};

struct CellStopTerminalSummaryDTO final {
  StopCertificateRefDTO certificate;
  StopCertificateResultDTO result{StopCertificateResultDTO::kFailedUncontained};
  bool escalated{};
  bool hard_stop_floor_used{};
  std::vector<CellStopStageSummaryDTO> executed_stages;

  friend bool operator==(const CellStopTerminalSummaryDTO&,
                         const CellStopTerminalSummaryDTO&) = default;
};

struct CellStopFaultSummaryDTO final {
  StopFaultLatchId latch_id;
  StopFaultLatchGeneration latch_generation;
  StopFaultLatchStateDTO state{StopFaultLatchStateDTO::kLatched};
  StopFaultTriggerDTO trigger{StopFaultTriggerDTO::kPersistenceDegraded};
  StopFaultRecoveryStatusDTO recovery_status{
      StopFaultRecoveryStatusDTO::kWaitingForObservation};
  std::string reason_code;
  std::optional<StopCertificateRefDTO> certificate;

  friend bool operator==(const CellStopFaultSummaryDTO&,
                         const CellStopFaultSummaryDTO&) = default;
};

struct CellStopActionProjectionDTO final {
  StopGeneration expected_stop_generation{0U};
  std::optional<CellStopTerminalSummaryDTO> latest_terminal;
  std::optional<CellStopFaultSummaryDTO> fault;

  friend bool operator==(const CellStopActionProjectionDTO&,
                         const CellStopActionProjectionDTO&) = default;
};

struct ResourceAddressedExecutionKernelConfig final {
  ResourceAddressedExecutionKernelConfig() = default;
  ResourceAddressedExecutionKernelConfig(
      RequestOwnerEpoch owner_epoch, CellId cell_identity, std::size_t replay_capacity,
      RuntimeParticipantAdmissionResolver resolver = {},
      IResourceOperationAdmissionGate* admission_gate = nullptr,
      std::optional<GraphRevision> current_graph_revision = {},
      std::optional<StopDeadlineBudgetDTO> stop_budget = {},
      std::size_t stop_correlation_capacity = kMaximumEmergencyStopCorrelationEntries,
      std::size_t maximum_live_stop_participants = 0U,
      std::size_t forensic_projection_capacity = 0U,
      std::optional<StopSurveillanceExecutionConfig> surveillance = {},
      StopFaultRecoveryFactsResolver recovery_facts_resolver = {})
      : request_owner_epoch(std::move(owner_epoch)),
        cell_id(std::move(cell_identity)),
        request_replay_capacity(replay_capacity),
        participant_resolver(std::move(resolver)),
        operation_admission_gate(admission_gate),
        graph_revision(std::move(current_graph_revision)),
        cell_stop_deadline_budget(std::move(stop_budget)),
        emergency_stop_correlation_capacity(stop_correlation_capacity),
        prequalified_max_live_stop_participants(maximum_live_stop_participants),
        stop_forensic_projection_capacity(forensic_projection_capacity),
        stop_surveillance(std::move(surveillance)),
        stop_fault_recovery_facts_resolver(std::move(recovery_facts_resolver)) {}

  RequestOwnerEpoch request_owner_epoch;
  CellId cell_id;
  std::size_t request_replay_capacity{1024U};
  /// Optional exact product-backed lookup. The Kernel revalidates the sealed
  /// binding against the request and accepts only a lifetime-pinned engine
  /// already present in registrations.
  RuntimeParticipantAdmissionResolver participant_resolver;
  /// Optional process-local configuration-fence interlock. It must outlive
  /// this Kernel. The interlock serializes the real slot-table reservation;
  /// it owns no duplicate operation counters or resource state.
  IResourceOperationAdmissionGate* operation_admission_gate{};
  /// Exact current cell graph used by component/resource Stop resolution. If
  /// omitted, construction derives it only when every registered participant
  /// names one identical non-empty graph revision.
  std::optional<GraphRevision> graph_revision;
  /// Cell-wide outer/reserve envelope. Participant phase tables remain local
  /// to their prepared plans. If omitted, construction derives the common
  /// envelope from verified readiness and fails when no such referent exists.
  std::optional<StopDeadlineBudgetDTO> cell_stop_deadline_budget;
  std::size_t emergency_stop_correlation_capacity{kMaximumEmergencyStopCorrelationEntries};
  /// Maximum number of participants whose Stop cascades may be live at once.
  /// Zero derives the exact bound from the initially registered execution
  /// engines. A nonzero value permits later reconfiguration growth without
  /// allocating attempt isolation after Stop ingress opens.
  std::size_t prequalified_max_live_stop_participants{};
  /// Fixed asynchronous acceptance/terminal forensic-projection slots. Zero
  /// derives the normal correlation-plus-terminal-history bound. A smaller
  /// positive bound is legal: exhaustion degrades persistence and closes
  /// admission, but cannot reject or pin an already accepted Stop.
  std::size_t stop_forensic_projection_capacity{};
  /// Optional only for explicitly unqualified compositions. When present,
  /// every field is validated together before operation admission opens.
  std::optional<StopSurveillanceExecutionConfig> stop_surveillance;
  /// Construction-fixed current-facts owner. It reads only owner-issued
  /// immutable facts; callers cannot supply recovery evidence.
  StopFaultRecoveryFactsResolver stop_fault_recovery_facts_resolver;
  /// Exact retained descriptor identity used only when the Kernel is
  /// bootstrapped with no runtime participants. It must be supplied together
  /// with `empty_runtime_graph_revision`; participant-backed configurations
  /// derive both identities from the verified registrations.
  std::optional<RetainedBundleId> empty_runtime_retained_bundle_id;
  /// Exact graph identity paired with `empty_runtime_retained_bundle_id` for
  /// an empty-runtime bootstrap. It is never a constraint on later non-empty
  /// participant reconfiguration.
  std::optional<GraphRevision> empty_runtime_graph_revision;
  std::size_t cell_admission_claim_capacity{1024U};
  CellAdmissionInterventionValidator cell_admission_intervention_validator;
  CellAdmissionSettlementValidator cell_admission_settlement_validator;
  CellAdmissionReconcileValidator cell_admission_reconcile_validator;
  std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set;
};

/// Private no-effect result used to project a capability-command preview at a
/// trusted transport boundary. The public receipt is minted by that boundary;
/// this value carries no reusable admission or reservation.
struct CapabilityCommandPreviewResultDTO final {
  RuntimeParticipantBindingDTO binding;
  GripperCommandRequestDTO command_request;
};

struct OperationRecoveryParticipantDTO final {
  OperationParticipantId operation_participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlBindingId control_binding_id;
  std::optional<CapabilityProviderBindingId> capability_provider_binding_id;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation{0U};
  std::vector<ResourceId> resource_ids;
};

struct OperationRecoveryProjectionDTO final {
  OperationRevision operation_revision{0U};
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  KernelRecoveryAssessmentDTO assessment;
  std::vector<OperationRecoveryParticipantDTO> participants;
};

/// Resource-addressed Kernel owner.
///
/// Independent operations own exact resource scopes and may remain active
/// concurrently when those scopes are disjoint. Existing ExecutionKernel
/// instances remain private single-participant subordinate mechanisms.
class ResourceAddressedExecutionKernel final : public IAddressedOperationOwner,
                                               public IAddressedMotionPreviewOwner,
                                               public IKernelCellAdmissionPort,
                                               public IManagerLivenessObservationPort,
                                               public IManagerLivenessFailSafePort,
                                               private IStopControlWorkReceiver,
                                               private IStopStageAttemptCompletionReceiver,
                                               private IOperationControlEffectCompletionReceiver {
 public:
  ResourceAddressedExecutionKernel(
      std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
      IScheduler& scheduler, IOperationJournal& journal,
      ResourceAddressedExecutionKernelConfig config);
  ~ResourceAddressedExecutionKernel();

  ResourceAddressedExecutionKernel(const ResourceAddressedExecutionKernel&) = delete;
  ResourceAddressedExecutionKernel& operator=(const ResourceAddressedExecutionKernel&) = delete;

  /// Resolve, reserve, and submit one portable operation. Exact replay returns
  /// the original owner-minted identities and performs no subordinate call.
  [[nodiscard]] Result<OperationAcceptedDTO> execute(const OperationIntentDTO& intent) override;

  [[nodiscard]] Result<std::unique_ptr<IAddressedMotionPreviewSession>>
  begin_motion_preview_session(const OperationIntentDTO& primary_motion) override;

  /// Resolve and validate one capability command without reserving,
  /// journaling, changing observed state, or dispatching a command.
  [[nodiscard]] Result<CapabilityCommandPreviewResultDTO> preview_capability_command(
      const OperationIntentDTO& intent);

  [[nodiscard]] Result<OperationControlAcceptedDTO> pause(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> resume(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> reset(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> acknowledge_completed(
      const OperationControlRequestDTO& request) override;

  [[nodiscard]] Result<StopAcceptedDTO> stop(const StopIntentDTO& intent);
  [[nodiscard]] Result<StopAcceptedDTO> full_stop(const FullStopIntentDTO& intent);
  [[nodiscard]] Result<StopAcceptedDTO> recognized_malformed_stop(
      const MalformedStopIngressDTO& ingress);

  [[nodiscard]] CellStopActionProjectionDTO cell_stop_action_projection() const;

  /// Atomically derives the current exact component scope or conservative
  /// Full-Stop promotion from the registered physical truth and slot table.
  /// This performs no I/O, fence acquisition, Stop, reservation, or replay
  /// mutation.
  [[nodiscard]] Result<KernelDisableHandoffRuntimeSnapshotDTO>
  capture_disable_handoff_runtime_snapshot(const DisableHandoffAdmissionQueryDTO& query) const;

  /// Re-resolve one already persisted Disable request from its immutable
  /// semantic expectations.  Unlike the observation overload, this does not
  /// reconstruct or self-attest the earlier component-snapshot digest.
  [[nodiscard]] Result<KernelDisableHandoffRuntimeSnapshotDTO> revalidate_disable_handoff_request(
      const DisableHandoffRequestDTO& request) const;

  [[nodiscard]] Result<StopStatusDTO> get_stop_status(const StopId& stop_id) const;
  [[nodiscard]] Result<StopCertificateDTO> get_stop_certificate(
      const StopCertificateId& certificate_id) const;
  [[nodiscard]] std::optional<StopFaultLatchDTO> get_stop_fault_latch() const;
  [[nodiscard]] Result<std::optional<StopFaultRecoveryOfferDTO>> get_stop_fault_recovery_offer()
      const;
  [[nodiscard]] Result<StopFaultRecoveryResultDTO> recover_stop_fault(
      const StopFaultRecoveryIntentDTO& intent);
  [[nodiscard]] Result<RuntimeScaleAcceptedDTO> set_runtime_scale(
      const RuntimeScaleRequestDTO& request);
  [[nodiscard]] Result<RuntimeScaleStatusDTO> get_runtime_scale(
      const OperationId& operation_id) const;

  [[nodiscard]] AuthoringAdmissionDTO GetAuthoringAdmission() const override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot) override;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) override;
  [[nodiscard]] Result<CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const override;
  [[nodiscard]] Result<void> Publish(
      const ManagerLivenessObservationDTO& observation) override;
  void LatchAndContain(
      const ManagerLivenessLossIntentDTO& intent) noexcept override;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReplaceManagerLivenessRequiredSet(
      const ManagerLivenessRequiredSetDTO& required_set);

  /// Read-only owner projection. Subordinate polling and state advancement are
  /// performed only by reconcile_subordinate_state(), never by this method.
  [[nodiscard]] Result<OperationStateDTO> get_operation_state(
      const OperationId& operation_id) const override;

  /// Read-only owner projection that atomically distinguishes an active
  /// resident operation from retained history.
  [[nodiscard]] Result<OperationReadProjectionDTO> get_operation_projection(
      const OperationId& operation_id) const;

  [[nodiscard]] Result<OperationRecoveryProjectionDTO>
  get_operation_recovery_assessment(const OperationId& operation_id) const;

  [[nodiscard]] Result<KernelRecoveryActionResultDTO>
  apply_operation_recovery_action(
      const OperationId& operation_id,
      OperationRevision expected_operation_revision,
      const KernelRecoveryActionRequestDTO& request);

  /// NRT owner-scheduler reconciliation point. This closes event-loss windows
  /// and advances capability-command completion without making public reads
  /// effectful.
  [[nodiscard]] Result<void> reconcile_subordinate_state() override;

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> resource_snapshot() const;

  /// One coherent generation-stamped read used by the private runtime
  /// reconciliation owner. Public callers cannot mutate or release slots from
  /// this projection.
  [[nodiscard]] ResourceSlotTableSnapshotDTO resource_table_snapshot() const;

  /// Command-free, process-local release of an exact subset of durable
  /// orphan-resource fences. Facts come only from the installed runtime owner;
  /// the Kernel independently joins them to its current binding, route, and
  /// slot generation before one atomic release.
  [[nodiscard]] Result<OrphanResourceReconciliationResultDTO> reconcile_orphaned_resources(
      const OrphanResourceReconciliationFactsDTO& facts);

  /// Atomically select a verified candidate participant set while preserving
  /// the stable addressed owner, operation journal, replay ledgers, and any
  /// unaffected in-flight operation. The caller must hold the exact durable
  /// configuration fence for every changed resource.
  [[nodiscard]] Result<void> reconfigure_runtime_participants(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver = {});

  /// Publish the candidate-side active-configuration identities already
  /// declared by the installed registrations while the exact durable Apply
  /// fence still closes every affected resource. This does not publish Stop
  /// readiness or create a CCLC route predecessor: route-fence admission sees
  /// the separately tracked authoritative publication inventory. CCLC must
  /// replace these staged facts with independently verified readiness before
  /// addressed command admission can reopen.
  [[nodiscard]] Result<void> stage_current_runtime_configuration_facts_under_apply_fence(
      const ConfigurationFenceScopeDTO& scope);

  /// Rehydrate one exact durable route predecessor after execution-host
  /// replacement without claiming that the new provider incarnation is
  /// Stop-ready. The supplied registrations must name the predecessor's
  /// domain/configuration but carry no readiness for that domain. This keeps
  /// operation admission closed while allowing CCLC to acquire the exact
  /// route fence and publish a freshly observed successor configuration.
  [[nodiscard]] Result<RuntimeRouteConfigurationReadbackDTO>
  adopt_runtime_route_configuration_predecessor(
      ActiveControlConfigurationDTO configuration,
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver = {});

  /// Atomically install one reconciled domain's complete runtime
  /// registrations and independently retained readiness projection. An
  /// unreconciled publication carries no candidate registrations and closes
  /// admission for that domain while leaving its observable old runtime
  /// inventory intact under the held route fence.
  [[nodiscard]] Result<RuntimeRouteConfigurationReadbackDTO> publish_runtime_route_configuration(
      ActiveControlConfigurationDTO configuration,
      std::vector<RuntimeParticipantExecutionRegistration> registrations = {},
      RuntimeParticipantAdmissionResolver participant_resolver = {});

  /// Read back only the exact current domain/configuration generation.
  [[nodiscard]] Result<RuntimeRouteConfigurationReadbackDTO> runtime_route_configuration_readback(
      const ControlDomainId& control_domain_id,
      const ActiveControlConfigurationId& active_configuration_id,
      ActiveControlConfigurationGeneration active_configuration_generation) const;

  /// Current Kernel-owned route publication fact for one domain. Absence is
  /// the exact pre-publication state; this performs no mutation or provider
  /// I/O.
  [[nodiscard]] Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>
  current_runtime_route_configuration(const ControlDomainId& control_domain_id) const;

  /// A removed subordinate engine stays lifetime-pinned while an operation
  /// still references it. Once this returns false, release_retired_engine()
  /// removes the remaining event subscription before the caller destroys the
  /// engine.
  [[nodiscard]] bool participant_engine_in_use(
      const IResolvedParticipantExecutionEngine* engine) const;
  [[nodiscard]] Result<void> release_retired_engine(IResolvedParticipantExecutionEngine* engine);

  [[nodiscard]] std::shared_ptr<const ResourceSlotTable> resource_admission_table() const noexcept {
    return resource_slots_;
  }
  [[nodiscard]] const RequestOwnerEpoch& request_owner_epoch() const noexcept override {
    return config_.request_owner_epoch;
  }

 private:
  struct ReplayEntry;
  struct ControlReplayEntry;
  struct StopReplayEntry;
  struct FullStopReplayEntry;
  struct RuntimeScaleReplayEntry;
  struct ActiveOperation;
  struct StopTransaction;
  struct StopTerminalRecord;
  struct StopProjectionJob;
  struct PartialDispatchStopScope;
  struct DisableRuntimeExpectation;
  class StopProjectionWorker;
  struct EngineSubscription;
  struct PreviewSession;

  [[nodiscard]] const RuntimeParticipantExecutionRegistration* registration_for(
      const RuntimeParticipantBinding& binding) const noexcept;
  [[nodiscard]] Result<RuntimeParticipantExecutionRegistration> resolve_registration(
      const OperationIntentDTO& operation, std::size_t participant_index) const;
  [[nodiscard]] StopScopeResolverSnapshotDTO capture_stop_scope_locked(
      const ResourceSlotTableSnapshotDTO& slots) const;
  [[nodiscard]] ActiveOperation* find_active_operation_locked(
      const OperationId& operation_id) noexcept;
  [[nodiscard]] const ActiveOperation* find_active_operation_locked(
      const OperationId& operation_id) const noexcept;
  [[nodiscard]] std::optional<OperationId> find_event_operation_locked(
      const IResolvedParticipantExecutionEngine* engine, std::uint64_t subordinate_id) const;
  void on_engine_event(IResolvedParticipantExecutionEngine* engine, const KernelEvent& event);
  [[nodiscard]] Result<void> apply_subordinate_state(const OperationId& operation_id,
                                                     IResolvedParticipantExecutionEngine* engine,
                                                     std::uint64_t subordinate_id,
                                                     OperationStateKind state, double progress,
                                                     std::optional<ErrorDTO> terminal_error);
  [[nodiscard]] Result<void> advance_prepared_motion(const OperationId& operation_id,
                                                     IResolvedParticipantExecutionEngine* engine);
  [[nodiscard]] Result<void> advance_prepared_resume(const OperationId& operation_id,
                                                     IResolvedParticipantExecutionEngine* engine);
  [[nodiscard]] Result<void> advance_prepared_capability_command(
      const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine);
  [[nodiscard]] Result<void> release_prepared_dispatch_barrier(
      const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
      std::uint64_t preparation_id, const PreparedOperationParticipantDTO& participant_product);
  [[nodiscard]] Result<OperationAcceptedDTO> execute_coordinated_motion_after_preflight(
      const OperationIntentDTO& intent);
  [[nodiscard]] Result<void> fail_coordinated_pre_dispatch(
      const OperationId& operation_id, IResolvedParticipantExecutionEngine* failing_engine,
      ErrorDTO failure);
  [[nodiscard]] Result<OperationControlAcceptedDTO> apply_control(
      const OperationControlRequestDTO& request, OperationControlKind control);
  void CompleteOperationControlEffect(OperationControlEffectResult result) noexcept;
  [[nodiscard]] Result<void> settle_control_operation_locked(const OperationId& operation_id,
                                                             OperationControlKind control);
  [[nodiscard]] Result<void> reconfigure_runtime_participants_impl(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver,
      bool retain_unready_configuration_facts);
  [[nodiscard]] StopIntentDTO internal_control_stop_intent_locked(
      const ActiveOperation& operation, const OperationControlAcceptedDTO& accepted,
      std::string_view reason) const;
  [[nodiscard]] Result<void> apply_runtime_scale_status(
      IResolvedParticipantExecutionEngine* engine,
      const ResolvedRuntimeScaleStatusDTO& resolved_status);
  void ProgressStop(StopControlWorkToken token) noexcept override;
  void StageAttemptCompleted(StopStageAttemptHandle handle) noexcept override;
  void OperationControlEffectCompleted(BoundedOperationControlExecutor& executor,
                                       OperationControlEffectHandle handle) noexcept override;
  void ProjectStop(StopControlWorkToken token, std::stop_token cancellation) noexcept;
  void RunStopSurveillance(StopCertificateId certificate_id,
                           std::uint64_t schedule_generation) noexcept;
  void ScheduleStopSurveillance(StopCertificateId certificate_id,
                                std::int64_t deadline_ns) noexcept;
  [[nodiscard]] std::optional<StopControlWorkToken> allocate_stop_projection_job_locked() noexcept;
  [[nodiscard]] bool commit_stop_terminal_locked(StopTransaction& transaction,
                                                 std::int64_t completed_at_ns) noexcept;
  [[nodiscard]] Result<StopAcceptedDTO> accept_stop(
      EmergencyStopProjectedIntentDTO intent,
      const PartialDispatchStopScope* partial_dispatch_scope = nullptr,
      const MalformedStopIngressDTO* malformed_ingress = nullptr);
  [[nodiscard]] Result<KernelDisableHandoffRuntimeSnapshotDTO>
  capture_disable_handoff_runtime_snapshot_impl(const DisableRuntimeExpectation& expectation) const;

  RuntimeParticipantRegistry registry_;
  std::shared_ptr<std::recursive_mutex> admission_mutex_;
  std::shared_ptr<ResourceSlotTable> resource_slots_;
  std::vector<RuntimeParticipantExecutionRegistration> registrations_;
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO>
      runtime_route_configuration_readbacks_;
  std::set<ControlDomainId> runtime_route_configuration_publication_required_;
  IClock& clock_;
  IScheduler& scheduler_;
  IOperationJournal& journal_;
  ResourceAddressedExecutionKernelConfig config_;
  std::unique_ptr<KernelCellAdmissionOwner> cell_admission_owner_;

  // Serializes public Execute admission while permitting synchronous engine
  // callbacks to take state_mutex_. The subordinate execute call is required
  // to be prompt/non-blocking by its port contract.
  mutable std::mutex submission_mutex_;
  // Serializes the non-emergency durable fault-clear workflow. Stop ingress
  // never takes this mutex, so a slow current-facts owner or journal cannot
  // delay emergency containment.
  mutable std::mutex stop_fault_recovery_mutex_;
  mutable std::mutex operation_recovery_mutex_;
  mutable std::mutex state_mutex_;
  std::vector<ReplayEntry> replay_ledger_;
  std::vector<ControlReplayEntry> control_replay_ledger_;
  std::vector<StopReplayEntry> stop_replay_ledger_;
  std::vector<FullStopReplayEntry> full_stop_replay_ledger_;
  std::vector<RuntimeScaleReplayEntry> runtime_scale_replay_ledger_;
  struct StopFaultRecoveryReplayEntry;
  std::vector<StopFaultRecoveryReplayEntry> stop_fault_recovery_replay_ledger_;
  struct OperationRecoveryReplayEntry;
  std::vector<OperationRecoveryReplayEntry> operation_recovery_replay_ledger_;
  std::vector<OperationStateDTO> historical_operations_;
  /// Canonical bounded resident operation table. Every accepted operation also
  /// consumes one entry in the fixed-capacity replay ledger, so this table can
  /// never exceed request_replay_capacity.
  std::map<OperationId, std::unique_ptr<ActiveOperation>> active_operations_;
  std::vector<EngineSubscription> engine_subscriptions_;
  // Engaged exactly once during successful construction and replaced only by
  // a fully validated runtime reconfiguration.
  std::optional<GraphRevision> stop_graph_revision_;
  std::optional<RetainedBundleId> stop_retained_bundle_id_;
  std::optional<StopDeadlineBudgetDTO> cell_stop_deadline_budget_;
  std::vector<StopScopeComponentSnapshotDTO> stop_component_scope_;
  EmergencyStopAcceptanceReserve emergency_stop_reserve_;
  std::vector<std::unique_ptr<StopTransaction>> stop_transactions_;
  std::vector<std::unique_ptr<StopTerminalRecord>> stop_terminal_history_;
  std::vector<std::optional<StopAcceptedDTO>> stop_correlations_;
  std::vector<std::unique_ptr<StopProjectionJob>> stop_projection_jobs_;
  std::vector<std::uint64_t> stop_projection_generations_;
  std::unique_ptr<StopProjectionWorker> stop_projection_worker_;
  std::unique_ptr<BoundedStopControlExecutor> stop_control_executor_;
  std::unique_ptr<BoundedStopStageAttemptExecutor> stop_stage_attempt_executor_;
  std::unique_ptr<BoundedOperationControlExecutor> operation_control_executor_;
  bool operation_control_reconfiguration_in_progress_{};
  std::size_t prequalified_max_live_stop_participants_{};
  std::condition_variable stop_surveillance_schedule_cv_;
  std::size_t stop_surveillance_schedule_publications_{};
  bool stop_surveillance_shutting_down_{};
  bool motion_admission_unreconciled_{};
  std::uint64_t next_operation_id_{1U};
  std::uint64_t next_stop_id_{1U};
  StopGeneration cell_stop_generation_{0U};
  StopFaultLatchGeneration cell_stop_fault_latch_generation_{0U};
  std::optional<StopFaultLatchDTO> active_stop_fault_latch_;
  bool manager_liveness_motion_inhibited_{};
  bool motion_admission_closed_{};
};

}  // namespace manyforge::core
