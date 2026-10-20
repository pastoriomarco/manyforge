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

}  // namespace

/// The next intent toward `target` from the achieved CCLC state, or nullopt
/// when the target is satisfied. There is no intent that lowers Active to
/// Prepared, so Active satisfies a `prepared` target.
[[nodiscard]] std::optional<ComponentLifecycleIntentKindDTO> NextLifecycleIntentTowardTarget(
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
[[nodiscard]] ComponentLifecycleIntentKindDTO NominalLifecycleIntentForTarget(const LifecycleTargetStateDTO target) {
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

namespace {

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
  /// The fresh intervention class must be retained durably (the caller
  /// records it, see RetainCause), observed at `observed_at_ns`.
  bool retain{};
  std::int64_t observed_at_ns{};
};

[[nodiscard]] bool Stale(const ErrorDTO& error) {
  return error.code == kLifecycleFactsStaleErrorCode;
}

[[nodiscard]] bool ReachedTargetRetired(const ErrorDTO& error) {
  const auto reason = error.details.find("reason");
  return Stale(error) && reason != error.details.end() &&
         reason->second == "target_retired_or_superseded";
}

/// Observes one occurrence and keeps the retained intervention cause of its
/// domain: fresh intervention-requiring status is to be recorded, and neither
/// communication loss, a later `availability` report nor fresh status without
/// a cause clears it; only the operator's Recover does (ADR-0026 §7).
///
/// `record_causes` is false while a CCLC lifecycle transaction of the
/// component is unsettled: that step verifies its own postcondition on fresh
/// status (for example the operator's Recover clearing a vendor error), so a
/// class it is about to clear is not retained again. The class still blocks
/// and is incident evidence in this pass; once the step settles, a class that
/// is still reported is retained.
[[nodiscard]] DomainObservation ObserveDomain(
    ICCLCLifecycleTargetDriverPorts& ports, const ControllableComponentId& component,
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
    const std::uint64_t target_generation, const LifecycleBlockingCauseClassDTO retained_cause,
    const std::optional<LifecycleBlockingCauseClassDTO> recovered_cause, const bool record_causes,
    IClock& clock) {
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
    result.retain = record_causes;
    result.observed_at_ns = now_ns;
  } else if (CauseRank(retained_cause) == 2) {
    // Fresh `none` or `availability` does not clear a retained class.
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
      snapshot_source_(&snapshot_source),
      catalog_source_(&catalog_source),
      port_resolver_(&port_resolver),
      kernel_cell_admission_port_(kernel_cell_admission_port),
      unsettled_components_(std::move(unsettled_components)) {}

CoordinatorLifecycleTargetDriverPorts::CoordinatorLifecycleTargetDriverPorts(
    ComponentLifecycleCoordinator& coordinator,
    IKernelCellAdmissionPort& kernel_cell_admission_port,
    std::function<Result<std::vector<ControllableComponentId>>()> unsettled_components) noexcept
    : coordinator_(coordinator),
      kernel_cell_admission_port_(kernel_cell_admission_port),
      unsettled_components_(std::move(unsettled_components)) {}

Result<void> CoordinatorLifecycleTargetDriverPorts::WithPinnedProduct(
    const std::function<void(ICCLCLifecycleTargetDriverPorts&)>& pass) {
  if (snapshot_source_ != nullptr) {
    pass(*this);
    return Result<void>::Ok();
  }
  auto acquired = coordinator_.AcquireProductPin();
  if (!acquired.ok()) return Result<void>::Err(acquired.error());
  const auto& context = acquired.value().context();
  CoordinatorLifecycleTargetDriverPorts bound(
      coordinator_, context.current_snapshot_source, context.lifecycle_plan_catalog_source,
      context.lifecycle_port_resolver, kernel_cell_admission_port_, unsettled_components_);
  bound.product_pin_ = &acquired.value();
  pass(bound);
  return Result<void>::Ok();
}

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

bool CoordinatorLifecycleTargetDriverPorts::RetiredTargetWorkSettled(
    const ControllableComponentId& component) {
  try {
    return !ComponentLifecycleSettling(component) && coordinator_.ReadLifecycleOwnerSettlement().ok();
  } catch (...) {
    return false;
  }
}

Result<ComponentLifecycleCurrentSnapshotDTO>
CoordinatorLifecycleTargetDriverPorts::ReadCurrentComponent(
    const ControllableComponentId& component) {
  if (snapshot_source_ == nullptr) {
    auto result = Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
        driver_error("cclc_product_read_unavailable", "the product read did not run",
                     "product_read_unavailable"));
    const auto pinned = WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& ports) {
      result = ports.ReadCurrentComponent(component);
    });
    return pinned.ok() ? result : Result<ComponentLifecycleCurrentSnapshotDTO>::Err(pinned.error());
  }
  auto snapshot = snapshot_source_->ReadCurrentComponentSnapshot(component);
  if (!snapshot.ok()) return snapshot;
  const auto verified = VerifyComponentLifecycleCurrentSnapshot(snapshot.value());
  if (!verified.ok()) return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(verified.error());
  return snapshot;
}

Result<BrandLifecyclePlanCatalogDTO> CoordinatorLifecycleTargetDriverPorts::LoadLifecyclePlanCatalog(
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence) {
  if (catalog_source_ == nullptr) {
    auto result = Result<BrandLifecyclePlanCatalogDTO>::Err(
        driver_error("cclc_product_read_unavailable", "the product read did not run",
                     "product_read_unavailable"));
    const auto pinned = WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& ports) {
      result = ports.LoadLifecyclePlanCatalog(occurrence);
    });
    return pinned.ok() ? result : Result<BrandLifecyclePlanCatalogDTO>::Err(pinned.error());
  }
  return catalog_source_->LoadExactLifecyclePlanCatalog(occurrence.provider_manifest_id,
                                                        occurrence.provider_manifest_revision,
                                                        occurrence.lifecycle_plan_catalog_digest);
}

Result<BrandLifecycleObservationDTO> CoordinatorLifecycleTargetDriverPorts::Observe(
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence,
    const BrandLifecycleObservationQueryDTO& query) {
  if (port_resolver_ == nullptr) {
    auto result = Result<BrandLifecycleObservationDTO>::Err(
        driver_error("cclc_product_read_unavailable", "the product read did not run",
                     "product_read_unavailable"));
    const auto pinned = WithPinnedProduct(
        [&](ICCLCLifecycleTargetDriverPorts& ports) { result = ports.Observe(occurrence, query); });
    return pinned.ok() ? result : Result<BrandLifecycleObservationDTO>::Err(pinned.error());
  }
  auto port = port_resolver_->ResolveExactLifecyclePort(occurrence);
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
  return product_pin_ ? coordinator_.SubmitLifecycle(intent, *product_pin_)
                      : coordinator_.SubmitLifecycle(intent);
}

