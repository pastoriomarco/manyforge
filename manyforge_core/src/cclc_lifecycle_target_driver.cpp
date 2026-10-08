#include <manyforge_core/cclc_lifecycle_target_driver.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

constexpr std::string_view kObservationProbeTransactionId = "manyforge_lifecycle_target.observation";
constexpr std::size_t kMaximumBlockingReasonBytes = 1024U;

[[nodiscard]] ErrorDTO driver_error(std::string code, std::string message, std::string reason) {
  return {std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

/// The next intent toward `target` from the achieved CCLC state, or nullopt
/// when the target is satisfied. There is no intent that lowers Active to
/// Prepared, so Active satisfies a `prepared` target.
[[nodiscard]] std::optional<ComponentLifecycleIntentKindDTO> NextIntent(
    const ComponentLifecycleStateDTO achieved, const LifecycleTargetStateDTO target) {
  using State = ComponentLifecycleStateDTO;
  using Intent = ComponentLifecycleIntentKindDTO;
  if (target == LifecycleTargetStateDTO::kDisabled) {
    return achieved == State::kDisabled ? std::nullopt : std::optional(Intent::kDisable);
  }
  switch (achieved) {
    case State::kActive:
      return std::nullopt;
    case State::kPrepared:
      return target == LifecycleTargetStateDTO::kActive ? std::optional(Intent::kEnable)
                                                        : std::nullopt;
    case State::kDegraded:
    case State::kFaulted:
      return Intent::kRecover;
    case State::kDisabling:
      return Intent::kDisable;
    case State::kUndeclared:
    case State::kDeclared:
    case State::kDisabled:
      return Intent::kPrepare;
  }
  return Intent::kPrepare;
}

/// The intent a target names before any achieved state is known.
[[nodiscard]] ComponentLifecycleIntentKindDTO NominalIntent(const LifecycleTargetStateDTO target) {
  switch (target) {
    case LifecycleTargetStateDTO::kActive:
      return ComponentLifecycleIntentKindDTO::kEnable;
    case LifecycleTargetStateDTO::kPrepared:
      return ComponentLifecycleIntentKindDTO::kPrepare;
    case LifecycleTargetStateDTO::kDisabled:
      return ComponentLifecycleIntentKindDTO::kDisable;
  }
  return ComponentLifecycleIntentKindDTO::kEnable;
}

[[nodiscard]] ComponentLifecycleStateDTO RequestedStateOf(const ComponentLifecycleIntentKindDTO intent) {
  switch (intent) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return ComponentLifecycleStateDTO::kPrepared;
    case ComponentLifecycleIntentKindDTO::kEnable:
      return ComponentLifecycleStateDTO::kActive;
    case ComponentLifecycleIntentKindDTO::kDisable:
    case ComponentLifecycleIntentKindDTO::kRecover:
      return ComponentLifecycleStateDTO::kDisabled;
  }
  return ComponentLifecycleStateDTO::kDisabled;
}

/// The adapter-reported cause class. An adapter that does not report the
/// standard fact states no cause; a malformed report is `unknown`.
[[nodiscard]] LifecycleBlockingCauseClassDTO CauseOf(const BrandLifecycleObservationDTO& observation) {
  for (const auto& fact : observation.facts) {
    if (fact.lifecycle_fact_id.value() != kLifecycleBlockingCauseFactId) continue;
    if (fact.value.kind != LifecycleFactValueKindDTO::kToken || !fact.value.token_value) {
      return LifecycleBlockingCauseClassDTO::kUnknown;
    }
    return parse_lifecycle_blocking_cause_class(*fact.value.token_value)
        .value_or(LifecycleBlockingCauseClassDTO::kUnknown);
  }
  return LifecycleBlockingCauseClassDTO::kNone;
}

[[nodiscard]] int CauseRank(const LifecycleBlockingCauseClassDTO cause) {
  if (cause == LifecycleBlockingCauseClassDTO::kNone) return 0;
  if (cause == LifecycleBlockingCauseClassDTO::kAvailability) return 1;
  return 2;
}

[[nodiscard]] LifecycleBlockingCauseClassDTO Worst(const LifecycleBlockingCauseClassDTO lhs,
                                                   const LifecycleBlockingCauseClassDTO rhs) {
  return CauseRank(rhs) > CauseRank(lhs) ? rhs : lhs;
}

[[nodiscard]] std::string Bounded(std::string reason) {
  if (reason.size() <= kMaximumBlockingReasonBytes) return reason;
  std::size_t end = kMaximumBlockingReasonBytes;
  // Never split a UTF-8 sequence.
  while (end > 0U && (static_cast<unsigned char>(reason[end]) & 0xC0U) == 0x80U) --end;
  reason.resize(end);
  return reason;
}

[[nodiscard]] std::string ReasonOf(const std::string& prefix, const ErrorDTO& error) {
  const auto detail = error.details.find("reason");
  return prefix + ":" + error.code +
         (detail != error.details.end() && !detail->second.empty() ? ":" + detail->second : "");
}

/// One read-only observation of one provider occurrence and the cause class
/// it establishes for that domain.
struct DomainObservation final {
  /// Present only when the observation is fresh for the exact occurrence.
  std::optional<BrandLifecycleObservationDTO> observation;
  LifecycleBlockingCauseClassDTO cause{LifecycleBlockingCauseClassDTO::kNone};
  /// Observation generation, or 0 when not fresh.
  std::uint64_t generation{};
  std::optional<std::string> unavailable_reason;
  /// The class the fresh observation itself reports (`none` when not fresh),
  /// before retention or the recovered-cause allowance.
  LifecycleBlockingCauseClassDTO reported{LifecycleBlockingCauseClassDTO::kNone};
};

/// Observes one occurrence and keeps the retained intervention cause of its
/// domain: fresh intervention-requiring status records it, only fresh status
/// with no cause clears it, and communication loss or a later `availability`
/// report never clears it.
[[nodiscard]] DomainObservation ObserveDomain(
    ICCLCLifecycleTargetDriverPorts& ports, ICCLCLifecycleTargetStore& store,
    const ControllableComponentId& component,
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
    const std::uint64_t target_generation, const LifecycleBlockingCauseClassDTO retained_cause,
    const std::optional<LifecycleBlockingCauseClassDTO> recovered_cause, IClock& clock) {
  const BrandLifecycleObservationQueryDTO query{
      LifecycleTransactionId(std::string(kObservationProbeTransactionId)),
      LifecycleTransactionGeneration(target_generation),
      component,
      occurrence.control_domain_id,
      occurrence.control_domain_revision,
      occurrence.provider_instance_id,
      occurrence.provider_manifest_id,
      occurrence.provider_manifest_revision,
      occurrence.provider_runtime_unit_id,
      occurrence.provider_runtime_epoch};
  Result<BrandLifecycleObservationDTO> observed = Result<BrandLifecycleObservationDTO>::Err(
      driver_error("lifecycle_target_observation_failed", "observation raised an exception",
                   "observe_exception"));
  try {
    observed = ports.Observe(occurrence, query);
  } catch (...) {
  }
  // Judge freshness after the observation returns: the adapter stamps it on
  // the same owner clock while Observe runs, so a pass-start time would make
  // every real observation look as if it came from the future.
  const std::int64_t now_ns = clock.now();
  const bool fresh = observed.ok() && observed.value().locally_valid() &&
                     observed.value().matches(query) &&
                     observed.value().received_timestamp_ns <= now_ns &&
                     now_ns < observed.value().fresh_until_ns;
  DomainObservation result;
  if (!fresh) {
    result.unavailable_reason = observed.ok()
                                    ? std::string("observation_not_fresh")
                                    : ReasonOf("observation_unavailable", observed.error());
    // Communication loss never clears an intervention-requiring class.
    result.cause = CauseRank(retained_cause) == 2 ? retained_cause
                                                  : LifecycleBlockingCauseClassDTO::kAvailability;
    return result;
  }
  result.generation = observed.value().observation_generation;
  auto domain_cause = CauseOf(observed.value());
  result.reported = domain_cause;
  if (recovered_cause && domain_cause == *recovered_cause) {
    // The operator's Recover already covers this cause: an adapter that keeps
    // reporting it until the next recovery step neither blocks that step nor
    // is retained again. Plan eligibility on this fresh status still decides.
    result.cause = LifecycleBlockingCauseClassDTO::kNone;
    result.observation = std::move(observed.value());
    return result;
  }
  if (lifecycle_cause_requires_intervention(domain_cause)) {
    static_cast<void>(store.RecordInterventionCause(component, occurrence.control_domain_id,
                                                    domain_cause, now_ns));
  } else if (domain_cause == LifecycleBlockingCauseClassDTO::kNone) {
    // Only fresh status with no cause clears a retained one; a later
    // `availability` report does not.
    if (retained_cause != LifecycleBlockingCauseClassDTO::kNone) {
      static_cast<void>(store.RecordInterventionCause(component, occurrence.control_domain_id,
                                                      std::nullopt, now_ns));
    }
  } else if (CauseRank(retained_cause) == 2) {
    domain_cause = retained_cause;
  }
  result.cause = domain_cause;
  result.observation = std::move(observed.value());
  return result;
}

/// Deterministic, bounded incident identity: component and detection time.
[[nodiscard]] std::string IncidentId(const ControllableComponentId& component,
                                     const std::int64_t detected_at_ns) {
  std::string readable = "incident." + component.value() + "." + std::to_string(detected_at_ns);
  if (readable.size() <= kMaxComponentIncidentIdBytes) return readable;
  return "incident." + content_identity::sha256_digest(readable).substr(0U, 71U);
}

[[nodiscard]] std::string_view IncidentDescription(const LifecycleBlockingCauseClassDTO cause) {
  switch (cause) {
    case LifecycleBlockingCauseClassDTO::kProviderLost:
      return "lost its provider or controller after reaching its target";
    case LifecycleBlockingCauseClassDTO::kEmergencyStop:
      return "reported an emergency stop";
    case LifecycleBlockingCauseClassDTO::kProtectiveStop:
      return "reported a protective stop";
    case LifecycleBlockingCauseClassDTO::kSafeguardStop:
      return "reported a safeguard stop";
    case LifecycleBlockingCauseClassDTO::kFault:
      return "reported a controller fault";
    case LifecycleBlockingCauseClassDTO::kCollision:
      return "reported a collision stop";
    case LifecycleBlockingCauseClassDTO::kExternalControl:
      return "was changed from the robot's own interface without a ManyForge request";
    case LifecycleBlockingCauseClassDTO::kConfiguration:
      return "reported an unexpected configuration change";
    case LifecycleBlockingCauseClassDTO::kUnknown:
    case LifecycleBlockingCauseClassDTO::kNone:
    case LifecycleBlockingCauseClassDTO::kAvailability:
      break;
  }
  return "reported an unclassified stop or fault";
}

[[nodiscard]] std::string IncidentSummary(const ControllableComponentId& component,
                                          const LifecycleBlockingCauseClassDTO cause,
                                          const std::vector<std::string>& vendor_codes) {
  std::string summary = "Component " + component.value() + " " +
                        std::string(IncidentDescription(cause)) + ".";
  if (!vendor_codes.empty()) {
    summary += " Vendor codes:";
    for (const auto& code : vendor_codes) summary += " " + code;
    summary += ".";
  }
  return Bounded(std::move(summary));
}

}  // namespace

