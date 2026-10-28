#pragma once

#include <manyforge_core/types/component_lifecycle_readiness.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/ports/icclc_brand_lifecycle_port_resolver.hpp>
#include <manyforge_core/ports/icclc_current_component_snapshot_source.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/icclc_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/ports/icclc_lifecycle_target_store.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/lifecycle_target.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {

/// Pure next intent toward the target from the achieved CCLC state, or
/// nullopt when satisfied. Active also satisfies a Prepared target.
[[nodiscard]] std::optional<ComponentLifecycleIntentKindDTO> NextLifecycleIntentTowardTarget(
    ComponentLifecycleStateDTO achieved, LifecycleTargetStateDTO target);

/// The intent a target names before any achieved state is known.
[[nodiscard]] ComponentLifecycleIntentKindDTO NominalLifecycleIntentForTarget(
    LifecycleTargetStateDTO target);

struct CCLCLifecycleTargetDriverConfig final {
  bool enabled{true};
  /// Re-evaluation period while a component waits for its target (not yet
  /// satisfied, no open incident) or a durable write is pending. A settled
  /// cell has no timer: adapter bad news is pushed (CP9 stream health wakes
  /// the driver).
  std::int64_t waiting_poll_period_ns{1'000'000'000LL};
  /// Status re-read period while one of the driver's transactions is open.
  std::int64_t in_flight_poll_period_ns{100'000'000LL};
  /// Readiness expectation ("slower than expected"): presentation only.
  std::int64_t readiness_expectation_ns{30'000'000'000LL};
  /// Known-failed attempts of one step before the driver stops and explains.
  std::uint32_t maximum_attempts_per_step{3U};
  /// A request rejected before acceptance is re-evaluated after a changed
  /// input, or at the latest after this bound (a cleared race leaves no
  /// observable change).
  std::int64_t rejection_retry_after_ns{10'000'000'000LL};
  /// Per-cause incident recovery policy (spec 240 profile data). Defaults to
  /// addendum 2 (every class requires the operator's Recover); a profile
  /// override replaces this table when it is wired (hook only today).
  std::vector<LifecycleIncidentRecoveryPolicyDTO> incident_recovery_policy{
      DefaultLifecycleIncidentRecoveryPolicy()};

  /// CP9 stream health of this process's adapters (optional). The driver
  /// reads verdicts per pass, reports health in the target view and clears
  /// `ack` latches on the operator's Recover; the composition wires its
  /// listener (Kernel hold + `Wake`).
  std::shared_ptr<ComponentStreamHealth> stream_health{};

  /// Optional host-owned scalar counts, read only by explicit status requests.
  /// No tail capture, lifecycle authority or health consequence; unavailable
  /// and throwing sources omit both informational fields.
  std::function<std::optional<std::pair<std::uint64_t, std::uint64_t>>(
      const ControllableComponentId&)>
      read_output_drop_counts{};

  [[nodiscard]] bool locally_valid() const noexcept {
    return waiting_poll_period_ns > 0 && in_flight_poll_period_ns > 0 &&
           readiness_expectation_ns > 0 && maximum_attempts_per_step > 0U &&
           rejection_retry_after_ns > 0;
  }
};

/// Narrow read/submit surface the driver needs from the CCLC composition.
/// Every call is the existing owner path; none holds a claim, fence or lock
/// beyond its own bounded duration.
class ICCLCLifecycleTargetDriverPorts {
 public:
  virtual ~ICCLCLifecycleTargetDriverPorts() = default;

  /// One complete follower pass on one version. Fixed ports keep their exact
  /// existing dependency set; the composed owner overrides this with one
  /// accounted context pin and a scoped bound adapter supplied to the pass.
  [[nodiscard]] virtual Result<void> WithPinnedProduct(
      const std::function<void(ICCLCLifecycleTargetDriverPorts&)>& pass) {
    pass(*this);
    return Result<void>::Ok();
  }

  [[nodiscard]] virtual Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component) = 0;
  [[nodiscard]] virtual Result<BrandLifecyclePlanCatalogDTO> LoadLifecyclePlanCatalog(
      const ResolvedLifecycleProviderOccurrenceDTO& occurrence) = 0;
  /// One read-only observation of the exact provider occurrence.
  [[nodiscard]] virtual Result<BrandLifecycleObservationDTO> Observe(
      const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
      const BrandLifecycleObservationQueryDTO& query) = 0;
  [[nodiscard]] virtual AuthoringAdmissionDTO ReadCellAdmission() = 0;
  [[nodiscard]] virtual RequestOwnerEpoch ReadRequestOwnerEpoch() = 0;
  /// CCLC currently admits new lifecycle work (startup recovery finished).
  [[nodiscard]] virtual bool AcceptingLifecycleRequests() = 0;
  /// The component's latest lifecycle transaction is not yet terminal.
  [[nodiscard]] virtual bool ComponentLifecycleSettling(
      const ControllableComponentId& component) = 0;
  /// Retirement bookkeeping only: actual owner claims/work/callbacks and
  /// this component's accepted work have settled. Missing proof is unknown.
  [[nodiscard]] virtual bool RetiredTargetWorkSettled(const ControllableComponentId&) {
    return false;
  }
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) = 0;
  /// Follower-only acceptance fences the actual target from its facts read.
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> SubmitTargetLifecycle(
      const ComponentLifecycleIntentDTO&, const ComponentLifecycleTargetExpectationDTO&) {
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        {"lifecycle_target_submission_unavailable", "target-fenced submission is unavailable", {}});
  }
  /// Exact original request observation; never admission or effect replay.
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"lifecycle_request_observation_unavailable", "request observation is unavailable", {}});
  }
  /// Only a pinned immutable product identity proves membership absence.
  [[nodiscard]] virtual std::optional<bool> ConfiguredComponentMembership(
      const ControllableComponentId&) const { return std::nullopt; }
  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) = 0;
  /// System-attributed cell containment for one incident through the
  /// Kernel's Stop ingress. Returns at acceptance; never waits for the Stop.
  [[nodiscard]] virtual Result<ComponentIncidentContainmentReceiptDTO> RequestIncidentContainment(
      const ComponentIncidentContainmentIntentDTO& intent) = 0;
  /// CP9 `fault` + `auto`: recovers the cell latch of exactly this closed
  /// incident's containment. True when nothing is left to do automatically.
  [[nodiscard]] virtual Result<bool> RecoverIncidentContainment(const std::string& incident_id) {
    static_cast<void>(incident_id);
    return Result<bool>::Err(
        {"lifecycle_incident_containment_recovery_unavailable",
         "incident containment recovery is unavailable", {}});
  }
};

