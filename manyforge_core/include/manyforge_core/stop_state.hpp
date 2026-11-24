#pragma once

// Stop state model (design 2026-11-13, §2): the Kernel's Stop state.
//
// Everything in this header is plain data guarded by the Kernel's
// `state_mutex_` (§2.5); nothing in it is written to or read back from disk.
// Every array whose size depends on configuration (`N_max` =
// `prequalified_max_live_stop_participants`, `R` = `request_replay_capacity`)
// is sized once at Kernel construction and never resized; a Stop never
// allocates in these structures (§9.2). Sizes that depend on the applied cell
// (captured nodes, cell components) are sized at capture or Apply.
//
// Interfaces only: the Kernel slices (K1, K2) implement every transition.

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/iio_signal_service.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/request_key.hpp>
#include <manyforge_core/types/stop_event.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

struct PreparedParticipantStopPlanDTO;
struct ProgramRunExecutionCaptureDTO;

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

/// Principal bytes kept in an event record (the former reserve's bound).
inline constexpr std::size_t kStopEventPrincipalBytes = 256U;
/// Reason bytes kept in an event record (`kMaximumStopRequestLabelBytes`).
inline constexpr std::size_t kStopEventReasonBytes = kMaximumStopRequestLabelBytes;
/// Request-context label bytes kept for display; a longer label is cut.
inline constexpr std::size_t kStopEventSourceLabelBytes = 64U;
/// Group indexes stored per event; `group_count` keeps the total.
inline constexpr std::size_t kStopEventGroupSlots = 16U;
/// Duplicate-index bucket markers (`StopEventRing::index`).
inline constexpr std::uint32_t kStopRingEmptyBucket = 0xFFFFFFFFU;
inline constexpr std::uint32_t kStopRingTombstoneBucket = 0xFFFFFFFEU;
/// Stop lane index meaning "none held".
inline constexpr std::uint16_t kNoStopLane = 0xFFFFU;
/// `StopCoveredEntry::error_code` of an error whose code is not catalogued.
inline constexpr std::uint16_t kStopErrorCodeUncatalogued = 0xFFFFU;

/// Fixed text with its length; never allocates. Bytes past `length` are
/// unspecified.
template <std::size_t Capacity>
struct BoundedStopText final {
  std::array<char, Capacity> bytes{};
  std::uint16_t length{};

  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(bytes.data(), length);
  }
};

// ---------------------------------------------------------------------------
// §2.1 Participant stop state
// ---------------------------------------------------------------------------

/// Who a Stop is attributed to. Raised monotonically by every Stop that
/// covers the participant, lexicographically on (class, rule priority).
/// Replaces `program_requested_stop` and the interrupt/global winner lists.
struct StopAttribution final {
  StopAttributionClass attribution_class{StopAttributionClass::kProgramOwn};
  std::int32_t rule_priority{};
  /// Index of the rule row (global or per-operation) that raised it.
  std::uint32_t rule_index{};

  friend bool operator==(const StopAttribution&, const StopAttribution&) = default;
};

/// `StopAttribution::rule_index` of a global rule's Stop (S6, S7, and the
/// operator's Stop of a rule that can't watch): the rule's catalogue index
/// with the global bit set, plus the watch-lost bit. Per-operation rule Stops
/// (S9) leave the global bit clear.
inline constexpr std::uint32_t kStopAttributionGlobalRule = 0x8000'0000U;
inline constexpr std::uint32_t kStopAttributionWatchLost = 0x4000'0000U;
inline constexpr std::uint32_t kStopAttributionRuleIndexMask = 0x3FFF'FFFFU;

