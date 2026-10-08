#pragma once

#include <condition_variable>
#include <functional>
#include <cstdint>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
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
#include <set>
#include <string>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace manyforge::core {

struct CCLCLifecycleTargetDriverConfig final {
  bool enabled{true};
  /// Low-rate fallback re-evaluation when no event arrives.
  std::int64_t fallback_period_ns{1'000'000'000LL};
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

  [[nodiscard]] bool locally_valid() const noexcept {
    return fallback_period_ns > 0 && in_flight_poll_period_ns > 0 &&
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
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) = 0;
  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) = 0;
  /// System-attributed cell containment for one incident through the
  /// Kernel's Stop ingress. Returns at acceptance; never waits for the Stop.
  [[nodiscard]] virtual Result<ComponentIncidentContainmentReceiptDTO> RequestIncidentContainment(
      const ComponentIncidentContainmentIntentDTO& intent) = 0;
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
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) override;
  [[nodiscard]] Result<ComponentIncidentContainmentReceiptDTO> RequestIncidentContainment(
      const ComponentIncidentContainmentIntentDTO& intent) override;

 private:
  ComponentLifecycleCoordinator& coordinator_;
  const ICCLCCurrentComponentSnapshotSource& snapshot_source_;
  const ICCLCLifecyclePlanCatalogSource& catalog_source_;
  const ICCLCBrandLifecyclePortResolver& port_resolver_;
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

/// CCLC-owned requested-target follower (ADR-0026 §7, 2026-10-08).
///
/// Incidents (addendum 2): once a component reached its target on fresh
/// status in the session, a lost/stale observation or `availability` becomes
/// a `provider_lost` incident and any intervention class an incident of that
/// class. The driver opens it once (durably), requests the Kernel's cell
/// containment once, blocks all progress and, while it is open, appends any
/// further unrequested change (the most recent cause becomes its cause). Only
/// the operator's Recover closes it; then the policy-permitted steps run.
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
/// (`Wake`: observation publication, transaction terminal, target change)
/// with a scheduler-timed fallback; `RunOnce` performs one pass inline for
/// deterministic tests.
class CCLCLifecycleTargetDriver final {
 public:
  CCLCLifecycleTargetDriver(CCLCLifecycleTargetDriverConfig config,
                            ICCLCLifecycleTargetStore& store,
                            ICCLCLifecycleTargetDriverPorts& ports, IClock& clock,
                            IScheduler& scheduler);
  ~CCLCLifecycleTargetDriver();

  CCLCLifecycleTargetDriver(const CCLCLifecycleTargetDriver&) = delete;
  CCLCLifecycleTargetDriver& operator=(const CCLCLifecycleTargetDriver&) = delete;

  /// Start the worker and the fallback timer. A disabled driver starts
  /// nothing and only answers target reads.
  [[nodiscard]] Result<void> Start();
  /// Stop the worker and cancel the fallback timer. Idempotent.
  void Stop() noexcept;

  /// One synchronous evaluation pass over every present target.
  void RunOnce();

  /// Event-driven wakeup; never evaluates on the caller's thread.
  void Wake() noexcept;

  /// Operator Prepare/Enable/Disable set the target (origin operator); a
  /// Disable also cancels outstanding progression. Recover and
  /// system-attributed requests change nothing and return `nullopt`.
  [[nodiscard]] Result<std::optional<ComponentLifecycleTargetDTO>> RecordOperatorRequest(
      const ComponentLifecycleIntentDTO& intent);

  /// Durable target plus owner-local progress for one component.
  [[nodiscard]] Result<ComponentLifecycleTargetViewDTO> ReadTargetView(
      const ControllableComponentId& component);

  /// The operator's Recover of an open incident (contract item 3): an
  /// accepted, authorization-only result that closes the incident, clears the
  /// retained cause and wakes the driver; admissible while the cell is
  /// latched because it dispatches nothing. An exact retransmission replays
  /// the recorded result. `nullopt` when the intent is not an operator
  /// Recover of an open (or this request's recovered) incident, so the
  /// ordinary CCLC admission decides it.
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> RecoverIncident(
      const ComponentLifecycleIntentDTO& intent);
  /// Read-only form of RecoverIncident for request-result reads.
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadIncidentRecovery(
      const ComponentLifecycleIntentDTO& intent);
  /// Durable incident history events for the historian pipe.
  [[nodiscard]] Result<std::vector<ComponentIncidentEventDTO>> LoadIncidentEvents(
      std::uint64_t after_sequence, std::size_t limit);