struct CCLCLifecycleTargetDriver::DomainCondition final {
  ControlDomainId control_domain_id;
  DomainObservation observed;
};

RequestId LifecycleTargetRequestId(const ControllableComponentId& component,
                                   const std::uint64_t target_generation, const std::uint32_t step,
                                   const ComponentLifecycleIntentKindDTO intent,
                                   const std::uint32_t attempt) {
  const std::string suffix = ".g" + std::to_string(target_generation) + ".s" +
                             std::to_string(step) + "." + std::string(to_string(intent)) + ".a" +
                             std::to_string(attempt);
  std::string readable =
      std::string(kLifecycleTargetSourceLabel) + "." + component.value() + suffix;
  if (readable.size() <= kMaxLifecycleIdentityBytes) return RequestId(std::move(readable));
  return RequestId(std::string(kLifecycleTargetSourceLabel) + "." +
                   content_identity::sha256_digest(readable).substr(0U, 71U));
}

CoordinatorLifecycleTargetDriverPorts::CoordinatorLifecycleTargetDriverPorts(
    ComponentLifecycleCoordinator& coordinator,
    const ICCLCCurrentComponentSnapshotSource& snapshot_source,
    const ICCLCLifecyclePlanCatalogSource& catalog_source,
    const ICCLCBrandLifecyclePortResolver& port_resolver,
    IKernelCellAdmissionPort& kernel_cell_admission_port,
    std::function<Result<std::vector<ControllableComponentId>>()> unsettled_components) noexcept
    : coordinator_(coordinator),
      snapshot_source_(snapshot_source),
      catalog_source_(catalog_source),
      port_resolver_(port_resolver),
      kernel_cell_admission_port_(kernel_cell_admission_port),
      unsettled_components_(std::move(unsettled_components)) {}

bool CoordinatorLifecycleTargetDriverPorts::AcceptingLifecycleRequests() {
  return coordinator_.accepting_new_requests();
}

bool CoordinatorLifecycleTargetDriverPorts::ComponentLifecycleSettling(
    const ControllableComponentId& component) {
  if (!unsettled_components_) return false;
  const auto unsettled = unsettled_components_();
  // An unreadable journal is treated as settling: nothing is submitted.
  return !unsettled.ok() || std::find(unsettled.value().begin(), unsettled.value().end(),
                                      component) != unsettled.value().end();
}

Result<ComponentLifecycleCurrentSnapshotDTO>
CoordinatorLifecycleTargetDriverPorts::ReadCurrentComponent(
    const ControllableComponentId& component) {
  auto snapshot = snapshot_source_.ReadCurrentComponentSnapshot(component);
  if (!snapshot.ok()) return snapshot;
  const auto verified = VerifyComponentLifecycleCurrentSnapshot(snapshot.value());
  if (!verified.ok()) return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(verified.error());
  return snapshot;
}