Result<CCLCLifecycleRequestResultDTO> CoordinatorLifecycleTargetDriverPorts::SubmitTargetLifecycle(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleTargetExpectationDTO& expected_target) {
  return product_pin_ ? coordinator_.SubmitTargetLifecycle(intent, expected_target, *product_pin_)
                      : coordinator_.SubmitTargetLifecycle(intent, expected_target);
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
CoordinatorLifecycleTargetDriverPorts::ReadLifecycleRequestResult(const ComponentLifecycleIntentDTO& intent) {
  return coordinator_.ReadLifecycleRequestResult(intent);
}

std::optional<bool> CoordinatorLifecycleTargetDriverPorts::ConfiguredComponentMembership(
    const ControllableComponentId& component) const {
  if (product_pin_ == nullptr || !product_pin_->context().identity) return std::nullopt;
  const auto& configured = product_pin_->context().identity->configured_components;
  return std::find(configured.begin(), configured.end(), component) != configured.end();
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

bool CCLCLifecycleTargetDriver::MatchesProgramResetPositionScope(
    const ProgramResetPositionScope& scope, const ComponentLifecycleTargetDTO& target,
    const ComponentLifecycleCurrentSnapshotDTO& current) const {
  const auto& request = scope.request;
  return scope.valid && target.controllable_component_id == request.component &&
         target.target_generation == scope.target_generation && target.reached &&
         target.requested_state == LifecycleTargetStateDTO::kActive &&
         current.controllable_component_id == request.component &&
         current.active_publication_receipt == request.expected_publication &&
         std::count(current.provider_occurrences.begin(), current.provider_occurrences.end(),
                    request.provider) == 1 &&
         std::count(current.domains.begin(), current.domains.end(), request.domain) == 1;
}

Result<void> CCLCLifecycleTargetDriver::ValidateProgramResetPositionScope(
    ICCLCLifecycleTargetDriverPorts& ports, const ProgramResetPositionScope& scope) {
  if (scope.step_deadline_ns && clock_.now() >= *scope.step_deadline_ns)
    return Result<void>::Err(driver_error("program_reset_position_scope_stale",
                                          "the current position Reset step deadline elapsed",
                                          "step_deadline_elapsed"));
  const auto facts = store_.LoadLifecycleTargetFacts(scope.request.component);
  if (!facts.ok()) return Result<void>::Err(facts.error());
  const auto current = ports.ReadCurrentComponent(scope.request.component);
  if (!current.ok()) return Result<void>::Err(current.error());
  if (!facts.value().target || facts.value().open_incident ||
      !facts.value().intervention_causes.empty() ||
      !MatchesProgramResetPositionScope(scope, *facts.value().target, current.value()) ||
      ports.ComponentLifecycleSettling(scope.request.component))
    return Result<void>::Err(driver_error("program_reset_position_scope_stale",
                                          "the exact active component Reset scope is unavailable",
                                          "scope_identity_or_work_changed"));
  return Result<void>::Ok();
}

Result<void> CCLCLifecycleTargetDriver::WithProgramResetPositionScope(
    const CCLCProgramResetPositionScopeRequest& request,
    const CCLCProgramResetPositionAction& action) {
  const auto refused = [](std::string reason) {
    return Result<void>::Err(driver_error("program_reset_position_scope_unavailable",
                                          "the component cannot enter the requested position Reset",
                                          std::move(reason)));
  };
  if (request.runner_owner_epoch.empty() || request.cycle_generation == 0U ||
      request.request_owner_epoch.empty() || request.request_id.empty() ||
      request.request_payload_digest.empty() || request.component.empty() ||
      !request.provider.locally_valid() || !request.domain.locally_valid() ||
      request.domain.provider() || !request.domain.controller().active_control_configuration ||
      request.domain.controller().active_control_configuration->state !=
          ActiveControlConfigurationStateDTO::kReconciled ||
      request.provider.control_domain_id != request.domain.control_domain_id ||
      !action)
    return refused("scope_request_invalid");
  std::shared_ptr<ProgramResetPositionScope> scope;
  {
    // An already-decided follower submission completes under this exclusion.
    // No driver/admission lock is retained over the effect action below.
    std::lock_guard run_lock(run_mutex_);
    {
      std::lock_guard lock(state_mutex_);
      if (program_reset_position_scope_) return refused("position_reset_already_scoped");
      const auto found = components_.find(request.component);
      if (found != components_.end() &&
          (found->second.in_flight || found->second.pending_intent ||
           found->second.unrecorded_incident || !found->second.unrecorded_causes.empty()))
        return refused("component_work_unsettled");
    }
    Result<void> validated = refused("product_pin_unavailable");
    const auto pinned = ports_.WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& ports) {
      const auto facts = store_.LoadLifecycleTargetFacts(request.component);
      if (!facts.ok()) {
        validated = Result<void>::Err(facts.error());
        return;
      }
      if (!facts.value().target) {
        validated = refused("active_target_unavailable");
        return;
      }
      scope = std::make_shared<ProgramResetPositionScope>(
          ProgramResetPositionScope{request, facts.value().target->target_generation});
      validated = ValidateProgramResetPositionScope(ports, *scope);
      if (!validated.ok()) return;
      const auto current = ports.ReadCurrentComponent(request.component);
      if (!current.ok()) {
        validated = Result<void>::Err(current.error());
        return;
      }
      if (current.value().component_lifecycle_state != ComponentLifecycleStateDTO::kActive)
        validated = refused("component_not_active");
    });
    if (!pinned.ok()) return Result<void>::Err(pinned.error());
    if (!validated.ok()) return validated;
    std::lock_guard lock(state_mutex_);
    program_reset_position_scope_ = scope;
  }
  struct CloseScope final {
    CCLCLifecycleTargetDriver& driver;
    const std::shared_ptr<ProgramResetPositionScope>& scope;
    ~CloseScope() {
      {
        std::lock_guard run_lock(driver.run_mutex_);
        std::lock_guard lock(driver.state_mutex_);
        scope->valid = false;
        if (driver.program_reset_position_scope_ == scope)
          driver.program_reset_position_scope_.reset();
      }
      driver.Wake();
    }
  } close_scope{*this, scope};
  const auto advance = [this, scope, refused](const CCLCProgramResetPositionPhase phase,
                                              const std::optional<TimePoint> step_deadline_ns) {
    std::lock_guard run_lock(run_mutex_);
    if (program_reset_position_scope_ != scope || !scope->valid)
      return refused("scope_closed_or_expired");
    const auto previous = scope->phase;
    const bool ordered = phase == previous ||
                         (phase == CCLCProgramResetPositionPhase::kSettingState &&
                          previous == CCLCProgramResetPositionPhase::kSwitchingToWriter) ||
                         (phase == CCLCProgramResetPositionPhase::kSwitchingToJtc &&
                          previous == CCLCProgramResetPositionPhase::kSettingState);
    Result<void> validated = refused("phase_out_of_order");
    if (ordered) {
      const auto pinned = ports_.WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& ports) {
        validated = ValidateProgramResetPositionScope(ports, *scope);
      });
      if (!pinned.ok()) validated = Result<void>::Err(pinned.error());
    }
    if (!validated.ok()) {
      scope->valid = false;
      return validated;
    }
    if (step_deadline_ns && *step_deadline_ns <= clock_.now()) {
      scope->valid = false;
      return refused("step_deadline_elapsed");
    }
    if (step_deadline_ns) scope->step_deadline_ns = step_deadline_ns;
    scope->phase = phase;
    Wake();
    return Result<void>::Ok();
  };
  Result<void> result = refused("effect_action_exception");
  try {
    result = action(advance);
  } catch (const std::exception& error) {
    result = Result<void>::Err(driver_error("program_reset_position_action_failed", error.what(),
                                            "effect_action_exception"));
  } catch (...) {
  }
  {
    std::lock_guard run_lock(run_mutex_);
    if (result.ok()) {
      if (scope->phase != CCLCProgramResetPositionPhase::kSwitchingToJtc) {
        result = refused("position_reset_activation_not_completed");
      } else {
        const auto pinned = ports_.WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& ports) {
          result = ValidateProgramResetPositionScope(ports, *scope);
          if (!result.ok()) return;
          const auto current = ports.ReadCurrentComponent(request.component);
          if (!current.ok())
            result = Result<void>::Err(current.error());
          else if (current.value().component_lifecycle_state != ComponentLifecycleStateDTO::kActive)
            result = refused("position_reset_component_not_active");
        });
        if (!pinned.ok()) result = Result<void>::Err(pinned.error());
      }
    }
  }
  return result;
}

void CCLCLifecycleTargetDriver::RunOnce() {
  std::lock_guard run_lock(run_mutex_);
  const auto pinned = ports_.WithPinnedProduct(
      [&](ICCLCLifecycleTargetDriverPorts& ports) { RunOncePinned(ports); });
  if (pinned.ok()) return;
  const auto now = clock_.now();
  std::lock_guard lock(state_mutex_);
  // Preserve in-flight and unrecorded facts. Pending product exchange grants
  // no new step; journal/status ports remain available independently.
  for (auto& [component, state] : components_) {
    std::optional<ComponentLifecycleIntentKindDTO> next_intent;
    if (state.progress) next_intent = state.progress->next_intent;
    if (!next_intent && state.requested_state)
      next_intent = NominalLifecycleIntentForTarget(*state.requested_state);
    if (next_intent)
      Publish(state, state.pending_intent || state.in_flight
                  ? LifecycleProgressPhaseDTO::kOutcomeUncertain : LifecycleProgressPhaseDTO::kBlocked, next_intent,
              state.progress ? state.progress->blocking_cause_class
                             : LifecycleBlockingCauseClassDTO::kNone,
              ReasonOf("lifecycle_product_unavailable", pinned.error()), now);
  }
}