/// Member `stop` of `ActiveOperationParticipant`, created with the
/// participant at admission. About 72 bytes plus the optional error.
struct ParticipantStopState final {
  StopPhase phase{StopPhase::kRunning};
  /// 0 while running; 1 for the first Stop; raised by each re-attempt.
  std::uint16_t attempt{};
  /// Fixed by the Stop that starts the attempt.
  StopMode mode{StopMode::kOrdinary};
  /// `kInterrupt` only: the rule's `stop_entry` and `success_through` as
  /// indexes into `plan`'s stages.
  std::uint8_t interrupt_entry{kNoStopStage};
  std::uint8_t interrupt_success_through{kNoStopStage};
  /// Highest stage whose attempt was authoritative, succeeded and error-free.
  std::uint8_t containing_stage{kNoStopStage};
  /// The containing stage is stronger than the intended first stage.
  bool escalated{};
  /// A collision was observed on the Stop path (the error is in `error`).
  bool stop_path_collision{};
  /// Degraded-Stop or failure cause, written only by the Stop lane at P4.
  std::optional<ErrorDTO> error{};
  /// The event that moved the participant to `kStopping`; 0 while running.
  StopEventId started_by{0U};
  StopAttribution attribution{};
  /// The executed plan: a pointer into immutable prepared data the
  /// participant holds (`ordinary_stop_plan`, `fault_stop_plan`, or the rule's
  /// prepared interrupt-scope plan); never a copy. Its
  /// `participant_stop_invocation_plan_digest` is the digest the interrupt
  /// node compares (§6.3.1); it is read through this pointer, not copied
  /// (ContentDigest is a heap string).
  const PreparedParticipantStopPlanDTO* plan{};
  /// Stop lane index held while `kStopping` (`kNoStopLane` otherwise) and its
  /// generation; together they identify this attempt in the event ring and
  /// form the Stop control executor token.
  std::uint16_t lane_index{kNoStopLane};
  std::uint32_t lane_generation{};
};

// ---------------------------------------------------------------------------
// §2.2 Stop lane indexes
// ---------------------------------------------------------------------------

/// Fixed free list of `N_max` Stop lane indexes, allocated at construction.
/// G4 pops an index for each participant it starts; P4 pushes it back and
/// raises its generation. The executor token is (index, generation).
struct StopLaneFreeList final {
  struct Owner final {
    OperationNumber operation_id{0U};
    std::uint32_t participant_ordinal{};
    /// Raised at every release; never 0 once the index was used.
    std::uint32_t generation{};
  };
  /// Stack of free indexes; the first `free_count` entries are valid.
  std::vector<std::uint16_t> free{};
  std::uint16_t free_count{};
  /// Per index: its current owner and generation.
  std::vector<Owner> owners{};
};

// ---------------------------------------------------------------------------
// §2.3 Hold state (per claimed run)
// ---------------------------------------------------------------------------

/// Stored node hold. `stopping`/`faulted` are derived (§4.2).
enum class RunNodeHold : std::uint8_t {
  kReady = 0,
  kRunning,
  kHeld,
};

/// Member `hold` of the claimed run. Exists only while a run is claimed.
struct RunHoldState final {
  /// One per captured node, allocated at capture.
  std::vector<RunNodeHold> node_hold{};
  /// A Stop with a hold component arrived after the claim, before capture.
  bool all_groups_held{};
  RunFrozenLevel frozen{RunFrozenLevel::kNone};
  /// The event that froze the run; 0 while not frozen.
  StopEventId frozen_by{0U};
  /// The global rule whose `hold` trip held the run (latest one).
  std::optional<InterruptPolicyId> held_by_rule{};
  /// Per captured node: 1 + the index of the Program node whose own Stop
  /// (OD-342) raised its hold, 0 for any other hold or none; cleared by its
  /// release. Allocated at capture.
  std::vector<std::uint32_t> held_by_program_node{};
  /// Per captured node: participants of resident operations scoped to the
  /// node whose phase is `kStopping` / `kFailed`. Allocated at capture.
  std::vector<std::uint16_t> stopping{};
  std::vector<std::uint16_t> failed{};
  /// Scratch bit set over captured nodes, sized at capture: the S1 group
  /// resolution writes the selected (coordination-widened) nodes here under
  /// the lock, so acceptance allocates nothing.
  std::vector<std::uint64_t> selected_nodes{};
};

// ---------------------------------------------------------------------------
// §2.4 Event record and ring
// ---------------------------------------------------------------------------

/// The failed attempt that raised a `kStopFailedHalt` (§3.6 F1).
struct StopCausedBy final {
  StopEventId event_id{0U};
  OperationNumber operation_id{0U};
  /// `OperationParticipantId` ordinal (index + 1), as published.
  std::uint32_t participant_ordinal{};
};