/// Production ports over the composed CCLC owner and its verified sources.
class CoordinatorLifecycleTargetDriverPorts final : public ICCLCLifecycleTargetDriverPorts {
 public:
  CoordinatorLifecycleTargetDriverPorts(
      ComponentLifecycleCoordinator& coordinator,
      const ICCLCCurrentComponentSnapshotSource& snapshot_source,
      const ICCLCLifecyclePlanCatalogSource& catalog_source,
      const ICCLCBrandLifecyclePortResolver& port_resolver,
      IKernelCellAdmissionPort& kernel_cell_admission_port,
      std::function<Result<std::vector<ControllableComponentId>>()> unsettled_components =
          {}) noexcept;

  /// Composed current-context ports. One whole-pass pin supplies the verified
  /// sources and is reused by admission; the fixed-reference overload remains.
  CoordinatorLifecycleTargetDriverPorts(
      ComponentLifecycleCoordinator& coordinator,
      IKernelCellAdmissionPort& kernel_cell_admission_port,
      std::function<Result<std::vector<ControllableComponentId>>()> unsettled_components =
          {}) noexcept;

  [[nodiscard]] Result<void> WithPinnedProduct(
      const std::function<void(ICCLCLifecycleTargetDriverPorts&)>& pass) override;

  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component) override;
  [[nodiscard]] Result<BrandLifecyclePlanCatalogDTO> LoadLifecyclePlanCatalog(
      const ResolvedLifecycleProviderOccurrenceDTO& occurrence) override;
  [[nodiscard]] Result<BrandLifecycleObservationDTO> Observe(
      const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
      const BrandLifecycleObservationQueryDTO& query) override;
  [[nodiscard]] AuthoringAdmissionDTO ReadCellAdmission() override;
  [[nodiscard]] RequestOwnerEpoch ReadRequestOwnerEpoch() override;
  [[nodiscard]] bool AcceptingLifecycleRequests() override;
  [[nodiscard]] bool ComponentLifecycleSettling(const ControllableComponentId& component) override;
  [[nodiscard]] bool RetiredTargetWorkSettled(const ControllableComponentId& component) override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitTargetLifecycle(
      const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleTargetExpectationDTO& expected_target) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] std::optional<bool> ConfiguredComponentMembership(
      const ControllableComponentId& component) const override;
  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) override;
  [[nodiscard]] Result<ComponentIncidentContainmentReceiptDTO> RequestIncidentContainment(
      const ComponentIncidentContainmentIntentDTO& intent) override;
  [[nodiscard]] Result<bool> RecoverIncidentContainment(const std::string& incident_id) override;

 private:
  ComponentLifecycleCoordinator& coordinator_;
  const ICCLCCurrentComponentSnapshotSource* snapshot_source_{};
  const ICCLCLifecyclePlanCatalogSource* catalog_source_{};
  const ICCLCBrandLifecyclePortResolver* port_resolver_{};
  const ComponentLifecycleProductPin* product_pin_{};
  IKernelCellAdmissionPort& kernel_cell_admission_port_;
  std::function<Result<std::vector<ControllableComponentId>>()> unsettled_components_;
};