void CCLCLifecycleTargetDriver::RunOncePinned(ICCLCLifecycleTargetDriverPorts& ports) {
  const std::int64_t now = clock_.now();
  auto targets = store_.LoadLifecycleTargets();
  if (!targets.ok()) {
    // Unreadable targets never read as "nothing to follow": every followed
    // component shows why nothing progresses, and its in-memory retries
    // (unrecorded causes, reached marker, incident record) are kept.
    std::lock_guard lock(state_mutex_);
    for (auto& [component, state] : components_) {
      // `blocked` always names its next intent (spec 210 §3.17): the one in
      // progress, otherwise the one toward the last target read.
      std::optional<ComponentLifecycleIntentKindDTO> next_intent;
      if (state.progress) next_intent = state.progress->next_intent;
      if (!next_intent && state.requested_state) {
        next_intent = NominalLifecycleIntentForTarget(*state.requested_state);
      }
      if (!next_intent) continue;
      Publish(state, state.pending_intent || state.in_flight
                  ? LifecycleProgressPhaseDTO::kOutcomeUncertain : LifecycleProgressPhaseDTO::kBlocked, next_intent,
              state.progress ? state.progress->blocking_cause_class
                             : LifecycleBlockingCauseClassDTO::kNone,
              ReasonOf("lifecycle_targets_unavailable", targets.error()), now);
    }
    return;
  }
  std::map<ControllableComponentId, ComponentState> states;
  {
    std::lock_guard lock(state_mutex_);
    states = components_;
  }
  std::map<ControllableComponentId, ComponentState> next;
  bool in_flight = false;
  for (const auto& target : targets.value()) {
    auto [found, inserted] = states.try_emplace(target.controllable_component_id);
    if (inserted) {
      found->second.target_generation = target.target_generation;
      found->second.requested_state = target.requested_state;
    }
  }
  // The same map also owns absent/superseded original work and evidence.
  for (auto& [component, state] : states) {
    if (!EvaluateComponent(ports, component, state, now)) continue;
    in_flight = in_flight || state.in_flight.has_value() || state.pending_intent.has_value();
    next.emplace(component, std::move(state));
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
                                                const std::int64_t now_ns,
                                                const std::optional<ErrorDTO>& diagnostic,
                                                const bool diagnostic_loaded) const {
  // Routine durable facts omit expensive auxiliary decoding. Preserve only
  // the immutable enrichment of this exact incident, never its cause or
  // containment columns from an earlier read.
  const auto previous_incident = state.progress ? state.progress->incident : std::nullopt;
  auto visible = incident;
  if (!diagnostic_loaded && !visible.diagnostic_snapshot && previous_incident &&
      previous_incident->incident_id == visible.incident_id) {
    visible.diagnostic = previous_incident->diagnostic;
    visible.diagnostic_snapshot = previous_incident->diagnostic_snapshot;
  }
  // Nothing proceeds while an incident is open; the operator's Recover is the
  // next intent. The short reason retains its set-once diagnostic too.
  const auto& reason_diagnostic = visible.diagnostic ? visible.diagnostic : diagnostic;
  Publish(state, LifecycleProgressPhaseDTO::kBlocked, ComponentLifecycleIntentKindDTO::kRecover,
          incident.cause_class,
          Bounded("incident_open:" + std::string(to_string(incident.cause_class)) + ":" +
                  incident.summary + (reason_diagnostic ? " " + reason_diagnostic->message : "")),
          now_ns);
  state.progress->incident = std::move(visible);
}

std::vector<CCLCLifecycleTargetDriver::DomainCondition>
CCLCLifecycleTargetDriver::ObserveAllDomains(
    ICCLCLifecycleTargetDriverPorts& ports, const ComponentLifecycleTargetDTO& target,
    ComponentState& state, const ComponentLifecycleCurrentSnapshotDTO& current,
    const std::map<ControlDomainId, LifecycleBlockingCauseClassDTO>& retained,
    const bool record_causes) {
  // Each domain judges freshness on the clock read after its own observation.
  std::vector<DomainCondition> conditions;
  conditions.reserve(current.provider_occurrences.size());
  for (const auto& occurrence : current.provider_occurrences) {
    const auto stored = retained.find(occurrence.control_domain_id);
    auto observed = ObserveDomain(
        ports, target.controllable_component_id, occurrence, target.target_generation,
        stored == retained.end() ? LifecycleBlockingCauseClassDTO::kNone : stored->second,
        target.recovered_cause, record_causes, clock_);
    if (observed.retain) {
      RetainCause(state, target.controllable_component_id, occurrence.control_domain_id,
                  observed.cause, observed.observed_at_ns);
    }
    conditions.push_back({occurrence.control_domain_id, std::move(observed)});
  }
  return conditions;
}

Result<bool> CCLCLifecycleTargetDriver::WriteInterventionCause(
    const ControllableComponentId& component, const ControlDomainId& domain,
    const UnrecordedCause& cause) {
  try {
    return store_.RecordInterventionCause(component, domain, cause.cause_class,
                                          cause.observed_at_ns, cause.recovery_generation);
  } catch (const std::exception& exception) {
    return Result<bool>::Err(driver_error("lifecycle_cause_record_failed", exception.what(),
                                          "cause_record_failed"));
  } catch (...) {
    return Result<bool>::Err(driver_error("lifecycle_cause_record_failed",
                                          "recording raised an exception", "cause_record_failed"));
  }
}

void CCLCLifecycleTargetDriver::RetainCause(ComponentState& state,
                                            const ControllableComponentId& component,
                                            const ControlDomainId& domain,
                                            const LifecycleBlockingCauseClassDTO cause,
                                            const std::int64_t observed_at_ns) {
  const UnrecordedCause observed{cause, observed_at_ns, state.recovery_generation};
  const auto written = WriteInterventionCause(component, domain, observed);
  if (written.ok() || Stale(written.error())) {
    // Durable, or the operator's Recover was accepted after this pass read
    // its facts: the next pass re-observes on facts that include it.
    state.unrecorded_causes.erase(domain);
    if (!written.ok()) Wake();
    return;
  }
  // Fail closed: the class keeps blocking from memory, as if retained, and
  // its record is retried on every pass until it is durable.
  state.unrecorded_causes.insert_or_assign(domain, observed);
  state.cause_record_error = ReasonOf("intervention_cause_record_unavailable", written.error());
}

void CCLCLifecycleTargetDriver::RetryUnrecordedCauses(
    ComponentState& state, const ControllableComponentId& component,
    std::map<ControlDomainId, LifecycleBlockingCauseClassDTO>& retained) {
  state.cause_record_error.reset();
  for (auto pending = state.unrecorded_causes.begin(); pending != state.unrecorded_causes.end();) {
    const auto written = WriteInterventionCause(component, pending->first, pending->second);
    if (!written.ok() && Stale(written.error())) {
      // A Recover (or cancellation) since the observation covers it.
      pending = state.unrecorded_causes.erase(pending);
      Wake();
      continue;
    }
    // Durable now (after this pass's read) or still only in memory: either
    // way it is retained in this pass.
    retained.insert_or_assign(pending->first, pending->second.cause_class);
    if (written.ok()) {
      pending = state.unrecorded_causes.erase(pending);
      continue;
    }
    state.cause_record_error =
        ReasonOf("intervention_cause_record_unavailable", written.error());
    ++pending;
  }
}

Result<bool> CCLCLifecycleTargetDriver::WriteReached(
    const ComponentLifecycleTargetExpectationDTO& expected_target,
    const std::uint64_t recovery_generation) {
  try {
    return store_.RecordLifecycleTargetReached(expected_target, true, recovery_generation);
  } catch (const std::exception& exception) {
    return Result<bool>::Err(driver_error("lifecycle_reached_record_failed", exception.what(),
                                          "reached_record_failed"));
  } catch (...) {
    return Result<bool>::Err(driver_error("lifecycle_reached_record_failed",
                                          "recording raised an exception",
                                          "reached_record_failed"));
  }
}

const CCLCLifecycleTargetDriver::ContainmentOutcome& CCLCLifecycleTargetDriver::Contain(
    ICCLCLifecycleTargetDriverPorts& ports, const ComponentLifecycleIncidentDTO& incident) {
  // At most once per incident from this driver. A replacement driver that
  // finds the record still pending repeats the request, which the Kernel
  // replays exactly from the incident identity.
  if (const auto found = containments_.find(incident.incident_id); found != containments_.end()) {
    return found->second;
  }
  constexpr std::size_t kMaximumRememberedContainments = 1024U;
  if (containments_.size() >= kMaximumRememberedContainments) containments_.clear();
  Result<ComponentIncidentContainmentReceiptDTO> contained =
      Result<ComponentIncidentContainmentReceiptDTO>::Err(driver_error(
          "lifecycle_incident_containment_failed", "containment raised an exception",
          "containment_exception"));
  try {
    contained = ports.RequestIncidentContainment(
        {incident.incident_id, incident.controllable_component_id, incident.cause_class});
  } catch (...) {
  }
  ContainmentOutcome outcome;
  if (contained.ok()) {
    outcome.receipt = contained.value();
  } else {
    outcome.error = Bounded(ReasonOf("containment_refused", contained.error()));
  }
  return containments_.insert_or_assign(incident.incident_id, std::move(outcome)).first->second;
}

void CCLCLifecycleTargetDriver::RecordContainment(ICCLCLifecycleTargetDriverPorts& ports,
                                                  const ComponentIncidentRecordDTO& record,
                                                  const std::int64_t now_ns) {
  const auto& incident = record.incident;
  if (incident.containment_state != ComponentIncidentContainmentStateDTO::kPending) return;
  const auto& outcome = Contain(ports, incident);
  const auto state = outcome.receipt ? ComponentIncidentContainmentStateDTO::kRequested
                                     : ComponentIncidentContainmentStateDTO::kFailed;
  try {
    const auto record_json = ComponentIncidentRecordWithContainment(record.record_json, state,
                                                                    outcome.receipt, outcome.error);
    // Safe to leave unchecked: a failed write leaves the durable incident
    // open, blocking and `pending`, so every later pass writes it again with
    // the remembered outcome; `false` means the incident is no longer
    // pending (already recorded, or closed by the operator's Recover).
    static_cast<void>(
        store_.RecordComponentIncidentContainment(incident.incident_id, state, record_json, now_ns));
  } catch (...) {
    // The open incident stays durable and blocking; while its record lacks
    // the Stop reference it stays pending and a later pass writes it again.
  }
}

bool CCLCLifecycleTargetDriver::ApplyReplacesComponent(const ControllableComponentId& component) {
  // Fail closed: an unreadable Apply fact explains nothing, so the change is
  // contained like any other.
  const auto replaced = store_.ComponentInUnadoptedApplyPlan(component);
  return replaced.ok() && replaced.value();
}

void CCLCLifecycleTargetDriver::ContainAndRecordIncident(ICCLCLifecycleTargetDriverPorts& ports,
                                                         ComponentState& state,
                                                         const std::int64_t now_ns) {
  auto incident = *state.unrecorded_incident;
  // Containment never depends on, waits for or is delayed by the record. Its
  // outcome is remembered and written into the record by RecordContainment.
  static_cast<void>(Contain(ports, incident));
  // Malformed optional text never prevents the original fault record.
  if (incident.diagnostic && !ComponentIncidentDiagnosticIsValid(*incident.diagnostic))
    incident.diagnostic.reset();
  Result<ComponentIncidentRecordDTO> stored = Result<ComponentIncidentRecordDTO>::Err(
      driver_error("lifecycle_incident_invalid", "the incident record is invalid",
                   "incident_invalid"));
  try {
    if (incident.locally_valid()) {
      stored = store_.OpenComponentIncident(
          {incident, ComponentIncidentEventPayload(
                         incident,
                         LifecycleIncidentRecoveryPolicyFor(config_.incident_recovery_policy,
                                                            incident.cause_class),
                         kComponentIncidentEventKind, incident.diagnostic)},
          state.unrecorded_incident_generation);
    }
  } catch (const std::exception& exception) {
    stored = Result<ComponentIncidentRecordDTO>::Err(driver_error(
        "lifecycle_incident_record_failed", exception.what(), "incident_record_failed"));
  } catch (...) {
    stored = Result<ComponentIncidentRecordDTO>::Err(
        driver_error("lifecycle_incident_record_failed", "recording raised an exception",
                     "incident_record_failed"));
  }
  if (!stored.ok() && Stale(stored.error())) {
    // The operator's Recover was accepted after the facts this detection is
    // based on were read: it is not recorded. The containment already
    // requested stands; the next pass re-evaluates on facts that include the
    // Recover and opens a new incident if the condition still warrants one.
    state.unrecorded_incident.reset();
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, ComponentLifecycleIntentKindDTO::kRecover,
            incident.cause_class, std::string("lifecycle_facts_changed"), now_ns);
    Wake();
    return;
  }
  if (!stored.ok()) {
    // The cell is contained; progress stays blocked on the incident's cause
    // and the record is retried with the same identity on the next pass.
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, ComponentLifecycleIntentKindDTO::kRecover,
            incident.cause_class, ReasonOf("incident_record_unavailable", stored.error()),
            now_ns);
    return;
  }
  state.unrecorded_incident.reset();
  RecordContainment(ports, stored.value(), now_ns);
  PublishIncident(state, stored.value().incident, now_ns, stored.value().diagnostic);
}