/// One covered participant of an event. Outcome fields are filled at P4 so
/// History and `read_stop` never need the (possibly retired) operation.
struct StopCoveredEntry final {
  OperationNumber operation_id{0U};
  /// `OperationParticipantId` ordinal (index + 1), as published.
  std::uint32_t participant_ordinal{};
  std::uint16_t lane_index{kNoStopLane};
  std::uint32_t lane_generation{};
  /// This event started the participant's attempt (false: already stopping).
  bool started{};
  bool done{};
  StopPhase phase{StopPhase::kStopping};
  std::uint8_t containing_stage{kNoStopStage};
  /// Kind of the containing stage (fixed enumeration), read from the
  /// executed plan at P4, so History needs neither the plan nor the retired
  /// operation (coordinator N4). Meaningful only when `containing_stage` is set.
  StopEscalationStageKind containing_stage_kind{StopEscalationStageKind::kNominal};
  bool escalated{};
  /// A collision was observed on this participant's Stop path (§6.5).
  bool stop_path_collision{};
  /// The participant's error code as 1 + its position in the generated,
  /// sorted error-catalog code array (`error_catalog.generated.hpp`, slice
  /// K2 adds the array); 0 = no error; `kStopErrorCodeUncatalogued` = a code
  /// not in the catalogue. Looked up at P4 without allocation.
  std::uint16_t error_code{};
};

/// One stale-metadata diagnostic (operation revision only).
struct StopStaleEntry final {
  OperationNumber operation_id{0U};
  OperationRevision expected{0U};
  OperationRevision actual{0U};
};

/// One accepted Stop request. Copied whole (bounded) by the reply builder and
/// the journal writer; copy-assignment into a record of the same sizes
/// does not allocate.
struct StopEventRecord final {
  /// 0 = empty slot.
  StopEventId event_id{0U};
  RequestKey request_key{};
  /// Hash of the normalised request content (kind, selector, run, groups,
  /// expected operation revisions, attribution). A record created by a key
  /// conflict stores the hash of the new (conflicting) request.
  std::uint64_t content_hash{};
  StopKind requested{StopKind::kOperation};
  StopKind applied{StopKind::kOperation};
  /// `kStopFailedHalt` only.
  StopCausedBy caused_by{};
  StopWidenReason widen_reason{StopWidenReason::kNone};
  RequestTransportDTO transport{RequestTransportDTO::kInternal};
  RequestAssuranceDTO assurance{RequestAssuranceDTO::kUnverified};
  /// Empty (length 0) when the request carries no principal.
  BoundedStopText<kStopEventPrincipalBytes> principal{};
  BoundedStopText<kStopEventReasonBytes> reason{};
  BoundedStopText<kStopEventSourceLabelBytes> source_label{};
  /// 0 when the Stop names no run (only S1, S10, S16 name one).
  ProgramRunNumber run_number{0U};
  /// Indexes into the capture's named selections: the first 16, and the
  /// total. `capture` lets the ordinary-thread writer print the names after
  /// the claim is gone.
  std::array<std::uint16_t, kStopEventGroupSlots> groups{};
  std::uint16_t group_count{};
  std::shared_ptr<const ProgramRunExecutionCaptureDTO> capture{};
  /// Bit set of `StopHoldChange`.
  std::uint8_t hold_change{};
  std::uint64_t hold_revision_after{};
  /// `N_max` entries, sized at construction; the first `covered_count` valid.
  std::vector<StopCoveredEntry> covered{};
  std::uint16_t covered_count{};
  /// `N_max` bits, one per Stop lane index: covered entries not yet `done`.
  std::vector<std::uint64_t> covered_lanes{};
  /// Covered entries not yet `done`; the event completes at 0.
  std::uint16_t outstanding{};
  /// E1's slot-table fence was refused; logged by the journal writer.
  bool fence_degraded{};
  /// The fence refusal's code, encoded as `StopCoveredEntry::error_code`.
  std::uint16_t fence_degraded_reason{};
  /// `N_max` entries, sized at construction; the first `stale_count` valid.
  std::vector<StopStaleEntry> stale{};
  std::uint16_t stale_count{};
  /// All stale diagnostics found, stored or not.
  std::uint32_t stale_total{};
  bool any_failed{};
  bool any_escalated{};
  std::int64_t accepted_at_ns{};
  std::int64_t completed_at_ns{};
  StopEventResult result{StopEventResult::kPending};
};

/// `R` records preallocated at construction, written at `head` (oldest
/// overwritten), plus an open-addressing index from request key to record
/// (`2R` buckets of record indexes; `kStopRingEmptyBucket`,
/// `kStopRingTombstoneBucket`).
struct StopEventRing final {
  std::vector<StopEventRecord> records{};
  std::vector<std::uint32_t> index{};
  std::uint32_t head{};
};