Result<BrandLifecyclePlanCatalogDTO> CoordinatorLifecycleTargetDriverPorts::LoadLifecyclePlanCatalog(
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence) {
  return catalog_source_.LoadExactLifecyclePlanCatalog(occurrence.provider_manifest_id,
                                                       occurrence.provider_manifest_revision,
                                                       occurrence.lifecycle_plan_catalog_digest);
}

Result<BrandLifecycleObservationDTO> CoordinatorLifecycleTargetDriverPorts::Observe(
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
    const BrandLifecycleObservationQueryDTO& query) {
  auto port = port_resolver_.ResolveExactLifecyclePort(occurrence);
  if (!port.ok()) return Result<BrandLifecycleObservationDTO>::Err(port.error());
  if (!port.value()) {
    return Result<BrandLifecycleObservationDTO>::Err(driver_error(
        "lifecycle_target_port_missing", "the resolver returned no lifecycle port",
        "brand_port_missing"));
  }
  return port.value()->Observe(query);
}

AuthoringAdmissionDTO CoordinatorLifecycleTargetDriverPorts::ReadCellAdmission() {
  return kernel_cell_admission_port_.GetAuthoringAdmission();
}

RequestOwnerEpoch CoordinatorLifecycleTargetDriverPorts::ReadRequestOwnerEpoch() {
  return coordinator_.request_owner_epoch();
}

Result<CCLCLifecycleRequestResultDTO> CoordinatorLifecycleTargetDriverPorts::SubmitLifecycle(
    const ComponentLifecycleIntentDTO& intent) {
  return coordinator_.SubmitLifecycle(intent);
}

Result<std::optional<ComponentLifecycleStatusDTO>>
CoordinatorLifecycleTargetDriverPorts::LoadLifecycleStatus(
    const LifecycleTransactionId& lifecycle_transaction_id) {
  return coordinator_.LoadLifecycleStatus(lifecycle_transaction_id);
}

Result<ComponentIncidentContainmentReceiptDTO>
CoordinatorLifecycleTargetDriverPorts::RequestIncidentContainment(
    const ComponentIncidentContainmentIntentDTO& intent) {
  // The Kernel is the composition's cell-admission port in both hosts.
  return kernel_cell_admission_port_.RequestComponentIncidentContainment(intent);
}

CCLCLifecycleTargetDriver::CCLCLifecycleTargetDriver(CCLCLifecycleTargetDriverConfig config,
                                                     ICCLCLifecycleTargetStore& store,
                                                     ICCLCLifecycleTargetDriverPorts& ports,
                                                     IClock& clock, IScheduler& scheduler)
    : config_(config),
      store_(store),
      ports_(ports),
      clock_(clock),
      scheduler_(scheduler),
      wake_(std::make_shared<WakeState>()) {}

CCLCLifecycleTargetDriver::~CCLCLifecycleTargetDriver() { Stop(); }

Result<void> CCLCLifecycleTargetDriver::Start() {
  if (!config_.locally_valid()) {
    return Result<void>::Err(driver_error("lifecycle_target_driver_invalid",
                                          "the lifecycle-target driver configuration is invalid",
                                          "config_invalid"));
  }
  if (!config_.enabled || worker_.joinable()) return Result<void>::Ok();
  {
    std::lock_guard lock(wake_->mutex);
    wake_->stopping = false;
    wake_->requested = true;
  }
  try {
    worker_ = std::thread([this] { WorkerLoop(); });
  } catch (const std::exception& exception) {
    return Result<void>::Err(driver_error("lifecycle_target_driver_start_failed", exception.what(),
                                          "worker_start_failed"));
  }
  return Result<void>::Ok();
}

void CCLCLifecycleTargetDriver::Stop() noexcept {
  {
    std::lock_guard lock(wake_->mutex);
    wake_->stopping = true;
  }
  wake_->condition.notify_all();
  if (worker_.joinable()) worker_.join();
  std::lock_guard lock(fallback_mutex_);
  if (fallback_handle_) {
    scheduler_.cancel(*fallback_handle_);
    fallback_handle_.reset();
  }
}

void CCLCLifecycleTargetDriver::Wake() noexcept {
  {
    std::lock_guard lock(wake_->mutex);
    wake_->requested = true;
  }
  wake_->condition.notify_all();
}

void CCLCLifecycleTargetDriver::ScheduleFallback(const std::int64_t delay_ns) {
  std::lock_guard lock(fallback_mutex_);
  if (fallback_handle_) scheduler_.cancel(*fallback_handle_);
  fallback_handle_.reset();
  const std::int64_t now = clock_.now();
  const std::int64_t deadline =
      now > std::numeric_limits<std::int64_t>::max() - delay_ns ? now : now + delay_ns;
  // The callback only signals the worker; it never evaluates on the
  // scheduler lane and survives the driver through the shared wake state.
  std::weak_ptr<WakeState> weak = wake_;
  fallback_handle_ = scheduler_.schedule(
      deadline,
      [weak] {
        if (const auto wake = weak.lock()) {
          {
            std::lock_guard wake_lock(wake->mutex);
            wake->requested = true;
          }
          wake->condition.notify_all();
        }
      },
      SchedulerPriority::Lifecycle);
}

void CCLCLifecycleTargetDriver::WorkerLoop() {
  for (;;) {
    {
      std::unique_lock lock(wake_->mutex);
      wake_->condition.wait(lock, [&] { return wake_->requested || wake_->stopping; });
      if (wake_->stopping) return;
      wake_->requested = false;
    }
    try {
      RunOnce();
    } catch (...) {
      // A failed pass is retried on the next event or fallback tick.
    }
    bool in_flight = false;
    {
      std::lock_guard lock(state_mutex_);
      in_flight = any_in_flight_;
    }
    try {
      ScheduleFallback(in_flight ? config_.in_flight_poll_period_ns : config_.fallback_period_ns);
    } catch (...) {
    }
  }
}

void CCLCLifecycleTargetDriver::RunOnce() {
  std::lock_guard run_lock(run_mutex_);
  const std::int64_t now = clock_.now();
  auto targets = store_.LoadLifecycleTargets();
  if (!targets.ok()) return;
  std::map<ControllableComponentId, ComponentState> states;
  {
    std::lock_guard lock(state_mutex_);
    states = components_;
  }
  std::map<ControllableComponentId, ComponentState> next;
  bool in_flight = false;
  for (const auto& target : targets.value()) {
    auto found = states.find(target.controllable_component_id);
    ComponentState state = found == states.end() ? ComponentState{} : std::move(found->second);
    EvaluateComponent(target, state, now);
    in_flight = in_flight || state.in_flight.has_value() || state.pending_intent.has_value();
    next.emplace(target.controllable_component_id, std::move(state));
  }
  std::lock_guard lock(state_mutex_);
  components_ = std::move(next);
  any_in_flight_ = in_flight;
}