bool CCLCLifecycleTargetDriver::EvaluateIncident(
    ICCLCLifecycleTargetDriverPorts& ports, const ComponentLifecycleTargetDTO& target,
    ComponentState& state, const ComponentLifecycleCurrentSnapshotDTO& current,
    const std::vector<DomainCondition>& conditions, std::optional<ComponentIncidentRecordDTO> open,
    const IncidentDetection detection,
    const std::optional<ComponentLifecycleIntentKindDTO> next_intent, const std::int64_t now_ns,
    const LifecycleBlockingCauseClassDTO unexplained) {
  // The condition this pass establishes: a freshly reported intervention
  // class first, then a retained one, then (only at the reached target or for
  // an open incident) a lost, stale or `availability` provider.
  auto intervention = LifecycleBlockingCauseClassDTO::kNone;
  bool intervention_fresh = false;
  bool lost = false;
  bool requested_controller_absence = false;
  const auto matches_target = [&](const ComponentLifecycleTargetExpectationDTO& expected) {
    return expected.controllable_component_id == target.controllable_component_id &&
           expected.target_generation == target.target_generation;
  };
  bool lifecycle_owned = false;
  bool lifecycle_transition = false;
  if (state.in_flight && matches_target(state.in_flight->expected_target)) {
    // Reuse the accepted transaction's own bound; an uncertain request or an
    // unreadable/expired status cannot create an unlimited absence window.
    try {
      const auto status = ports.LoadLifecycleStatus(state.in_flight->lifecycle_transaction_id);
      if (status.ok() && status.value()) {
        const auto& value = *status.value();
        const auto& accepted = value.acceptance;
        lifecycle_owned =
            accepted.lifecycle_transaction_id == state.in_flight->lifecycle_transaction_id &&
            value.accepted_intent.controllable_component_id == target.controllable_component_id &&
            value.accepted_intent.intent == state.in_flight->intent;
        lifecycle_transition = lifecycle_owned && !value.terminal_result &&
            accepted.accepted_at_ns >= 0 && accepted.timeout_duration_ns > 0 &&
            accepted.accepted_at_ns <= std::numeric_limits<std::int64_t>::max() - accepted.timeout_duration_ns &&
            clock_.now() < accepted.accepted_at_ns + accepted.timeout_duration_ns;
      }
    } catch (...) {
    }
  }
  std::vector<std::string> codes;
  std::optional<ControlDomainId> trigger;
  std::optional<ErrorDTO> diagnostic;
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot;
  for (const auto& condition : conditions) {
    const auto& observed = condition.observed;
    const bool fresh = observed.observation.has_value();
    // A retained class shown beside fresh status without it is not reported now.
    const bool reported_now = fresh && observed.reported == observed.cause;
    if (lifecycle_cause_requires_intervention(observed.cause) &&
        (reported_now ? !intervention_fresh : intervention == LifecycleBlockingCauseClassDTO::kNone)) {
      intervention = observed.cause;
      intervention_fresh = reported_now;
      trigger = condition.control_domain_id;
      diagnostic = reported_now ? observed.observation->diagnostic : std::nullopt;
      diagnostic_snapshot = reported_now ? observed.observation->diagnostic_snapshot : nullptr;
      codes = reported_now ? ComponentIncidentVendorCodes(observed.observation->facts)
                           : std::vector<std::string>{};
      continue;
    }
    const bool reset_transition = program_reset_position_scope_ &&
        MatchesProgramResetPositionScope(*program_reset_position_scope_, target, current) &&
        program_reset_position_scope_->step_deadline_ns &&
        clock_.now() < *program_reset_position_scope_->step_deadline_ns &&
        condition.control_domain_id == program_reset_position_scope_->request.domain.control_domain_id;
    // 150: a valid in-deadline Reset step owns expected availability of its
    // domain; genuine communication loss is reported by the Reset's own step
    // failure/deadline. Intervention classes above stay immediate.
    if (reset_transition &&
        (!fresh || observed.reported == LifecycleBlockingCauseClassDTO::kAvailability)) {
      requested_controller_absence = true;
      continue;
    }
    const auto boolean_fact = [&](std::string_view id, bool expected) {
      return fresh && std::any_of(observed.observation->facts.begin(),
                                 observed.observation->facts.end(), [&](const auto& fact) {
        return fact.lifecycle_fact_id.value() == id &&
               fact.value.kind == LifecycleFactValueKindDTO::kBoolean &&
               fact.value.boolean_value == expected;
      });
    };
    // 142: the accepted lifecycle transaction explains controller inactivity,
    // not transport loss, unloaded hardware or an independently reported fault.
    const bool expected_inactivity = lifecycle_transition && fresh &&
        observed.reported == LifecycleBlockingCauseClassDTO::kAvailability &&
        observed.cause == LifecycleBlockingCauseClassDTO::kAvailability &&
        observed.observation->observed_state == ComponentLifecycleStateDTO::kDegraded &&
        boolean_fact("external.controller_loaded", true) &&
        boolean_fact("external.transport_ready", true) &&
        boolean_fact("external.fault_present", false);
    requested_controller_absence = requested_controller_absence || expected_inactivity;
    if (!fresh || (observed.reported == LifecycleBlockingCauseClassDTO::kAvailability &&
                   !expected_inactivity)) {
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
  // Whether a loss is attributable follows the durable `reached` marker
  // (`kAny`), which a component retained unchanged across an Apply or an
  // in-session owner replacement keeps, never this driver's own history. A
  // freshly reported intervention class counts in every detecting mode.
  // Disable intentionally suppresses absence by default. Its accepted
  // transition still detects real loss after this same durable target had
  // previously reached readiness; first-Prepare availability stays waiting.
  const bool any_mode = open.has_value() || detection == IncidentDetection::kAny ||
                        (lifecycle_owned && target.reached);
  auto cause = intervention != LifecycleBlockingCauseClassDTO::kNone &&
                       (intervention_fresh || any_mode)
                   ? intervention
               : any_mode && lost ? LifecycleBlockingCauseClassDTO::kProviderLost
                                  : LifecycleBlockingCauseClassDTO::kNone;
  bool loss = cause == LifecycleBlockingCauseClassDTO::kProviderLost;
  // A verified loss the observations do not explain is still an incident.
  if (cause == LifecycleBlockingCauseClassDTO::kNone && any_mode &&
      unexplained != LifecycleBlockingCauseClassDTO::kNone &&
      (!requested_controller_absence ||
       current.component_lifecycle_state != ComponentLifecycleStateDTO::kDegraded)) {
    cause = unexplained;
    loss = true;
  }
  // Requested replacement (ADR-0026 §7): a component the current unadopted
  // Apply replaces or removes is starting over, so nothing about it is an
  // incident. Its loss is `availability` waiting; a reported intervention
  // class stays retained and blocking until the operator's Recover. A
  // component the Apply retains unchanged keeps its reached status and the
  // incident rules. Only the Apply facts end this, never time or status.
  const bool replaced = cause != LifecycleBlockingCauseClassDTO::kNone &&
                        ApplyReplacesComponent(target.controllable_component_id);
  if (replaced) cause = LifecycleBlockingCauseClassDTO::kNone;

  if (open) {
    // A guard/readiness incident can precede the launch's exit evidence. The
    // same owner keeps that first record; one later diagnostic is set once.
    const bool already_enriched =
        open->diagnostic_snapshot ||
        (state.progress && state.progress->incident &&
         state.progress->incident->incident_id == open->incident.incident_id &&
         state.progress->incident->diagnostic_snapshot);
    const bool already_has_error =
        open->diagnostic || (state.progress && state.progress->incident &&
                             state.progress->incident->incident_id == open->incident.incident_id &&
                             state.progress->incident->diagnostic);
    if (diagnostic && diagnostic->code == "controller_launch_exited" &&
        (diagnostic_snapshot ? !already_enriched : !already_has_error)) {
      // A replacement owner may find pending containment. Request it before
      // any new diagnostic read/write; auxiliary IO never gates the Stop.
      RecordContainment(ports, *open, now_ns);
      try {
        const auto appended = store_.AppendComponentIncidentDiagnostic(
            open->incident.incident_id, now_ns, *diagnostic, diagnostic_snapshot);
        if (appended.ok()) {
          const auto reloaded = store_.LoadOpenComponentIncident(target.controllable_component_id);
          if (reloaded.ok() && reloaded.value()) open = reloaded.value();
        }
      } catch (...) {
        // Containment and the primary incident are independent of evidence.
      }
    }
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
        } else if (updated.ok()) {
          // Closed by the operator's Recover since this pass read it.
          Wake();
        }
        // A failed write is retried: the cause stays retained (durably or in
        // memory), so every later pass finds it differing again.
      } catch (...) {
      }
    }
    RecordContainment(ports, *open, now_ns);
    PublishIncident(state, open->incident, now_ns, open->diagnostic, open->diagnostic_loaded);
    return true;
  }
  if (replaced && loss) {
    // Requalification continues without an incident; motion stays
    // unavailable behind the Apply. The readiness expectation only marks the
    // wait as delayed. Adoption of the Apply ends this window.
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
            next_intent ? next_intent : std::optional(NominalLifecycleIntentForTarget(target.requested_state)),
            LifecycleBlockingCauseClassDTO::kAvailability,
            std::string("apply_replacement_in_progress"), now_ns);
    return true;
  }
  if (cause == LifecycleBlockingCauseClassDTO::kNone) {
    return false;
  }

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
  incident.diagnostic = diagnostic;
  state.unrecorded_incident = std::move(incident);
  state.unrecorded_incident_generation = state.recovery_generation;
  ContainAndRecordIncident(ports, state, now_ns);
  return true;
}