/// Deterministic request identity of one system-attributed step:
/// (component, target_generation, step, intent, attempt). Long components are
/// digested so the identity stays within the bounded request-id size.
[[nodiscard]] RequestId LifecycleTargetRequestId(const ControllableComponentId& component,
                                                 std::uint64_t target_generation,
                                                 std::uint32_t step,
                                                 ComponentLifecycleIntentKindDTO intent,
                                                 std::uint32_t attempt);

/// Private coordination of an already-authorized Program Reset effect. These
/// values are neither lifecycle intents nor persisted/public operations.
enum class CCLCProgramResetPositionPhase {
  kSwitchingToWriter,
  kSettingState,
  kSwitchingToJtc,
};

struct CCLCProgramResetPositionScopeRequest final {
  RequestOwnerEpoch runner_owner_epoch;
  std::uint64_t cycle_generation{};
  RequestOwnerEpoch request_owner_epoch;
  RequestId request_id;
  ContentDigest request_payload_digest;
  ActiveBundlePublicationReceiptDTO expected_publication;
  ControllableComponentId component;
  ResolvedLifecycleProviderOccurrenceDTO provider;
  ResolvedLifecycleControlDomainScopeDTO domain;
};

/// A deadline is the current SDK switch/readback step's existing deadline on
/// the same owner clock. nullopt validates without arming or extending a step.
using CCLCProgramResetPositionAdvance = std::function<Result<void>(
    CCLCProgramResetPositionPhase, std::optional<TimePoint>)>;
using CCLCProgramResetPositionAction =
    std::function<Result<void>(const CCLCProgramResetPositionAdvance&)>;