void CCLCLifecycleTargetDriver::Publish(ComponentState& state, const LifecycleProgressPhaseDTO phase,
                                        std::optional<ComponentLifecycleIntentKindDTO> next_intent,
                                        const LifecycleBlockingCauseClassDTO cause,
                                        std::optional<std::string> reason,
                                        const std::int64_t now_ns) const {
  const bool settled = phase == LifecycleProgressPhaseDTO::kSatisfied ||
                       phase == LifecycleProgressPhaseDTO::kCancelled;
  if (settled) {
    state.waiting_since_ns.reset();
    next_intent.reset();
  } else if (!state.waiting_since_ns) {
    state.waiting_since_ns = now_ns;
  }
  if (reason && reason->empty()) reason.reset();
  if (phase == LifecycleProgressPhaseDTO::kBlocked && !reason) reason = "blocked";
  if (reason) reason = Bounded(std::move(*reason));
  state.progress = ComponentLifecycleProgressDTO{
      phase,
      next_intent,
      cause,
      std::move(reason),
      state.waiting_since_ns,
      state.waiting_since_ns.has_value() &&
          now_ns - *state.waiting_since_ns > config_.readiness_expectation_ns,
      std::nullopt};
}

void CCLCLifecycleTargetDriver::PublishIncident(ComponentState& state,
                                                const ComponentLifecycleIncidentDTO& incident,
                                                const std::int64_t now_ns) const {
  // Nothing proceeds while an incident is open; the operator's Recover is the
  // next intent.
  Publish(state, LifecycleProgressPhaseDTO::kBlocked, ComponentLifecycleIntentKindDTO::kRecover,
          incident.cause_class,
          "incident_open:" + std::string(to_string(incident.cause_class)), now_ns);
  state.progress->incident = incident;
}

std::vector<CCLCLifecycleTargetDriver::DomainCondition> CCLCLifecycleTargetDriver::ObserveAllDomains(
    const ComponentLifecycleTargetDTO& target, ComponentState& state,
    const ComponentLifecycleCurrentSnapshotDTO& current,
    const std::map<ControlDomainId, LifecycleBlockingCauseClassDTO>& retained,
    const std::int64_t now_ns) {
  // Each domain judges freshness on the clock read after its own observation.
  static_cast<void>(state);
  static_cast<void>(now_ns);
  std::vector<DomainCondition> conditions;
  conditions.reserve(current.provider_occurrences.size());
  for (const auto& occurrence : current.provider_occurrences) {
    const auto stored = retained.find(occurrence.control_domain_id);
    conditions.push_back(
        {occurrence.control_domain_id,
         ObserveDomain(ports_, store_, target.controllable_component_id, occurrence,
                       target.target_generation,
                       stored == retained.end() ? LifecycleBlockingCauseClassDTO::kNone
                                                : stored->second,
                       target.recovered_cause, clock_)});
  }
  return conditions;
}

void CCLCLifecycleTargetDriver::RequestContainment(const ComponentIncidentRecordDTO& record,
                                                   const std::int64_t now_ns) {
  const auto& incident = record.incident;
  // At most once per incident from this driver; a replacement driver finding
  // it still pending repeats the request, which the Kernel replays exactly.
  if (incident.containment_state != ComponentIncidentContainmentStateDTO::kPending ||
      !containment_requested_.insert(incident.incident_id).second) {
    return;
  }
  constexpr std::size_t kMaximumRememberedContainments = 1024U;
  if (containment_requested_.size() > kMaximumRememberedContainments) {
    containment_requested_ = {incident.incident_id};
  }
  Result<ComponentIncidentContainmentReceiptDTO> contained =
      Result<ComponentIncidentContainmentReceiptDTO>::Err(driver_error(
          "lifecycle_incident_containment_failed", "containment raised an exception",
          "containment_exception"));
  try {
    contained = ports_.RequestIncidentContainment(
        {incident.incident_id, incident.controllable_component_id, incident.cause_class});
  } catch (...) {
  }
  const auto state = contained.ok() ? ComponentIncidentContainmentStateDTO::kRequested
                                    : ComponentIncidentContainmentStateDTO::kFailed;
  try {
    const auto record_json = ComponentIncidentRecordWithContainment(
        record.record_json, state,
        contained.ok() ? std::optional(contained.value()) : std::nullopt,
        contained.ok() ? std::nullopt
                       : std::optional(Bounded(ReasonOf("containment_refused", contained.error()))));
    static_cast<void>(
        store_.RecordComponentIncidentContainment(incident.incident_id, state, record_json, now_ns));
  } catch (...) {
    // The open incident stays durable and blocking; only its record lacks the
    // Stop reference.
  }
}