bool CCLCLifecycleTargetDriver::FollowInFlight(ICCLCLifecycleTargetDriverPorts& ports,
                                               const std::optional<ComponentLifecycleTargetDTO>& target,
                                               ComponentState& state, const std::int64_t now_ns) {
  if (state.pending_intent) {
    const auto pending = *state.pending_intent;
    const auto expected = state.pending_target;
    const bool current = target && expected &&
        target->controllable_component_id == expected->controllable_component_id &&
        target->target_generation == expected->target_generation;
    if (current) {
      SubmitIntent(ports, state, pending, *expected, now_ns);
      return true;
    }
    // Absence/supersession never grants an effect replay, even on a lost reply.
    Result<std::optional<CCLCLifecycleRequestResultDTO>> observed =
        Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
            driver_error("lifecycle_request_observation_failed", "request observation raised an exception",
                         "request_observation_exception"));
    try { observed = ports.ReadLifecycleRequestResult(pending); } catch (...) {}
    if (!observed.ok() || !observed.value() || !expected ||
        (observed.value()->replay_status != RequestReplayStatusDTO::kAcceptedNew &&
         observed.value()->replay_status != RequestReplayStatusDTO::kReplayedExact) ||
        (observed.value()->acceptance.has_value() == observed.value()->rejection.has_value())) {
      Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain, pending.intent,
              LifecycleBlockingCauseClassDTO::kNone,
              observed.ok() ? std::string("retired_request_outcome_unavailable")
                            : ReasonOf("retired_request_outcome_unavailable", observed.error()), now_ns);
      return true;
    }
    ConsumeSubmissionResult(state, pending, *expected, *observed.value(), now_ns);
  }
  if (!state.in_flight) return false;
  const auto& in_flight = *state.in_flight;
  // Fresh interventions were checked before following the transaction;
  // ordinary in-flight progress keeps the last known class presented.
  const auto known_cause =
      state.progress ? state.progress->blocking_cause_class : LifecycleBlockingCauseClassDTO::kNone;
  const auto status = ports.LoadLifecycleStatus(in_flight.lifecycle_transaction_id);
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
            target && in_flight.expected_target.controllable_component_id == target->controllable_component_id &&
                in_flight.expected_target.target_generation == target->target_generation
                ? (uncertain ? std::optional<std::string>("reconciling_from_fresh_observation")
                             : std::nullopt)
                : std::optional<std::string>("superseded_step_settling"),
            now_ns);
    return true;
  }
  // CCLC has recorded a known outcome (after reconciliation when needed).
  const bool current = target && in_flight.expected_target.controllable_component_id == target->controllable_component_id &&
                in_flight.expected_target.target_generation == target->target_generation;
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

void CCLCLifecycleTargetDriver::SubmitIntent(ICCLCLifecycleTargetDriverPorts& ports,
                                             ComponentState& state,
                                             const ComponentLifecycleIntentDTO& intent,
                                             const ComponentLifecycleTargetExpectationDTO& expected_target,
                                             const std::int64_t now_ns) {
  state.pending_intent.reset();
  state.pending_target.reset();
  Result<CCLCLifecycleRequestResultDTO> result =
      Result<CCLCLifecycleRequestResultDTO>::Err(driver_error(
          "lifecycle_target_submit_failed", "lifecycle submission raised an exception",
          "submit_exception"));
  try {
    result = ports.SubmitTargetLifecycle(intent, expected_target);
  } catch (...) {
  }
  if (!result.ok()) {
    // Unknown whether CCLC admitted it: retain both original identities.
    // Exact effect replay is allowed only while that target remains current.
    state.pending_intent = intent;
    state.pending_target = expected_target;
    Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain, intent.intent,
            LifecycleBlockingCauseClassDTO::kNone,
            ReasonOf("submission_outcome_unknown", result.error()), now_ns);
    return;
  }
  ConsumeSubmissionResult(state, intent, expected_target, result.value(), now_ns);
}