/// CCLC-owned requested-target follower (ADR-0026 §7, 2026-10-08).
///
/// Incidents (addendum 2): once a component reached its target on fresh
/// status in the session (the durable `reached` marker, which survives an
/// in-session owner replacement and a retaining Apply), a lost/stale
/// observation or `availability` becomes a `provider_lost` incident and any
/// intervention class an incident of that class. The driver requests the
/// Kernel's cell containment once per incident before and independently of
/// recording it, records it durably (retrying a failed record on later passes
/// while progress stays blocked), and, while it is open, appends any further
/// unrequested change (the most recent cause becomes its cause). Only the
/// operator's Recover closes it; then the policy-permitted steps run.
///
/// Storage fails closed. Each pass reads its facts (target, retained causes,
/// open incident, recovery generation) in one snapshot; unreadable facts
/// block the component. A cause, reached marker or incident whose record
/// fails is kept in memory and retried every pass: the cause keeps blocking,
/// the unrecorded marker classifies the component as reached. Every such
/// write names the recovery generation it was based on, and the journal
/// refuses it once an operator Recover (or cancellation) advanced it, so a
/// pass never re-retains, reopens or re-marks what a concurrent Recover
/// cleared; the next pass re-evaluates on the Recover's facts.
///
/// Requested replacement: while a recorded, not yet adopted Apply plan
/// replaces or removes the component, the component is starting over and
/// opens no incident: its loss is `availability` waiting and a reported
/// intervention class stays retained and blocking until the operator's
/// Recover. Adoption resets the durable `reached` marker of a replaced
/// component (a fresh startup phase); a component the Apply retains unchanged
/// keeps it, and with it the incident rules. Only the Apply facts end the
/// window, never a timer or an observation.
///
/// From the achieved CCLC state and the brand plan catalog it computes the
/// next intent toward each component's requested target, waits until that
/// intent's own plan is eligible on a fresh observation and the blocking
/// cause permits it, then submits the ordinary CCLC lifecycle transaction with
/// system attribution. An uncertain outcome is left to CCLC reconciliation; a
/// step is retried only after a known terminal failure and a newer
/// observation. Waiting holds no claim, fence or lock, and a pass in which
/// nothing changed submits and writes nothing.
///
/// Evaluation runs on one owned worker thread. Wakeups are event-driven
/// (`Wake`: operator request, stream-health verdict, target change). A
/// scheduler-timed re-check runs only while a transaction is in flight or a
/// component still waits; a settled cell is not polled. `RunOnce` performs
/// one pass inline for deterministic tests.
class CCLCLifecycleTargetDriver final {
 public:
  CCLCLifecycleTargetDriver(CCLCLifecycleTargetDriverConfig config,
                            ICCLCLifecycleTargetStore& store,
                            ICCLCLifecycleTargetDriverPorts& ports, IClock& clock,
                            IScheduler& scheduler);
  ~CCLCLifecycleTargetDriver();

  CCLCLifecycleTargetDriver(const CCLCLifecycleTargetDriver&) = delete;
  CCLCLifecycleTargetDriver& operator=(const CCLCLifecycleTargetDriver&) = delete;

  /// Start the worker (first pass at once). A disabled driver starts
  /// nothing and only answers target reads.
  [[nodiscard]] Result<void> Start();
  /// Stop the worker and cancel the re-check timer. Idempotent.
  void Stop() noexcept;

  /// One synchronous evaluation pass over every present target.
  void RunOnce();

  /// Event-driven wakeup; never evaluates on the caller's thread.
  void Wake() noexcept;

  /// Durable target plus owner-local progress for one component.
  [[nodiscard]] Result<ComponentLifecycleTargetViewDTO> ReadTargetView(
      const ControllableComponentId& component);

  /// Side-effect-free current retained facts, including a no-target incident
  /// and unrecorded owner causes. Never retries writes or evaluates a target.
  [[nodiscard]] Result<ComponentLifecycleReadinessTargetFactsDTO> ReadReadinessFacts(
      const ControllableComponentId& component);

  /// Recovery guard across all component incidents in the same journal/owner
  /// (134), including removed/unrecorded components. Pure read; no
  /// configured-product subset, fabricated generation or new owner.
  [[nodiscard]] Result<void> CheckNoOpenIncidents();

  /// The operator's Recover of an open incident (contract item 3): an
  /// accepted, authorization-only result that closes the incident, clears the
  /// retained cause and wakes the driver; admissible while the cell is
  /// latched because it dispatches nothing. The journal applies the ordinary
  /// request identity, owner-epoch and replay checks. `nullopt` when the
  /// intent is not an operator Recover of an open (or this request's
  /// recovered) incident, so the ordinary CCLC admission decides it.
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> RecoverIncident(
      const ComponentLifecycleIntentDTO& intent);
  /// Durable incident history events for the historian pipe.
  [[nodiscard]] Result<std::vector<ComponentIncidentEventDTO>> LoadIncidentEvents(
      std::uint64_t after_sequence, std::size_t limit);

  /// Synchronous private delegation. Begin/end serialize with RunOnce, but no
  /// driver lock spans the effect action or its ROS waits. The caller retains
  /// its original Kernel Reset authorization and exact installed product pin.
  /// Before returning Ok, the action joins acknowledged return activation to
  /// ordinary current CM/action health. This scope requires no pose dwell.
  [[nodiscard]] Result<void> WithProgramResetPositionScope(
      const CCLCProgramResetPositionScopeRequest& request,
      const CCLCProgramResetPositionAction& action);

  [[nodiscard]] const CCLCLifecycleTargetDriverConfig& config() const noexcept { return config_; }