bool CCLCLifecycleTargetDriver::EvaluateIncident(
    const ComponentLifecycleTargetDTO& target, ComponentState& state,
    const ComponentLifecycleCurrentSnapshotDTO& current,
    const std::vector<DomainCondition>& conditions, std::optional<ComponentIncidentRecordDTO> open,
    const IncidentDetection detection, const std::int64_t now_ns,
    const LifecycleBlockingCauseClassDTO unexplained) {
  // The condition this pass establishes: a fresh intervention class first,
  // then a retained one, then (only at the reached target or for an open
  // incident) a lost, stale or `availability` provider.
  auto intervention = LifecycleBlockingCauseClassDTO::kNone;
  bool intervention_fresh = false;
  bool lost = false;
  std::vector<std::string> codes;
  std::optional<ControlDomainId> trigger;
  for (const auto& condition : conditions) {
    const auto& observed = condition.observed;
    const bool fresh = observed.observation.has_value();
    if (lifecycle_cause_requires_intervention(observed.cause) &&
        (fresh ? !intervention_fresh : intervention == LifecycleBlockingCauseClassDTO::kNone)) {
      intervention = observed.cause;
      intervention_fresh = fresh;
      trigger = condition.control_domain_id;
      codes = fresh ? ComponentIncidentVendorCodes(observed.observation->facts)
                    : std::vector<std::string>{};
      continue;
    }
    if (!fresh || observed.reported == LifecycleBlockingCauseClassDTO::kAvailability) {
      if (!lost && !trigger) trigger = condition.control_domain_id;
      lost = true;
      continue;
    }
    if (observed.cause == LifecycleBlockingCauseClassDTO::kNone) {
      const auto& value = *observed.observation;
      state.last_good.insert_or_assign(
          condition.control_domain_id,
          ComponentIncidentObservationDTO{condition.control_domain_id,
                                          value.observation_generation,
                                          value.received_timestamp_ns, value.observed_state,
                                          value.facts});
    }
  }
  if (!trigger && !conditions.empty()) trigger = conditions.front().control_domain_id;
  // A loss (or a retained, not freshly reported class) is attributable only
  // once this driver itself saw the component fresh and cause-free; before
  // that, missing status is re-establishment waiting. A fresh vendor report
  // of an intervention class is an incident at once.
  const bool own_baseline = open.has_value() || state.observed_fresh;
  const bool any_mode = open.has_value() || detection == IncidentDetection::kAny;
  auto cause = intervention != LifecycleBlockingCauseClassDTO::kNone &&
                       (intervention_fresh || (any_mode && own_baseline))
                   ? intervention
               : any_mode && own_baseline && lost ? LifecycleBlockingCauseClassDTO::kProviderLost
                                                  : LifecycleBlockingCauseClassDTO::kNone;
  // A verified loss the observations do not explain is still an incident.
  if (cause == LifecycleBlockingCauseClassDTO::kNone && detection == IncidentDetection::kAny &&
      own_baseline) {
    cause = unexplained;
  }
  if (!conditions.empty() &&
      std::all_of(conditions.begin(), conditions.end(), [](const DomainCondition& condition) {
        return condition.observed.observation.has_value() &&
               condition.observed.cause == LifecycleBlockingCauseClassDTO::kNone &&
               condition.observed.reported != LifecycleBlockingCauseClassDTO::kAvailability;
      })) {
    state.observed_fresh = true;
  }

  if (open) {
    // One open incident per component: a further unrequested change is
    // appended and its cause becomes the incident's (most recent wins).
    if (cause != LifecycleBlockingCauseClassDTO::kNone && cause != open->incident.cause_class) {
      auto merged = open->incident.vendor_codes;
      for (const auto& code : codes) {
        if (merged.size() >= kMaxComponentIncidentVendorCodes) break;
        if (std::find(merged.begin(), merged.end(), code) == merged.end()) merged.push_back(code);
      }
      try {
        auto record_json =
            ComponentIncidentRecordWithChange(open->record_json, now_ns, cause, merged);
        const auto updated = store_.UpdateOpenComponentIncident(open->incident.incident_id, cause,
                                                                merged, record_json);
        if (updated.ok() && updated.value()) {
          open->incident.cause_class = cause;
          open->incident.vendor_codes = std::move(merged);
          open->record_json = std::move(record_json);
        }
      } catch (...) {
      }
    }
    RequestContainment(*open, now_ns);
    PublishIncident(state, open->incident, now_ns);
    return true;
  }
  if (detection == IncidentDetection::kNone || cause == LifecycleBlockingCauseClassDTO::kNone) {
    return false;
  }
  // A recorded but not yet adopted Apply plan names this component: its
  // change is requested, never an incident. An unreadable plan state opens
  // nothing in this pass; the next pass decides again.
  const auto applying =
      store_.ComponentInUnadoptedApplyPlan(target.controllable_component_id);
  if (!applying.ok() || applying.value()) return false;

  if (codes.size() > kMaxComponentIncidentVendorCodes) codes.resize(kMaxComponentIncidentVendorCodes);
  ComponentLifecycleIncidentDTO incident{
      IncidentId(target.controllable_component_id, now_ns),
      target.controllable_component_id,
      cause,
      codes,
      now_ns,
      IncidentSummary(target.controllable_component_id, cause, codes),
      std::nullopt,
      {},
      ComponentIncidentContainmentStateDTO::kPending,
      std::nullopt,
      std::nullopt,
      false,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  if (trigger) {
    if (const auto good = state.last_good.find(*trigger); good != state.last_good.end()) {
      incident.last_good_observation = good->second;
    }
  }
  for (const auto& occurrence : current.provider_occurrences) {
    incident.providers.push_back({occurrence.control_domain_id, occurrence.provider_instance_id,
                                  occurrence.provider_runtime_unit_id,
                                  occurrence.provider_runtime_epoch});
  }
  Result<ComponentIncidentRecordDTO> stored = Result<ComponentIncidentRecordDTO>::Err(
      driver_error("lifecycle_incident_invalid", "the incident could not be recorded",
                   "incident_invalid"));
  try {
    if (incident.locally_valid()) {
      stored = store_.OpenComponentIncident(
          {incident,
           ComponentIncidentEventPayload(
               incident,
               LifecycleIncidentRecoveryPolicyFor(config_.incident_recovery_policy, cause),
               kComponentIncidentEventKind)});
    }
  } catch (...) {
  }
  if (!stored.ok()) {
    // Fail closed: progress stays blocked on the condition even when the
    // record could not be written; the next pass retries the record.
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, ComponentLifecycleIntentKindDTO::kRecover,
            cause, ReasonOf("incident_record_unavailable", stored.error()), now_ns);
    return true;
  }
  RequestContainment(stored.value(), now_ns);
  PublishIncident(state, stored.value().incident, now_ns);
  return true;
}

bool CCLCLifecycleTargetDriver::FollowInFlight(const ComponentLifecycleTargetDTO& target,
                                               ComponentState& state, const std::int64_t now_ns) {
  if (state.pending_intent) {
    // Exact replay of the unknown submission before anything else.
    const auto pending = *state.pending_intent;
    SubmitIntent(state, pending, now_ns);
    return true;
  }
  if (!state.in_flight) return false;
  const auto& in_flight = *state.in_flight;
  // Not observed while open: the last known class stays presented.
  const auto known_cause =
      state.progress ? state.progress->blocking_cause_class : LifecycleBlockingCauseClassDTO::kNone;
  const auto status = ports_.LoadLifecycleStatus(in_flight.lifecycle_transaction_id);
  if (!status.ok() || !status.value()) {
    Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain, in_flight.intent, known_cause,
            status.ok() ? std::string("transaction_status_unavailable")
                        : ReasonOf("transaction_status_unavailable", status.error()),
            now_ns);
    return true;
  }
  const auto& value = *status.value();
  if (!value.terminal_result) {
    const bool uncertain = value.reconciliation_required ||
                           value.phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown ||
                           value.phase == ComponentLifecycleTransactionPhaseDTO::kReconciling;
    Publish(state,
            uncertain ? LifecycleProgressPhaseDTO::kOutcomeUncertain
                      : LifecycleProgressPhaseDTO::kTransitionInProgress,
            in_flight.intent, known_cause,
            in_flight.target_generation == target.target_generation
                ? (uncertain ? std::optional<std::string>("reconciling_from_fresh_observation")
                             : std::nullopt)
                : std::optional<std::string>("superseded_step_settling"),
            now_ns);
    return true;
  }
  // CCLC has recorded a known outcome (after reconciliation when needed).
  const bool current = in_flight.target_generation == target.target_generation;
  const bool succeeded = *value.terminal_result == ComponentLifecycleTerminalResultDTO::kSucceeded;
  state.in_flight.reset();
  if (!current) return false;
  if (succeeded) {
    ++state.step;
    state.attempt = 1U;
    state.failures = 0U;
    state.failed_basis.reset();
    state.failure_reason.reset();
    state.rejected_at_ns.reset();
  } else {
    ++state.attempt;
    ++state.failures;
    state.failed_basis = state.attempt_basis;
    state.rejected_at_ns.reset();
    state.failure_reason =
        value.error ? ReasonOf("step_failed", *value.error) : std::string("step_failed");
  }
  return false;
}