void CCLCLifecycleTargetDriver::ConsumeSubmissionResult(
    ComponentState& state, const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleTargetExpectationDTO& expected_target,
    const CCLCLifecycleRequestResultDTO& value, const std::int64_t now_ns) {
  state.pending_intent.reset();
  state.pending_target.reset();
  if (value.acceptance && !value.rejection &&
      (value.replay_status == RequestReplayStatusDTO::kAcceptedNew ||
       value.replay_status == RequestReplayStatusDTO::kReplayedExact)) {
    state.in_flight = InFlight{value.acceptance->lifecycle_transaction_id, intent.intent,
                               expected_target};
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

bool CCLCLifecycleTargetDriver::EvaluateComponent(ICCLCLifecycleTargetDriverPorts& ports,
                                                  const ControllableComponentId& component,
                                                  ComponentState& state,
                                                  const std::int64_t now_ns) {
  // One consistent read of every durable fact this pass decides from, with
  // the recovery generation that fences this pass's writes against a
  // concurrent operator Recover. Unreadable facts never read as "nothing
  // retained" or "no incident": nothing progresses.
  Result<ComponentLifecycleTargetFactsDTO> facts = Result<ComponentLifecycleTargetFactsDTO>::Err(
      driver_error("lifecycle_target_facts_failed", "the facts read raised an exception",
                   "facts_read_exception"));
  try {
    facts = store_.LoadLifecycleTargetFacts(component);
  } catch (...) {
  }
  if (!facts.ok()) {
    auto known = state.progress ? state.progress->blocking_cause_class
                                : LifecycleBlockingCauseClassDTO::kNone;
    for (const auto& [domain, pending] : state.unrecorded_causes) {
      known = Worst(known, pending.cause_class);
    }
    Publish(state, state.pending_intent || state.in_flight
                ? LifecycleProgressPhaseDTO::kOutcomeUncertain : LifecycleProgressPhaseDTO::kBlocked,
            state.progress && state.progress->next_intent ? state.progress->next_intent
                : std::optional(NominalLifecycleIntentForTarget(*state.requested_state)),
            known, ReasonOf("lifecycle_facts_unavailable", facts.error()), now_ns);
    return true;
  }
  const auto& actual_target = facts.value().target;
  const bool retired = !actual_target || actual_target->target_generation != state.target_generation;
  state.recovery_generation = facts.value().recovery_generation;

  // Retry what earlier passes observed but could not record (fail closed).
  // An intervention class keeps blocking from memory until it is durable; a
  // stale refusal means the operator's Recover covers it.
  std::map<ControlDomainId, LifecycleBlockingCauseClassDTO> retained;
  for (const auto& cause : facts.value().intervention_causes) {
    retained[cause.control_domain_id] = cause.cause_class;
  }
  RetryUnrecordedCauses(state, component, retained);
  bool reached_observed = !retired && state.unrecorded_reached.has_value();
  if (state.unrecorded_reached) {
    // While the marker is not durable the component is classified as having
    // reached its target: a later loss is then an incident (contained, and
    // only the operator's Recover proceeds) rather than ordinary waiting
    // that would progress on its own. A Recover since (stale) reset it.
    const auto written = WriteReached({component, state.target_generation}, *state.unrecorded_reached);
    if (!written.ok() && Stale(written.error())) {
      const bool target_retired = ReachedTargetRetired(written.error());
      state.unrecorded_reached.reset();
      if (target_retired) state.retired_reached_observation = true;
      reached_observed = false;
      Wake();
      if (target_retired && !retired) {
        // The atomic writer proves these earlier target facts are obsolete.
        // Stop this pass before provider observation or old-target pursuit.
        Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain,
                NominalLifecycleIntentForTarget(*state.requested_state),
                state.progress ? state.progress->blocking_cause_class : LifecycleBlockingCauseClassDTO::kNone,
                "target_retired:retired_target_reached_observation_not_applied", now_ns);
        return true;
      }
    } else if (written.ok()) {
      state.unrecorded_reached.reset();
    }
  }

  // An unsettled lifecycle request still owns its exact transaction, but
  // cannot hide a current intervention until its terminal response arrives.
  // Do not retain classes while that request settles; its normal postcondition
  // and Recover suppression remain the existing owner's responsibility.
  if ((state.in_flight || state.pending_intent) && actual_target) {
    const auto pending_current = ports.ReadCurrentComponent(component);
    if (pending_current.ok()) {
      auto pending_target = *actual_target;
      if (reached_observed) pending_target.reached = true;
      const auto pending_detection =
          pending_target.requested_state == LifecycleTargetStateDTO::kDisabled
              ? IncidentDetection::kNone
          : pending_target.reached ? IncidentDetection::kAny
          : pending_target.recovered_cause ? IncidentDetection::kNewCause
                                            : IncidentDetection::kNone;
      const auto pending_conditions = ObserveAllDomains(
          ports, pending_target, state, pending_current.value(), retained, false);
      if (EvaluateIncident(ports, pending_target, state, pending_current.value(),
                           pending_conditions, facts.value().open_incident,
                           pending_detection, std::nullopt, now_ns)) {
        return true;
      }
    }
  }
  if (FollowInFlight(ports, actual_target, state, now_ns)) return true;
  if (retired) {
    if (state.unrecorded_incident) {
      ContainAndRecordIncident(ports, state, now_ns);
      return true;
    }
    if (facts.value().open_incident) {
      PublishIncident(state, facts.value().open_incident->incident, now_ns,
                      facts.value().open_incident->diagnostic,
                      facts.value().open_incident->diagnostic_loaded);
      return true;
    }
    if (!state.unrecorded_causes.empty() || state.unrecorded_reached ||
        !ports.RetiredTargetWorkSettled(component)) {
      const auto intent = state.progress && state.progress->next_intent ? state.progress->next_intent
          : std::optional(NominalLifecycleIntentForTarget(*state.requested_state));
      Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain, intent,
              state.progress ? state.progress->blocking_cause_class : LifecycleBlockingCauseClassDTO::kNone,
              state.cause_record_error.value_or("retired_target_evidence_settling"), now_ns);
      return true;
    }
    // Cancelled here means retirement of target pursuit, never robot work.
    if (!actual_target) {
      Publish(state, LifecycleProgressPhaseDTO::kCancelled, std::nullopt,
              LifecycleBlockingCauseClassDTO::kNone,
              state.retired_reached_observation
                  ? "target_retired:retired_target_reached_observation_not_applied"
                  : "target_retired:original_work_settled", now_ns);
      std::optional<bool> configured;
      try { configured = ports.ConfiguredComponentMembership(component); } catch (...) {}
      return !configured.has_value() || *configured;
    }
    // The successor gets fresh pursuit state only after the original settles.
    state = ComponentState{};
    state.target_generation = actual_target->target_generation;
    state.recovery_generation = facts.value().recovery_generation;
  }
  auto target = *actual_target;
  state.requested_state = target.requested_state;
  if (reached_observed) {
    target.reached = true;
    target.recovered_cause.reset();
  }

  auto retained_worst = LifecycleBlockingCauseClassDTO::kNone;
  for (const auto& [domain, cause] : retained) retained_worst = Worst(retained_worst, cause);

  // An open incident blocks everything until the operator's Recover.
  const auto& open = facts.value().open_incident;
  if (state.unrecorded_incident) {
    if (!open) {
      // A detected incident is already contained; only its record is retried.
      ContainAndRecordIncident(ports, state, now_ns);
      return true;
    }
    // Another record is open for the component; the open incident governs.
    state.unrecorded_incident.reset();
  }
  const auto snapshot = ports.ReadCurrentComponent(component);
  if (!snapshot.ok()) {
    if (open) {
      PublishIncident(state, open->incident, now_ns, open->diagnostic, open->diagnostic_loaded);
      return true;
    }
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
            NominalLifecycleIntentForTarget(target.requested_state), retained_worst,
            ReasonOf("component_snapshot_unavailable", snapshot.error()), now_ns);
    return true;
  }
  const auto& current = snapshot.value();
  const auto achieved = current.component_lifecycle_state;
  const auto next = NextLifecycleIntentTowardTarget(achieved, target.requested_state);
  // While any CCLC transaction of the component is unsettled no newly
  // reported class is retained (see ObserveDomain) and no step is submitted.
  const bool settling = ports.ComponentLifecycleSettling(component);
  // Losses are incidents only after the component reached its target on
  // fresh status in this session (the durable marker, kept across in-session
  // owner replacement and for a component an Apply retains unchanged); while
  // recovering from an incident only a newly reported intervention class is.
  // Before that, `availability` is ordinary waiting.
  const bool following = target.requested_state != LifecycleTargetStateDTO::kDisabled;
  const auto detection = !following ? IncidentDetection::kNone
                         : target.reached ? IncidentDetection::kAny
                         : target.recovered_cause ? IncidentDetection::kNewCause
                                                  : IncidentDetection::kNone;
  if (open) {
    // Always true with an open incident: it published the incident.
    static_cast<void>(
        EvaluateIncident(ports, target, state, current,
                         ObserveAllDomains(ports, target, state, current, retained, !settling),
                         open, detection, next, now_ns));
    return true;
  }
  if (program_reset_position_scope_ &&
      program_reset_position_scope_->request.component == component) {
    auto& scope = *program_reset_position_scope_;
    if (!MatchesProgramResetPositionScope(scope, target, current) ||
        (scope.step_deadline_ns && clock_.now() >= *scope.step_deadline_ns)) scope.valid = false;
    const auto conditions = ObserveAllDomains(ports, target, state, current, retained, !settling);
    const auto unexplained = achieved == ComponentLifecycleStateDTO::kFaulted ||
                                     achieved == ComponentLifecycleStateDTO::kDegraded
                                 ? LifecycleBlockingCauseClassDTO::kUnknown
                                 : LifecycleBlockingCauseClassDTO::kNone;
    if (EvaluateIncident(ports, target, state, current, conditions, std::nullopt, detection, next,
                         now_ns, unexplained)) {
      scope.valid = false;
      return true;
    }
    Publish(
        state, LifecycleProgressPhaseDTO::kTransitionInProgress, next, retained_worst,
        scope.valid ? "program_reset_position_in_progress" : "program_reset_position_scope_expired",
        now_ns);
    return true;
  }
  // A fresh intervention from the exact current occurrence is catastrophic
  // in every lifecycle state (129). Availability still needs the durable
  // reached marker. Reuse this one observation pass for plan eligibility too;
  // a missing plan catalog cannot hide an explicitly reported intervention.
  const auto conditions =
      ObserveAllDomains(ports, target, state, current, retained, !settling);
  const bool verified_loss = achieved == ComponentLifecycleStateDTO::kDegraded ||
                             achieved == ComponentLifecycleStateDTO::kFaulted;
  if (EvaluateIncident(ports, target, state, current, conditions, std::nullopt,
                       detection, next, now_ns,
                       verified_loss ? LifecycleBlockingCauseClassDTO::kUnknown
                                     : LifecycleBlockingCauseClassDTO::kNone)) {
    return true;
  }
  if (!next) {
    state.failed_basis.reset();
    state.failure_reason.reset();
    // A component at its target is still observed, read only, so that a
    // safety stop, fault or lost controller after it became ready stays
    // visible in its progress (spec 530 lifecycle blocking causes). Nothing
    // is submitted; an unchanged class writes nothing.
    auto cause = current.provider_occurrences.empty() ? retained_worst
                                                      : LifecycleBlockingCauseClassDTO::kNone;
    bool all_fresh_without_cause = !conditions.empty();
    for (const auto& condition : conditions) {
      cause = Worst(cause, condition.observed.cause);
      all_fresh_without_cause = all_fresh_without_cause &&
                                condition.observed.observation.has_value() &&
                                condition.observed.reported == LifecycleBlockingCauseClassDTO::kNone &&
                                condition.observed.cause == LifecycleBlockingCauseClassDTO::kNone;
    }
    // First time at the target on fresh status without a reported or
    // retained cause: from now on a loss or an unexpected change is an
    // incident (and a recovered incident is over).
    std::optional<std::string> reached_reason;
    if (following && !target.reached && all_fresh_without_cause) {
      const auto written = WriteReached({component, target.target_generation}, state.recovery_generation);
      if (!written.ok() && ReachedTargetRetired(written.error())) {
        state.retired_reached_observation = true;
        Publish(state, LifecycleProgressPhaseDTO::kOutcomeUncertain, std::nullopt, cause,
                "target_retired:retired_target_reached_observation_not_applied", now_ns);
        Wake();
        return true;
      } else if (!written.ok() && Stale(written.error())) {
        // Reached on facts a Recover has since replaced: re-evaluate.
        Wake();
      } else if (!written.ok()) {
        // Kept and retried; meanwhile classified as reached (see above).
        state.unrecorded_reached = state.recovery_generation;
        reached_reason = ReasonOf("reached_record_unavailable", written.error());
      }
    } else if (state.unrecorded_reached) {
      reached_reason = std::string("reached_record_unavailable");
    }
    Publish(state, LifecycleProgressPhaseDTO::kSatisfied, std::nullopt, cause,
            std::move(reached_reason), now_ns);
    return true;
  }
  if (achieved == ComponentLifecycleStateDTO::kUndeclared) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next, retained_worst,
            std::string("component_undeclared"), now_ns);
    return true;
  }
  if (achieved == ComponentLifecycleStateDTO::kDisabling) {
    Publish(state, LifecycleProgressPhaseDTO::kTransitionInProgress, *next, retained_worst,
            std::string("disabling_in_progress"), now_ns);
    return true;
  }

  // One fresh observation per provider occurrence, read only.
  std::vector<BrandLifecyclePlanCatalogDTO> catalogs;
  std::vector<std::optional<BrandLifecycleObservationDTO>> observations;
  AttemptBasis basis{{}, 0U, current.snapshot_digest.value()};
  auto cause = LifecycleBlockingCauseClassDTO::kNone;
  bool all_fresh = true;
  std::optional<std::string> unavailable_reason;
  for (std::size_t index = 0U; index < current.provider_occurrences.size(); ++index) {
    const auto& occurrence = current.provider_occurrences[index];
    auto catalog = ports.LoadLifecyclePlanCatalog(occurrence);
    if (!catalog.ok()) {
      Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next,
              LifecycleBlockingCauseClassDTO::kConfiguration,
              ReasonOf("lifecycle_catalog_unavailable", catalog.error()), now_ns);
      return true;
    }
    catalogs.push_back(std::move(catalog.value()));
    const auto& observed = conditions[index].observed;
    basis.observation_generations.push_back(observed.generation);
    if (!observed.observation) {
      all_fresh = false;
      if (!unavailable_reason) unavailable_reason = observed.unavailable_reason;
    }
    cause = Worst(cause, observed.cause);
    observations.push_back(observed.observation);
  }

  // A retained class without a fresh report still blocks ordinary progress
  // until Recover. Fresh interventions already went through the incident
  // path above, including when Disable is the requested target.
  if (lifecycle_cause_requires_intervention(cause) &&
      *next != ComponentLifecycleIntentKindDTO::kDisable) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next, cause,
            !state.unrecorded_causes.empty() && state.cause_record_error
                ? *state.cause_record_error
                : std::string("intervention_required:") + std::string(to_string(cause)),
            now_ns);
    return true;
  }
  if (!all_fresh) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            unavailable_reason, now_ns);
    return true;
  }

  const auto admission = ports.ReadCellAdmission();
  basis.admission_generation = admission.admission_generation.value();
  if (state.failures >= config_.maximum_attempts_per_step) {
    Publish(state, LifecycleProgressPhaseDTO::kBlocked, *next, cause,
            "attempts_exhausted" +
                (state.failure_reason ? ":" + *state.failure_reason : std::string()),
            now_ns);
    return true;
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
    return true;
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
      return true;
    }
    requested_state = selected.value();
  }
  for (std::size_t index = 0U; index < catalogs.size(); ++index) {
    const auto decision =
        EvaluateLifecycleDomainPlan(*next, requested_state, catalogs[index], *observations[index]);
    if (!decision.ok()) {
      Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
              ReasonOf("prerequisites_unmet", decision.error()), now_ns);
      return true;
    }
  }
  // Known no-admission conditions are waited out without a request, so a
  // waiting component leaves no rejection records behind.
  if (!admission.authoring_allowed || !admission.admission_generation.nonzero()) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            std::string("cell_admission_closed"), now_ns);
    return true;
  }
  if (!ports.AcceptingLifecycleRequests()) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            std::string("cclc_admission_closed"), now_ns);
    return true;
  }
  if (settling) {
    Publish(state, LifecycleProgressPhaseDTO::kWaitingForPrerequisites, *next, cause,
            std::string("component_lifecycle_in_progress"), now_ns);
    return true;
  }

  std::vector<ExpectedControlDomainRevisionDTO> expected_domains;
  expected_domains.reserve(current.domains.size());
  for (const auto& domain : current.domains) {
    ExpectedActiveControlConfigurationDTO configuration{
        ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
    if (const auto active = domain.configuration_identity()) {
      configuration = {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
                       active->active_control_configuration_id,
                       active->active_control_configuration_generation};
    }
    if (const auto* provider = domain.provider()) {
      expected_domains.emplace_back(
          domain.control_domain_id, domain.control_domain_revision,
          ExpectedProviderDomainScopeDTO{provider->provider, configuration});
      continue;
    }
    expected_domains.push_back({domain.control_domain_id, domain.control_domain_revision,
                                domain.controller().controller_topology_id,
                                domain.controller().topology_revision, configuration,
                                domain.control_bindings()});
  }
  ComponentLifecycleIntentDTO intent{
      {LifecycleTargetRequestId(component, target.target_generation, state.step, *next,
                                state.attempt),
       ports.ReadRequestOwnerEpoch(), std::string(kLifecycleTargetSourceLabel),
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
  SubmitIntent(ports, state, intent, {component, target.target_generation}, now_ns);
  if (state.progress && state.progress->phase == LifecycleProgressPhaseDTO::kTransitionInProgress) {
    state.progress->blocking_cause_class = cause;
  }
  return true;
}

Result<void> CCLCLifecycleTargetDriver::CheckNoOpenIncidents() {
  // A pass evaluates local state copies before publishing components_. Keep
  // that existing owner transition coherent with the durable all-row query.
  // The Kernel calls this without its State mutex; no Kernel/admission lock
  // is held over the journal read or an in-progress containment call.
  std::lock_guard run_lock(run_mutex_);
  const auto open = store_.HasOpenIncident();
  if (!open.ok()) return Result<void>::Err(open.error());
  if (open.value())
    return Result<void>::Err(driver_error("incident_open",
        "A component or cell-observer incident remains open", "incident_open"));
  std::vector<std::pair<ControllableComponentId, std::uint64_t>> unrecorded;
  {
    std::lock_guard state_lock(state_mutex_);
    for (const auto& [component, state] : components_)
      if (state.unrecorded_incident)
        unrecorded.emplace_back(component, state.unrecorded_incident_generation);
  }
  for (const auto& [component, generation] : unrecorded) {
    const auto facts = store_.LoadLifecycleTargetFacts(component);
    if (!facts.ok()) return Result<void>::Err(facts.error());
    if (facts.value().recovery_generation == generation)
      return Result<void>::Err(driver_error("incident_open",
          "A component incident is not yet durably recorded", "incident_open"));
  }
  return Result<void>::Ok();
}

Result<ComponentLifecycleReadinessTargetFactsDTO>
CCLCLifecycleTargetDriver::ReadReadinessFacts(const ControllableComponentId& component) {
  auto facts = store_.LoadLifecycleTargetFacts(component);
  if (!facts.ok()) return Result<ComponentLifecycleReadinessTargetFactsDTO>::Err(facts.error());
  ComponentLifecycleReadinessTargetFactsDTO result{std::move(facts.value()), std::nullopt};
  if (result.facts.open_incident) {
    const auto& incident = *result.facts.open_incident;
    result.blocker = incident.diagnostic.value_or(driver_error(
        "component_incident_open", incident.incident.summary, "incident_open"));
    result.blocker->details.emplace("cause_class", std::string(to_string(incident.incident.cause_class)));
  }
  for (const auto& cause : result.facts.intervention_causes) {
    if (!result.blocker && lifecycle_cause_requires_intervention(cause.cause_class))
      result.blocker = driver_error("intervention_required", "the component retains an intervention cause",
                                   std::string(to_string(cause.cause_class)));
  }
  {
    std::lock_guard lock(state_mutex_);
    if (!result.blocker && program_reset_position_scope_ &&
        program_reset_position_scope_->request.component == component)
      result.blocker = driver_error("program_reset_position_in_progress",
                                    "the component position Reset has not completed",
                                    "position_reset_scope_open");
    const auto found = components_.find(component);
    if (found != components_.end() && !result.blocker) {
      const auto& state = found->second;
      if (state.unrecorded_incident &&
          state.unrecorded_incident_generation == result.facts.recovery_generation) {
        result.blocker = driver_error("component_incident_open", state.unrecorded_incident->summary,
                                     "incident_not_yet_recorded");
      } else {
        for (const auto& [domain, cause] : state.unrecorded_causes) {
          static_cast<void>(domain);
          if (cause.recovery_generation == result.facts.recovery_generation &&
              lifecycle_cause_requires_intervention(cause.cause_class)) {
            result.blocker = driver_error("intervention_required",
                state.cause_record_error.value_or("the component retains an unrecorded intervention cause"),
                std::string(to_string(cause.cause_class)));
            break;
          }
        }
      }
    }
  }
  return Result<ComponentLifecycleReadinessTargetFactsDTO>::Ok(std::move(result));
}

Result<ComponentLifecycleTargetViewDTO> CCLCLifecycleTargetDriver::ReadTargetView(
    const ControllableComponentId& component) {
  auto target = store_.LoadLifecycleTarget(component);
  if (!target.ok()) return Result<ComponentLifecycleTargetViewDTO>::Err(target.error());
  // Owner-clock times are full-width nonnegative int64; a negative reading is
  // invalid, never clamped into range.
  const auto now = clock_.now();
  if (now < 0) {
    return Result<ComponentLifecycleTargetViewDTO>::Err(driver_error(
        "lifecycle_target_clock_invalid", "the owner clock is negative", "owner_clock_invalid"));
  }
  ComponentLifecycleTargetViewDTO view;
  if (config_.read_output_drop_counts) {
    try {
      if (const auto counts = config_.read_output_drop_counts(component)) {
        view.dropped_output_lines = counts->first;
        view.dropped_output_bytes = counts->second;
      }
    } catch (...) {
      // Optional observation never breaks lifecycle or control status.
    }
  }
  view.target = target.value();
  {
    std::lock_guard lock(state_mutex_);
    const auto found = components_.find(component);
    // The original pursuit's actual progress stays visible while its work
    // settles, including after a target is absent or superseded.
    if (found != components_.end() && found->second.progress) {
      view.progress = found->second.progress;
      // `delayed` is a function of the owner clock; reads never publish.
      if (view.progress->waiting_since_ns) {
        view.progress->delayed =
            now - *view.progress->waiting_since_ns > config_.readiness_expectation_ns;
      }
    }
  }
  if (!view.progress && view.target) {
    // Recorded but not yet evaluated: waiting from now on.
    view.progress = ComponentLifecycleProgressDTO{
        LifecycleProgressPhaseDTO::kWaitingForPrerequisites,
        NominalLifecycleIntentForTarget(view.target->requested_state),
        LifecycleBlockingCauseClassDTO::kNone,
        std::string("not_yet_evaluated"),
        now,
        false,
        std::nullopt};
  }
  // The incident is read from its durable record, so a Recover or a newly
  // opened incident is visible before the next evaluation pass.
  const auto open = store_.LoadOpenComponentIncident(component);
  if (!open.ok()) return Result<ComponentLifecycleTargetViewDTO>::Err(open.error());
  if (open.value()) {
    const auto& incident = open.value()->incident;
    if (incident.detected_at_ns < 0) {
      return Result<ComponentLifecycleTargetViewDTO>::Err(
          driver_error("lifecycle_incident_record_invalid",
                       "the open incident has a negative detection time", "incident_time_invalid"));
    }
    if (!view.progress) {
      view.progress = ComponentLifecycleProgressDTO{
          LifecycleProgressPhaseDTO::kBlocked, ComponentLifecycleIntentKindDTO::kRecover,
          incident.cause_class, std::nullopt, incident.detected_at_ns, false, std::nullopt};
    }
    view.progress->phase = LifecycleProgressPhaseDTO::kBlocked;
    view.progress->next_intent = ComponentLifecycleIntentKindDTO::kRecover;
    view.progress->blocking_cause_class = incident.cause_class;
    view.progress->blocking_reason = Bounded(
        "incident_open:" + std::string(to_string(incident.cause_class)) + ":" + incident.summary +
        (open.value()->diagnostic ? " " + open.value()->diagnostic->message : ""));
    if (!view.progress->waiting_since_ns) {
      view.progress->waiting_since_ns = incident.detected_at_ns;
    }
    view.progress->incident = incident;
  } else if (view.progress && view.progress->incident) {
    // Recovered since the last pass: the driver re-evaluates next.
    view.progress->incident.reset();
    view.progress->phase = LifecycleProgressPhaseDTO::kWaitingForPrerequisites;
    view.progress->blocking_reason = "incident_recovered";
  }
  return Result<ComponentLifecycleTargetViewDTO>::Ok(std::move(view));
}

namespace {

[[nodiscard]] bool OperatorRecover(const ComponentLifecycleIntentDTO& intent) {
  return intent.intent == ComponentLifecycleIntentKindDTO::kRecover &&
         intent.request_context.source_label != kLifecycleTargetSourceLabel;
}

}  // namespace

Result<std::optional<CCLCLifecycleRequestResultDTO>> CCLCLifecycleTargetDriver::RecoverIncident(
    const ComponentLifecycleIntentDTO& intent) {
  using Value = std::optional<CCLCLifecycleRequestResultDTO>;
  if (!OperatorRecover(intent) || !intent.locally_valid()) {
    if (intent.close_incident_recovery)
      return Result<Value>::Err(driver_error("component_lifecycle_close_recovery_invalid",
          "Close recovery is not an operator incident recovery", "close_recovery_invalid"));
    // Ordinary CCLC admission decides (and rejects an invalid one).
    return Result<Value>::Ok(std::nullopt);
  }
  auto result = Result<Value>::Ok(std::nullopt);
  const auto pinned = ports_.WithPinnedProduct([&](ICCLCLifecycleTargetDriverPorts& ports) {
    const auto recover = [&]() -> Result<Value> {
      const auto now = clock_.now();
      if (now < 0) {
        return Result<Value>::Err(driver_error("lifecycle_incident_clock_invalid",
                                               "the owner clock is negative",
                                               "incident_clock_invalid"));
      }
      // Authorization only: the journal checks the request identity, owner epoch
      // and replay, records the Recover and closes the incident; no vendor
      // primitive is dispatched here, so it is admissible while the cell is
      // latched. The policy-permitted steps follow through ordinary admission.
      auto recovered = store_.RecoverComponentIncident(
          ports.ReadRequestOwnerEpoch(), intent,
          intent.attribution.principal_id.value_or(intent.request_context.source_label), now);
      if (recovered.ok() && !recovered.value() && intent.close_incident_recovery)
        return Result<Value>::Err(driver_error("component_lifecycle_close_incident_mismatch",
            "Close recovery has no exact open incident", "close_incident_mismatch"));
      if (recovered.ok() && recovered.value()) Wake();
      return recovered;
    };
    result = recover();
  });
  return pinned.ok() ? result : Result<Value>::Err(pinned.error());
}

Result<std::vector<ComponentIncidentEventDTO>> CCLCLifecycleTargetDriver::LoadIncidentEvents(
    const std::uint64_t after_sequence, const std::size_t limit) {
  return store_.LoadComponentIncidentEvents(after_sequence, limit);
}

}  // namespace manyforge::core
