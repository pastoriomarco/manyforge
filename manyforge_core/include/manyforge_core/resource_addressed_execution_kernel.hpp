#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_core/bounded_operation_control_executor.hpp>
#include <manyforge_core/bounded_stop_control_executor.hpp>
#include <manyforge_core/bounded_stop_stage_attempt_executor.hpp>
#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/indexed_ledger.hpp>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <manyforge_core/ports/iaddressed_motion_preview.hpp>
#include <manyforge_core/ports/iaddressed_operation_owner.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/ports/iio_signal_service.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/runtime_route_configuration.hpp>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/stop_state.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_core/types/io_activation_rule.hpp>
#include <manyforge_core/types/io_rule_service_configuration.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <manyforge_core/types/program_blackboard_context.hpp>
#include <manyforge_core/types/program_execution_control.hpp>
#include <manyforge_core/types/program_io.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::core {

struct ActiveOperationParticipant;
/// The E1 slot-table fence scratch, reserved at construction for
/// `kMaxKernelResourceSlots` slots and `N_max` owners. Defined with the
/// span-based `stop_fence_batch` API in `resource_slot_table.hpp` (slice K2).
struct ResourceSlotStopFenceScratch;

inline constexpr std::size_t kMaxI6OperationReplayEntries = 4096U;

/// An immediate component step. A short direct Kernel call: admitted
/// like an operation of its component, then the adapter's atomic step runs
/// synchronously, outside the Kernel locks, and nothing is stored (no
/// operation record, journal entry or acknowledgement). No Stop ends it.
/// Users: Set TCP; the tool payload update (`set_tcp_load`).
struct ImmediateComponentStep final {
  /// Hold the component's engine for the step: refused (`participant_engine_busy`)
  /// while an operation of that engine is admitted, and no operation of that
  /// engine is admitted while the step runs.
  bool holds_engine{};
  /// The adapter's atomic step; its result is the call's result.
  std::function<Result<void>()> run;
};

/// Active TCP: a robot's Set TCP, an immediate component
/// step. The resolver supplies it with the robot's motion-target registration;
/// the resolved entry is `tcp_entry_of(*binding.motion_endpoint,
/// motion_endpoint_owner.value_or(binding.controllable_component_id))`.
struct SetTcpStep final {
  /// "Send TCP offset to controller" on: Set TCP holds the robot
  /// (`ImmediateComponentStep::holds_engine`). Off: it holds nothing and may
  /// run during a move.
  bool holds_robot{};
  /// The robot's confirmed TCP step, then the commit. Ok once the robot's
  /// Active TCP is `entry` (also when it already was, and while the robot is
  /// not enabled). `robot_control`: the registration's
  /// `intrinsic_direct_control` (Robot control's Set TCP, else a Program's),
  /// recorded with the change.
  std::function<Result<void>(const TcpEntryReferenceDTO& entry, bool robot_control)> apply;
};

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

  /// Non-owning. The engine must remain alive through addressed-owner
  /// destruction while the owner closes, unsubscribes and drains callback
  /// ingress.
  IResolvedParticipantExecutionEngine* engine{};

  /// Exact prepared-stage realization boundary. This is deliberately
  /// separate from the historical subordinate engine: StopControl never
  /// calls that engine's deployment-wide Stop surface.
  IControlDomainExecutionPort* stop_execution_port{};

  /// Exact motion policy sealed against this participant binding.
  /// Absence remains observable but motion admission fails closed.
  std::optional<RuntimeParticipantPolicySelection> participant_policy{};

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

  /// Sole configured TCP linear-speed ceiling for this exact applied
  /// Deployment target. Motion admission fails closed when it is absent.
  std::optional<double> deployment_maximum_tcp_linear_speed_mps;

  /// Composition-owned classification from an exact compiler-direct product,
  /// never inferred from missing Program origin or a caller's source label.
  bool intrinsic_direct_control{};

  /// Retained Deployment ceilings for this exact target's manual moves.
  /// Applied by the shared preview/execute builder only for intrinsic direct
  /// control; caller omission cannot remove an owner-held ceiling.
  PerJointDynamicsLimitsDTO direct_control_joint_limits{};

  /// Cell-level policy from the currently applied Deployment. Absence leaves
  /// moving takeover unavailable; ordinary motion has no preparation budget.
  std::optional<double> motion_takeover_preparation_budget_s{};
  /// Exact existing sealed policy-catalog bindings for this participant.
  std::vector<IoMotionRulePolicyBindingDTO> io_motion_rule_policies{};
  /// Server-derived overlay from the exact current captured Program node.
  /// Never copied into the public operation intent or its replay digest.
  std::optional<MotionInterruptScopeDTO> captured_node_interrupt_scope{};
  /// Optional earlier suffix-planning deadline from the applied Deployment.
  std::optional<double> motion_takeover_planning_budget_s{};
  /// The owner of this operation's motion-endpoint frame (the motion
  /// component or a tool mounted on it), resolved from the cell kinematic
  /// index. It stamps the request's TCP frame; absent means the motion
  /// component itself.
  std::optional<ControllableComponentId> motion_endpoint_owner{};
  /// Active TCP: present on a robot's motion-target registration that
  /// can set its Active TCP. Absent: Set TCP is refused (`set_tcp_unavailable`).
  std::optional<SetTcpStep> set_tcp{};
  std::optional<PreviewTcpCapture> preview_tcp_capture{};
  // Kernel-assigned event subscription identity; never supplied as authority by a resolver.
  std::uint64_t engine_subscription_id{};
};

/// The third argument is the held run's resolution context when the operation
/// names the run the Kernel holds, else null (OD-234).
using RuntimeParticipantAdmissionResolver =
    std::function<Result<RuntimeParticipantExecutionRegistration>(
        const OperationIntentDTO&, std::size_t, const ProgramRunResolutionContext&)>;

/// Bounded canonical-safe infrastructure diagnostic. Valid NFC text retains
/// its bytes; otherwise an explicit reversible ASCII-byte representation is
/// used. This text never grants fault or recovery authority.
[[nodiscard]] std::string bounded_operation_journal_diagnostic_text(
    std::string_view value, std::size_t maximum_bytes);

/// First ordinary operation-journal write failure in this Workbench owner.
/// Independent of journal readability and retained until owner destruction.
/// This fact grants no Stop, recovery or physical-effect authority.
struct OperationJournalWriteFaultDTO final {
  std::string write_kind;
  /// Bounded code/message only; arbitrary backend details are not retained.
  ErrorDTO error;
  std::optional<OperationNumber> operation_id{};
  std::optional<OperationRevision> operation_revision{};
  std::optional<RequestKey> request_key{};
  std::optional<ProgramRunNumber> program_run_id{};
  std::optional<ProgramNodeExecutionScopeDTO> execution_scope{};
  /// OD-215: a level, not part of the first-record fact: set when a record
  /// is dropped because the queue is full, cleared when a batch leaves the
  /// queue written empty. The Headless disk check stops operation on it.
  bool queue_full{};

  friend bool operator==(const OperationJournalWriteFaultDTO&,
                         const OperationJournalWriteFaultDTO&) = default;
};

/// First contained internal fault (a callback threw on the Stop/safety
/// path) in this Workbench owner. Motion admission stays closed while it is
/// held; it is retained until owner destruction (Workbench restart), like the
/// operation-journal write fault. Later throws only increase `later_count`.
struct KernelInternalFaultDTO final {
  /// Typed code of the first cause, e.g. `scheduler_runtime_safety_callback_exception`.
  /// Its containment Halt (S11) is never refused, so no acceptance or refusal
  /// field is kept.
  std::string reason_code;
  /// Contained throws after the first one; they request no further Halt.
  std::uint64_t later_count{};

  friend bool operator==(const KernelInternalFaultDTO&, const KernelInternalFaultDTO&) = default;
};

/// Atomic read-only projection of one retained operation. `resident` is the
/// exact Kernel-owned distinction between the active set and retained history;
/// separate admission and recovery projections retain their own authority.
struct OperationReadProjectionDTO final {
  OperationStateDTO operation;
  /// True while the Kernel retains any authority over the operation: it is
  /// active, or its retired manual operation still holds a cell-admission
  /// claim. False only after that claim's release is visible in admission.
  bool resident{};
  std::optional<OperationControlAcceptedDTO> last_control{};
  bool control_pending{};

  friend bool operator==(const OperationReadProjectionDTO&,
                         const OperationReadProjectionDTO&) = default;
};

/// The runtime monitor's Stop of an operation (a `runtime_collision` Stop
/// event), copied from the Stop ring while its record is there. Fixed bytes:
/// the copy under the Kernel lock never allocates.
struct OperationTableRuntimeMonitorStopDTO final {
  BoundedStopText<kStopEventReasonBytes> reason{};
  BoundedStopText<kStopEventSourceLabelBytes> source_label{};
  RequestKey request_key{};
};

/// Bounds of the event stream's operations table (OD-205 (b)), chosen so the
/// `state` message cannot exceed its 4 MiB frame: at most
/// `kOperationTableMaximumEntries` entries (lowest operation ids first); an
/// entry carries its alarm facts only when every identity string is at most
/// `kOperationTableMaximumIdentityBytes` and it has at most
/// `kOperationTableMaximumParticipants` participants (else `complete` is
/// false). The first participant's error message is cut at
/// `kOperationTableMaximumMessageBytes` (the whole text stays in History).
inline constexpr std::size_t kOperationTableMaximumEntries = 32U;
inline constexpr std::size_t kOperationTableMaximumIdentityBytes = 128U;
inline constexpr std::size_t kOperationTableMaximumParticipants = 64U;
inline constexpr std::size_t kOperationTableMaximumMessageBytes = 256U;
/// The alarm subject's resource bound (`kMaximumOperatorAlarmSubjectResources`).
inline constexpr std::size_t kOperationTableMaximumSubjectResources = 16U;

/// The first participant's terminal error as an alarm shows it.
struct OperationTableErrorDTO final {
  std::string code;
  std::optional<std::string> reason;  // `details.reason`
  std::string message;                // cut at kOperationTableMaximumMessageBytes
  bool message_truncated{};
};

/// How a participant's terminal fact's error relates to its terminal error.
enum class OperationTableFactError : std::uint8_t { kNone, kSameAsTerminalError, kDifferent };

struct OperationTableTerminalFactDTO final {
  ParticipantExecutionFactKind kind{};
  ParticipantEffectKnowledge effect{};
  OperationTableFactError error{};
};

/// One participant's raw facts the Headless API reads for an operation alarm
/// (the "ordinary Program failure" inputs and the collision reason).
struct OperationTableParticipantDTO final {
  OperationStateKind state{};
  bool execution_scope{};
  bool terminal_error{};
  std::optional<std::string> terminal_error_reason;  // `details.reason`
  std::optional<OperationTableTerminalFactDTO> terminal_fact;
};

/// One resident operation that currently raises an operation alarm: its
/// state is failed, interrupted or intervention required, or its Stop event
/// is the runtime monitor's. Only what the alarm decision and text need.
struct OperationTableEntryDTO final {
  OperationNumber operation_id{0U};
  OperationRevision operation_revision{0U};
  OperationStateKind state{};
  std::optional<StopEventId> stop_event_id{};
  /// The kind of the accepted control still pending, if any.
  std::optional<OperationControlKind> pending_control{};
  std::optional<OperationTableRuntimeMonitorStopDTO> runtime_monitor_stop{};
  /// False when an identity or the participant count exceeds the table's
  /// bounds: the alarm facts below are then empty.
  bool complete{true};
  std::string program_binding_id;
  std::optional<ProgramRunNumber> program_run_id;
  /// The sorted union of every participant's resources, at most
  /// `kOperationTableMaximumSubjectResources`; `resource_total` counts all.
  std::vector<ResourceId> resources;
  std::size_t resource_total{};
  std::optional<OperationTableErrorDTO> terminal_error;
  std::vector<OperationTableParticipantDTO> participants;
};