 private:
  struct ProgramResetPositionScope final {
    CCLCProgramResetPositionScopeRequest request;
    std::uint64_t target_generation;
    CCLCProgramResetPositionPhase phase{CCLCProgramResetPositionPhase::kSwitchingToWriter};
    std::optional<TimePoint> step_deadline_ns{};
    bool valid{true};
  };
  [[nodiscard]] bool MatchesProgramResetPositionScope(
      const ProgramResetPositionScope& scope, const ComponentLifecycleTargetDTO& target,
      const ComponentLifecycleCurrentSnapshotDTO& current) const;
  [[nodiscard]] Result<void> ValidateProgramResetPositionScope(
      ICCLCLifecycleTargetDriverPorts& ports, const ProgramResetPositionScope& scope);

  struct InFlight final {
    LifecycleTransactionId lifecycle_transaction_id;
    ComponentLifecycleIntentKindDTO intent;
    ComponentLifecycleTargetExpectationDTO expected_target;
  };
  /// Inputs a known failure or rejection was based on. The same step is not
  /// tried again until one of them changes (or the target does).
  struct AttemptBasis final {
    std::vector<std::uint64_t> observation_generations;
    std::uint64_t admission_generation{};
    std::string snapshot_digest;

    friend bool operator==(const AttemptBasis&, const AttemptBasis&) = default;
  };
  /// A freshly observed intervention class whose durable record failed. It
  /// blocks from memory as if retained and is re-recorded with the recovery
  /// generation it was observed under until that succeeds, or until a stale
  /// refusal shows the operator's Recover (or a cancellation) covers it.
  struct UnrecordedCause final {
    LifecycleBlockingCauseClassDTO cause_class{LifecycleBlockingCauseClassDTO::kUnknown};
    std::int64_t observed_at_ns{};
    std::uint64_t recovery_generation{};
  };
  struct ComponentState final {
    std::uint64_t target_generation{};
    /// Requested state of the target last read; names the next intent while
    /// the targets cannot be read.
    std::optional<LifecycleTargetStateDTO> requested_state;
    std::uint32_t step{};
    /// Request-identity attempt; advances after every consumed request id.
    std::uint32_t attempt{1U};
    /// Known terminal failures of the current step.
    std::uint32_t failures{};
    std::optional<InFlight> in_flight;
    /// Exact unknown request plus its original target fence. Retired requests
    /// are observed only; current requests retain ordinary exact replay.
    std::optional<ComponentLifecycleIntentDTO> pending_intent;
    std::optional<ComponentLifecycleTargetExpectationDTO> pending_target;
    AttemptBasis attempt_basis;
    std::optional<AttemptBasis> failed_basis;
    std::optional<std::string> failure_reason;
    /// Set when the gate stems from a rejection before acceptance.
    std::optional<std::int64_t> rejected_at_ns;
    std::optional<std::int64_t> waiting_since_ns;
    std::optional<ComponentLifecycleProgressDTO> progress;
    /// Last fresh observation without a cause, per domain (incident record).
    std::map<ControlDomainId, ComponentIncidentObservationDTO> last_good;
    /// A detected incident whose containment was requested but whose durable
    /// record failed; recording is retried with the same identity and the
    /// recovery generation of the facts it was detected on.
    std::optional<ComponentLifecycleIncidentDTO> unrecorded_incident;
    std::uint64_t unrecorded_incident_generation{};
    /// Recovery generation of the facts the current pass evaluates; every
    /// durable write of the pass is conditional on it.
    std::uint64_t recovery_generation{};
    /// Intervention classes held only in memory (their record failed), per
    /// domain, and the latest record failure that explains the block.
    std::map<ControlDomainId, UnrecordedCause> unrecorded_causes;
    std::optional<std::string> cause_record_error;
    /// The component reached its present target but the durable marker
    /// failed: the recovery generation it was reached under. Until it is
    /// durable the component is classified as reached (fail closed).
    std::optional<std::uint64_t> unrecorded_reached;
    /// Positive target-fence refusal of the original marker; owner-local
    /// presentation only, retained until the other obligations settle.
    bool retired_reached_observation{};
    /// CP9 stream verdict read at the start of this pass.
    ComponentStreamVerdictDTO streams;
    /// Recovery generation under which this driver's last Recover of the
    /// current target settled: a still-observed recovered cause gets one
    /// driver Recover per operator Recover (ADR-0026 §7).
    std::optional<std::uint64_t> recover_settled_generation;
    /// m5.5: the Kernel's authoring admission read when this external-move
    /// episode was decided (no lifecycle work of the component in flight).
    /// Kept while the component's own re-arm holds the cell's lifecycle
    /// claim; cleared once no domain reports `device.moved_externally`.
    std::optional<AuthoringAdmissionDTO> external_move_decision;
  };
  /// The Kernel's answer to one incident's containment request.
  struct ContainmentOutcome final {
    std::optional<ComponentIncidentContainmentReceiptDTO> receipt;
    std::optional<std::string> error;
  };
  struct DomainCondition;
  /// What this pass may open a new incident for.
  enum class IncidentDetection : std::uint8_t {
    kNone = 0,
    /// Recovering after the operator's Recover: only a fresh report of an
    /// intervention class other than the recovered one.
    kNewCause,
    /// At the reached target: any loss, stale status or intervention class.
    kAny,
  };