// ---------------------------------------------------------------------------
// §3.6 F2 stop unconfirmed, and the component table it and PD-2 index
// ---------------------------------------------------------------------------

/// The applied cell's controllable components, sorted, replaced as a whole at
/// Apply. Bit i of a component bit set names `(*table)[i]`. Shared immutable,
/// so a hand-off can keep it across the unlock without copying strings.
using StopComponentTable = std::vector<ControllableComponentId>;

/// Kernel-owned "stop unconfirmed" marks over the component table (sized at
/// Apply): set at P5 for the failed participant's component, cleared only by
/// an idle state of that component arriving after the mark.
struct StopUnconfirmedComponents final {
  std::shared_ptr<const StopComponentTable> table{};
  std::vector<std::uint8_t> marked{};
};

// ---------------------------------------------------------------------------
// §3.1 The one Stop core: request and hand-off
// ---------------------------------------------------------------------------

/// One participant named by a Kernel-internal Stop (S12, S13).
struct StopParticipantRef final {
  OperationNumber operation_id{0U};
  /// Index into the operation's participants (ordinal − 1).
  std::uint32_t participant_ordinal{};
};

/// A rule's prepared interrupt entry for one participant (S6, S9): indexes
/// into the rule's prepared interrupt-scope plan, which the participant holds.
struct StopInterruptSelection final {
  OperationNumber operation_id{0U};
  std::uint32_t participant_ordinal{};
  const PreparedParticipantStopPlanDTO* plan{};
  std::uint8_t stop_entry{kNoStopStage};
  std::uint8_t success_through{kNoStopStage};
};

/// What a Stop names.
enum class StopSelectorKind : std::uint8_t {
  /// Whole cell: Stop all, Halt, cell Stop, internal fault, planned stop,
  /// failed-Stop Halt, a global rule with `everything`.
  kCell = 0,
  kOperations,
  kComponents,
  kResources,
  /// S1: `program_run_id` + `group_ids`.
  kProgramGroups,
  /// S10, S16: every operation of `program_run_id` (Program and handover).
  kProgramRun,
  /// S12, S13 (and S14/S15 on one operation's participants).
  kParticipants,
  /// S6 (not `everything`), S7, S9.
  kInterruptSelections,
};

/// The value every Stop producer hands to `request_stop_locked`. Small: the
/// spans and pointers name the caller's own data, valid for the call. No
/// field is copied into Kernel state except into the event record.
struct StopRequest final {
  RequestKey request_key{};
  StopKind kind{StopKind::kOperation};
  /// Set by an entry that already knows the request is invalid (malformed
  /// ingress, stale application start); G2 widens it.
  StopWidenReason preset_widen{StopWidenReason::kNone};
  StopSelectorKind selector{StopSelectorKind::kCell};
  std::span<const StopOperationTargetDTO> operations{};
  std::span<const StopComponentTargetDTO> components{};
  std::span<const StopResourceTargetDTO> resources{};
  std::optional<ProgramRunNumber> program_run_id{};
  std::span<const std::string> group_ids{};
  std::span<const StopParticipantRef> participants{};
  std::span<const StopInterruptSelection> interrupt_selections{};
  /// Null for Kernel-internal Stops (transport internal, unverified).
  const RequestAttributionDTO* attribution{};
  std::string_view source_label{};
  std::string_view reason{};
  StopAttribution attribution_rank{};
  /// Kernel-internal physical mode (`kFault`, `kHard`, `kSpringReturn`);
  /// absent: chosen as today's acceptance chooses it.
  std::optional<StopMode> mode{};
  /// S7: the rule that holds the run.
  const InterruptPolicyId* held_by_rule{};
  /// S1 only: the Program node whose own Stop this is (OD-342).
  const ProgramNodeInstanceId* held_by_program_node{};
  /// S12: this Stop is the takeover's own forced Stop of the incumbent, so
  /// E13 does not cancel that incumbent's pending and queued candidates.
  bool own_takeover_stop{};
  /// S18 only.
  StopCausedBy caused_by{};
};

/// One engine fence call (1) made for a named execution (contract §14 (1)):
/// the accepting thread makes call (2) for every one after the unlock, before
/// publishing lane indexes, also when `closed_gates` is 0 (call (2) revokes
/// the lookahead permits on every Stop).
struct StopEngineFenceCompletion final {
  IResolvedParticipantExecutionEngine* engine{};
  std::uint64_t execution_id{};
  /// Bit set of `AddressedParticipantStopFenceGate`.
  std::uint8_t closed_gates{};
};