void CCLCLifecycleTargetDriver::SubmitIntent(ComponentState& state,
                                             const ComponentLifecycleIntentDTO& intent,
                                             const std::int64_t now_ns) {
  state.pending_intent.reset();
  Result<CCLCLifecycleRequestResultDTO> result =
      Result<CCLCLifecycleRequestResultDTO>::Err(driver_error(
          "lifecycle_target_submit_failed", "lifecycle submission raised an exception",
          "submit_exception"));
  try {
    result = ports_.SubmitLifecycle(intent);
  } catch (...) {
  }
  if (!result.ok()) {
    // Unknown whether CCLC admitted it: only the exact same request may be
    // sent again, which CCLC classifies as replay.
    state.pending_intent = intent;
    Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain, intent.intent,
            LifecycleBlockingCauseClassDTO::kNone,
            ReasonOf("submission_outcome_unknown", result.error()), now_ns);
    return;
  }
  const auto& value = result.value();
  if (value.acceptance && !value.rejection &&
      (value.replay_status == RequestReplayStatusDTO::kAcceptedNew ||
       value.replay_status == RequestReplayStatusDTO::kReplayedExact)) {
    state.in_flight = InFlight{value.acceptance->lifecycle_transaction_id, intent.intent,
                               state.target_generation};
    Publish(state, LifecycleProgressPhaseDTO::kTransitionInProgress, intent.intent,
            LifecycleBlockingCauseClassDTO::kNone, std::nullopt, now_ns);
    return;
  }
  // A rejection before acceptance (or a consumed/foreign request identity)
  // is a waiting condition, re-evaluated only after an input changes.
  ++state.attempt;
  std::string reason = "request_not_accepted";
  if (value.rejection) {
    reason = ReasonOf("request_rejected", *value.rejection);
  } else if (value.replay_status == RequestReplayStatusDTO::kRequestIdConflict) {
    reason = "request_id_conflict";
  } else if (value.replay_status == RequestReplayStatusDTO::kRequestLedgerFull) {
    reason = "request_ledger_full";
  } else if (value.replay_status == RequestReplayStatusDTO::kStaleRequestOwnerEpoch) {
    reason = "stale_request_owner_epoch";
  } else if (value.replay_status == RequestReplayStatusDTO::kRequestResultExpired) {
    reason = "request_result_expired";
  }
  if (value.replay_status != RequestReplayStatusDTO::kRequestIdConflict) {
    state.failed_basis = state.attempt_basis;
    state.rejected_at_ns = now_ns;
  }
  state.failure_reason = reason;
  Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, intent.intent,
          LifecycleBlockingCauseClassDTO::kNone, reason, now_ns);
}