  [[nodiscard]] const CCLCLifecycleTargetDriverConfig& config() const noexcept { return config_; }

 private:
  struct InFlight final {
    LifecycleTransactionId lifecycle_transaction_id;
    ComponentLifecycleIntentKindDTO intent;
    std::uint64_t target_generation{};
  };
  /// Inputs a known failure or rejection was based on. The same step is not
  /// tried again until one of them changes (or the target does).
  struct AttemptBasis final {
    std::vector<std::uint64_t> observation_generations;
    std::uint64_t admission_generation{};
    std::string snapshot_digest;

    friend bool operator==(const AttemptBasis&, const AttemptBasis&) = default;
  };
  struct ComponentState final {
    std::uint64_t target_generation{};
    std::uint32_t step{};
    /// Request-identity attempt; advances after every consumed request id.
    std::uint32_t attempt{1U};
    /// Known terminal failures of the current step.
    std::uint32_t failures{};
    std::optional<InFlight> in_flight;
    /// Exact intent whose submission outcome is unknown; re-sent unchanged
    /// (exact replay) before any other request for this component.
    std::optional<ComponentLifecycleIntentDTO> pending_intent;
    AttemptBasis attempt_basis;
    std::optional<AttemptBasis> failed_basis;
    std::optional<std::string> failure_reason;
    /// Set when the gate stems from a rejection before acceptance.
    std::optional<std::int64_t> rejected_at_ns;
    std::optional<std::int64_t> waiting_since_ns;
    std::optional<ComponentLifecycleProgressDTO> progress;
    /// Last fresh observation without a cause, per domain (incident record).
    std::map<ControlDomainId, ComponentIncidentObservationDTO> last_good;
    /// This driver instance has itself observed the component fresh and
    /// cause-free. Until then (startup, in-session host replacement, driver
    /// restart with an inherited `reached`) missing status is re-establishment
    /// waiting, never `provider_lost`.
    bool observed_fresh{};
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

  void EvaluateComponent(const ComponentLifecycleTargetDTO& target, ComponentState& state,
                         std::int64_t now_ns);
  /// Follows a pending or open request. True when this pass must stop.
  bool FollowInFlight(const ComponentLifecycleTargetDTO& target, ComponentState& state,
                      std::int64_t now_ns);
  void SubmitIntent(ComponentState& state, const ComponentLifecycleIntentDTO& intent,
                    std::int64_t now_ns);
  void Publish(ComponentState& state, LifecycleProgressPhaseDTO phase,
               std::optional<ComponentLifecycleIntentKindDTO> next_intent,
               LifecycleBlockingCauseClassDTO cause, std::optional<std::string> reason,
               std::int64_t now_ns) const;
  /// Opens, extends or follows the component's incident from this pass's
  /// observations. True when an incident is open (progress was published).
  bool EvaluateIncident(const ComponentLifecycleTargetDTO& target, ComponentState& state,
                        const ComponentLifecycleCurrentSnapshotDTO& current,
                        const std::vector<DomainCondition>& conditions,
                        std::optional<ComponentIncidentRecordDTO> open,
                        IncidentDetection detection, std::int64_t now_ns,
                        LifecycleBlockingCauseClassDTO unexplained =
                            LifecycleBlockingCauseClassDTO::kNone);
  void PublishIncident(ComponentState& state, const ComponentLifecycleIncidentDTO& incident,
                       std::int64_t now_ns) const;
  void RequestContainment(const ComponentIncidentRecordDTO& record, std::int64_t now_ns);
  [[nodiscard]] std::vector<DomainCondition> ObserveAllDomains(
      const ComponentLifecycleTargetDTO& target, ComponentState& state,
      const ComponentLifecycleCurrentSnapshotDTO& current,
      const std::map<ControlDomainId, LifecycleBlockingCauseClassDTO>& retained,
      std::int64_t now_ns);
  void ScheduleFallback(std::int64_t delay_ns);
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
  /// Incidents whose containment this driver already requested (run lane).
  std::set<std::string> containment_requested_;

  std::shared_ptr<WakeState> wake_;
  std::mutex fallback_mutex_;
  std::optional<ScheduleHandle> fallback_handle_;
  std::thread worker_;
};

}  // namespace manyforge::core