/// What the accepting thread does after `state_mutex_` is released (§3.1,
/// steps 1-5), filled under the lock by `request_stop_locked` (and by P4 for
/// a failed-Stop Halt). Owned by the accepting thread; its arrays are sized
/// to `N_max` (lanes, completions) and to the component table once, before
/// the first Stop of that thread, and only cleared afterwards.
struct StopHandoff final {
  /// `N_max` bits: lane indexes to publish to the Stop control executor.
  std::vector<std::uint64_t> lanes{};
  /// First `fence_completion_count` valid; at most `N_max`.
  std::vector<StopEngineFenceCompletion> fence_completions{};
  std::uint16_t fence_completion_count{};
  /// A hold fact changed: raise the event-stream change signal once.
  bool fence_changed{};
  /// A new (not replayed) event was recorded: call the Stop listener.
  bool accepted{};
  /// PD-2: interrupt every component action, or those whose bit is set.
  bool interrupt_all_components{};
  std::vector<std::uint64_t> interrupt_components{};
  std::shared_ptr<const StopComponentTable> component_table{};
  /// Stop-lane journal records were posted under the lock: wake the journal
  /// writer after the unlock.
  bool journal_pending{};
};

/// The component actions a Stop interrupts (PD-2), passed to the listener
/// after the unlock. Bit i names `(*table)[i]`.
struct StopComponentInterrupts final {
  bool all{};
  std::span<const std::uint64_t> bits{};
  const StopComponentTable* table{};
};

/// The execution host's Stop listener: interrupts component actions in
/// progress (component action "Reset position" can move a robot). Called
/// after the Kernel's locks are released, once per new event. Installed once
/// by `set_stop_listener`; read through an atomic pointer with no lock.
class IStopListener {
 public:
  virtual ~IStopListener() = default;
  virtual void OnStopAccepted(const StopComponentInterrupts& interrupts) noexcept = 0;
};

// ---------------------------------------------------------------------------
// K16, K19: reads of the event ring
// ---------------------------------------------------------------------------

/// One covered participant with its outcome (filled at P4).
struct StopEventCoveredOutcomeDTO final {
  OperationNumber operation_id;
  std::uint32_t participant_ordinal{};
  bool started{};
  bool done{};
  StopPhase phase{StopPhase::kStopping};
  /// Absent when no stage contained it.
  std::optional<std::uint8_t> containing_stage_index{};
  bool escalated{};
  bool stop_path_collision{};
  /// Typed error code text (catalog row resolved); absent without error.
  std::optional<std::string> error_code{};

  friend bool operator==(const StopEventCoveredOutcomeDTO&,
                         const StopEventCoveredOutcomeDTO&) = default;
};

/// `read_stop` reply (`manyforge.execution_stop_status.v2`, K16).
struct StopEventStatusDTO final {
  StopEventId event_id{0U};
  RequestKey request_key{};
  StopKind requested{StopKind::kOperation};
  StopKind applied{StopKind::kOperation};
  StopWidenReason widen_reason{StopWidenReason::kNone};
  /// false: pending; true: complete.
  bool complete{};
  StopEventResult result{StopEventResult::kPending};
  std::vector<StopEventCoveredOutcomeDTO> covered{};
  std::int64_t accepted_at_ns{};
  std::optional<std::int64_t> completed_at_ns{};

  friend bool operator==(const StopEventStatusDTO&, const StopEventStatusDTO&) = default;
};

/// The newest completed event, for `cell_stop_action` (K19).
struct CellStopLastCompletedDTO final {
  StopEventId event_id{0U};
  StopKind applied{StopKind::kStopAll};
  StopEventResult result{StopEventResult::kPending};
  bool escalated{};
  std::int64_t completed_at_ns{};

  friend bool operator==(const CellStopLastCompletedDTO&,
                         const CellStopLastCompletedDTO&) = default;
};

/// `cell_stop_action` of the execution control state (K19). Clients detect
/// "a new Stop happened" from `last_event_id`.
struct CellStopActionProjectionDTO final {
  std::optional<StopEventId> last_event_id{};
  std::optional<CellStopLastCompletedDTO> last_completed{};

  friend bool operator==(const CellStopActionProjectionDTO&,
                         const CellStopActionProjectionDTO&) = default;
};

}  // namespace manyforge::core