  void RunOncePinned(ICCLCLifecycleTargetDriverPorts& ports);

  /// Follows original work/evidence before replacing or dropping its target.
  bool EvaluateComponent(ICCLCLifecycleTargetDriverPorts& ports,
                         const ControllableComponentId& component, ComponentState& state,
                         std::int64_t now_ns);
  /// Follows a pending or open request. True when this pass must stop.
  bool FollowInFlight(ICCLCLifecycleTargetDriverPorts& ports,
                      const std::optional<ComponentLifecycleTargetDTO>& target, ComponentState& state,
                      std::int64_t now_ns);
  void SubmitIntent(ICCLCLifecycleTargetDriverPorts& ports, ComponentState& state,
                    const ComponentLifecycleIntentDTO& intent,
                    const ComponentLifecycleTargetExpectationDTO& expected_target, std::int64_t now_ns);
  void ConsumeSubmissionResult(ComponentState& state, const ComponentLifecycleIntentDTO& intent,
                    const ComponentLifecycleTargetExpectationDTO& expected_target,
                    const CCLCLifecycleRequestResultDTO& result, std::int64_t now_ns);
  void Publish(ComponentState& state, LifecycleProgressPhaseDTO phase,
               std::optional<ComponentLifecycleIntentKindDTO> next_intent,
               LifecycleBlockingCauseClassDTO cause, std::optional<std::string> reason,
               std::int64_t now_ns) const;
  /// Opens, extends or follows the component's incident from this pass's
  /// observations. True when this pass published progress and must stop: an
  /// incident is open or detected, or a replaced component's loss is waiting.
  bool EvaluateIncident(
      ICCLCLifecycleTargetDriverPorts& ports, const ComponentLifecycleTargetDTO& target,
      ComponentState& state, const ComponentLifecycleCurrentSnapshotDTO& current,
      const std::vector<DomainCondition>& conditions,
      std::optional<ComponentIncidentRecordDTO> open, IncidentDetection detection,
      std::optional<ComponentLifecycleIntentKindDTO> next_intent, std::int64_t now_ns,
      LifecycleBlockingCauseClassDTO unexplained = LifecycleBlockingCauseClassDTO::kNone);
  /// Requests containment of `state.unrecorded_incident` (once) and records
  /// it; a failed record keeps it for the next pass.
  void ContainAndRecordIncident(ICCLCLifecycleTargetDriverPorts& ports, ComponentState& state,
                                std::int64_t now_ns);
  /// CP9 `fault` + `auto`: the open incident was opened by a `fault` stream
  /// with `auto` recovery that is live again, and nothing else holds it.
  [[nodiscard]] bool StreamIncidentRecovered(const ControllableComponentId& component,
                                             const ComponentState& state,
                                             const ComponentIncidentRecordDTO& open);
  /// Closes that incident (recorded like a Recover by the stream itself) and
  /// queues the release of its containment latch. False when not closed.
  [[nodiscard]] bool CloseRecoveredStreamIncident(ICCLCLifecycleTargetDriverPorts& ports,
                                                  const ComponentLifecycleCurrentSnapshotDTO& current,
                                                  const ComponentIncidentRecordDTO& open,
                                                  std::int64_t now_ns);
  /// Outside the pass (the Kernel's recovery reads CheckNoOpenIncidents):
  /// recovers the latches of incidents closed by their stream.
  void RecoverClosedIncidentContainments();
  void RunPass();
  void PublishIncident(ComponentState& state, const ComponentLifecycleIncidentDTO& incident,
                       std::int64_t now_ns,
                       const std::optional<ErrorDTO>& diagnostic = std::nullopt,
                       bool diagnostic_loaded = true) const;
  /// The Kernel's containment of one incident, requested at most once per
  /// incident by this driver; a later call returns the remembered outcome.
  const ContainmentOutcome& Contain(ICCLCLifecycleTargetDriverPorts& ports,
                                    const ComponentLifecycleIncidentDTO& incident);
  /// Writes the containment outcome into a still-pending incident record.
  void RecordContainment(ICCLCLifecycleTargetDriverPorts& ports,
                         const ComponentIncidentRecordDTO& record, std::int64_t now_ns);
  /// The current unadopted Apply replaces or removes the component, so it is
  /// starting over. An unreadable fact explains nothing.
  [[nodiscard]] bool ApplyReplacesComponent(const ControllableComponentId& component);
  [[nodiscard]] std::vector<DomainCondition> ObserveAllDomains(
      ICCLCLifecycleTargetDriverPorts& ports, const ComponentLifecycleTargetDTO& target,
      ComponentState& state, const ComponentLifecycleCurrentSnapshotDTO& current,
      const std::map<ControlDomainId, LifecycleBlockingCauseClassDTO>& retained,
      bool record_causes);
  [[nodiscard]] Result<bool> WriteInterventionCause(const ControllableComponentId& component,
                                                    const ControlDomainId& domain,
                                                    const UnrecordedCause& cause);
  /// Records a freshly observed intervention class under this pass's
  /// recovery generation; a failed record keeps it in `unrecorded_causes`.
  void RetainCause(ComponentState& state, const ControllableComponentId& component,
                   const ControlDomainId& domain, LifecycleBlockingCauseClassDTO cause,
                   std::int64_t observed_at_ns);
  /// Re-records every unrecorded cause and adds each one not covered by a
  /// Recover to `retained` for this pass.
  void RetryUnrecordedCauses(ComponentState& state, const ControllableComponentId& component,
                             std::map<ControlDomainId, LifecycleBlockingCauseClassDTO>& retained);
  [[nodiscard]] Result<bool> WriteReached(const ComponentLifecycleTargetExpectationDTO& expected_target,
                                          std::uint64_t recovery_generation);
  /// Arms the re-check timer, or cancels it when `delay_ns` is absent.
  void ScheduleRecheck(std::optional<std::int64_t> delay_ns);
  /// True while the component's state can progress without an event
  /// (waiting for its target, or a durable write to retry).
  [[nodiscard]] static bool Waiting(const ComponentState& state) noexcept;
  void WorkerLoop();