void CCLCLifecycleTargetDriver::EvaluateComponent(const ComponentLifecycleTargetDTO& target,
                                                  ComponentState& state,
                                                  const std::int64_t now_ns) {
  if (state.target_generation != target.target_generation) {
    // A new target (operator, superseding Apply) restarts progression; an
    // open transaction of the old one is still followed until terminal.
    state.target_generation = target.target_generation;
    state.step = 0U;
    state.attempt = 1U;
    state.failures = 0U;
    state.failed_basis.reset();
    state.failure_reason.reset();
    state.rejected_at_ns.reset();
    state.waiting_since_ns.reset();
    // Waiting is cancelled at once. An unknown submission of the old target
    // is not re-sent; if CCLC accepted it, it completes under its deadline
    // and the component stays busy for any new request until then.
    state.pending_intent.reset();
  }
  if (FollowInFlight(target, state, now_ns)) return;

  const auto& component = target.controllable_component_id;
  std::map<ControlDomainId, LifecycleBlockingCauseClassDTO> retained;
  if (const auto causes = store_.LoadInterventionCauses(component); causes.ok()) {
    for (const auto& cause : causes.value()) retained[cause.control_domain_id] = cause.cause_class;
  }
  auto retained_worst = LifecycleBlockingCauseClassDTO::kNone;
  for (const auto& [domain, cause] : retained) retained_worst = Worst(retained_worst, cause);

  // An open incident blocks everything until the operator's Recover.
  const auto open = store_.LoadOpenComponentIncident(component);
  if (!open.ok()) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, NominalIntent(target.requested_state),
            retained_worst, ReasonOf("incident_record_unavailable", open.error()), now_ns);
    return;
  }
  const auto snapshot = ports_.ReadCurrentComponent(component);
  if (!snapshot.ok()) {
    if (open.value()) {
      PublishIncident(state, open.value()->incident, now_ns);
      return;
    }
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
            NominalIntent(target.requested_state), retained_worst,
            ReasonOf("component_snapshot_unavailable", snapshot.error()), now_ns);
    return;
  }
  const auto& current = snapshot.value();
  const auto achieved = current.component_lifecycle_state;
  const auto next = NextIntent(achieved, target.requested_state);
  // Losses are incidents only after the component reached its target on
  // fresh status in this session; while recovering from an incident only a
  // newly reported intervention class is. Before that, `availability` is
  // ordinary waiting.
  const bool following = target.requested_state != LifecycleTargetStateDTO::kDisabled;
  const auto detection = !following ? IncidentDetection::kNone
                         : target.reached ? IncidentDetection::kAny
                         : target.recovered_cause ? IncidentDetection::kNewCause
                                                  : IncidentDetection::kNone;
  if (open.value()) {
    static_cast<void>(EvaluateIncident(target, state, current,
                                       ObserveAllDomains(target, state, current, retained, now_ns),
                                       open.value(), detection, now_ns));
    return;
  }
  if (!next) {
    state.failed_basis.reset();
    state.failure_reason.reset();
    // A component at its target is still observed, read only, so that a
    // safety stop, fault or lost controller after it became ready stays
    // visible in its progress (spec 530 lifecycle blocking causes). Nothing
    // is submitted; an unchanged class writes nothing.
    const auto conditions = ObserveAllDomains(target, state, current, retained, now_ns);
    if (EvaluateIncident(target, state, current, conditions, std::nullopt, detection, now_ns)) {
      return;
    }
    auto cause = current.provider_occurrences.empty() ? retained_worst
                                                      : LifecycleBlockingCauseClassDTO::kNone;
    bool all_fresh_without_cause = !conditions.empty();
    for (const auto& condition : conditions) {
      cause = Worst(cause, condition.observed.cause);
      all_fresh_without_cause = all_fresh_without_cause &&
                                condition.observed.observation.has_value() &&
                                condition.observed.reported == LifecycleBlockingCauseClassDTO::kNone;
    }
    // First time at the target on fresh status: from now on a loss or an
    // unexpected change is an incident (and a recovered incident is over).
    if (following && !target.reached && all_fresh_without_cause) {
      static_cast<void>(store_.RecordLifecycleTargetReached(component, true));
    }
    Publish(state, LifecycleProgressPhaseDTO::kSatisfied, std::nullopt, cause, std::nullopt,
            now_ns);
    return;
  }
  if (achieved == ComponentLifecycleStateDTO::kUndeclared) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next, retained_worst,
            std::string("component_undeclared"), now_ns);
    return;
  }
  if (achieved == ComponentLifecycleStateDTO::kDisabling) {
    Publish(state, LifecycleProgressPhaseDTO::kTransitionInProgress, *next, retained_worst,
            std::string("disabling_in_progress"), now_ns);
    return;
  }

  // One fresh observation per provider occurrence, read only.
  std::vector<BrandLifecyclePlanCatalogDTO> catalogs;
  std::vector<std::optional<BrandLifecycleObservationDTO>> observations;
  AttemptBasis basis{{}, 0U, current.snapshot_digest.value()};
  auto cause = LifecycleBlockingCauseClassDTO::kNone;
  bool all_fresh = true;
  std::optional<std::string> unavailable_reason;
  std::vector<DomainCondition> conditions;
  for (const auto& occurrence : current.provider_occurrences) {
    auto catalog = ports_.LoadLifecyclePlanCatalog(occurrence);
    if (!catalog.ok()) {
      Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next,
              LifecycleBlockingCauseClassDTO::kConfiguration,
              ReasonOf("lifecycle_catalog_unavailable", catalog.error()), now_ns);
      return;
    }
    catalogs.push_back(std::move(catalog.value()));
    const auto stored = retained.find(occurrence.control_domain_id);
    auto observed = ObserveDomain(
        ports_, store_, component, occurrence, target.target_generation,
        stored == retained.end() ? LifecycleBlockingCauseClassDTO::kNone : stored->second,
        target.recovered_cause, clock_);
    basis.observation_generations.push_back(observed.generation);
    if (!observed.observation) {
      all_fresh = false;
      if (!unavailable_reason) unavailable_reason = observed.unavailable_reason;
    }
    cause = Worst(cause, observed.cause);
    observations.push_back(observed.observation);
    conditions.push_back({occurrence.control_domain_id, std::move(observed)});
  }
  if (!conditions.empty() &&
      std::all_of(conditions.begin(), conditions.end(), [](const DomainCondition& condition) {
        return condition.observed.observation.has_value() &&
               condition.observed.cause == LifecycleBlockingCauseClassDTO::kNone &&
               condition.observed.reported != LifecycleBlockingCauseClassDTO::kAvailability;
      })) {
    state.observed_fresh = true;
  }
  // A component that reached its target and that CCLC verified as degraded
  // or faulted, or a new intervention class while recovering, is an incident.
  const bool verified_loss = achieved == ComponentLifecycleStateDTO::kDegraded ||
                             achieved == ComponentLifecycleStateDTO::kFaulted;
  if ((detection == IncidentDetection::kNewCause ||
       (detection == IncidentDetection::kAny && verified_loss)) &&
      EvaluateIncident(target, state, current, conditions, std::nullopt,
                       verified_loss ? detection : IncidentDetection::kNewCause, now_ns,
                       LifecycleBlockingCauseClassDTO::kUnknown)) {
    return;
  }

  // An intervention-requiring class blocks every step except moving toward
  // a requested Disable; ManyForge never clears it itself.
  if (lifecycle_cause_requires_intervention(cause) &&
      *next != ComponentLifecycleIntentKindDTO::kDisable) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next, cause,
            std::string("intervention_required:") + std::string(to_string(cause)), now_ns);
    return;
  }
  if (!all_fresh) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            unavailable_reason, now_ns);
    return;
  }

  const auto admission = ports_.ReadCellAdmission();
  basis.admission_generation = admission.admission_generation.value();
  if (state.failures >= config_.maximum_attempts_per_step) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next, cause,
            "attempts_exhausted" +
                (state.failure_reason ? ":" + *state.failure_reason : std::string()),
            now_ns);
    return;
  }
  if (state.failed_basis && *state.failed_basis == basis &&
      (!state.rejected_at_ns ||
       now_ns - *state.rejected_at_ns < config_.rejection_retry_after_ns)) {
    // Known failure or rejection: wait for a changed observation, admission
    // or achieved state before trying the same step again.
    Publish(state,
            state.failures > 0U ? LifecycleProgressPhaseDTO::kBlocked
                                : LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
            *next, cause, state.failure_reason, now_ns);
    return;
  }

  // Eligibility of the next intent's own plan on the fresh observations.
  auto requested_state = RequestedStateOf(*next);
  if (*next == ComponentLifecycleIntentKindDTO::kRecover) {
    std::vector<LifecycleRecoverSelectionInput> inputs;
    for (std::size_t index = 0U; index < catalogs.size(); ++index) {
      inputs.push_back({current.provider_occurrences[index], catalogs[index],
                        *observations[index]});
    }
    const auto selected = SelectCommonRecoverTarget(inputs);
    if (!selected.ok()) {
      Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
              ReasonOf("prerequisites_unmet", selected.error()), now_ns);
      return;
    }
    requested_state = selected.value();
  }
  for (std::size_t index = 0U; index < catalogs.size(); ++index) {
    const auto decision =
        EvaluateLifecycleDomainPlan(*next, requested_state, catalogs[index], *observations[index]);
    if (!decision.ok()) {
      Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
              ReasonOf("prerequisites_unmet", decision.error()), now_ns);
      return;
    }
  }
  // Known no-admission conditions are waited out without a request, so a
  // waiting component leaves no rejection records behind.
  if (!admission.authoring_allowed || !admission.admission_generation.nonzero()) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            std::string("cell_admission_closed"), now_ns);
    return;
  }
  if (!ports_.AcceptingLifecycleRequests()) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            std::string("cclc_admission_closed"), now_ns);
    return;
  }
  if (ports_.ComponentLifecycleSettling(component)) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            std::string("component_lifecycle_in_progress"), now_ns);
    return;
  }

  std::vector<ExpectedControlDomainRevisionDTO> expected_domains;
  expected_domains.reserve(current.domains.size());
  for (const auto& domain : current.domains) {
    ExpectedActiveControlConfigurationDTO configuration{
        ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
    if (domain.active_control_configuration) {
      configuration = {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
                       domain.active_control_configuration->active_control_configuration_id,
                       domain.active_control_configuration->active_control_configuration_generation};
    }
    expected_domains.push_back({domain.control_domain_id, domain.control_domain_revision,
                                domain.controller_topology_id, domain.topology_revision,
                                configuration, domain.current_control_bindings});
  }
  ComponentLifecycleIntentDTO intent{
      {LifecycleTargetRequestId(component, target.target_generation, state.step, *next,
                                state.attempt),
       ports_.ReadRequestOwnerEpoch(), std::string(kLifecycleTargetSourceLabel),
       "lifecycle_target.g" + std::to_string(target.target_generation)},
      component,
      *next,
      current.active_publication_receipt.request.candidate_graph_revision,
      current.component_lifecycle_generation,
      std::move(expected_domains),
      "requested lifecycle target " + std::string(to_string(target.requested_state)) + " (" +
          std::string(to_string(target.origin)) + ", generation " +
          std::to_string(target.target_generation) + ")",
      RequestAttributionDTO{},
      admission.admission_generation,
      std::nullopt};
  state.attempt_basis = basis;
  SubmitIntent(state, intent, now_ns);
  if (state.progress && state.progress->phase == LifecycleProgressPhaseDTO::kTransitionInProgress) {
    state.progress->blocking_cause_class = cause;
  }
}

Result<std::optional<ComponentLifecycleTargetDTO>> CCLCLifecycleTargetDriver::RecordOperatorRequest(
    const ComponentLifecycleIntentDTO& intent) {
  const auto requested = lifecycle_target_of(intent.intent);
  if (!requested || intent.request_context.source_label == kLifecycleTargetSourceLabel) {
    return Result<std::optional<ComponentLifecycleTargetDTO>>::Ok(std::nullopt);
  }
  auto target = store_.SetLifecycleTarget(intent.controllable_component_id, *requested,
                                          LifecycleTargetOriginDTO::kOperator);
  if (!target.ok()) return Result<std::optional<ComponentLifecycleTargetDTO>>::Err(target.error());
  Wake();
  return Result<std::optional<ComponentLifecycleTargetDTO>>::Ok(target.value());
}