/// Bound of the operations table's `resident` list (OD-213): at most this
/// many resident operations, lowest operation ids first (`resident_total`
/// counts them all). The host lists an operation without its state when the
/// encoded state exceeds `kOperationTableMaximumResidentOperationBytes`; a
/// reader then reads that operation. With the alarm list's bound the `state`
/// message stays within its 4 MiB frame.
inline constexpr std::size_t kOperationTableMaximumResidentEntries = 64U;
inline constexpr std::size_t kOperationTableMaximumResidentOperationBytes = 16U * 1024U;

/// The event stream's operations member: a counter raised at every change of
/// an operation's state, residence or pending control, the resident
/// operations that raise an alarm (lowest ids first, at most
/// `kOperationTableMaximumEntries`; `faulted_total` counts them all), and
/// every resident operation as the `read` reply projects it (lowest ids
/// first, at most `kOperationTableMaximumResidentEntries`; `resident_total`
/// counts them all). One Kernel section reads them all and `change`.
struct OperationTableDTO final {
  std::uint64_t change{};
  std::vector<OperationTableEntryDTO> faulted{};
  std::size_t faulted_total{};
  std::vector<OperationReadProjectionDTO> resident{};
  std::size_t resident_total{};
};

/// The Kernel's state lock. A section that changes an operation in memory
/// owes the event stream's change signal; the unlock raises it after the
/// mutex is released, so no second lock and no system call run under it.
class KernelStateMutex final {
 public:
  void lock() { mutex_.lock(); }
  [[nodiscard]] bool try_lock() { return mutex_.try_lock(); }
  void unlock() noexcept {
    const bool owed = std::exchange(change_signal_owed_, false);
    mutex_.unlock();
    if (owed) execution_event_change_signal().Notify();
  }
  /// Held: the change signal is raised at this section's unlock.
  void owe_change_signal() noexcept { change_signal_owed_ = true; }

 private:
  std::mutex mutex_;
  bool change_signal_owed_{false};
};

/// Private, read-only orchestration input. Shared verified products and node
/// indices identify the tasks to retire; this is not a motion admission offer.
struct ResolvedManualProgramHandover final {
  ProgramRunExecutionCaptureDTO capture;
  /// The Kernel's hold revision when resolved; the runner's handover cleanup
  /// is fenced on it (PD-8: cleanup at approve).
  std::uint64_t hold_revision{};
  std::vector<std::uint32_t> producer_node_indices;
};

/// Current eligibility to prepare direct control, not a dispatch permission.
/// A held Program may first require existing stopped-task cleanup.
struct ResolvedManualControlAvailability final {
  bool available{};
  std::optional<std::string> unavailable_reason;
  std::optional<ResolvedManualProgramHandover> handover;
  bool cleanup_required{};
};

struct ResourceAddressedExecutionKernelConfig final {
  ResourceAddressedExecutionKernelConfig() = default;
  ResourceAddressedExecutionKernelConfig(
      ApplicationStartId start_id, CellId cell_identity, std::size_t replay_capacity,
      RuntimeParticipantAdmissionResolver resolver = {},
      IResourceOperationAdmissionGate* admission_gate = nullptr,
      std::optional<GraphRevision> current_graph_revision = {},
      std::size_t maximum_live_stop_participants = 0U)
      : application_start_id(start_id),
        cell_id(std::move(cell_identity)),
        request_replay_capacity(replay_capacity),
        participant_resolver(std::move(resolver)),
        operation_admission_gate(admission_gate),
        graph_revision(std::move(current_graph_revision)),
        prequalified_max_live_stop_participants(maximum_live_stop_participants) {}

  /// The application-start identity `A` (contract 1.1): the Kernel's own
  /// identity for this start, written once into the journal metadata.
  ApplicationStartId application_start_id;
  CellId cell_id;
  /// Also `R`, the number of Stop event records (§2.4, OP-2: 1,024).
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
  /// `N_max`: maximum number of participants whose Stop cascades may be live
  /// at once, the ceiling a reconfiguration may grow to. It sizes, once at
  /// construction, the Stop lane free list, the Stop control executor, each
  /// event record's covered and stale arrays and every Stop hand-off. Zero
  /// derives it from the initially registered execution engines. The pre-started Stop-stage
  /// attempt workers are sized to the current cell's Stop-qualified
  /// participants (at least one), never to this ceiling: reconfiguration
  /// builds a resized pool before its commit, swaps it in at the commit and
  /// drains and joins the retired pool after releasing every Kernel lock, so
  /// no worker is ever created on the Stop path.
  std::size_t prequalified_max_live_stop_participants{};
  /// Exact retained descriptor identity used only when the Kernel is
  /// bootstrapped with no runtime participants. It must be supplied together
  /// with `empty_runtime_graph_revision`; participant-backed configurations
  /// derive both identities from the verified registrations.
  std::optional<RetainedBundleId> empty_runtime_retained_bundle_id;
  /// Exact graph identity paired with `empty_runtime_retained_bundle_id` for
  /// an empty-runtime bootstrap. It is never a constraint on later non-empty
  /// participant reconfiguration.
  std::optional<GraphRevision> empty_runtime_graph_revision;
  /// Verified selected resource inventory before physical engines exist.
  /// Only valid with the paired empty-runtime bundle/graph identities. These
  /// are ordinary slot-table entries, not ready participants or rest evidence.
  std::vector<ResourceId> empty_runtime_resource_inventory;
  std::size_t cell_admission_claim_capacity{1024U};
  CellAdmissionInterventionValidator cell_admission_intervention_validator;
  CellAdmissionSettlementValidator cell_admission_settlement_validator;
  std::optional<ManagerLivenessRequiredSetDTO> manager_liveness_required_set;
  /// Present exactly in compositions that host the Headless Alarm Lifecycle
  /// Owner. Program Run/Resume, motion-producing recovery and the final
  /// pre-dispatch recheck then evaluate the published motion-attention
  /// projection and fail closed while it is missing, unreconciled or stale.
  std::optional<OperatorAlarmAttentionPolicyDTO> operator_alarm_attention;
  /// Queue existing rule processing for this exact affected service after
  /// owner/state locks unwind. This lifetime pin grants no health/currentness.
  std::function<void(const std::shared_ptr<IIoSignalService>&)>
      io_rule_evidence_processing_notification{};
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
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  std::vector<ResourceId> resource_ids;
};

struct OperationRecoveryProjectionDTO final {
  OperationRevision operation_revision{0U};
  /// The event that started the newest stop attempt on the operation.
  std::optional<StopEventId> stop_event_id;
  KernelRecoveryAssessmentDTO assessment;
  std::vector<OperationRecoveryParticipantDTO> participants;
};