  struct WakeState final {
    std::mutex mutex;
    std::condition_variable condition;
    bool requested{};
    bool stopping{};
  };

  CCLCLifecycleTargetDriverConfig config_;
  ICCLCLifecycleTargetStore& store_;
  ICCLCLifecycleTargetDriverPorts& ports_;
  IClock& clock_;
  IScheduler& scheduler_;

  std::mutex run_mutex_;
  mutable std::mutex state_mutex_;
  std::map<ControllableComponentId, ComponentState> components_;
  bool any_in_flight_{};
  bool any_waiting_{};
  // Protected by run_mutex_ for use/mutation, state_mutex_ for readiness reads.
  std::shared_ptr<ProgramResetPositionScope> program_reset_position_scope_;

  /// Containment outcomes of incidents this driver already contained (run
  /// lane), so neither a retried record nor a later pass requests it again.
  std::map<std::string, ContainmentOutcome> containments_;
  /// Incidents closed by their `fault` + `auto` stream whose containment
  /// latch is not yet recovered (state_mutex_).
  std::set<std::string> containment_recoveries_;

  std::shared_ptr<WakeState> wake_;
  std::mutex recheck_mutex_;
  std::optional<ScheduleHandle> recheck_handle_;
  std::thread worker_;
};

}  // namespace manyforge::core