Result<ComponentLifecycleTargetViewDTO> CCLCLifecycleTargetDriver::ReadTargetView(
    const ControllableComponentId& component) {
  auto target = store_.LoadLifecycleTarget(component);
  if (!target.ok()) return Result<ComponentLifecycleTargetViewDTO>::Err(target.error());
  ComponentLifecycleTargetViewDTO view;
  if (!target.value()) return Result<ComponentLifecycleTargetViewDTO>::Ok(view);
  view.target = target.value();
  {
    std::lock_guard lock(state_mutex_);
    const auto found = components_.find(component);
    if (found != components_.end() &&
        found->second.target_generation == view.target->target_generation &&
        found->second.progress) {
      view.progress = found->second.progress;
      // `delayed` is a function of the owner clock; reads never publish.
      if (view.progress->waiting_since_ns) {
        view.progress->delayed =
            clock_.now() - *view.progress->waiting_since_ns > config_.readiness_expectation_ns;
      }
    }
  }
  if (!view.progress) {
    // Recorded but not yet evaluated: waiting from now on.
    view.progress = ComponentLifecycleProgressDTO{
        LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
        NominalIntent(view.target->requested_state),
        LifecycleBlockingCauseClassDTO::kNone,
        std::string("not_yet_evaluated"),
        std::max<std::int64_t>(0, clock_.now()),
        false,
        std::nullopt};
  }
  // The incident is read from its durable record, so a Recover or a newly
  // opened incident is visible before the next evaluation pass.
  const auto open = store_.LoadOpenComponentIncident(component);
  if (!open.ok()) return Result<ComponentLifecycleTargetViewDTO>::Err(open.error());
  if (open.value()) {
    const auto& incident = open.value()->incident;
    view.progress->phase = LifecycleProgressPhaseDTO::kBlocked;
    view.progress->next_intent = ComponentLifecycleIntentKindDTO::kRecover;
    view.progress->blocking_cause_class = incident.cause_class;
    view.progress->blocking_reason = "incident_open:" + std::string(to_string(incident.cause_class));
    if (!view.progress->waiting_since_ns) {
      view.progress->waiting_since_ns = std::max<std::int64_t>(0, incident.detected_at_ns);
    }
    view.progress->incident = incident;
  } else if (view.progress->incident) {
    // Recovered since the last pass: the driver re-evaluates next.
    view.progress->incident.reset();
    view.progress->phase = LifecycleProgressPhaseDTO::kWaitingForPrerequisites;
    view.progress->blocking_reason = "incident_recovered";
  }
  return Result<ComponentLifecycleTargetViewDTO>::Ok(std::move(view));
}

namespace {

[[nodiscard]] CCLCLifecycleRequestResultDTO IncidentRecoveryResult(
    const RequestReplayStatusDTO replay_status, ComponentIncidentRecoveryDTO recovery) {
  CCLCLifecycleRequestResultDTO result{replay_status, std::nullopt, std::nullopt, std::nullopt};
  result.incident_recovery = std::move(recovery);
  return result;
}

[[nodiscard]] bool OperatorRecover(const ComponentLifecycleIntentDTO& intent) {
  return intent.intent == ComponentLifecycleIntentKindDTO::kRecover &&
         intent.request_context.source_label != kLifecycleTargetSourceLabel;
}

}  // namespace

Result<std::optional<CCLCLifecycleRequestResultDTO>> CCLCLifecycleTargetDriver::ReadIncidentRecovery(
    const ComponentLifecycleIntentDTO& intent) {
  using Value = std::optional<CCLCLifecycleRequestResultDTO>;
  if (!OperatorRecover(intent)) return Result<Value>::Ok(std::nullopt);
  const auto recorded = store_.LoadComponentIncidentRecovery(intent.controllable_component_id,
                                                             intent.request_context.request_id);
  if (!recorded.ok()) return Result<Value>::Err(recorded.error());
  if (!recorded.value()) return Result<Value>::Ok(std::nullopt);
  return Result<Value>::Ok(
      IncidentRecoveryResult(RequestReplayStatusDTO::kReplayedExact, *recorded.value()));
}

Result<std::optional<CCLCLifecycleRequestResultDTO>> CCLCLifecycleTargetDriver::RecoverIncident(
    const ComponentLifecycleIntentDTO& intent) {
  using Value = std::optional<CCLCLifecycleRequestResultDTO>;
  if (!OperatorRecover(intent) || !intent.locally_valid() ||
      intent.request_context.request_owner_epoch != ports_.ReadRequestOwnerEpoch()) {
    // Ordinary CCLC admission decides (and rejects a stale or invalid one).
    return Result<Value>::Ok(std::nullopt);
  }
  auto replay = ReadIncidentRecovery(intent);
  if (!replay.ok() || replay.value()) return replay;
  const auto open = store_.LoadOpenComponentIncident(intent.controllable_component_id);
  if (!open.ok()) return Result<Value>::Err(open.error());
  if (!open.value()) return Result<Value>::Ok(std::nullopt);
  const auto now = clock_.now();
  if (now < 0 || now > kMaxComponentIncidentTimeNs) {
    return Result<Value>::Err(driver_error("lifecycle_incident_clock_invalid",
                                           "the owner clock is outside the incident time range",
                                           "incident_clock_invalid"));
  }
  // Authorization only: the Recover is recorded and the incident closed; no
  // vendor primitive is dispatched here, so it is admissible while the cell
  // is latched. The policy-permitted steps follow through ordinary admission.
  auto incident = open.value()->incident;
  incident.acknowledged = true;
  incident.acknowledged_by = intent.attribution.principal_id.value_or(
      intent.request_context.source_label);
  incident.acknowledged_at_ns = now;
  incident.acknowledgement_request_id = intent.request_context.request_id;
  std::string payload;
  try {
    payload = ComponentIncidentAcknowledgedPayload(open.value()->record_json, incident);
  } catch (const std::exception& exception) {
    return Result<Value>::Err(driver_error("lifecycle_incident_record_invalid", exception.what(),
                                           "incident_record_invalid"));
  }
  const auto closed = store_.AcknowledgeComponentIncident(incident, payload);
  if (!closed.ok()) return Result<Value>::Err(closed.error());
  if (!closed.value()) return Result<Value>::Ok(std::nullopt);
  Wake();
  return Result<Value>::Ok(IncidentRecoveryResult(
      RequestReplayStatusDTO::kAcceptedNew,
      {incident.incident_id, incident.controllable_component_id, incident.cause_class,
       *incident.acknowledged_by, now}));
}

Result<std::vector<ComponentIncidentEventDTO>> CCLCLifecycleTargetDriver::LoadIncidentEvents(
    const std::uint64_t after_sequence, const std::size_t limit) {
  return store_.LoadComponentIncidentEvents(after_sequence, limit);
}

}  // namespace manyforge::core