/// The key of a Kernel-internal request whose cause stands for one operation
/// revision (contract 1.4: class 1, minter 3, stored on the object that owns
/// the cause). A trigger at the same revision reuses the key, so it replays
/// exactly; a trigger at another revision is another cause.
struct RevisionRequestKeySlot final {
  std::optional<RequestKey> key;
  OperationRevision revision{0U};
  /// A second coordinate of the cause where one revision is not enough
  /// (an assessment revision, a preparation id).
  std::uint64_t detail{};
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
                                               private IStopControlWorkReceiver,
                                               private IStopStageAttemptCompletionReceiver,
                                               private IOperationControlEffectCompletionReceiver {
 public:
  ResourceAddressedExecutionKernel(
      std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
      IOperationJournal& journal,
      ResourceAddressedExecutionKernelConfig config);
  ~ResourceAddressedExecutionKernel();

  ResourceAddressedExecutionKernel(const ResourceAddressedExecutionKernel&) = delete;
  ResourceAddressedExecutionKernel& operator=(const ResourceAddressedExecutionKernel&) = delete;

  /// Lock-only first-fault snapshot; it never consults or writes the journal.
  /// Reset and successful reads/writes do not clear this Workbench fault;
  /// motion admission stays closed until restart.
  [[nodiscard]] std::optional<OperationJournalWriteFaultDTO>
  get_operation_journal_write_fault() const;
  /// Active TCP: the robot owner's `active_tcp_changed` row, built under its
  /// own lock and posted here after that lock is released, like the other
  /// producers outside the Kernel lock (`post_journal_records`). Never waits
  /// for the disk; a row the queue cannot take or the writer cannot write is
  /// counted and alarmed as every other row. Wakes the event stream.
  void post_active_tcp_change(ActiveTcpChangeDTO change);
  /// Lock-only snapshot of the contained internal fault (see ContainInternalFault),
  /// published as soon as it is recorded (its containment Halt is never refused).
  [[nodiscard]] std::optional<KernelInternalFaultDTO> get_internal_fault() const;

  /// Resolve, reserve, and submit one portable operation. Exact replay returns
  /// the original owner-minted identities and performs no subordinate call.
  ///
  /// Active TCP: a Set TCP intent
  /// (`SetTcpParticipantIntentDTO`) runs as an immediate component step
  /// (`execute_immediate_step`). The call returns when the step finished:
  /// Ok = the robot's Active TCP is the resolved entry; Err = typed refusal or
  /// step failure. Its operation id is not readable afterwards; there is
  /// nothing to acknowledge.
  [[nodiscard]] Result<OperationAcceptedDTO> execute(const OperationIntentDTO& intent) override;

  [[nodiscard]] Result<std::unique_ptr<IAddressedMotionPreviewSession>>
  begin_motion_preview_session(const OperationIntentDTO& primary_motion) override;

  /// Resolve and validate one capability command without reserving,
  /// journaling, changing observed state, or dispatching a command.
  [[nodiscard]] Result<CapabilityCommandPreviewResultDTO> preview_capability_command(
      const OperationIntentDTO& intent);

  [[nodiscard]] Result<OperationControlAcceptedDTO> reset(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> acknowledge_completed(
      const OperationControlRequestDTO& request) override;

  // ---- Stop requests (stop state model §3.2). A Stop is never refused: an
  // invalid request is widened (PD-1) and the reply says so. Each entry
  // builds a `StopRequest` and runs the one Stop core (`submit_stop`).
  /// S4 Operation Stop: operation, component or resource targets.
  [[nodiscard]] StopAcceptedDTO stop(const StopIntentDTO& intent);
  /// S2 Stop all: every Stop-qualified participant; holds every group of a
  /// claimed run (all-groups flag before capture).
  [[nodiscard]] StopAcceptedDTO stop_all(const FullStopIntentDTO& intent);
  /// S3 Halt: Stop all's hold and scope, and freezes a claimed run.
  [[nodiscard]] StopAcceptedDTO halt(const FullStopIntentDTO& intent);
  /// S5 Cell Stop, the Program node `kernel.cell_stop`: every participant,
  /// no hold change.
  [[nodiscard]] StopAcceptedDTO cell_stop(const FullStopIntentDTO& intent);
  /// S16 Forced Program End (Program Reset's first step, runner only).
  [[nodiscard]] StopAcceptedDTO end_program_run(const ProgramRunEndIntentDTO& intent);
  /// Install the sealed immutable Program association before any node effect.
  /// Repeated exact registration preserves current admission holds. This is
  /// process-local composition, not a public caller-supplied table interface.
  [[nodiscard]] Result<void> register_program_run_capture(ProgramRunExecutionCaptureDTO capture);
  [[nodiscard]] Result<std::optional<ResolvedManualProgramHandover>> get_manual_program_handover(
      const OperationIntentDTO& intent);
  [[nodiscard]] Result<ResolvedManualControlAvailability> get_manual_control_availability(
      const OperationIntentDTO& intent);
  /// S1 Stop (groups) of the current claim. Not captured: all groups held.
  /// A run not current and not higher than the highest run started has no
  /// effect; a run higher than any started is invalid (Stop all).
  [[nodiscard]] ProgramExecutionSelectionAcceptedDTO stop_program_selection(
      const ProgramExecutionSelectionRequestDTO& request);
  /// A Stop the transport recognised but could not read, or that names
  /// another application start or none: widened by its recognised route
  /// (PD-1).
  [[nodiscard]] StopAcceptedDTO recognized_stop(const RecognizedStopIngressDTO& ingress);

  // ---- Reads (memory only).
  /// The scope read (`manyforge.program_execution_scope.v4`).
  [[nodiscard]] Result<ProgramExecutionScopeStateDTO> get_program_execution_scope(
      const ProgramRunNumber& program_run_id) const;
  /// The event stream's Stop facts (K18): the claimed run's hold facts and
  /// the cell-wide markers.
  [[nodiscard]] StopStreamFactsDTO get_stop_stream_facts() const;
  /// The event stream's operations table: one bounded memory-only section.
  [[nodiscard]] OperationTableDTO get_operation_table() const;
  /// The operations table's `change` counter now (OD-213 P4): an acceptance
  /// reply carries it, read after the acceptance, so a reader knows when its
  /// pushed table includes that acceptance.
  [[nodiscard]] std::uint64_t get_operation_change() const;
  /// `read_stop` (K16): the ring record of one event; `stop_status_not_resident`
  /// once it was overwritten.
  [[nodiscard]] Result<StopEventStatusDTO> read_stop(StopEventId event_id) const;
  [[nodiscard]] CellStopActionProjectionDTO cell_stop_action_projection() const;
  /// Components whose last Stop ended uncontained and whose controller has
  /// not reported idle since (§3.6 F2). The host shows them with activity
  /// unknown and reason `stop_unconfirmed`.
  [[nodiscard]] std::vector<ControllableComponentId> stop_unconfirmed_components() const;

  // ---- Releases (§3.4). Each names the hold revision it saw.
  /// L1 group Resume (and individual node release).
  [[nodiscard]] Result<ProgramExecutionScopeStateDTO> release_program_selection(
      const ProgramExecutionSelectionRequestDTO& request, std::uint64_t expected_hold_revision);
  /// L2 Run.
  [[nodiscard]] Result<ProgramExecutionScopeStateDTO> start_program_selection(
      const ProgramExecutionSelectionRequestDTO& request, std::uint64_t expected_hold_revision);
  /// L3 Execute after Halt: lowers `frozen` from Halt or runner loss to none;
  /// node holds stay until Resume. A run that is not the current claim: no
  /// effect, Ok with no scope.
  [[nodiscard]] Result<std::optional<ProgramExecutionScopeStateDTO>> release_halt(
      const ProgramRunNumber& program_run_id, std::uint64_t expected_hold_revision);
  /// Component model v2: the execution host passes the
  /// current state of every component in the cell on every change. Components
  /// absent from `states` no longer count. Nothing is stored or journaled.
  /// OD-379: a component entering `fault` while an operation executes also
  /// stops every robot in motion with its fault Stop profile (one
  /// `component_fault` Stop); the call never waits for that Stop.
  void observe_component_states(std::vector<ComponentState> states);
  /// The cell latch, derived from the last observed component states and the
  /// Kernel's own Stops and execution. While latched, Program dispatch, node
  /// commands, manual motion and manual I/O are refused with `cell_latched`
  /// and interrupt evaluation is paused.
  [[nodiscard]] CellLatch get_cell_latch() const;
  /// Readiness reasons the Kernel owns that belong to no component: the
  /// internal fault (`execution_host_internal_fault`, until the application
  /// is started again). It blocks motion; nothing blocks invisibly.
  [[nodiscard]] std::vector<ErrorDTO> ReadExecutionBlocks() const;
  /// Called once for every new Stop event, outside the Kernel's locks, with
  /// the component actions it interrupts (PD-2). Set once by the execution
  /// host before the Kernel runs; stored in an atomic pointer and read with
  /// no lock. The listener must outlive the Kernel.
  void set_stop_listener(IStopListener* listener) noexcept;
  [[nodiscard]] Result<RuntimeScaleStatusDTO> get_runtime_scale(
      const OperationNumber& operation_id) const;
  [[nodiscard]] Result<RuntimeScaleControlAcceptedDTO> set_runtime_scale_control(
      const RuntimeScaleControlRequestDTO& request);
  [[nodiscard]] Result<RuntimeScaleControlStatusDTO> get_runtime_scale_control(
      const RuntimeScaleControlSelectorDTO& selector) const;
  /// Presentation query: expose the complete active synchronized scope for a
  /// selected target. Setters still require that explicit returned selector.
  [[nodiscard]] Result<RuntimeScaleControlStatusDTO> present_runtime_scale_control(
      const RuntimeScaleControlSelectorDTO& selector) const;

  [[nodiscard]] AuthoringAdmissionDTO GetAuthoringAdmission() const override;
  [[nodiscard]] Result<void> SetRouteRecoveryPending(bool pending);
  [[nodiscard]] Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO& snapshot,
      std::shared_ptr<IIoSignalService>* actual_retired_service = nullptr) override;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override;
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireProgramResetClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const ProgramCycleStatusDTO& program_cycle_status) override;
  /// Trusted ordinary Apply convergence on this Kernel's existing claim owner.
  /// The host supplies actual installed-state and immutable terminal checks;
  /// no public request can provide or skip this process-local validator.
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireConfigurationApplyCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent,
      const CellAdmissionConfigurationApplyValidator& validate);
  /// Fresh ordinary Apply success, after every exact old fence is released.
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReleaseTerminalApplyClaimsAfterSuccess(
      const CellAdmissionClaimDTO& fresh_claim,
      const CellAdmissionClaimReleaseDTO& fresh_success_release,
      const CellAdmissionConfigurationApplySuccessValidator& validate);
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) override;
  [[nodiscard]] Result<CellAdmissionRecoveryReceiptDTO> GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const override;
  /// Copy the current held ledger entry for a subsequent exact recovery query.
  /// The observation itself grants no acquisition, effects or release.
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> ReadHeldCellAdmissionClaim(
      CellAdmissionClaimKindDTO kind) const;
  [[nodiscard]] std::optional<CellAdmissionClaimDTO> ReadHeldCellAdmissionClaim(
      CellAdmissionClaimKindDTO kind,
      const CellAdmissionOwnerTransactionId& owner_transaction_id) const;
  [[nodiscard]] std::vector<CellAdmissionClaimDTO> ReadHeldCellAdmissionClaims(
      CellAdmissionClaimKindDTO kind) const;
  [[nodiscard]] Result<void> ValidateProgramResetSceneAuthorization(
      const ProgramResetSceneAuthorizationDTO& authorization) const override;
  /// Exact selected Reset grant; no motion or target is accepted here.
  [[nodiscard]] Result<void> ValidateProgramResetPositionAuthorization(
      const ProgramResetPositionAuthorizationDTO& authorization) const;
  /// OD-219: the refusal `reset_position_disk_cannot_keep_records` that
  /// `ValidateProgramResetPositionAuthorization` gives the move back now
  /// (the same predicate); nothing otherwise. Read by the move-back
  /// availability answer.
  [[nodiscard]] std::optional<ErrorDTO> program_reset_position_disk_refusal() const;
  [[nodiscard]] std::optional<ProgramRunnerContactLossDTO> get_program_runner_loss() const;
  [[nodiscard]] Result<void> Publish(const ManagerLivenessObservationDTO& observation) override;
  /// Retain the newest generation of the Alarm Lifecycle Owner's attention
  /// projection. A projection from another host boot, a lower generation, or
  /// different content under the retained generation is refused; identical
  /// content refreshes the receipt time (heartbeat).
  [[nodiscard]] Result<OperatorAlarmAttentionAcceptedDTO> PublishOperatorAlarmAttention(
      const OperatorAlarmAttentionSnapshotDTO& snapshot) override;
  /// No-effect evaluation of the operator-alarm attention test against one
  /// captured node's effectful target resources. The Program owner uses it
  /// before a motion-producing recovery of that held node takes effect.
  [[nodiscard]] Result<void> check_program_node_motion_attention(
      const ProgramNodeMotionAttentionCheckDTO& request) const;
  /// Existing activation owner installs the same immutable service published
  /// by the host, after exact reconfiguration and before composition publication.
  /// Candidate-only installation under the original held Apply effect and
  /// composition pin. No registration, active catalog or state event is exposed.
  [[nodiscard]] Result<void> StageGlobalIoRuleCatalog(
      IoRuleAppliedContextDTO context,
      std::shared_ptr<const IoActivationRuleCatalogDTO> catalog,
      std::shared_ptr<IIoSignalService> service,
      const ConfigurationFenceStatusQueryDTO& held_fence,
      const CellAdmissionClaimDTO& actual_parent_claim,
      std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> waiting_for_program = {});
  /// Trusted host calls only after actual successful NamedApply proof, holding
  /// the current composition pin. A candidate head alone is not this proof.
  [[nodiscard]] Result<void> ActivateGlobalIoRuleCatalog(
      const IoRuleAppliedContextDTO& actual_applied_context,
      const std::shared_ptr<IIoSignalService>& expected_service);
  /// Reconstruct the same owner from actual successful current Applied proof
  /// during the existing route-recovery exclusion, without inventing an Apply
  /// fence. The host retains its final current composition/publication pin.
  [[nodiscard]] Result<void> RestoreGlobalIoRuleCatalog(
      IoRuleAppliedContextDTO actual_applied_context,
      std::shared_ptr<const IoActivationRuleCatalogDTO> catalog,
      std::shared_ptr<IIoSignalService> service,
      std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> waiting_for_program = {});
  [[nodiscard]] Result<void> DiscardStagedGlobalIoRuleCatalog(
      const ConfigurationFenceStatusQueryDTO& held_fence,
      const CellAdmissionClaimDTO& actual_parent_claim);
  [[nodiscard]] std::shared_ptr<const IoActivationRuleCatalogDTO>
  installed_global_io_rule_catalog() const;
  [[nodiscard]] std::optional<IoRuleStateProjectionDTO> ReadInterruptRuleStateBaseline() const;

  /// Rule activation (node `rules.set_active`): succeeds at once; an
  /// unknown rule id is refused with `rule_unknown`. Activating a rule whose
  /// condition holds trips it at once.
  [[nodiscard]] Result<void> set_rule_active(const InterruptPolicyId& rule_id, bool active);
  /// Every rule back to its `active_at_start` (run end, Load).
  [[nodiscard]] Result<void> reset_rule_activation();
  /// The rules gating their scope now (`rule_holds`, with `rule_id`): a
  /// `stop` rule whose condition holds after its trip, or any active rule
  /// that can't watch its signals. Readiness reads this; nothing recomputes it.
  [[nodiscard]] std::vector<ErrorDTO> ReadRuleHolds() const;
  /// Node `io.clear_outside_change`: clears the core-held
  /// `<signal_id>.changed_outside` flag of one output; writes nothing.
  [[nodiscard]] Result<void> clear_outside_change(const IoSignalId& output);
  /// One write path: admission of every output write, manual
  /// or Program, made through the execution host's `set_io_output`. Refused
  /// `cell_latched` while the cell latch holds, and `rule_holds` (with
  /// `rule_id`) while an active rule whose scope covers the signal
  /// (`everything`) gates it. A `manual` write is refused
  /// `io_manual_write_motion_executing` while any motion executes in the
  /// cell; otherwise allowed (idle, pause, Program without motion). Effect-free.
  [[nodiscard]] Result<void> admit_output_write(const IoSignalId& output, bool manual) const;


  [[nodiscard]] Result<void> InstallProgramIoSignalService(
      std::shared_ptr<IIoSignalService> service,
      const RetainedBundleId& expected_retained_bundle_id,
      const GraphRevision& expected_graph_revision);
  /// Trusted Applied owner proof, constructed outside locks by the host. The
  /// host retains its shared composition lock through this exact recheck.
  [[nodiscard]] Result<void> InstallAppliedProgramBlackboardContext(
      AppliedProgramBlackboardContextDTO context,
      const std::shared_ptr<IIoSignalService>& expected_service);
  /// Trusted no-effect scope observation only. Host must first prove the
  /// actual pending NamedApply/qualified candidate and retain its composition
  /// pin. This grants no context/source/value admission or acknowledgement.
  [[nodiscard]] Result<void> ValidatePendingProgramBlackboardScope(
      const IoProgramBlackboardScopeDTO& scope, const ProgramBindingDTO& actual_candidate_binding,
      ActiveBundlePublicationGeneration actual_candidate_publication,
      const std::shared_ptr<IIoSignalService>& expected_service);
  /// The host retains its actual shared composition currentness guard through
  /// this call and verifies the retained context against that publication.
  /// Public fields never establish Applied ownership or receiver time.
  /// An accepted frame is evaluated by the rules before the ACK
  /// returns, so the Runner's next admission sees it.
  [[nodiscard]] Result<IoProgramBlackboardAcknowledgementDTO> PublishProgramBlackboard(
      const IoProgramBlackboardPublicationDTO& publication);
  [[nodiscard]] std::shared_ptr<const AppliedProgramBlackboardContextDTO>
  applied_program_blackboard_context() const;
  /// The host first matches rows to this SAME sealed Program node's literal
  /// simulation data under its product lease. This joins actual current Run,
  /// claim, Binding and inputs, then calls the same service under state_mutex_.
  [[nodiscard]] Result<ProgramNodeMockSimulationAcknowledgementDTO>
  StartProgramNodeMockSimulation(const ProgramNodeMockSimulationRequestDTO& request);
  [[nodiscard]] Result<void> SetProgramRunMockSimulationPaused(
      const ProgramRunMockSimulationPauseRequestDTO& request);
  [[nodiscard]] Result<void> EndProgramNodeMockSimulation(
      const ProgramNodeMockSimulationScopeDTO& scope);
  [[nodiscard]] Result<IoWatchRegistrationDTO> RegisterProgramIoWatch(
      const ProgramIoWatchRequestDTO& request);
  [[nodiscard]] Result<IoWatchResultDTO> ReadProgramIoWatch(
      const IoWatchRegistrationDTO& registration) const;
  [[nodiscard]] Result<void> ReleaseProgramIoWatch(const IoWatchRegistrationDTO& registration);
  [[nodiscard]] Result<void> ReleaseProgramIoWatchScope(
      const ProgramIoWatchScopeReleaseRequestDTO& request);
  /// Existing host scheduling calls after ingress drain and at NextIoRuleExpiry.
  [[nodiscard]] Result<void> ProcessIoRuleEvidence();
  /// Local managed I/O lifecycle boundary, before input Pause/Resume. Disabling
  /// requires an idle cell.
  [[nodiscard]] Result<void> SetGlobalIoSourcesPlannedDisabled(
      const std::shared_ptr<IIoSignalService>& expected_service,
      std::span<const InterruptSourceId> sources, bool disabled);
  [[nodiscard]] Result<std::optional<TimePoint>> NextIoRuleExpiry() const;
  /// A callback threw on the Stop/safety path (scheduler lane, runtime-
  /// collision lookahead or the Kernel's Stop worker). The first call records `reason_code` as the
  /// internal fault, closes motion admission until a Workbench restart and
  /// raises one Halt (S11, kind `kInternalFault`) under this Kernel's own
  /// request identity, with `reason_code` as its reason, and writes one
  /// typed line. Later calls only count.
  void ContainInternalFault(std::string_view reason_code) noexcept;
  [[nodiscard]] Result<AuthoringAdmissionDTO> ReplaceManagerLivenessRequiredSet(
      const ManagerLivenessRequiredSetDTO& required_set);

  /// Read-only owner projection. Subordinate polling and state advancement are
  /// performed only by reconcile_subordinate_state(), never by this method.
  /// Journal an exact active subordinate's unresolved late cancellation.
  /// Success means an effect-uncertain intervention is durably projected;
  /// callers retain their pending evidence on any refusal or write failure.
  [[nodiscard]] Result<void> record_late_cancel_uncertain(
      IResolvedParticipantExecutionEngine* engine, std::uint64_t execution_id);
  [[nodiscard]] Result<void> record_late_cancel_uncertain(
      IResolvedParticipantExecutionEngine* engine, std::uint64_t preparation_id,
      std::uint64_t command_id, std::uint64_t generation, const CommandTargetId& target);

  [[nodiscard]] Result<OperationStateDTO> get_operation_state(
      const OperationNumber& operation_id) const override;

  /// Read-only owner projection that atomically distinguishes an active
  /// resident operation from retained history.
  [[nodiscard]] Result<OperationReadProjectionDTO> get_operation_projection(
      const OperationNumber& operation_id) const;

  /// Coherent, deterministic projection of every still-resident operation
  /// of the exact Program run, or of every resident manual operation (no
  /// run) for nullopt. This lets a successor client recover an operation
  /// without guessing its owner-minted identity.
  [[nodiscard]] std::vector<OperationStateDTO> get_resident_operations(
      const std::optional<ProgramRunNumber>& program_run_id) const;

  /// The resolution context of the held capture when `program_run_id` names
  /// the run the Kernel holds, else null (OD-234). A pointer copy under the
  /// state lock; no I/O.
  [[nodiscard]] ProgramRunResolutionContext program_run_resolution_context(
      const std::optional<ProgramRunNumber>& program_run_id) const;

  /// One coherent current manual result per requested target: every resident
  /// manual operation on that target, or else the latest retained operation
  /// with no run on that target. Reuses bounded owner history; reads never
  /// settle/release operations.
  [[nodiscard]] std::vector<OperationReadProjectionDTO> get_latest_manual_operations(
      const std::vector<ProgramTargetReferenceDTO>& targets) const;

  [[nodiscard]] Result<OperationRecoveryProjectionDTO> get_operation_recovery_assessment(
      const OperationNumber& operation_id) const;

  [[nodiscard]] Result<KernelRecoveryActionResultDTO> apply_operation_recovery_action(
      const OperationNumber& operation_id, OperationRevision expected_operation_revision,
      const KernelRecoveryActionRequestDTO& request);

  /// NRT owner-scheduler reconciliation point. This closes event-loss windows
  /// and advances capability-command completion without making public reads
  /// effectful.
  [[nodiscard]] Result<void> reconcile_subordinate_state() override;

  /// Retire the exclusive manual claim only after exact operation cleanup.
  /// Reuses the ordinary owner reconciliation lane; Stop never waits for it.
  [[nodiscard]] Result<void> reconcile_manual_operation_admission();

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> resource_snapshot() const;

  /// One coherent generation-stamped read used by the private runtime
  /// reconciliation owner. Public callers cannot mutate or release slots from
  /// this projection.
  [[nodiscard]] ResourceSlotTableSnapshotDTO resource_table_snapshot() const;

  /// Atomically select a verified candidate participant set while preserving
  /// the stable addressed owner, operation journal, replay ledgers, and any
  /// unaffected in-flight operation. The caller must hold the exact durable
  /// configuration fence for every changed resource.
  [[nodiscard]] Result<void> reconfigure_runtime_participants(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver = {});

  /// Live participants the current pre-started Stop-stage attempt pool
  /// isolates (its lane count is this times kMaxStopEscalationStages).
  [[nodiscard]] std::size_t stop_stage_attempt_pool_participants() const;

  /// Select one verified runtime product before route readiness is published.
  [[nodiscard]] Result<void> reconfigure_runtime_product(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver,
      std::function<Result<void>()> commit_publication = {});

  /// Install an exact component-set transition beneath one held Manager Apply
  /// fence. Unchanged registrations retain their engines and route facts. An
  /// optional prevalidated commit callback runs at the inventory/registration
  /// publication boundary while admission and state remain locked; it must not
  /// call back into the Kernel.
  [[nodiscard]] Result<void> reconfigure_runtime_component_set(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver,
      RetainedBundleId candidate_retained_bundle_id, GraphRevision candidate_graph_revision,
      const ConfigurationFenceId& apply_fence_id, const ConfigurationFenceScopeDTO& scope,
      const std::vector<ControllableComponentId>& retained_components = {},
      std::function<Result<void>()> commit_publication = {});

  /// Publish the candidate-side active-configuration identities already
  /// declared by the installed registrations while the exact durable Apply
  /// fence still closes every affected resource. This does not publish Stop
  /// readiness or create a published route predecessor: route-fence admission
  /// sees the separately tracked authoritative publication inventory. The
  /// execution host must replace these staged facts with independently
  /// verified readiness before addressed command admission can reopen.
  [[nodiscard]] Result<void> stage_current_runtime_configuration_facts_under_apply_fence(
      const ConfigurationFenceScopeDTO& scope);

  /// Rehydrate one exact durable route predecessor after execution-host
  /// replacement without claiming that the new provider incarnation is
  /// Stop-ready. The supplied registrations must name the predecessor's
  /// domain/configuration but carry no readiness for that domain. This keeps
  /// operation admission closed while allowing the execution host to acquire
  /// the exact route fence and publish a freshly observed successor
  /// configuration.
  [[nodiscard]] Result<RuntimeRouteConfigurationReadbackDTO>
  adopt_runtime_route_configuration_predecessor(
      ActiveControlConfigurationDTO configuration,
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver = {});

  /// Atomically install one reconciled domain's complete runtime
  /// registrations and independently retained readiness projection. An
  /// unreconciled publication carries no candidate registrations and closes
  /// admission for that domain while leaving its observable old runtime
  /// inventory intact under the held route fence. Route readiness says only
  /// that the domain route and its Stop readiness are coherent; it never
  /// grants or removes a component's command eligibility.
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
  [[nodiscard]] ApplicationStartId application_start_id() const noexcept override {
    return config_.application_start_id;
  }

 private:
  friend class StopControlExecutorTestAccess;
  [[nodiscard]] Result<std::optional<ResolvedManualProgramHandover>>
  resolve_manual_program_handover(const OperationIntentDTO& intent);
  [[nodiscard]] Result<std::optional<ResolvedManualProgramHandover>>
  resolve_manual_program_handover(const RuntimeParticipantExecutionRegistration& registration);
  [[nodiscard]] Result<void> validate_manual_program_handover_locked(
      const ResolvedManualProgramHandover& handover) const;
  struct ReplayEntry;
  struct ControlReplayEntry;
  struct RuntimeScaleControlReplayEntry;
  struct RuntimeScaleControlSetting;
  struct RuntimeScaleControlResolution;
  struct RuntimeScaleControlScope;
  struct ActiveOperation;
  [[nodiscard]] MotionRetryEligibilityDTO operation_retry_eligibility_locked(
      const ActiveOperation& operation, const std::vector<KernelRecoveryAssessmentDTO>& assessments,
      bool recovery_owner = false) const;
  [[nodiscard]] bool operation_close_eligible_locked(
      const ActiveOperation& operation, const std::vector<KernelRecoveryAssessmentDTO>& assessments,
      bool closure_owner = false) const;
  [[nodiscard]] Result<KernelRecoveryActionResultDTO> close_operation(
      const OperationNumber& operation_id, OperationRevision expected_operation_revision,
      const KernelRecoveryActionRequestDTO& request);
  [[nodiscard]] Result<void> retire_preempted_motion_operations();
  [[nodiscard]] Result<void> retire_rebuilt_motion_operations_for_program_reset(
      const ProgramRunNumber& run_id);
  [[nodiscard]] Result<bool> retire_rebuilt_unknown_takeover_for_program_reset(
      const OperationNumber& incumbent_id);
  [[nodiscard]] Result<void> retire_operation_ownership_locked(
      const OperationNumber& operation_id, bool clear_continuation,
      bool discard_node_continuations = false);
  [[nodiscard]] std::optional<std::vector<std::size_t>> fresh_motion_node_indices_locked(
      const ActiveOperation& operation) const;
  void mark_motion_retry_run_end_locked(const ProgramRunNumber& run_id);
  class OperationCommitGuard;
  struct ProgramCycleClaimRun;
  struct EngineEventIngress;
  class JournalWriter;
  struct EngineSubscription;
  struct PreviewSession;

  [[nodiscard]] bool operation_control_reconfiguration_busy_locked() const;
  /// Attention test for Program work over exact resources.
  /// No refusal when the composition carries no alarm-attention policy.
  [[nodiscard]] std::optional<ErrorDTO> operator_alarm_attention_refusal_locked(
      const std::vector<ResourceId>& reserved_resources) const;
  /// OD-215: while the retained attention projection says the disk cannot
  /// keep ManyForge's records, the refusal `reason` of a manual move or of a
  /// Reset's move back; nothing otherwise (other interlocks leave both).
  [[nodiscard]] std::optional<ErrorDTO> disk_cannot_keep_records_refusal_locked(
      std::string_view reason, std::string message) const;
  /// The Reset's move back: `disk_cannot_keep_records_refusal_locked` with
  /// its reason and message.
  [[nodiscard]] std::optional<ErrorDTO> program_reset_position_disk_refusal_locked() const;
  /// Union of the resources that the captured Program Binding binds to every
  /// effectful target of the selected captured Program nodes.
  [[nodiscard]] std::vector<ResourceId> program_selection_resources_locked(
      const std::vector<std::uint32_t>& node_indices) const;
  /// The one eligibility predicate shared by single, coordinated, manual,
  /// Program, arm, gripper and output dispatch: the component must report
  /// `enabled` (`component_not_enabled`). A domain without route authority has
  /// no component to consult. Returns the refusal, if any.
  [[nodiscard]] std::optional<ErrorDTO> component_command_refusal_locked(
      const RuntimeParticipantExecutionRegistration& registration) const;
  /// The component's last observed state is `enabled` (state_mutex_).
  [[nodiscard]] bool component_enabled_locked(const ControllableComponentId& component) const;
  [[nodiscard]] Result<void> require_idle_for_component_readiness_locked() const;
  [[nodiscard]] std::vector<IResolvedParticipantExecutionEngine*> operation_control_engines_locked(
      const std::vector<RuntimeParticipantExecutionRegistration>& registrations) const;
  [[nodiscard]] const RuntimeParticipantExecutionRegistration* registration_for(
      const RuntimeParticipantBinding& binding) const noexcept;
  [[nodiscard]] Result<RuntimeParticipantExecutionRegistration> resolve_registration(
      const OperationIntentDTO& operation, std::size_t participant_index) const;
  [[nodiscard]] Result<RuntimeParticipantExecutionRegistration> bind_captured_node_io_rule(
      const OperationIntentDTO& operation, std::size_t participant_index,
      RuntimeParticipantExecutionRegistration registration) const;
  [[nodiscard]] Result<void> validate_preview_participant(
      const OperationIntentDTO& intent,
      const RuntimeParticipantExecutionRegistration& registration);
  [[nodiscard]] Result<void> validate_program_node_claim(
      const ProgramNodeAdmissionEvidenceDTO& evidence) const;
  [[nodiscard]] Result<void> validate_program_execution_scope_locked(
      const OperationIntentDTO& intent, std::optional<bool> non_motion_only = {},
      bool intrinsic_direct_control = false) const;
  [[nodiscard]] bool program_node_effectively_running_locked(
      const ProgramRunNumber& program_run_id, std::size_t index) const;
  [[nodiscard]] bool program_node_non_motion_locked(
      const ProgramRunNumber& program_run_id, std::size_t index) const;
  /// E11 call (1) for the run's held prepared non-motion commands, under
  /// the lock; the engines whose gates closed are added to
  /// `handoff.fence_completions` for call (2) after the unlock. Manual work
  /// (no run) has no held Program commands.
  void fence_held_prepared_program_commands_locked(
      const std::optional<ProgramRunNumber>& program_run_id, StopHandoff& handoff) noexcept;
  [[nodiscard]] Result<void> reconcile_manual_operation_admission_locked();
  [[nodiscard]] Result<void> validate_manual_operation_settlement(
      const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release) const;
  [[nodiscard]] Result<std::optional<CellAdmissionSettlementEvidenceDTO>>
  manual_operation_settlement_locked(const CellAdmissionClaimDTO& claim) const;
  /// Caller holds admission_mutex_. Returns whether a Program-run fence fact
  /// changed (claim run, retire to ended); the caller raises the event change
  /// signal once after it released its locks (contract 1.12).
  [[nodiscard]] bool refresh_execution_claims();
  [[nodiscard]] ActiveOperation* find_active_operation_locked(
      const OperationNumber& operation_id) noexcept;
  [[nodiscard]] const ActiveOperation* find_active_operation_locked(
      const OperationNumber& operation_id) const noexcept;
  [[nodiscard]] OperationReadProjectionDTO operation_projection_locked(
      const OperationStateDTO& state, bool active, bool control_pending) const;
  [[nodiscard]] std::optional<OperationNumber> find_event_operation_locked(
      const IResolvedParticipantExecutionEngine* engine, std::uint64_t subordinate_id,
      std::optional<std::uint64_t> subscription_id = std::nullopt) const;
  void on_engine_event(IResolvedParticipantExecutionEngine* engine, std::uint64_t subscription_id,
                       const KernelEvent& event);
  [[nodiscard]] Result<void> apply_subordinate_state(
      const OperationNumber& operation_id, IResolvedParticipantExecutionEngine* engine,
      std::uint64_t subordinate_id, OperationStateKind state, double progress,
      std::optional<ErrorDTO> terminal_error, bool observe_current_motion = false,
      std::optional<ParticipantExecutionFact> terminal_fact = std::nullopt,
      bool require_unfinalized_motion = false, bool* durable_applied = nullptr,
      std::optional<std::uint64_t> exact_gripper_preparation = std::nullopt,
      std::optional<std::uint64_t> exact_gripper_generation = std::nullopt,
      std::optional<CommandTargetId> exact_gripper_target = std::nullopt);
  void stamp_operation_capture_locked(ActiveOperation& operation, OperationStateDTO& state);
  /// Keeps the first journal write fault as the runtime-fault fact; true when
  /// this one is the first. Memory only.
  bool retain_operation_journal_write_fault_fact_locked(
      std::string_view write_kind, const ErrorDTO& failure,
      const OperationStateDTO* state = nullptr, std::optional<RequestKey> request_key = std::nullopt);

  // ==== Journal writer queue (state_mutex_). Posting never waits.
  /// Appends one record in the caller's state_mutex_ section, so queue order
  /// is action order, and wakes the writer.
  void post_journal_record_locked(JournalRecord record);
  /// Stop lane: appends without waking (no system call under the lock);
  /// `complete_stop_handoff` wakes the writer after the unlock.
  void post_stop_journal_record_locked(JournalRecord record) noexcept;
  /// One operation state, latest-wins: an unwritten earlier state of the same
  /// operation in the queue is marked skipped and this one is appended.
  void post_operation_state_locked(ActiveOperation& operation, JournalOperationState state,
                                   const char* write_kind,
                                   std::optional<RequestKey> request_key = std::nullopt,
                                   bool stop_lane = false);
  /// Producers outside the Kernel lock (cell admission): one short
  /// state_mutex_ section after their own unlock, then the wake.
  void post_journal_records(std::span<JournalRecord> records);
  void wake_journal_writer() noexcept;
  /// Memory only: raises `operation_change_` and, unless `signal` is false
  /// (the Stop lane, which raises after its own unlock), owes the stream's
  /// change signal to this section's unlock.
  void note_operation_change_locked(bool signal = true) noexcept;
  /// The queue is full: the newest record is dropped and counted as lost.
  void on_journal_queue_full_locked(const JournalRecord& record) noexcept;
  /// `count` records were not written (dropped, failed, or a reused Stop ring
  /// slot). The first one of a loss episode becomes the journal write fault
  /// (the operator alarm) and owes the episode's one log line. Memory only.
  void note_journal_records_lost_locked(const JournalRecord& first, std::uint64_t count,
                                        const ErrorDTO& error);
  /// Writer thread: takes the queue, copies Stop rows, writes, records
  /// losses, logs an episode start, ends an episode once the queue is written
  /// empty. False when nothing was posted.
  bool write_journal_batch(std::vector<JournalRecord>& taken) noexcept;
  /// Writer thread: copies each posted Stop row's ring record (one short
  /// lock per row); a row whose ring slot was reused is skipped and noted
  /// lost. Logs `fence_degraded`. Returns the rows lost.
  std::uint64_t copy_journal_stop_rows(std::span<JournalRecord> taken) noexcept;
  /// Writer thread, at shutdown: one line with the records not written in
  /// this start (`undrained` still queued), when there are any.
  void report_journal_records_not_written(std::uint64_t undrained) noexcept;

  [[nodiscard]] Result<bool> record_operation_state_locked(
      OperationStateDTO& state, const RuntimeScaleStatusDTO* scale, const char* reason,
      std::span<const PreparedMotionDiagnosticsDTO> diagnostics = {},
      std::optional<RequestKey> request_key = std::nullopt);
  [[nodiscard]] Result<bool> confirm_prepared_command_no_effect(
      const OperationNumber& operation_id, IResolvedParticipantExecutionEngine* engine,
      std::uint64_t preparation_id);
  [[nodiscard]] Result<void> discard_uncommitted_ordinary_preparations(
      const OperationNumber& operation_id, IResolvedParticipantExecutionEngine* failing_engine = nullptr,
      std::optional<ErrorDTO> failure = {});
  using ActiveOperationNode = std::map<OperationNumber, std::unique_ptr<ActiveOperation>>::node_type;
  /// Called under Execute's submission ordering; `end_admission` releases it
  /// once the takeover is durably published, before its cancel and advance.
  [[nodiscard]] Result<std::optional<OperationAcceptedDTO>> try_accept_motion_takeover(
      ActiveOperationNode& prepared_operation, ReplayEntry& prepared_replay,
      const RuntimeParticipantExecutionRegistration& registration,
      const std::function<void()>& end_admission);
  [[nodiscard]] Result<void> advance_motion_takeover(const OperationNumber& incumbent_id);
  [[nodiscard]] Result<void> advance_motion_takeover_impl(const OperationNumber& incumbent_id);
  [[nodiscard]] Result<void> commit_motion_takeover_owner_locked(
      ActiveOperation& incumbent, ActiveOperation& candidate, ResourceSlotOwnerTransferMode mode,
      std::pair<OperationStateDTO, OperationStateDTO>& captures);

  /// K11: `incumbent` is the incumbent participant's outcome of the attempt
  /// started by the fallback's Stop event; the commit also requires the
  /// incumbent's `takeover_cancelled` to be false.
  [[nodiscard]] Result<bool> commit_settled_motion_takeover(
      const OperationNumber& incumbent_id, const OperationNumber& candidate_id,
      const ParticipantStopOutcomeDTO& incumbent);
  [[nodiscard]] Result<bool> fail_pending_motion_takeover(
      const OperationNumber& incumbent_id, const OperationNumber& candidate_id, ErrorDTO cause);
  [[nodiscard]] Result<bool> end_pending_motion_takeover(
      const OperationNumber& incumbent_id, const OperationNumber& candidate_id,
      std::optional<ErrorDTO> failure);
  [[nodiscard]] Result<void> advance_unsent_motion_takeover(const OperationNumber& incumbent_id);
  [[nodiscard]] Result<void> reserve_cold_motion_takeover_goal_locked(ActiveOperation& candidate);

  [[nodiscard]] Result<void> begin_motion_takeover_fallback(
      const OperationNumber& incumbent_id, const OperationNumber& candidate_id, ErrorDTO cause);
  [[nodiscard]] Result<void> advance_motion_takeover_fallback(
      const OperationNumber& incumbent_id, const OperationNumber& candidate_id);
  [[nodiscard]] ActiveOperation* find_pending_motion_operation_locked(
      const OperationNumber& operation_id) noexcept;
  [[nodiscard]] const ActiveOperation* find_pending_motion_operation_locked(
      const OperationNumber& operation_id) const noexcept;
  [[nodiscard]] Result<void> advance_prepared_motion(const OperationNumber& operation_id,
                                                     IResolvedParticipantExecutionEngine* engine);
  [[nodiscard]] Result<void> advance_prepared_capability_command(
      const OperationNumber& operation_id, IResolvedParticipantExecutionEngine* engine);
  [[nodiscard]] Result<void> arm_io_rules_locked(ActiveOperation& operation);
  [[nodiscard]] bool io_rule_scope_current_locked(const ActiveOperation& operation,
                                                  const ActiveOperationParticipant& participant,
                                                  const IoRuleRegistrationDTO& registration) const;
  [[nodiscard]] Result<void> retire_io_rules_locked(ActiveOperation& operation);
  [[nodiscard]] Result<void> retire_participant_io_rules_locked(
      ActiveOperationParticipant& participant);
  struct GlobalIoRuleRuntime;
  struct StagedGlobalIoRuleCatalog;
  [[nodiscard]] Result<void> retire_global_io_rules_locked();
  [[nodiscard]] Result<void> retire_staged_global_io_rules_locked();
  [[nodiscard]] Result<void> install_global_io_rule_catalog_locked(
      StagedGlobalIoRuleCatalog& candidate);
  void set_global_io_rule_state_locked(std::size_t index, IoGlobalRuleState state,
      std::optional<IoRuleAcquisitionKeyDTO> acquisition = {},
      std::optional<ErrorDTO> unavailable_reason = {});
  /// The row can't watch: retire its registration, withdraw its key, gate.
  [[nodiscard]] Result<void> unwatch_global_io_rule_locked(std::size_t index, ErrorDTO reason);
  [[nodiscard]] std::optional<ErrorDTO> global_io_rule_hold_locked(std::size_t index,
                                                                   bool output_write) const;
  [[nodiscard]] Result<void> set_global_io_rule_active_locked(std::size_t index, bool active);
  /// One operation's pending speed step (OD-149), on the speed-scale thread
  /// or the host's reconcile pass; a busy commit guard leaves it pending.
  void run_runtime_scale_step(const OperationNumber& operation_id) noexcept;
  /// Teardown only: retires every rule it can and returns each failure.
  [[nodiscard]] std::vector<ErrorDTO> retire_all_io_rules_locked();
  [[nodiscard]] bool has_active_io_rules_locked() const;
  struct GlobalInterruptStopParticipant;
  struct GlobalInterruptStopSelection;
  [[nodiscard]] bool global_io_rule_applies_locked(
      const IoActivationRulePolicyBindingDTO& rule, const RuntimeParticipantBindingDTO& binding) const;
  [[nodiscard]] std::optional<ErrorDTO> global_io_rule_command_refusal_locked(
      const RuntimeParticipantBindingDTO& binding, const ActiveOperation* operation = nullptr) const;
  [[nodiscard]] bool program_blackboard_source_matches_operation_locked(
      const ActiveOperation& operation) const;
  [[nodiscard]] Result<void> arm_pending_global_io_rules_locked();
  [[nodiscard]] Result<IoRuleRegistrationDTO> arm_global_io_rule_locked(
      const IoActivationRulePolicyBindingDTO& rule, const IoRuleStateContextDTO& context,
      const std::shared_ptr<IIoSignalService>& service);
  [[nodiscard]] Result<bool> global_io_rule_supersedes_node_locked(
      const IoMotionRulePolicyBindingDTO& policy, const IoRuleEvidenceDTO& evidence,
      const ActiveOperationParticipant& participant) const;
  [[nodiscard]] Result<void> process_global_io_rule_evidence_locked(
      StopHandoff& handoff, const OperationNumber* dispatch_boundary = nullptr);
  [[nodiscard]] Result<void> validate_global_io_rule_stop_locked(
      const GlobalInterruptStopSelection& selection) const;
  [[nodiscard]] Result<void> process_io_rule_evidence_locked(
      StopHandoff& handoff, const OperationNumber* selected_operation = nullptr);
  /// At P4: write the participant's outcome (event id, result) into its rule
  /// outcome rows (`IoMotionRuleOutcomeDTO`).
  void finalize_io_rule_outcomes_locked(ActiveOperation& operation,
                                        std::size_t participant_ordinal);
  [[nodiscard]] Result<void> release_prepared_dispatch_barrier(
      const OperationNumber& operation_id, IResolvedParticipantExecutionEngine* engine,
      std::uint64_t preparation_id, const PreparedOperationParticipantDTO& participant_product);
  /// The exact local settlement a completed Stop handed to this
  /// command's dispatch return. A failed settlement fails closed.
  void settle_deferred_capability_stop(const OperationNumber& operation_id,
                                       IResolvedParticipantExecutionEngine* participant_engine,
                                       IResolvedParticipantExecutionEngine* settlement_engine,
                                       const AddressedParticipantStopSettlementRequestDTO& request);
  /// Perform, on the Stop lane, the exact settlements that closed Stops
  /// deferred to their never-dispatched participants' late cancellation
  /// terminals. A late failure is retained on the participant; one not proved
  /// an already-failed preparation fails closed (as a failed Stop settlement).
  void settle_deferred_pre_dispatch_cancellations() noexcept;
  /// Runs under execute()'s submission_mutex_; calls `end_admission` (which
  /// releases it) once the operation is durably admitted, before dispatch.
  [[nodiscard]] Result<OperationAcceptedDTO> execute_coordinated_motion_after_preflight(
      const OperationIntentDTO& intent, const std::function<void()>& end_admission);
  /// Run one immediate component step. Validates and resolves the
  /// single-participant intent, asks `step_for` for the step of the resolved
  /// registration (its refusal is returned), admits it (origin: Program node
  /// scope as a non-motion transaction, or manual readiness unless Robot
  /// control's own manual claim already covers the component; the
  /// `holds_engine` busy rule; no "enabled" or "motion admission open"
  /// check), then runs it outside the Kernel locks. Nothing is stored.
  [[nodiscard]] Result<OperationAcceptedDTO> execute_immediate_step(
      const OperationIntentDTO& intent,
      const std::function<Result<ImmediateComponentStep>(
          const RuntimeParticipantExecutionRegistration&)>& step_for);
  /// Active TCP: the Set TCP user of `execute_immediate_step`.
  [[nodiscard]] Result<OperationAcceptedDTO> execute_set_tcp(const OperationIntentDTO& intent);
  /// Dispatch none, cancel every prepared participant and release the
  /// reservation. A coordinated participant failure names its engine; a null
  /// engine is a whole-operation admission refusal (single or coordinated)
  /// that ends every participant InterventionRequired with `failure`.
  [[nodiscard]] Result<void> fail_pre_dispatch(const OperationNumber& operation_id,
                                               IResolvedParticipantExecutionEngine* failing_engine,
                                               ErrorDTO failure);
  [[nodiscard]] Result<OperationControlAcceptedDTO> apply_control(
      const OperationControlRequestDTO& request, OperationControlKind control,
      bool automatic_manual_retirement = false);
  [[nodiscard]] Result<KernelRecoveryActionResultDTO> apply_coordinated_operation_recovery_action(
      const OperationNumber& operation_id, OperationRevision expected_operation_revision,
      const KernelRecoveryActionRequestDTO& request);
  void CompleteOperationControlEffect(OperationControlEffectResult result) noexcept;
  [[nodiscard]] OperationStateDTO publish_settlement_fault_locked(ActiveOperation& operation);
  [[nodiscard]] Result<void> settle_control_operation_locked(const OperationNumber& operation_id,
                                                             OperationControlKind control);
  void clear_motion_continuation_locked(const ActiveOperation& operation);
  [[nodiscard]] std::vector<OperationNumber> durable_operation_retention_ids_locked() const;
  [[nodiscard]] bool evict_oldest_historical_operation_locked(
      std::span<const OperationNumber> protected_operation_ids);
  [[nodiscard]] std::size_t resident_operation_count_locked() const noexcept;
  [[nodiscard]] bool make_operation_admission_capacity_locked();
  void retain_historical_operation_locked(OperationStateDTO state);
  [[nodiscard]] Result<void> reconfigure_runtime_participants_impl(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver,
      bool retain_unready_configuration_facts, const ConfigurationFenceId* apply_fence_id = nullptr,
      const ConfigurationFenceScopeDTO* apply_scope = nullptr,
      const std::vector<ControllableComponentId>* retained_components = nullptr,
      const RetainedBundleId* candidate_retained_bundle_id = nullptr,
      const GraphRevision* candidate_graph_revision = nullptr,
      const std::function<Result<void>()>* commit_publication = nullptr);
  /// A fresh key for a request the Kernel itself makes (class 1, minter 3).
  /// Lock-free, no allocation.
  [[nodiscard]] RequestKey mint_kernel_request_key() noexcept {
    return RequestKey::Make(RequestKeyClass::kInternal, 0U,
                            static_cast<std::uint64_t>(RequestKeyMinter::kKernel),
                            next_kernel_request_sequence_.fetch_add(1U, std::memory_order_relaxed));
  }
  /// The key of a Kernel-internal request stored on the object that owns its
  /// cause (contract 1.4): minted on the first trigger and reused while that
  /// object stands, so a repeated trigger replays exactly. Called under the
  /// lock that guards the owner; no allocation.
  [[nodiscard]] RequestKey owned_kernel_request_key(std::optional<RequestKey>& slot) noexcept {
    if (!slot) slot = mint_kernel_request_key();
    return *slot;
  }
  [[nodiscard]] RequestKey owned_kernel_request_key(RevisionRequestKeySlot& slot,
                                                    const OperationRevision revision,
                                                    const std::uint64_t detail = 0U) noexcept {
    if (!slot.key || slot.revision != revision || slot.detail != detail) {
      slot.key = mint_kernel_request_key();
      slot.revision = revision;
      slot.detail = detail;
    }
    return *slot.key;
  }
  /// The key of a Kernel-internal close of `operation_id` at these
  /// coordinates, stored on the operation (contract 1.4; build spec 11.5
  /// "class 1 key stored on the owning object"). Takes state_mutex_.
  [[nodiscard]] RequestKey operation_recovery_close_key(const OperationNumber& operation_id,
                                                        OperationRevision revision,
                                                        std::uint64_t assessment_revision);
  [[nodiscard]] Result<void> apply_runtime_scale_status(
      IResolvedParticipantExecutionEngine* engine,
      const ResolvedRuntimeScaleStatusDTO& resolved_status);
  [[nodiscard]] Result<void> retain_runtime_scale_observation_locked(
      ActiveOperation& operation, RuntimeScaleStatusDTO status, const char* reason);
  /// Records the accepted value of one live operation and acknowledges it at
  /// its engine (run_runtime_scale_step, under the operation's try-locked
  /// `commit`, which it releases before the engine call). `value_sequence`:
  /// the order of this value at the participant (OD-72).
  [[nodiscard]] Result<RuntimeScaleAcceptedDTO> set_runtime_scale_impl(
      OperationCommitGuard& commit, const RuntimeScaleRequestDTO& request,
      std::uint64_t value_sequence, const std::optional<RequestKey>& control_request_id_if_sole);
  [[nodiscard]] Result<RuntimeScaleControlResolution> resolve_runtime_scale_control_target_locked(
      const RuntimeScaleControlSelectorDTO& selector) const;
  [[nodiscard]] Result<RuntimeScaleControlScope> resolve_runtime_scale_control_locked(
      const RuntimeScaleControlSelectorDTO& selector) const;
  [[nodiscard]] Result<RuntimeScaleControlStatusDTO> runtime_scale_control_locked(
      const RuntimeScaleControlSelectorDTO& selector, bool present_group) const;
  [[nodiscard]] double runtime_scale_setting_for_binding_locked(
      const RuntimeParticipantBindingDTO& binding) const noexcept;
  /// Progresses the one participant that holds the token's lane index
  /// (token = (lane index, lane generation)).
  void ProgressStop(StopControlWorkToken token) noexcept override;
  void StageAttemptCompleted(BoundedStopStageAttemptExecutor& source,
                             StopStageAttemptHandle handle) noexcept override;
  void OperationControlEffectCompleted(BoundedOperationControlExecutor& executor,
                                       OperationControlEffectHandle handle) noexcept override;
  void settle_completed_control_locked(const OperationNumber& operation_id);
  void wake_stop_after_preparation_borrow_locked(const OperationNumber& operation_id) noexcept;
  OperationStateDTO capture_control_failure_locked(ActiveOperation& operation);
  /// `program_reset_status` is the carried status of a Program Reset claim.
  [[nodiscard]] Result<CellAdmissionClaimDTO> acquire_cell_admission_claim(
      const CellAdmissionClaimIntentDTO& intent,
      const ProgramCycleStatusDTO* program_reset_status);
  /// Cell latch, state_mutex_ held. `arm` arms the latch
  /// condition; the arm clears when every component is healthy and idle.
  /// Returns the typed `component_fault` cause when a component entered
  /// `fault` in this update while an operation executes (OD-379).
  [[nodiscard]] std::optional<ErrorDTO> observe_component_states_locked(
      std::vector<ComponentState> states);
  /// OD-379: the cell's `component_fault` Stop (fault Stop profiles) through
  /// `request_stop_locked`; the participants it starts keep `cause`. The
  /// caller completes `handoff` after the unlock.
  void request_component_fault_stop_locked(const ErrorDTO& cause, StopHandoff& handoff);
  void refresh_cell_latch_locked(bool arm);
  /// Every component in the cell is healthy and idle (or latch-exempt).
  [[nodiscard]] bool cell_settled_locked() const;
  [[nodiscard]] bool cell_latched_locked() const;
  /// `cell_latched` while latched, else nothing.
  [[nodiscard]] std::optional<ErrorDTO> cell_latch_refusal_locked() const;
  /// A run, a node, manual motion or I/O is executing.
  [[nodiscard]] bool execution_in_progress_locked() const;
  // ==== Stop core (slice K1: implemented in resource_addressed_kernel_stop.cpp)
  /// The one Stop core (§3.1, G1-G8 with E1-E13). Called with state_mutex_
  /// held; never refused, never allocates. Returns the index of the event
  /// record (new, or the recorded one for an exact duplicate) in
  /// `stop_events_`; the caller copies it under the same lock.
  [[nodiscard]] std::uint32_t request_stop_locked(const StopRequest& request,
                                                  StopHandoff& handoff) noexcept;
  /// Lock, `request_stop_locked`, copy the record, unlock,
  /// `complete_stop_handoff`, build the reply. Used by every public Stop
  /// entry and by Kernel-internal producers that hold no Kernel lock.
  [[nodiscard]] StopAcceptedDTO submit_stop(const StopRequest& request);
  /// The after-unlock steps 1-5 of §3.1: engine fence call (2), publish lane
  /// indexes, change signal, Stop listener, projection worker wake. Then
  /// clears the hand-off for reuse.
  void complete_stop_handoff(StopHandoff& handoff) noexcept;
  /// The calling thread's hand-off and record copy, reserved to `N_max` and
  /// the component table on the thread's first use (§7.2).
  [[nodiscard]] StopHandoff& thread_stop_handoff();
  [[nodiscard]] StopEventRecord& thread_stop_record_copy();
  /// Bounded copy of one record (copy-assignment into equal sizes).
  void copy_stop_event_locked(std::uint32_t record_index, StopEventRecord& out) const noexcept;
  /// The reply of K15 from a record copy, after the unlock.
  [[nodiscard]] static StopAcceptedDTO stop_accepted_from_record(
      const StopEventRecord& record, RequestReplayStatusDTO replay_status);
  /// P4 (and P5 with F1/F2): one participant's attempt ended. Phase, lane
  /// release, counters, per-operation completion, ring completion; a failure
  /// not itself started by a `kStopFailedHalt` raises one into `handoff`.
  void complete_participant_stop_locked(ActiveOperation& operation,
                                        std::size_t participant_ordinal,
                                        std::int64_t completed_at_ns,
                                        StopHandoff& handoff) noexcept;
  /// The participant's outcome, for the takeover readers and the engine
  /// (K11).
  [[nodiscard]] static ParticipantStopOutcomeDTO participant_stop_outcome(
      const ActiveOperationParticipant& participant) noexcept;
  /// Per-operation Stop facts for the recovery assessment (K6, K20, C16),
  /// read from the participants' stop states
  /// (resource_addressed_kernel_status.cpp).
  [[nodiscard]] static bool any_participant_stopping(const ActiveOperation& operation) noexcept;
  [[nodiscard]] static std::string_view operation_stop_phase(
      const ActiveOperation& operation) noexcept;
  [[nodiscard]] static std::string operation_containing_stage_id(
      const ActiveOperation& operation);
  [[nodiscard]] static bool requested_controlled_stop_succeeded(
      const ActiveOperation& operation) noexcept;
  // Program-run Stop for each new Runner loss (S10); called with no Core
  // lock held.
  void stop_program_run_after_runner_loss(const ProgramRunNumber& program_run_id) noexcept;
  /// Replace the Stop component table (sorted component ids of the applied
  /// cell; PD-2 bits and F2 marks index it) and resize the stop-unconfirmed
  /// marks, keeping the marks of components that stay. Construction and Apply
  /// only (allocates).
  void rebuild_stop_component_table_locked();
  /// Bit index of a component in `stop_component_table_`; absent when it is
  /// not in the applied cell. Binary search, no allocation.
  [[nodiscard]] std::optional<std::size_t> stop_component_index_locked(
      const ControllableComponentId& component) const noexcept;
  /// Wake a stopping participant's Stop lane now (an event its Stop work
  /// waits for arrived): arms the lane's deadline at the current instant and
  /// returns the token the caller publishes after its unlock. Absent when the
  /// participant is not stopping.
  [[nodiscard]] std::optional<StopControlWorkToken> wake_participant_stop_locked(
      const ActiveOperationParticipant& participant) noexcept;
  /// The component states with the F2 overlay: a stop-unconfirmed component
  /// shows activity unknown with reason `stop_unconfirmed`. Allocates; not on
  /// the Stop acceptance path.
  [[nodiscard]] std::vector<ComponentState> latch_component_states_locked() const;
  /// Posts a copy of the operation's current state (with its participants'
  /// `stop` objects) to the journal writer, latest-wins, without a wake.
  /// Stop lane only (P1, P4): the copy allocates there, never on acceptance.
  void capture_stop_operation_state_locked(ActiveOperation& operation, const char* reason);
  /// A component's stop is unconfirmed (F2).
  [[nodiscard]] bool stop_unconfirmed_locked(std::string_view component_id) const noexcept;

  // ==== Holds and releases (slice K2: resource_addressed_kernel_operations.cpp)
  /// G3 for one event: the hold effect of the applied kind (rows S1-S18 of
  /// §3.2) as a join, E11 call (1) when a node becomes held or the run frozen,
  /// `record.hold_change`. Returns whether G6 must raise `hold_revision_`
  /// (the hold changed, or a release could lower what the Stop asks now).
  [[nodiscard]] bool apply_stop_hold_locked(const StopRequest& request, StopKind applied,
                                            StopEventRecord& record,
                                            StopHandoff& handoff) noexcept;
  /// S1 group resolution against the capture's named selections, widened by
  /// the coordinated closure, into `run.hold.selected_nodes`. False when a
  /// group is unknown (invalid: Stop all).
  [[nodiscard]] bool select_program_groups_locked(ProgramCycleClaimRun& run,
                                                  std::span<const std::string> group_ids) noexcept;
  /// §4.2 counters: a participant scoped to a captured node of the claimed
  /// run changed phase (G4: running/failed/stopped → stopping; P4: stopping →
  /// stopped/failed; retire by Recover or Reset: failed → gone). Called by K1
  /// at G4 and P4, by K2 at retire.
  void count_participant_phase_locked(const ActiveOperationParticipant& participant,
                                      StopPhase from, std::optional<StopPhase> to) noexcept;
  /// §4.2 node display state (stored hold, else derived stopping/faulted).
  [[nodiscard]] ProgramNodeHoldStateDTO node_display_state_locked(
      const ProgramCycleClaimRun& run, std::uint32_t node_index) const noexcept;
  /// §4.2 "run stopping": any node stopping, or a handover participant of
  /// the run stopping.
  [[nodiscard]] bool run_stopping_locked(const ProgramCycleClaimRun& run) const noexcept;
  /// §4.2 "cell stopping": `stopping_count_ > 0`.
  [[nodiscard]] bool cell_stopping_locked() const noexcept;
  /// §4.2 Halt releasable: frozen at Halt or runner loss and nothing in the
  /// run stopping or uncontained.
  [[nodiscard]] bool halt_releasable_locked(const ProgramCycleClaimRun& run) const noexcept;
  /// §4.2 manual handover allowed: `frozen == kNone` (plus the existing
  /// producer-held rule, checked by the caller).
  [[nodiscard]] bool manual_handover_allowed_locked() const noexcept;
  /// The release check of §4.1 (existing clauses, C1, C2, C3, C3', C4) over
  /// `scope_nodes` (Execute after Halt: every node). The caller took the
  /// admission-lock snapshot of unsettled children first.
  [[nodiscard]] std::optional<ErrorDTO> program_release_refusal_locked(
      const ProgramCycleClaimRun& run, std::span<const std::uint32_t> scope_nodes,
      std::uint64_t expected_hold_revision, bool halt_release) const;

  RuntimeParticipantRegistry registry_;
  std::shared_ptr<std::recursive_mutex> admission_mutex_;
  std::shared_ptr<ResourceSlotTable> resource_slots_;
  std::vector<RuntimeParticipantExecutionRegistration> registrations_;
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO>
      runtime_route_configuration_readbacks_;
  std::set<ControlDomainId> runtime_route_configuration_publication_required_;
  IClock& clock_;
  IOperationJournal& journal_;
  ResourceAddressedExecutionKernelConfig config_;
  // Activation-pinned; changed only under state_mutex_, never a mutable forwarder.
  std::shared_ptr<IIoSignalService> io_signal_service_;
  // One activation owner, independent of Run watch lifetime. Candidate state
  // remains private until actual successful Applied publication is proven.
  std::shared_ptr<const IoActivationRuleCatalogDTO> global_io_rule_catalog_;
  // Mutually exclusive no-service alternative of this same Applied owner.
  std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> global_io_rule_waiting_for_program_;
  std::optional<IoRuleAppliedContextDTO> global_io_rule_applied_context_;
  std::shared_ptr<IIoSignalService> global_io_rule_service_;
  std::vector<GlobalIoRuleRuntime> global_io_rules_;
  std::set<InterruptSourceId> planned_disabled_io_sources_;
  std::unique_ptr<StagedGlobalIoRuleCatalog> staged_global_io_rule_catalog_;
  // Applied Program source lifetime is independent of a held Run/watch record.
  // Actual typed values live only in the installed shared signal subsystem.
  std::shared_ptr<const AppliedProgramBlackboardContextDTO> applied_program_blackboard_context_;
  std::optional<IoProgramBlackboardSourceBindingDTO> program_blackboard_source_;
  bool program_blackboard_source_retired_{};
  [[nodiscard]] Result<void> notify_io_rule_evidence_processing(
      const std::shared_ptr<IIoSignalService>& affected_service) const;
  [[nodiscard]] Result<void> retire_program_blackboard_source_locked(
      std::shared_ptr<IIoSignalService>* actual_retired_service = nullptr);
  [[nodiscard]] Result<IoProgramBlackboardAcknowledgementDTO> admit_program_blackboard_frame(
      const IoProgramBlackboardPublicationDTO& publication);
  [[nodiscard]] Result<void> reconcile_program_blackboard_cycle_locked(
      const std::optional<ProgramCycleStatusDTO>& cycle,
      std::shared_ptr<IIoSignalService>* actual_retired_service = nullptr);
  [[nodiscard]] bool program_blackboard_scope_current_locked(
      const IoProgramBlackboardScopeDTO& scope, const ProgramCycleStatusDTO& cycle,
      const AppliedProgramBlackboardContextDTO& context) const;
  [[nodiscard]] bool program_blackboard_scope_current_locked(
      const IoProgramBlackboardScopeDTO& scope, const ProgramCycleStatusDTO& cycle,
      ActiveBundlePublicationGeneration expected_publication,
      const ProgramBindingDTO& actual_binding) const;
  std::unique_ptr<KernelCellAdmissionOwner> cell_admission_owner_;

  // Serializes public Execute admission (through registration, durable
  // acceptance and the subordinate Prepare) with reconfiguration and the
  // other ordered submissions, while permitting synchronous engine callbacks
  // to take state_mutex_. Not held across operation dispatch (OD-67, audit
  // 3.8); motion takeover acceptance still advances under it. Speed scaling
  // never takes it: Execute reads the speed setting in the state_mutex_
  // section that inserts its operation.
  mutable std::mutex submission_mutex_;
  mutable std::mutex operation_recovery_mutex_;
  mutable KernelStateMutex state_mutex_;
  /// Raised (state_mutex_) at every change of an operation's state, residence
  /// or pending control; the stream's `operations.change`.
  std::uint64_t operation_change_{0U};
  struct ProgramCycleClaimRun final {
    ProgramRunNumber program_run_id;
    CellAdmissionClaimGeneration claim_generation{0U};
    ProgramRevision program_revision;
    /// Shared immutable so a Stop event record can keep the group names
    /// after the claim retired (§2.4 `groups`); null until captured.
    std::shared_ptr<const ProgramRunExecutionCaptureDTO> capture{};
    /// The run's hold state (§2.3); dropped with the claim.
    RunHoldState hold{};
    struct MotionContinuation;
    // Aligned with the immutable captured node table, bounded by that table.
    std::vector<std::shared_ptr<const MotionContinuation>> motion_continuations{};
    bool io_watches_retired{};
    struct IoWatchScope final {
      ProgramNodeExecutionScopeDTO node;
      // Live reservations are released only by their exact known token.
      // A missing token is a cancelled uncertain scope, retained to Run end.
      std::optional<IoWatchRegistrationDTO> registration;
    };
    // Live reservations plus cancelled uncertain scopes share the captured
    // node-count bound. Ordinary known-token releases recycle their slots.
    std::vector<IoWatchScope> io_watch_scopes{};
    bool io_watch_admission_closed{};
  };
  void retire_program_io_watches_locked(const ProgramRunNumber& run_id);
  [[nodiscard]] Result<void> validate_current_program_io_run_scope_locked(
      const ProgramRunMockSimulationScopeDTO& request) const;
  [[nodiscard]] Result<void> validate_current_program_io_node_scope_locked(
      const ProgramIoWatchScopeReleaseRequestDTO& request) const;
  [[nodiscard]] Result<void> validate_program_io_watch_scope_locked(
      const ProgramIoWatchScopeReleaseRequestDTO& request) const;
  [[nodiscard]] Result<std::vector<std::uint32_t>> resolve_program_selection_locked(
      const ProgramExecutionSelectionRequestDTO& request,
      bool allow_node_release = false) const;
  [[nodiscard]] ProgramExecutionScopeStateDTO program_execution_scope_locked(
      const ProgramCycleClaimRun& run) const;
  [[nodiscard]] std::optional<ProgramSelectionReleaseBlockReasonDTO>
  program_selection_release_block_reason_locked(const ProgramCycleClaimRun& run) const;
  // Whether a captured node can have physical effects. Unknown capture or
  // index is treated as effectful, so containment is never skipped.
  [[nodiscard]] bool program_node_effectful_locked(const ProgramRunNumber& program_run_id,
                                                   std::size_t index) const;
  // The request replay ledgers, indexed by request id. Each holds at most
  // `request_replay_capacity` entries: the runtime-scale ledger evicts its
  // oldest entry at capacity, the other two leave with the historical
  // operation they belong to. Stops are recognised by the event ring.
  struct ReplayRequestIndex final {
    using Key = RequestKey;
    static const RequestKey& key(const ReplayEntry& entry);
  };
  struct ControlReplayRequestIndex final {
    using Key = RequestKey;
    static const RequestKey& key(const ControlReplayEntry& entry);
  };
  struct RuntimeScaleControlReplayRequestIndex final {
    using Key = RequestKey;
    static const RequestKey& key(const RuntimeScaleControlReplayEntry& entry);
  };
  IndexedLedger<ReplayEntry, ReplayRequestIndex> replay_ledger_;
  IndexedLedger<ControlReplayEntry, ControlReplayRequestIndex> control_replay_ledger_;
  IndexedLedger<RuntimeScaleControlReplayEntry, RuntimeScaleControlReplayRequestIndex>
      runtime_scale_control_replay_ledger_;
  std::vector<RuntimeScaleControlSetting> runtime_scale_control_settings_;
  struct OperationRecoveryReplayEntry;
  std::vector<OperationRecoveryReplayEntry> operation_recovery_replay_ledger_;
  struct HistoricalOperationIndex final {
    using Key = OperationNumber;
    static const OperationNumber& key(const OperationStateDTO& state) { return state.operation_id; }
  };
  // Oldest first; at most `request_replay_capacity` operations (resident plus
  // historical).
  IndexedLedger<OperationStateDTO, HistoricalOperationIndex> historical_operations_;
  std::optional<ProgramRunNumber> pending_program_run_admission_;
  // Program-cycle acquisition is synchronous and precedes all Run effects.
  // Its transaction identity is the exact ProgramRunNumber, so Stop never waits
  // for the asynchronously refreshed Manager status projection to know a run.
  std::optional<ProgramCycleClaimRun> program_cycle_claim_run_;
  // Same ledger's current exact manual claim; also protects its operation
  // from diagnostic-history retirement until settlement/reconciliation.
  std::optional<CellAdmissionClaimDTO> manual_operation_claim_;
  // The loss in the last accepted readiness report: the Runner cannot be heard now.
  std::optional<ProgramRunnerContactLossDTO> program_runner_loss_;
  /// Canonical bounded resident operation table. Unresolved operations are
  /// never evicted; resolved history rotates before new admission.
  std::map<OperationNumber, std::unique_ptr<ActiveOperation>> active_operations_;
  // Shared with every engine callback snapshot. The ingress owns the only
  // callback-visible Kernel pointer and drains entered callbacks before any
  // Kernel state or asynchronous lane is retired.
  std::shared_ptr<EngineEventIngress> engine_event_ingress_;
  std::vector<EngineSubscription> engine_subscriptions_;
  std::uint64_t next_engine_subscription_id_{1};
  // Engaged exactly once during successful construction and replaced only by
  // a fully validated runtime reconfiguration.
  std::optional<GraphRevision> stop_graph_revision_;
  std::optional<RetainedBundleId> stop_retained_bundle_id_;
  std::vector<StopScopeComponentSnapshotDTO> stop_component_scope_;
  // ---- Stop state (§2, all guarded by state_mutex_ unless atomic).
  /// The one hold revision for the whole start (§2.3); releases name it.
  std::uint64_t hold_revision_{};
  /// Participants in `kStopping`, cell-wide.
  std::uint32_t stopping_count_{};
  /// Raised at every P4 (a participant ended an attempt). Change marker.
  std::uint64_t stop_progress_sequence_{};
  /// The newest event; change marker. 0 before the first Stop.
  StopEventId last_stop_event_id_{0U};
  std::uint64_t next_stop_event_id_{1U};
  /// The highest Program run number ever claimed in this start (PD-13 as
  /// decided: no ring of retired runs). A group Stop naming a run that is
  /// not current and not above it has no effect; above it is invalid.
  ProgramRunNumber highest_program_run_started_{0U};
  /// E11 scratch (slice K2): one bit per captured node of the claimed run,
  /// sized at capture, for the allocation-free held-subtree closure of
  /// `fence_held_prepared_program_commands_locked`.
  std::vector<std::uint64_t> hold_subtree_scratch_;
  StopLaneFreeList stop_lanes_;
  /// Per Stop lane index, its current generation, mirrored from
  /// `stop_lanes_` under the lock so the accepting thread publishes the
  /// exact token after its unlock without reading Kernel state (§3.1 step 2).
  std::unique_ptr<std::atomic<std::uint32_t>[]> stop_lane_generations_;
  StopEventRing stop_events_;
  /// Operations touched by the Stop being accepted (G4 scratch, at most
  /// `N_max` started participants' operations), reserved at construction.
  std::vector<ActiveOperation*> stop_touched_operations_;
  /// Size of `stop_component_table_`, readable without the lock: a thread
  /// sizes its hand-off's component bit set from it before taking the lock.
  std::atomic<std::size_t> stop_component_count_{0U};
  /// The applied cell's component table (sorted), replaced at Apply.
  std::shared_ptr<const StopComponentTable> stop_component_table_;
  StopUnconfirmedComponents stop_unconfirmed_;
  /// E1 scratch, reserved at construction (slice K2 defines the type).
  std::unique_ptr<ResourceSlotStopFenceScratch> stop_fence_scratch_;
  /// The journal writer's queue: posts append to `journal_queue_` under
  /// state_mutex_; the writer swaps it with its own empty buffer. Both are
  /// reserved once at construction and never grown.
  std::vector<JournalRecord> journal_queue_;
  /// Bumped at every take: a latest-wins ticket of an older take is void.
  std::uint64_t journal_queue_generation_{1U};
  /// Records the full queue dropped since the last take.
  std::uint64_t journal_records_dropped_{};
  /// Records not written in this start (dropped, failed, reused Stop slot).
  std::uint64_t journal_records_not_written_{};
  /// A loss episode started and its log line is not yet printed.
  bool journal_loss_line_owed_{};
  /// Records appended so far; the writer's drain compares it with what it wrote.
  std::atomic<std::uint64_t> journal_posted_count_{0U};
  std::unique_ptr<JournalWriter> journal_writer_;
  std::unique_ptr<BoundedStopControlExecutor> stop_control_executor_;
  std::unique_ptr<BoundedStopStageAttemptExecutor> stop_stage_attempt_executor_;
  std::unique_ptr<BoundedOperationControlExecutor> operation_control_executor_;
  bool operation_control_reconfiguration_in_progress_{};
  /// OD-72: speed-effect calls running on captured raw engine pointers with
  /// no lock held (an operation's scale effects, a setting stored at its
  /// participants). Guarded by `state_mutex_`. A lifetime borrow only: it
  /// makes engine retirement wait for them (it never refuses or delays a
  /// speed change).
  std::size_t runtime_scale_effect_calls_in_flight_{};
  /// `N_max` (§2.2).
  std::size_t prequalified_max_live_stop_participants_{};
  std::uint64_t next_operation_id_{1U};
  /// Sequence of the Kernel's own request keys (class 1, minter 3, contract
  /// 1.4), from 1 per start. Atomic so any Kernel thread mints without a lock.
  std::atomic<std::uint64_t> next_kernel_request_sequence_{1U};
  /// Cell latch inputs: the last observed state of every
  /// component in the cell, and whether a latch condition is armed.
  std::vector<ComponentState> component_states_;
  bool cell_latch_armed_{};
  /// Written once by `set_stop_listener`; read after unlock with no lock.
  std::atomic<IStopListener*> stop_listener_{nullptr};
  /// Engines held by a running immediate component step
  /// (`ImmediateComponentStep::holds_engine`). Guarded by `state_mutex_`.
  std::set<IResolvedParticipantExecutionEngine*> immediate_step_engines_;
  std::optional<OperationJournalWriteFaultDTO> operation_journal_write_fault_{};
  std::optional<KernelInternalFaultDTO> internal_fault_{};
  // New motion admission stays closed only for the Kernel's internal fault
  // (until host restart); it belongs to no component and shows in readiness
  // (`ReadExecutionBlocks`). A journal write fault blocks nothing. Every
  // other cause is component state the cell latch derives.
  [[nodiscard]] bool motion_admission_closed_locked() const noexcept {
    return internal_fault_.has_value();
  }
  std::optional<RetainedOperatorAlarmAttentionDTO> operator_alarm_attention_;
};

}  // namespace manyforge::core
