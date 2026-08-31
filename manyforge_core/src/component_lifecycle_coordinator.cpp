#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core {
namespace {

inline constexpr std::size_t kMaxLifecycleWorkerCount = 64U;
inline constexpr std::size_t kMaxKernelWorkerCount = 16U;
inline constexpr std::size_t kMaxCoordinatorActiveTransactions = 64U;
inline constexpr std::size_t kMaximumAdmissionResolutionAttempts = 3U;

template <typename Function>
class ScopeExit final {
 public:
  explicit ScopeExit(Function function) : function_(std::move(function)) {}
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ~ScopeExit() { function_(); }

 private:
  Function function_;
};

template <typename Function>
ScopeExit(Function) -> ScopeExit<Function>;

[[nodiscard]] ErrorDTO coordinator_error(std::string code, std::string message,
                                         std::string reason) {
  return {std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

[[nodiscard]] Result<std::uint64_t> NextEffectSequence(std::uint64_t current) {
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    return Result<std::uint64_t>::Err(coordinator_error(
        "component_lifecycle_effect_sequence_exhausted",
        "the lifecycle transaction effect sequence is exhausted", "effect_sequence_overflow"));
  }
  return Result<std::uint64_t>::Ok(current + 1U);
}

[[nodiscard]] ErrorDTO replay_error(RequestReplayStatusDTO status) {
  switch (status) {
    case RequestReplayStatusDTO::kRequestResultExpired:
      return coordinator_error("component_lifecycle_request_result_expired",
                               "the lifecycle request membership is retained without its result",
                               "request_result_expired");
    case RequestReplayStatusDTO::kRequestIdConflict:
      return coordinator_error("component_lifecycle_request_id_conflict",
                               "the request ID is already bound to different lifecycle intent",
                               "request_id_conflict");
    case RequestReplayStatusDTO::kRequestLedgerFull:
      return coordinator_error("request_ledger_full", "the bounded CCLC request membership is full",
                               "request_membership_capacity_exhausted");
    case RequestReplayStatusDTO::kStaleRequestOwnerEpoch:
      return coordinator_error("stale_request_owner_epoch",
                               "the lifecycle request targets a stale CCLC owner epoch",
                               "request_owner_epoch_stale");
    case RequestReplayStatusDTO::kAcceptedNew:
    case RequestReplayStatusDTO::kReplayedExact:
      break;
  }
  return coordinator_error("component_lifecycle_replay_invalid",
                           "the lifecycle journal returned an invalid replay disposition",
                           "unexpected_replay_status");
}

[[nodiscard]] CCLCDomainReservationOwnerDTO reservation_owner(
    const ComponentLifecycleAcceptedDTO& acceptance, const ComponentLifecycleIntentDTO& intent) {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          intent.controllable_component_id,
          acceptance.lifecycle_transaction_id,
          acceptance.lifecycle_transaction_generation,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] CCLCDomainReservationOwnerDTO reservation_owner(
    const RouteSelectionAcceptedDTO& acceptance) {
  std::vector<ControllableComponentId> components;
  components.reserve(acceptance.component_expectations.size());
  for (const auto& expectation : acceptance.component_expectations) {
    components.push_back(expectation.controllable_component_id);
  }
  return CCLCDomainReservationOwnerDTO(RouteSelectionDomainReservationOwnerDTO{
      acceptance.route_selection_transaction_id, acceptance.route_selection_transaction_generation,
      std::move(components)});
}

[[nodiscard]] bool ControllerSourceEpochOnlyChanged(
    const ControllerLifecycleObservationQueryDTO& accepted,
    const ControllerLifecycleObservationQueryDTO& current) {
  auto normalized = accepted;
  normalized.source_runtime_epoch = current.source_runtime_epoch;
  return accepted.source_runtime_epoch != current.source_runtime_epoch &&
         normalized == current;
}

class FixedWorkerPool final {
 public:
  explicit FixedWorkerPool(std::size_t capacity) : capacity_(capacity) {}

  FixedWorkerPool(const FixedWorkerPool&) = delete;
  FixedWorkerPool& operator=(const FixedWorkerPool&) = delete;

  ~FixedWorkerPool() { Stop(); }

  [[nodiscard]] bool Start(std::size_t worker_count) {
    std::vector<std::jthread> partial_workers;
    bool start_failed = false;
    {
      std::lock_guard lock(mutex_);
      if (started_ || worker_count == 0U || worker_count > kMaxLifecycleWorkerCount) {
        return false;
      }
      started_ = true;
      accepting_ = true;
      try {
        workers_.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
          workers_.emplace_back([this](std::stop_token cancellation) { Run(cancellation); });
        }
      } catch (...) {
        start_failed = true;
        accepting_ = false;
        for (auto& worker : workers_) {
          worker.request_stop();
        }
        partial_workers.swap(workers_);
        started_ = false;
      }
    }
    if (start_failed) {
      work_ready_.notify_all();
      partial_workers.clear();
      return false;
    }
    return true;
  }

  template <typename Task>
  [[nodiscard]] bool Submit(Task&& task) noexcept {
    try {
      std::function<void(std::stop_token)> owned_task(std::forward<Task>(task));
      std::lock_guard lock(mutex_);
      if (!accepting_ || !owned_task || queued_.size() >= capacity_) {
        return false;
      }
      queued_.push_back(std::move(owned_task));
    } catch (...) {
      return false;
    }
    work_ready_.notify_one();
    return true;
  }

  void StopAccepting() noexcept {
    {
      std::lock_guard lock(mutex_);
      accepting_ = false;
    }
    work_ready_.notify_all();
  }

  void Stop() noexcept {
    std::vector<std::jthread> workers;
    {
      std::lock_guard lock(mutex_);
      if (!started_) {
        return;
      }
      accepting_ = false;
      for (auto& worker : workers_) {
        worker.request_stop();
      }
      workers.swap(workers_);
      started_ = false;
    }
    work_ready_.notify_all();
    workers.clear();
    drained_.notify_all();
  }

  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout) {
    std::unique_lock lock(mutex_);
    return drained_.wait_for(lock, timeout, [&] { return queued_.empty() && running_ == 0U; });
  }

 private:
  void Run(std::stop_token cancellation) noexcept {
    while (!cancellation.stop_requested()) {
      std::function<void(std::stop_token)> task;
      {
        std::unique_lock lock(mutex_);
        work_ready_.wait(lock, cancellation, [&] { return !queued_.empty() || !accepting_; });
        if (cancellation.stop_requested()) {
          return;
        }
        if (queued_.empty()) {
          if (!accepting_) {
            return;
          }
          continue;
        }
        task = std::move(queued_.front());
        queued_.pop_front();
        ++running_;
      }
      try {
        task(cancellation);
      } catch (...) {
        // Transaction tasks contain their own typed failure boundary. A final
        // catch prevents one malformed adapter from terminating the process.
      }
      {
        std::lock_guard lock(mutex_);
        --running_;
        if (queued_.empty() && running_ == 0U) {
          drained_.notify_all();
        }
      }
    }
  }

  const std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable_any work_ready_;
  std::condition_variable drained_;
  bool started_ = false;
  bool accepting_ = false;
  std::size_t running_ = 0U;
  std::deque<std::function<void(std::stop_token)>> queued_;
  std::vector<std::jthread> workers_;
};

[[nodiscard]] std::int64_t deadline_at(const ComponentLifecycleStatusDTO& status) noexcept {
  if (status.acceptance.accepted_at_ns >
      std::numeric_limits<std::int64_t>::max() - status.acceptance.timeout_duration_ns) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return status.acceptance.accepted_at_ns + status.acceptance.timeout_duration_ns;
}

[[nodiscard]] std::optional<std::int64_t> disable_plan_timeout(
    const BrandLifecyclePlanCatalogDTO& catalog) noexcept {
  if (!catalog.locally_valid()) {
    return std::nullopt;
  }
  const auto match = std::find_if(catalog.plans.begin(), catalog.plans.end(), [](const auto& plan) {
    return plan.intent == ComponentLifecycleIntentKindDTO::kDisable;
  });
  if (match == catalog.plans.end() ||
      std::find_if(std::next(match), catalog.plans.end(), [](const auto& plan) {
        return plan.intent == ComponentLifecycleIntentKindDTO::kDisable;
      }) != catalog.plans.end()) {
    return std::nullopt;
  }
  return match->timeout_duration_ns;
}

[[nodiscard]] std::int64_t checked_schedule_time(std::int64_t now_ns,
                                                 std::int64_t delay_ns) noexcept {
  if (now_ns < 0 || delay_ns <= 0 || now_ns > std::numeric_limits<std::int64_t>::max() - delay_ns) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return now_ns + delay_ns;
}

[[nodiscard]] ComponentLifecycleDomainEffectDTO append_domain_effect(
    ComponentLifecycleDomainEffectDTO previous, const BrandLifecyclePrimitiveResultDTO& result) {
  if (result.dispatch_state == LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain ||
      result.effect == LifecyclePrimitiveEffectDTO::kPossible) {
    return ComponentLifecycleDomainEffectDTO::kEffectUncertain;
  }
  if (result.effect == LifecyclePrimitiveEffectDTO::kObserved) {
    return ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
  }
  if (previous == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
      previous == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved) {
    return previous;
  }
  return ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
}

enum class UncertainEffectResolution : std::uint8_t {
  kUnresolved = 0,
  kKnownNoEffect,
  kPartialEffectObserved,
  kRequestedEffectObserved,
};

[[nodiscard]] UncertainEffectResolution classify_uncertain_effect(
    const LifecyclePrimitiveTransitionDTO& transition,
    const ComponentLifecycleDomainStatusDTO& retained_domain,
    const std::optional<BrandLifecycleObservationDTO>& pre_dispatch_observation,
    const BrandLifecycleObservationDTO& fresh_observation,
    LifecyclePostconditionResultDTO postcondition) noexcept {
  if (postcondition == LifecyclePostconditionResultDTO::kSatisfied) {
    return UncertainEffectResolution::kRequestedEffectObserved;
  }
  if (postcondition != LifecyclePostconditionResultDTO::kNotSatisfied) {
    return UncertainEffectResolution::kUnresolved;
  }
  if (pre_dispatch_observation.has_value()) {
    return fresh_observation.observed_state == pre_dispatch_observation->observed_state &&
                   fresh_observation.facts == pre_dispatch_observation->facts
               ? UncertainEffectResolution::kKnownNoEffect
               : UncertainEffectResolution::kPartialEffectObserved;
  }
  if (retained_domain.current_state.has_value() &&
      fresh_observation.observed_state != *retained_domain.current_state) {
    return UncertainEffectResolution::kPartialEffectObserved;
  }
  // Recovery does not invent a pre-dispatch fact snapshot. State-only plans
  // can still prove no semantic change from the retained state; fact-bearing
  // plans remain fenced until an owning adapter supplies conclusive evidence.
  if (transition.eligibility_predicate.fact_predicates.empty() &&
      transition.postcondition_predicate.fact_predicates.empty() &&
      retained_domain.current_state.has_value() &&
      fresh_observation.observed_state == *retained_domain.current_state) {
    return UncertainEffectResolution::kKnownNoEffect;
  }
  return UncertainEffectResolution::kUnresolved;
}

[[nodiscard]] bool handoff_pair_matches_request(const DisableHandoffRequestDTO& request,
                                                const DisableHandoffAcceptedDTO& acceptance,
                                                const DisableHandoffStatusDTO& status) noexcept {
  return acceptance.matches(request) && DisableHandoffPreservesOrWidensScope(request, status) &&
         status.lifecycle_transaction_id == acceptance.lifecycle_transaction_id &&
         status.lifecycle_transaction_generation == acceptance.lifecycle_transaction_generation &&
         status.controllable_component_id == acceptance.controllable_component_id &&
         status.scope_resolution == acceptance.scope_resolution &&
         status.component_scope_digest == acceptance.component_scope_digest &&
         status.configuration_fence_id == acceptance.configuration_fence_id &&
         status.configuration_fence_generation == acceptance.configuration_fence_generation;
}

[[nodiscard]] bool snapshot_is_within_fence(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot,
    const ConfigurationFenceScopeDTO& fence_scope) noexcept {
  if (!snapshot.locally_valid() || !fence_scope.valid() ||
      !std::binary_search(fence_scope.controllable_component_ids.begin(),
                          fence_scope.controllable_component_ids.end(),
                          snapshot.controllable_component_id)) {
    return false;
  }
  for (const auto& domain : snapshot.domains) {
    const auto fenced_domain = std::lower_bound(
        fence_scope.domains.begin(), fence_scope.domains.end(), domain.control_domain_id,
        [](const auto& candidate, const auto& id) { return candidate.control_domain_id < id; });
    if (fenced_domain == fence_scope.domains.end() ||
        fenced_domain->control_domain_id != domain.control_domain_id ||
        fenced_domain->predecessor_control_domain_revision !=
            std::optional(domain.control_domain_revision)) {
      return false;
    }
    for (const auto& binding : domain.current_control_bindings) {
      const auto fenced_binding = std::lower_bound(
          fence_scope.bindings.begin(), fence_scope.bindings.end(), binding.control_binding_id,
          [](const auto& candidate, const auto& id) { return candidate.control_binding_id < id; });
      if (fenced_binding == fence_scope.bindings.end() ||
          fenced_binding->control_binding_id != binding.control_binding_id ||
          fenced_binding->predecessor_control_binding_revision !=
              std::optional(binding.control_binding_revision)) {
        return false;
      }
    }
    if (!std::includes(fence_scope.resources.begin(), fence_scope.resources.end(),
                       domain.current_resources.begin(), domain.current_resources.end())) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool terminal(const ComponentLifecycleStatusDTO& status) noexcept {
  return status.phase == ComponentLifecycleTransactionPhaseDTO::kSucceeded ||
         status.phase == ComponentLifecycleTransactionPhaseDTO::kFailed;
}

}  // namespace

bool ComponentLifecycleCoordinatorConfig::locally_valid() const noexcept {
  return !request_owner_epoch.empty() &&
         request_owner_epoch.value().size() <= kMaxLifecycleIdentityBytes &&
         request_membership_capacity > 0U &&
         request_membership_capacity <= kMaximumCCLCRequestMembership &&
         lifecycle_worker_count > 0U && lifecycle_worker_count <= kMaxLifecycleWorkerCount &&
         kernel_worker_count > 0U && kernel_worker_count <= kMaxKernelWorkerCount &&
         observation_poll_period_ns > 0 &&
         observation_poll_period_ns <= kMaxLifecycleTransactionTimeoutNs &&
         containment_only_disable_timeout_duration_ns > 0 &&
         containment_only_disable_timeout_duration_ns <= kMaxLifecycleTransactionTimeoutNs &&
         route_selection_timeout_duration_ns > 0 &&
         route_selection_timeout_duration_ns <= kMaxRouteSelectionTimeoutNs;
}

bool ComponentLifecycleCoordinatorRouteServices::locally_valid() const noexcept {
  return current_snapshot_source != nullptr && controller_lifecycle_port_resolver != nullptr &&
         kernel_route_fence_port != nullptr && configuration_publication_port != nullptr;
}

class ComponentLifecycleCoordinator::Impl final {
 public:
  Impl(ComponentLifecycleCoordinatorConfig config, ICCLCJournal& journal,
       CCLCDomainReservationTable& domain_reservations,
       const ICCLCCurrentComponentSnapshotSource& current_snapshot_source,
       const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source,
       const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver,
       ICCLCKernelDisableHandoffPort& kernel_disable_handoff_port, IClock& clock,
       IScheduler& scheduler,
       IKernelCellAdmissionPort* kernel_cell_admission_port,
       std::optional<ComponentLifecycleCoordinatorRouteServices> route_services)
      : config_(std::move(config)),
        journal_(journal),
        domain_reservations_(domain_reservations),
        current_snapshot_source_(current_snapshot_source),
        lifecycle_plan_catalog_source_(lifecycle_plan_catalog_source),
        lifecycle_port_resolver_(lifecycle_port_resolver),
        kernel_disable_handoff_port_(kernel_disable_handoff_port),
        clock_(clock),
        scheduler_(scheduler),
        kernel_cell_admission_port_(
            kernel_cell_admission_port != nullptr
                ? kernel_cell_admission_port
                : dynamic_cast<IKernelCellAdmissionPort*>(
                      &kernel_disable_handoff_port)),
        route_services_(std::move(route_services)),
        worker_pool_(kMaxCoordinatorActiveTransactions),
        kernel_worker_pool_(kMaxCoordinatorActiveTransactions) {}

  ~Impl() { Shutdown(); }

  [[nodiscard]] Result<void> Start() {
    if (!config_.locally_valid() || kernel_cell_admission_port_ == nullptr ||
        (route_services_.has_value() && !route_services_->locally_valid())) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_coordinator_configuration_invalid",
          "the CCLC coordinator configuration is invalid", "configuration_invalid"));
    }
    {
      std::lock_guard lock(state_mutex_);
      if (started_ || starting_) {
        return Result<void>::Err(
            coordinator_error("component_lifecycle_coordinator_already_started",
                              "the CCLC coordinator has already started", "start_repeated"));
      }
      starting_ = true;
      try {
        active_transactions_.reserve(kMaxCoordinatorActiveTransactions);
        active_route_transactions_.reserve(kMaxCoordinatorActiveTransactions);
        component_generation_watermarks_.reserve(kMaxCoordinatorActiveTransactions);
        active_configuration_generation_watermarks_.reserve(kMaxCoordinatorActiveTransactions);
      } catch (...) {
        starting_ = false;
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_runtime_capacity_unavailable",
            "the bounded lifecycle runtime table could not reserve its fixed capacity",
            "runtime_capacity_allocation_failed"));
      }
    }
    if (!worker_pool_.Start(config_.lifecycle_worker_count)) {
      std::lock_guard lock(state_mutex_);
      starting_ = false;
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_worker_start_failed",
          "the bounded lifecycle worker pool could not start", "worker_start_failed"));
    }
    if (!kernel_worker_pool_.Start(config_.kernel_worker_count)) {
      worker_pool_.Stop();
      std::lock_guard lock(state_mutex_);
      starting_ = false;
      return Result<void>::Err(
          coordinator_error("component_lifecycle_kernel_worker_start_failed",
                            "the bounded Kernel lifecycle worker pool could not start",
                            "kernel_worker_start_failed"));
    }

    std::int64_t now_ns = -1;
    try {
      now_ns = clock_.now();
    } catch (...) {
      Shutdown();
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_clock_invalid",
          "the injected lifecycle clock raised during owner startup", "clock_exception"));
    }
    if (now_ns < 0) {
      Shutdown();
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_clock_invalid",
          "the injected lifecycle clock returned a negative time", "clock_negative"));
    }
    Result<CCLCJournalRecoveryDTO> recovered =
        Result<CCLCJournalRecoveryDTO>::Err(coordinator_error(
            "component_lifecycle_journal_recovery_failed",
            "the CCLC journal owner transition raised an exception", "journal_exception"));
    try {
      recovered = journal_.BeginOwnerEpoch(config_.request_owner_epoch, now_ns,
                                           config_.request_membership_capacity);
    } catch (...) {
    }
    const bool current_owner_interrupted =
        recovered.ok() &&
        std::any_of(recovered.value().interrupted_transactions.begin(),
                    recovered.value().interrupted_transactions.end(), [&](const auto& status) {
                      return status.status_owner_epoch == config_.request_owner_epoch;
                    });
    if (!recovered.ok() || !recovered.value().locally_valid() || current_owner_interrupted) {
      Shutdown();
      return Result<void>::Err(
          recovered.ok()
              ? coordinator_error("component_lifecycle_journal_recovery_invalid",
                                  "the CCLC journal returned an invalid recovery set",
                                  current_owner_interrupted ? "current_owner_interrupted_status"
                                                            : "recovery_shape_invalid")
              : recovered.error());
    }
    for (auto& row : recovered.value().lifecycle_claims) {
      Result<CellAdmissionRecoveryReceiptDTO> receipt =
          Result<CellAdmissionRecoveryReceiptDTO>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "Kernel predecessor claim receipt raised an exception",
              "claim_receipt_exception"));
      try {
        receipt = kernel_cell_admission_port_->GetCellAdmissionRecoveryReceipt(
            {row.intent, row.claim, std::nullopt});
      } catch (...) {
      }
      std::optional<CellAdmissionClaimDTO> recovered_claim = row.claim;
      if (!recovered_claim.has_value() && receipt.ok() &&
          receipt.value().status ==
              CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
          receipt.value().matched_claim.has_value() &&
          receipt.value().matched_claim->intent == row.intent) {
        recovered_claim = receipt.value().matched_claim;
        if (!journal_
                 .RecordLifecycleClaimAcquired(config_.request_owner_epoch,
                                               *recovered_claim)
                 .ok()) {
          Shutdown();
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "recovered lifecycle claim could not be journaled",
              "claim_recovery_journal_failed"));
        }
        row.claim = recovered_claim;
      }
      if (!recovered_claim.has_value()) {
        if (!receipt.ok() ||
            receipt.value().status !=
                CellAdmissionRecoveryReceiptStatusDTO::kUnknown ||
            !journal_
                 .RecordLifecycleClaimNotAcquired(
                     config_.request_owner_epoch, row.intent)
                 .ok()) {
          Shutdown();
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "lifecycle acquire attempt could not be proven absent",
              "claim_acquire_unresolved"));
        }
        continue;
      }
      if (row.lifecycle_accepted && !row.release_intent.has_value()) {
        if (!receipt.ok() ||
            receipt.value().status !=
                CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
            receipt.value().matched_claim != recovered_claim) {
          Shutdown();
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "accepted lifecycle transaction no longer has its exact held claim",
              "accepted_claim_not_held"));
        }
        continue;
      }
      std::optional<CellAdmissionClaimReleaseDTO> recovery_release =
          row.release_intent;
      if (!row.lifecycle_accepted && !recovery_release.has_value()) {
        recovery_release = CellAdmissionClaimReleaseDTO{
            row.intent.owner_role,
            row.intent.owner_epoch,
            row.intent.owner_transaction_id,
            row.intent.request_id,
            row.intent.request_payload_digest,
            recovered_claim->acquired_admission_generation,
            recovered_claim->claim_generation,
            {CellAdmissionSettlementKindDTO::kProvenNoEffect,
             row.intent.request_payload_digest}};
        if (!journal_
                 .RecordUnacceptedLifecycleClaimRelease(
                     config_.request_owner_epoch, *recovery_release)
                 .ok()) {
          Shutdown();
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "unaccepted lifecycle claim release could not be journaled",
              "claim_release_journal_failed"));
        }
      }
      if (receipt.ok() &&
          receipt.value().status ==
              CellAdmissionRecoveryReceiptStatusDTO::kHeldExact) {
        try {
          static_cast<void>(kernel_cell_admission_port_
                                ->ReleaseCellAdmissionClaim(*recovery_release));
          receipt =
              kernel_cell_admission_port_->GetCellAdmissionRecoveryReceipt(
                  {row.intent, row.claim, std::nullopt});
        } catch (...) {
        }
      }
      if (!receipt.ok() ||
          (receipt.value().status !=
               CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact &&
           receipt.value().status !=
               CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact) ||
          !journal_
               .RecordLifecycleClaimReleaseReceipt(
                   config_.request_owner_epoch, *recovery_release,
                   receipt.value())
               .ok()) {
        Shutdown();
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_claim_recovery_unknown",
            "predecessor lifecycle release could not be proven exact",
            "claim_release_unresolved"));
      }
    }
    try {
      std::lock_guard lock(admission_mutex_);
      highest_transaction_generation_ = recovered.value().highest_lifecycle_transaction_generation;
      highest_route_transaction_generation_ =
          recovered.value().highest_route_selection_transaction_generation;
      for (const auto& watermark : recovered.value().component_watermarks) {
        component_generation_watermarks_.emplace_back(
            watermark.controllable_component_id.value(),
            watermark.component_lifecycle_generation.value());
      }
      for (const auto& watermark : recovered.value().active_configuration_watermarks) {
        active_configuration_generation_watermarks_.emplace_back(
            watermark.control_domain_id.value(),
            watermark.active_control_configuration_generation.value());
      }
    } catch (...) {
      Shutdown();
      return Result<void>::Err(
          coordinator_error("component_lifecycle_runtime_capacity_unavailable",
                            "the durable lifecycle generation watermarks could not be restored",
                            "recovery_watermark_allocation_failed"));
    }
    {
      std::lock_guard lock(state_mutex_);
      starting_ = false;
      started_ = true;
    }
    if (!recovered.value().interrupted_route_selections.empty() && !route_services_.has_value()) {
      Shutdown();
      return Result<void>::Err(coordinator_error(
          "route_selection_recovery_services_unavailable",
          "the journal contains interrupted route work but the route owner is not composed",
          "route_services_required_for_recovery"));
    }
    if (!recovered.value().interrupted_transactions.empty()) {
      std::vector<std::shared_ptr<RuntimeState>> recovered_runtimes;
      std::vector<std::pair<CCLCDomainReservationOwnerDTO, std::vector<ControlDomainId>>>
          restored_reservations;
      bool restoration_committed = false;
      const ScopeExit release_uncommitted_reservations([&] {
        if (restoration_committed) {
          return;
        }
        for (const auto& restored : restored_reservations) {
          static_cast<void>(domain_reservations_.release(restored.first, restored.second));
        }
      });
      std::size_t restored_owner_wide_gates = 0U;
      try {
        // Preserve the fixed post-recovery capacity promised by Start(). Moving
        // this vector into active_transactions_ must not replace its reserved
        // capacity with only the number of interrupted transactions: the next
        // durable admission is not allowed to allocate after journal commit.
        recovered_runtimes.reserve(kMaxCoordinatorActiveTransactions);
        restored_reservations.reserve(recovered.value().interrupted_transactions.size());
        for (std::size_t index = 0U; index < recovered.value().interrupted_transactions.size();
             ++index) {
          const auto& status = recovered.value().interrupted_transactions[index];
          std::vector<ControlDomainId> domains;
          for (const auto& domain : status.domains) {
            if (domain.current_scope.has_value()) {
              domains.push_back(domain.control_domain_id);
            }
          }
          const auto owner = reservation_owner(status.acceptance, status.accepted_intent);
          if (!domains.empty()) {
            const auto reserved = domain_reservations_.reserve(owner, domains);
            if (!reserved.ok()) {
              Shutdown();
              return Result<void>::Err(reserved.error());
            }
            try {
              restored_reservations.emplace_back(owner, domains);
            } catch (...) {
              static_cast<void>(domain_reservations_.release(owner, domains));
              throw;
            }
          }
          auto runtime = std::make_shared<RuntimeState>(status, std::nullopt, false, std::nullopt,
                                                        std::move(domains));
          const auto recovered_claim = std::find_if(
              recovered.value().lifecycle_claims.begin(),
              recovered.value().lifecycle_claims.end(), [&](const auto& row) {
                return row.intent.owner_transaction_id ==
                       status.acceptance.lifecycle_transaction_id.value();
              });
          if (recovered_claim == recovered.value().lifecycle_claims.end() ||
              !recovered_claim->claim.has_value()) {
            Shutdown();
            return Result<void>::Err(coordinator_error(
                "component_lifecycle_claim_recovery_missing",
                "interrupted lifecycle transaction has no exact Kernel claim",
                "claim_recovery_missing"));
          }
          runtime->cell_claim = recovered_claim->claim;
          runtime->cell_claim_release = recovered_claim->release_intent;
          runtime->recovering = true;
          // An interrupted transaction cannot regain mutation budget under a
          // replacement owner. All successor rows therefore retain zero
          // remaining duration, including reconciliation and fence release.
          runtime->effective_deadline_ns = runtime->status.accepted_at_ns;
          runtime->effect_sequence =
              recovered.value().effect_sequence_watermarks[index].highest_effect_sequence;
          const auto unresolved = std::find_if(recovered.value().unresolved_effect_attempts.begin(),
                                               recovered.value().unresolved_effect_attempts.end(),
                                               [&](const auto& attempt) {
                                                 return attempt.lifecycle_transaction_id ==
                                                        status.acceptance.lifecycle_transaction_id;
                                               });
          if (unresolved != recovered.value().unresolved_effect_attempts.end()) {
            runtime->unresolved_recovery_effect = *unresolved;
          }
          runtime->owner_wide_containment_gate =
              runtime->permanent_containment_only ||
              (runtime->status.disable_handoff.has_value() &&
               runtime->status.disable_handoff->scope_resolution ==
                   DisableHandoffScopeResolutionDTO::kPromotedToFull);
          if (runtime->owner_wide_containment_gate) {
            ++restored_owner_wide_gates;
          }
          recovered_runtimes.push_back(std::move(runtime));
        }

        {
          std::lock_guard lock(admission_mutex_);
          owner_wide_containment_gate_count_ = restored_owner_wide_gates;
        }
        {
          std::lock_guard lock(state_mutex_);
          active_transactions_ = recovered_runtimes;
          recovery_in_progress_ = true;
        }
      } catch (...) {
        Shutdown();
        return Result<void>::Err(
            coordinator_error("component_lifecycle_runtime_capacity_unavailable",
                              "the interrupted lifecycle runtime state could not be restored",
                              "recovery_runtime_allocation_failed"));
      }
      restoration_committed = true;
      for (const auto& runtime : recovered_runtimes) {
        const bool kernel_lane =
            runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable ||
            (runtime->unresolved_recovery_effect.has_value() &&
             runtime->unresolved_recovery_effect->effect_kind !=
                 CCLCEffectKindDTO::kBrandLifecyclePrimitive);
        auto& pool = kernel_lane ? kernel_worker_pool_ : worker_pool_;
        bool submitted = false;
        try {
          submitted = pool.Submit(
              [this, runtime](std::stop_token cancellation) { Process(runtime, cancellation); });
        } catch (...) {
        }
        if (!submitted) {
          CloseAdmission();
          return Result<void>::Ok();
        }
      }
      if (recovered.value().interrupted_route_selections.empty()) {
        return Result<void>::Ok();
      }
    }
    if (!recovered.value().interrupted_route_selections.empty()) {
      const auto restored = RestoreRouteRecovery(recovered.value());
      if (!restored.ok()) {
        Shutdown();
        return restored;
      }
      return Result<void>::Ok();
    }
    if (recovered.value().interrupted_transactions.empty()) {
      if (!RotateLifecycleOwnerEpoch()) {
        Shutdown();
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_owner_epoch_reconciliation_failed",
            "Kernel lifecycle-owner epoch could not be rotated",
            "owner_epoch_reconciliation_failed"));
      }
      accepting_.store(true, std::memory_order_release);
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) {
    if (!intent.locally_valid()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_intent_invalid",
          "the lifecycle intent is not a closed valid request", "intent_invalid"));
    }
    {
      std::lock_guard lock(state_mutex_);
      if (!started_) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_coordinator_not_started",
                              "the CCLC coordinator has not started", "start_required"));
      }
    }

    Result<std::optional<CCLCLifecycleRequestResultDTO>> inspected =
        Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(coordinator_error(
            "component_lifecycle_journal_inspection_failed",
            "the lifecycle journal inspection raised an exception", "journal_exception"));
    try {
      inspected = journal_.InspectLifecycleRequest(config_.request_owner_epoch, intent);
    } catch (...) {
    }
    if (!inspected.ok()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(inspected.error());
    }
    if (inspected.value().has_value()) {
      return ReplayResult(*inspected.value());
    }
    std::uint64_t admission_gate_generation = 0U;
    {
      std::lock_guard lock(admission_mutex_);
      if (!accepting_.load(std::memory_order_acquire)) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_admission_closed",
                              "the CCLC coordinator is not accepting new lifecycle work",
                              "coordinator_not_accepting"));
      }
      admission_gate_generation = admission_gate_generation_;
    }
    if (!accepting_.load(std::memory_order_acquire)) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_admission_closed",
          "the CCLC coordinator is not accepting new lifecycle work", "coordinator_not_accepting"));
    }

    for (std::size_t attempt = 0U; attempt < kMaximumAdmissionResolutionAttempts; ++attempt) {
      const bool force_full_disable = intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
                                      attempt + 1U == kMaximumAdmissionResolutionAttempts;
      const auto facts = ReadAdmissionFacts(intent, force_full_disable);
      if (!facts.ok()) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(facts.error());
      }
      const auto admitted = CommitAdmission(intent, facts.value(), admission_gate_generation);
      if (admitted.ok()) {
        return admitted;
      }
      if (admitted.error().code != "component_lifecycle_snapshot_changed_during_admission") {
        return admitted;
      }
      if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(admitted.error());
      }
    }
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        coordinator_error("component_lifecycle_admission_unstable",
                          "the current component scope could not be stabilized for admission",
                          "admission_resolution_attempts_exhausted"));
  }

  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id) {
    try {
      return journal_.LoadRetainedLifecycleStatus(lifecycle_transaction_id);
    } catch (...) {
      return Result<std::optional<ComponentLifecycleStatusDTO>>::Err(coordinator_error(
          "component_lifecycle_journal_status_load_failed",
          "the lifecycle journal status lookup raised an exception", "journal_exception"));
    }
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent) {
    if (!intent.locally_valid()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_intent_invalid",
                            "the route intent is not a closed valid request", "intent_invalid"));
    }
    {
      std::lock_guard lock(state_mutex_);
      if (!started_) {
        return Result<CCLCRouteSelectionRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_coordinator_not_started",
                              "the CCLC coordinator has not started", "start_required"));
      }
    }
    if (!route_services_.has_value()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_unsupported", "the CCLC route owner is not composed",
                            "route_services_unavailable"));
    }

    Result<std::optional<CCLCRouteSelectionRequestResultDTO>> inspected =
        Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(coordinator_error(
            "route_selection_journal_inspection_failed",
            "the route journal inspection raised an exception", "journal_exception"));
    try {
      inspected = journal_.InspectRouteSelectionRequest(config_.request_owner_epoch, intent);
    } catch (...) {
    }
    if (!inspected.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(inspected.error());
    }
    if (inspected.value().has_value()) {
      return ReplayRouteResult(*inspected.value());
    }

    std::uint64_t admission_gate_generation = 0U;
    {
      std::lock_guard lock(admission_mutex_);
      if (!accepting_.load(std::memory_order_acquire)) {
        return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
            "route_selection_admission_closed",
            "the CCLC coordinator is not accepting new route work", "coordinator_not_accepting"));
      }
      admission_gate_generation = admission_gate_generation_;
    }
    return CommitRouteAdmission(intent, admission_gate_generation);
  }

  [[nodiscard]] Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id) {
    try {
      return journal_.LoadRetainedRouteSelectionStatus(route_selection_transaction_id);
    } catch (...) {
      return Result<std::optional<RouteSelectionStatusDTO>>::Err(coordinator_error(
          "route_selection_journal_status_load_failed",
          "the route journal status lookup raised an exception", "journal_exception"));
    }
  }

  [[nodiscard]] bool accepting_new_requests() const noexcept {
    return accepting_.load(std::memory_order_acquire);
  }

  [[nodiscard]] RequestOwnerEpoch request_owner_epoch() const {
    return config_.request_owner_epoch;
  }

  void CloseAdmission() noexcept {
    std::unique_lock lock(admission_mutex_);
    CloseAdmissionLocked();
    admission_resolution_ready_.wait(lock, [&] { return !admission_persistence_active_; });
  }

  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!worker_pool_.DrainFor(timeout)) {
      return false;
    }
    const auto after_lifecycle = std::chrono::steady_clock::now();
    if (after_lifecycle >= deadline ||
        !kernel_worker_pool_.DrainFor(
            std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - after_lifecycle))) {
      return false;
    }
    std::unique_lock lock(state_mutex_);
    return state_changed_.wait_until(lock, deadline, [&] {
      return active_transactions_.empty() && active_route_transactions_.empty();
    });
  }

 private:
  [[nodiscard]] bool RotateLifecycleOwnerEpoch() noexcept {
    const auto probe_digest = ContentDigest(
        "sha256:0000000000000000000000000000000000000000000000000000000000000000");
    const auto reconciliation_digest =
        CellAdmissionOwnerEpochReconciliationEvidenceDigest(
            CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
            config_.request_owner_epoch);
    if (!reconciliation_digest.ok()) {
      return false;
    }
    const CellAdmissionClaimIntentDTO probe{
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        config_.request_owner_epoch,
        "cclc-owner-epoch-probe",
        RequestId("cclc-owner-epoch-probe"),
        probe_digest,
        AdmissionGeneration(1U),
        {},
        std::nullopt,
        std::nullopt};
    Result<CellAdmissionRecoveryReceiptDTO> receipt =
        Result<CellAdmissionRecoveryReceiptDTO>::Err(coordinator_error(
            "component_lifecycle_owner_epoch_reconciliation_failed",
            "Kernel lifecycle-owner receipt raised an exception",
            "recovery_receipt_exception"));
    try {
      receipt = kernel_cell_admission_port_->GetCellAdmissionRecoveryReceipt(
          {probe, std::nullopt, std::nullopt});
    } catch (...) {
    }
    if (!receipt.ok()) {
      return false;
    }
    auto owners = receipt.value().current_owner_epochs;
    std::erase_if(owners, [](const auto& owner) {
      return owner.owner_role ==
             CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
    });
    owners.push_back({CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
                      config_.request_owner_epoch});
    std::sort(owners.begin(), owners.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.owner_role < rhs.owner_role;
    });
    const CellAdmissionReconcileDTO reconcile{
        receipt.value().kernel_owner_epoch,
        receipt.value().held_claims,
        std::move(owners),
        reconciliation_digest.value()};
    try {
      return kernel_cell_admission_port_
          ->ReconcileCellAdmissionClaims(reconcile)
          .ok();
    } catch (...) {
      return false;
    }
  }

  struct AdmissionFacts final {
    std::optional<ComponentLifecycleCurrentSnapshotDTO> current_snapshot;
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs;
    std::optional<DisableHandoffAdmissionSnapshotDTO> disable_admission;
  };

  struct RuntimeState final {
    RuntimeState(ComponentLifecycleStatusDTO initial_status,
                 std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> accepted_catalogs,
                 bool accepted_lifecycle_mutation_eligible,
                 std::optional<DisableHandoffAdmissionSnapshotDTO> accepted_disable_admission,
                 std::vector<ControlDomainId> accepted_reserved_domains)
        : status(std::move(initial_status)),
          qualified_catalogs(std::move(accepted_catalogs)),
          lifecycle_mutation_eligible(accepted_lifecycle_mutation_eligible),
          disable_admission(std::move(accepted_disable_admission)),
          reserved_domains(std::move(accepted_reserved_domains)),
          effective_deadline_ns(deadline_at(status)),
          ports(status.domains.size()),
          observations(status.domains.size()),
          decisions(status.domains.size()) {
      permanent_containment_only =
          status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
          std::none_of(status.domains.begin(), status.domains.end(),
                       [](const auto& domain) { return domain.current_scope.has_value(); });
    }

    ComponentLifecycleStatusDTO status;
    std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> qualified_catalogs;
    bool lifecycle_mutation_eligible;
    std::optional<DisableHandoffAdmissionSnapshotDTO> disable_admission;
    std::vector<ControlDomainId> reserved_domains;
    std::int64_t effective_deadline_ns;
    std::vector<std::shared_ptr<IBrandLifecyclePort>> ports;
    std::vector<std::optional<BrandLifecycleObservationDTO>> observations;
    std::vector<std::optional<LifecycleDomainPlanDecision>> decisions;
    std::size_t current_domain_index = 0U;
    std::uint64_t effect_sequence = 0U;
    std::optional<std::size_t> verifying_domain_index;
    std::optional<LifecyclePrimitiveTransitionDTO> verifying_transition;
    std::optional<CCLCEffectAttemptDTO> unresolved_recovery_effect;
    std::optional<CellAdmissionClaimDTO> cell_claim;
    std::optional<CellAdmissionClaimReleaseDTO> cell_claim_release;
    std::uint64_t expected_observation_generation = 0U;
    bool initialized = false;
    bool lifecycle_lane_active = false;
    bool release_scheduled = false;
    bool release_retry_scheduled = false;
    bool recovering = false;
    bool permanent_containment_only = false;
    bool owner_wide_containment_gate = false;
    bool unresolved_effect_admission_gate = false;
    bool scheduled = false;
  };

  struct RouteRuntimeState final {
    RouteRuntimeState(
        RouteSelectionStatusDTO initial_status, RouteSelectionCurrentSnapshot accepted_snapshot,
        std::optional<RouteConfigurationFenceAdmissionSnapshotDTO> accepted_fence_admission,
        std::vector<ControlDomainId> accepted_reserved_domains)
        : status(std::move(initial_status)),
          snapshot(std::move(accepted_snapshot)),
          fence_admission(std::move(accepted_fence_admission)),
          reserved_domains(std::move(accepted_reserved_domains)),
          effective_deadline_ns(status.accepted_at_ns + status.acceptance.timeout_duration_ns) {}

    RouteSelectionStatusDTO status;
    RouteSelectionCurrentSnapshot snapshot;
    std::optional<RouteConfigurationFenceAdmissionSnapshotDTO> fence_admission;
    std::vector<ControlDomainId> reserved_domains;
    std::shared_ptr<IControllerLifecyclePort> controller_port;
    std::uint64_t effect_sequence{};
    std::optional<CCLCRouteEffectAttemptDTO> unresolved_recovery_effect;
    std::int64_t effective_deadline_ns{};
    bool recovering{};
    bool scheduled{};
    bool controller_source_replaced_before_transition{};
  };

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> ReplayResult(
      const CCLCLifecycleRequestResultDTO& result) const {
    if (!result.locally_valid()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_journal_result_invalid",
          "the lifecycle journal returned an invalid request result", "journal_result_invalid"));
    }
    if (result.replay_status == RequestReplayStatusDTO::kAcceptedNew ||
        result.replay_status == RequestReplayStatusDTO::kReplayedExact) {
      return Result<CCLCLifecycleRequestResultDTO>::Ok(result);
    }
    return Result<CCLCLifecycleRequestResultDTO>::Err(replay_error(result.replay_status));
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> ReplayRouteResult(
      const CCLCRouteSelectionRequestResultDTO& result) const {
    if (!result.locally_valid()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
          "route_selection_journal_result_invalid",
          "the route journal returned an invalid request result", "journal_result_invalid"));
    }
    if (result.replay_status == RequestReplayStatusDTO::kAcceptedNew ||
        result.replay_status == RequestReplayStatusDTO::kReplayedExact) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Ok(result);
    }
    return Result<CCLCRouteSelectionRequestResultDTO>::Err(replay_error(result.replay_status));
  }

  [[nodiscard]] static ExpectedActiveControlConfigurationDTO ExpectedConfiguration(
      const std::optional<ActiveControlConfigurationDTO>& configuration) {
    if (!configuration.has_value()) {
      return {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
    }
    return {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
            configuration->active_control_configuration_id,
            configuration->active_control_configuration_generation};
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> CommitRouteAdmission(
      const RouteSelectionIntentDTO& intent, std::uint64_t admission_gate_generation) {
    std::unique_lock admission_lock(admission_mutex_);
    admission_resolution_ready_.wait(admission_lock, [&] { return !admission_resolution_active_; });
    admission_resolution_active_ = true;
    ScopeExit resolution_guard([&] {
      if (!admission_lock.owns_lock()) {
        admission_lock.lock();
      }
      admission_resolution_active_ = false;
      admission_resolution_ready_.notify_all();
    });

    if (!accepting_.load(std::memory_order_acquire) ||
        admission_gate_generation != admission_gate_generation_) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
          "route_selection_admission_closed",
          "the CCLC admission gate changed before route resolution", "admission_gate_changed"));
    }
    if (highest_route_transaction_generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
          "route_selection_generation_exhausted", "the route transaction generation is exhausted",
          "transaction_generation_overflow"));
    }
    const auto next_transaction_generation = highest_route_transaction_generation_ + 1U;
    const auto domain_key = intent.desired_configuration.control_domain_id.value();
    const auto watermark = std::find_if(active_configuration_generation_watermarks_.begin(),
                                        active_configuration_generation_watermarks_.end(),
                                        [&](const auto& row) { return row.first == domain_key; });
    const auto current_watermark =
        watermark == active_configuration_generation_watermarks_.end() ? 0U : watermark->second;
    if (current_watermark == std::numeric_limits<std::uint64_t>::max()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_generation_exhausted",
                            "the active-configuration generation is exhausted",
                            "active_configuration_generation_overflow"));
    }
    const auto candidate_generation = current_watermark + 1U;
    const RouteSelectionTransactionId transaction_id("route-selection." +
                                                     std::to_string(next_transaction_generation));
    const ActiveControlConfigurationId configuration_id("active-configuration." +
                                                        std::to_string(candidate_generation));
    admission_lock.unlock();

    Result<std::optional<CCLCRouteSelectionRequestResultDTO>> reinspection =
        Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(coordinator_error(
            "route_selection_journal_inspection_failed",
            "the route journal inspection raised an exception", "journal_exception"));
    try {
      reinspection = journal_.InspectRouteSelectionRequest(config_.request_owner_epoch, intent);
    } catch (...) {
    }
    if (!reinspection.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(reinspection.error());
    }
    if (reinspection.value().has_value()) {
      return ReplayRouteResult(*reinspection.value());
    }

    Result<RouteSelectionCurrentSnapshot> snapshot =
        Result<RouteSelectionCurrentSnapshot>::Err(coordinator_error(
            "route_selection_snapshot_failed", "the route snapshot source raised an exception",
            "snapshot_source_exception"));
    try {
      snapshot = route_services_->current_snapshot_source->ReadCurrentRouteSelectionSnapshot(
          intent.expected_retained_bundle_id, intent.expected_graph_revision,
          intent.desired_configuration);
    } catch (...) {
    }
    if (!snapshot.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(snapshot.error());
    }

    Result<std::optional<ActiveControlConfigurationDTO>> journal_current =
        Result<std::optional<ActiveControlConfigurationDTO>>::Err(coordinator_error(
            "route_selection_current_configuration_failed",
            "the current route publication read raised an exception", "journal_exception"));
    try {
      journal_current = journal_.LoadCurrentActiveControlConfiguration(
          intent.desired_configuration.control_domain_id);
    } catch (...) {
    }
    if (!journal_current.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(journal_current.error());
    }
    if (journal_current.value() != snapshot.value().value().active_control_configuration) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_current_snapshot_mismatch",
                            "the route snapshot and durable current configuration differ",
                            "durable_current_configuration_mismatch"));
    }

    std::int64_t accepted_at_ns = -1;
    try {
      accepted_at_ns = clock_.now();
    } catch (...) {
    }
    if (accepted_at_ns < 0 || accepted_at_ns > std::numeric_limits<std::int64_t>::max() -
                                                   config_.route_selection_timeout_duration_ns) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
          "route_selection_clock_invalid", "the route acceptance deadline is not representable",
          "clock_or_deadline_invalid"));
    }

    const auto& current = snapshot.value().value();
    const RouteConfigurationFenceAdmissionQueryDTO fence_query{
        transaction_id,
        RouteSelectionTransactionGeneration(next_transaction_generation),
        current.configuration_fence_scope,
        current.retained_bundle_id,
        current.graph_revision,
        ExpectedConfiguration(current.active_control_configuration)};
    Result<RouteConfigurationFenceAdmissionSnapshotDTO> fence_snapshot =
        Result<RouteConfigurationFenceAdmissionSnapshotDTO>::Err(
            coordinator_error("route_selection_kernel_admission_failed",
                              "the Kernel route-fence admission observation raised an exception",
                              "kernel_admission_exception"));
    try {
      fence_snapshot =
          route_services_->kernel_route_fence_port->ObserveRouteConfigurationFenceAdmission(
              fence_query);
    } catch (...) {
    }
    if (!fence_snapshot.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(fence_snapshot.error());
    }

    const RouteSelectionAdmissionCandidate candidate{
        transaction_id,
        RouteSelectionTransactionGeneration(next_transaction_generation),
        configuration_id,
        ActiveControlConfigurationGeneration(candidate_generation),
        ActiveControlConfigurationGeneration(current_watermark),
        accepted_at_ns,
        config_.route_selection_timeout_duration_ns,
        current.retained_bundle_id,
        current.graph_revision,
        current.component_expectations,
        current.active_control_configuration,
        current.expected_controller_domain,
        current.configuration_fence_scope,
        fence_snapshot.value()};
    const auto prepared = PrepareRouteSelectionAdmission(intent, candidate);
    if (!prepared.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(prepared.error());
    }
    const auto owner = reservation_owner(prepared.value().acceptance);
    Result<void> reserved = Result<void>::Err(coordinator_error(
        "route_selection_domain_reservation_failed",
        "the route domain reservation raised an exception", "reservation_exception"));
    try {
      reserved = domain_reservations_.reserve(owner, prepared.value().reserved_control_domains);
    } catch (...) {
    }
    if (!reserved.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(reserved.error());
    }
    bool reservation_held = true;
    ScopeExit reservation_guard([&] {
      if (reservation_held) {
        try {
          static_cast<void>(
              domain_reservations_.release(owner, prepared.value().reserved_control_domains));
        } catch (...) {
        }
      }
    });

    Result<RouteSelectionCurrentSnapshot> reread =
        Result<RouteSelectionCurrentSnapshot>::Err(coordinator_error(
            "route_selection_snapshot_failed", "the route snapshot reread raised an exception",
            "snapshot_source_exception"));
    try {
      reread = route_services_->current_snapshot_source->ReadCurrentRouteSelectionSnapshot(
          intent.expected_retained_bundle_id, intent.expected_graph_revision,
          intent.desired_configuration);
    } catch (...) {
    }
    if (!reread.ok() || reread.value().value() != snapshot.value().value()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          reread.ok() ? coordinator_error("route_selection_snapshot_changed_during_admission",
                                          "the route snapshot changed after domain reservation",
                                          "snapshot_reread_mismatch")
                      : reread.error());
    }

    std::shared_ptr<RouteRuntimeState> runtime;
    try {
      runtime = std::make_shared<RouteRuntimeState>(
          prepared.value().initial_status, std::move(snapshot.value()), fence_snapshot.value(),
          prepared.value().reserved_control_domains);
    } catch (...) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_runtime_capacity_unavailable",
                            "the route runtime state could not be allocated before persistence",
                            "runtime_state_allocation_failed"));
    }

    admission_lock.lock();
    if (!accepting_.load(std::memory_order_acquire) ||
        admission_gate_generation != admission_gate_generation_ ||
        highest_route_transaction_generation_ + 1U != next_transaction_generation) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_admission_closed",
                            "the CCLC admission gate changed before durable route persistence",
                            "admission_precommit_gate_changed"));
    }
    const auto live_watermark =
        std::find_if(active_configuration_generation_watermarks_.begin(),
                     active_configuration_generation_watermarks_.end(),
                     [&](const auto& row) { return row.first == domain_key; });
    if ((live_watermark == active_configuration_generation_watermarks_.end()
             ? 0U
             : live_watermark->second) != current_watermark) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_generation_conflict",
                            "the route generation watermark changed before persistence",
                            "active_configuration_watermark_changed"));
    }
    const bool provisional_watermark =
        live_watermark == active_configuration_generation_watermarks_.end();
    if (provisional_watermark) {
      if (active_configuration_generation_watermarks_.size() >= kMaxCoordinatorActiveTransactions) {
        return Result<CCLCRouteSelectionRequestResultDTO>::Err(
            coordinator_error("route_selection_journal_capacity_exhausted",
                              "the bounded active-configuration watermark table is full",
                              "active_configuration_watermark_capacity_exhausted"));
      }
      active_configuration_generation_watermarks_.emplace_back(domain_key, current_watermark);
    }
    const auto rollback_watermark = [&] {
      if (provisional_watermark) {
        active_configuration_generation_watermarks_.pop_back();
      }
    };

    admission_persistence_active_ = true;
    admission_lock.unlock();
    Result<CCLCRouteSelectionRequestResultDTO> accepted =
        Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
            "route_selection_journal_accept_failed",
            "the route journal acceptance raised an exception", "journal_exception"));
    try {
      accepted = journal_.AcceptOrReplayRouteSelection(config_.request_owner_epoch, intent,
                                                       prepared.value().acceptance,
                                                       prepared.value().initial_status);
    } catch (...) {
    }
    admission_lock.lock();
    admission_persistence_active_ = false;
    admission_resolution_ready_.notify_all();
    if (!accepted.ok()) {
      rollback_watermark();
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(accepted.error());
    }
    if (!accepted.value().locally_valid()) {
      rollback_watermark();
      CloseAdmissionLocked();
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_journal_result_invalid",
                            "the route journal returned an invalid accepted result",
                            "accepted_result_shape_invalid"));
    }
    if (accepted.value().replay_status != RequestReplayStatusDTO::kAcceptedNew) {
      rollback_watermark();
      return ReplayRouteResult(accepted.value());
    }
    reservation_held = false;
    if (!accepted.value().acceptance.has_value() || !accepted.value().status.has_value() ||
        *accepted.value().acceptance != prepared.value().acceptance ||
        *accepted.value().status != prepared.value().initial_status) {
      CloseAdmissionLocked();
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_journal_result_invalid",
                            "the durable route acceptance differs from its prepared candidate",
                            "accepted_result_candidate_mismatch"));
    }
    highest_route_transaction_generation_ = next_transaction_generation;
    const auto accepted_watermark =
        std::find_if(active_configuration_generation_watermarks_.begin(),
                     active_configuration_generation_watermarks_.end(),
                     [&](const auto& row) { return row.first == domain_key; });
    accepted_watermark->second = candidate_generation;
    admission_lock.unlock();

    try {
      {
        std::lock_guard lock(state_mutex_);
        active_route_transactions_.push_back(runtime);
      }
      if (!worker_pool_.Submit([this, runtime](std::stop_token cancellation) {
            ProcessRoute(runtime, cancellation);
          })) {
        CloseAdmission();
      }
    } catch (...) {
      CloseAdmission();
    }
    return Result<CCLCRouteSelectionRequestResultDTO>::Ok(std::move(accepted.value()));
  }

  [[nodiscard]] bool AdvanceRouteStatus(const std::shared_ptr<RouteRuntimeState>& runtime,
                                        RouteSelectionStatusDTO& status,
                                        RouteSelectionTransactionPhaseDTO phase) noexcept {
    if (status.status_sequence == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    std::int64_t now_ns = -1;
    try {
      now_ns = clock_.now();
    } catch (...) {
      return false;
    }
    if (now_ns < 0) {
      return false;
    }
    ++status.status_sequence;
    status.status_owner_epoch = config_.request_owner_epoch;
    status.phase = phase;
    status.latest_updated_at_ns = now_ns;
    status.remaining_duration_ns =
        now_ns >= runtime->effective_deadline_ns ? 0 : runtime->effective_deadline_ns - now_ns;
    return true;
  }

  [[nodiscard]] bool RecordRouteStatus(const std::shared_ptr<RouteRuntimeState>& runtime,
                                       RouteSelectionStatusDTO status) noexcept {
    if (!status.locally_valid()) {
      CloseAdmission();
      return false;
    }
    Result<std::uint64_t> recorded = Result<std::uint64_t>::Err(coordinator_error(
        "route_selection_journal_status_failed",
        "the route journal status write raised an exception", "journal_exception"));
    try {
      recorded = journal_.RecordRouteSelectionStatus(config_.request_owner_epoch, status);
    } catch (...) {
    }
    if (!recorded.ok()) {
      CloseAdmission();
      return false;
    }
    runtime->status = std::move(status);
    return true;
  }

  [[nodiscard]] bool RecordRouteEffectPrepared(const std::shared_ptr<RouteRuntimeState>& runtime,
                                               CCLCRouteEffectAttemptDTO attempt,
                                               RouteSelectionStatusDTO status) noexcept {
    if (!attempt.locally_valid() || attempt.state != CCLCRouteEffectAttemptStateDTO::kPrepared ||
        !status.locally_valid()) {
      CloseAdmission();
      return false;
    }
    Result<std::uint64_t> recorded = Result<std::uint64_t>::Err(
        coordinator_error("route_selection_journal_effect_failed",
                          "the route effect preparation raised an exception", "journal_exception"));
    try {
      recorded = journal_.RecordRouteEffectPrepared(config_.request_owner_epoch, attempt, status);
    } catch (...) {
    }
    if (!recorded.ok()) {
      CloseAdmission();
      return false;
    }
    runtime->effect_sequence = attempt.effect_sequence;
    runtime->status = std::move(status);
    return true;
  }

  [[nodiscard]] bool RecordRouteEffectCompleted(const std::shared_ptr<RouteRuntimeState>& runtime,
                                                CCLCRouteEffectAttemptDTO attempt,
                                                RouteSelectionStatusDTO status) noexcept {
    if (!attempt.locally_valid() || attempt.state != CCLCRouteEffectAttemptStateDTO::kCompleted ||
        !status.locally_valid()) {
      CloseAdmission();
      return false;
    }
    Result<std::uint64_t> recorded = Result<std::uint64_t>::Err(
        coordinator_error("route_selection_journal_effect_failed",
                          "the route effect completion raised an exception", "journal_exception"));
    try {
      recorded = journal_.RecordRouteEffectCompleted(config_.request_owner_epoch, attempt, status);
    } catch (...) {
    }
    if (!recorded.ok()) {
      CloseAdmission();
      return false;
    }
    runtime->effect_sequence = attempt.effect_sequence;
    runtime->unresolved_recovery_effect.reset();
    runtime->status = std::move(status);
    return true;
  }

  [[nodiscard]] std::optional<std::uint64_t> NextRouteEffectSequence(
      const std::shared_ptr<RouteRuntimeState>& runtime) noexcept {
    if (runtime->effect_sequence == std::numeric_limits<std::uint64_t>::max()) {
      CloseAdmission();
      return std::nullopt;
    }
    return runtime->effect_sequence + 1U;
  }

  [[nodiscard]] RouteConfigurationFenceIntentDTO MakeRouteFenceIntent(
      const std::shared_ptr<RouteRuntimeState>& runtime, std::uint64_t effect_sequence) const {
    const auto& acceptance = runtime->status.acceptance;
    const auto& sampled = *runtime->fence_admission;
    return {
        {RequestId("cclc.route.acquire." +
                   std::to_string(acceptance.route_selection_transaction_generation.value()) + "." +
                   std::to_string(effect_sequence)),
         sampled.execution_host_epoch.request_owner_epoch, "cclc-route-selection", std::nullopt},
        acceptance.request_id,
        acceptance.route_selection_transaction_id,
        acceptance.route_selection_transaction_generation,
        acceptance.configuration_fence_scope,
        sampled.snapshot_digest,
        RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy,
        acceptance.accepted_retained_bundle_id,
        acceptance.accepted_graph_revision,
        sampled.operation_resource_generation,
        sampled.configuration_fence_generation,
        sampled.current_active_control_configuration,
        acceptance.candidate_active_control_configuration_id,
        acceptance.candidate_active_control_configuration_generation,
        "complete-domain route selection"};
  }

  void ProcessRoute(const std::shared_ptr<RouteRuntimeState>& runtime,
                    std::stop_token cancellation) noexcept {
    runtime->scheduled = false;
    if (cancellation.stop_requested() ||
        runtime->status.phase == RouteSelectionTransactionPhaseDTO::kSucceeded ||
        runtime->status.phase == RouteSelectionTransactionPhaseDTO::kFailed) {
      return;
    }
    try {
      if (runtime->recovering) {
        RecoverRoute(runtime, cancellation);
        return;
      }
      if (!runtime->status.accepted_fence.has_value()) {
        AcquireRouteFence(runtime, cancellation);
        return;
      }
      if (!runtime->status.resulting_active_control_configuration.has_value()) {
        AdvanceRouteController(runtime, cancellation);
        return;
      }
      PublishAndReleaseRoute(runtime, cancellation);
    } catch (...) {
      CloseAdmission();
    }
  }

  void AcquireRouteFence(const std::shared_ptr<RouteRuntimeState>& runtime,
                         std::stop_token cancellation) noexcept {
    if (cancellation.stop_requested()) {
      return;
    }
    const auto next_effect = NextRouteEffectSequence(runtime);
    if (!next_effect.has_value()) {
      return;
    }
    auto successor = runtime->status;
    if (!AdvanceRouteStatus(runtime, successor,
                            RouteSelectionTransactionPhaseDTO::kAcquiringFence)) {
      CloseAdmission();
      return;
    }
    const auto request = MakeRouteFenceIntent(runtime, *next_effect);
    const CCLCRouteEffectAttemptDTO prepared{
        runtime->status.acceptance.route_selection_transaction_id,
        runtime->status.acceptance.route_selection_transaction_generation,
        *next_effect,
        CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire,
        CCLCRouteEffectAttemptStateDTO::kPrepared,
        request,
        std::nullopt};
    if (!RecordRouteEffectPrepared(runtime, prepared, std::move(successor))) {
      return;
    }

    Result<RouteConfigurationFenceAcceptedDTO> accepted =
        Result<RouteConfigurationFenceAcceptedDTO>::Err(coordinator_error(
            "route_fence_acquire_failed", "the Kernel route-fence Acquire raised an exception",
            "kernel_exception"));
    try {
      accepted = route_services_->kernel_route_fence_port->AcquireRouteConfigurationFence(request);
    } catch (...) {
    }
    RouteConfigurationFenceAcquireEffectResultDTO effect_result{
        request, RouteConfigurationFenceAcquireEffectDispositionDTO::kProvenNotSeen, std::nullopt};
    if (accepted.ok() && accepted.value().matches(request)) {
      effect_result.disposition = RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted;
      effect_result.acceptance = accepted.value();
    } else {
      Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO> reconciled =
          Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>::Err(
              accepted.ok() ? coordinator_error("route_fence_acquire_invalid",
                                                "the Kernel returned a foreign Acquire receipt",
                                                "acquire_receipt_mismatch")
                            : accepted.error());
      try {
        reconciled = route_services_->kernel_route_fence_port
                         ->ReconcileRouteConfigurationFenceAcquireAttempt(request);
      } catch (...) {
      }
      if (!reconciled.ok() || !reconciled.value().locally_valid() ||
          reconciled.value().intent != request ||
          (reconciled.value().disposition !=
               RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact &&
           reconciled.value().disposition !=
               RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen)) {
        auto unknown = runtime->status;
        if (AdvanceRouteStatus(runtime, unknown,
                               RouteSelectionTransactionPhaseDTO::kOutcomeUnknown)) {
          unknown.effect = RouteSelectionEffectDTO::kEffectUncertain;
          unknown.reconciliation_required = true;
          unknown.error = reconciled.ok() && reconciled.value().error.has_value()
                              ? reconciled.value().error
                              : std::optional<ErrorDTO>(coordinator_error(
                                    "route_fence_acquire_outcome_unknown",
                                    "the prepared route-fence Acquire is unresolved",
                                    "acquire_reconciliation_inconclusive"));
          static_cast<void>(RecordRouteStatus(runtime, std::move(unknown)));
        } else {
          CloseAdmission();
        }
        runtime->unresolved_recovery_effect = prepared;
        return;
      }
      if (reconciled.value().disposition ==
          RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact) {
        effect_result.disposition = RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted;
        effect_result.acceptance = reconciled.value().acceptance;
      }
    }

    auto completed_status = runtime->status;
    CCLCRouteEffectAttemptDTO completed = prepared;
    completed.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
    completed.result = effect_result;
    if (effect_result.disposition ==
        RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted) {
      if (!AdvanceRouteStatus(runtime, completed_status,
                              RouteSelectionTransactionPhaseDTO::kObserving)) {
        CloseAdmission();
        return;
      }
      completed_status.accepted_fence = effect_result.acceptance;
    } else {
      if (!AdvanceRouteStatus(runtime, completed_status,
                              RouteSelectionTransactionPhaseDTO::kFailed)) {
        CloseAdmission();
        return;
      }
      completed_status.effect = RouteSelectionEffectDTO::kKnownNoEffect;
      completed_status.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
      completed_status.error = coordinator_error(
          "route_fence_not_acquired", "Kernel proved that route-fence Acquire had no effect",
          "acquire_proven_not_seen");
      completed_status.remaining_duration_ns = 0;
    }
    if (!RecordRouteEffectCompleted(runtime, std::move(completed), std::move(completed_status))) {
      return;
    }
    if (effect_result.disposition ==
        RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted) {
      ProcessRoute(runtime, cancellation);
    } else {
      CompleteRoute(runtime);
    }
  }

  void AdvanceRouteController(const std::shared_ptr<RouteRuntimeState>& runtime,
                              std::stop_token cancellation) noexcept {
    if (cancellation.stop_requested()) {
      return;
    }
    if (!runtime->controller_port) {
      Result<std::shared_ptr<IControllerLifecyclePort>> resolved =
          Result<std::shared_ptr<IControllerLifecyclePort>>::Err(
              coordinator_error("route_controller_port_resolution_failed",
                                "the controller-lifecycle port resolver raised an exception",
                                "controller_port_resolver_exception"));
      try {
        resolved = route_services_->controller_lifecycle_port_resolver
                       ->ResolveExactControllerLifecyclePort(
                           runtime->status.acceptance.controller_lifecycle_observation_query);
      } catch (...) {
      }
      if (!resolved.ok() || !resolved.value()) {
        CloseAdmission();
        return;
      }
      runtime->controller_port = std::move(resolved.value());
    }

    Result<ControllerLifecycleObservationDTO> observed =
        Result<ControllerLifecycleObservationDTO>::Err(coordinator_error(
            "route_controller_observation_failed",
            "the controller-lifecycle observation raised an exception", "observe_exception"));
    try {
      observed = runtime->controller_port->Observe(
          runtime->status.acceptance.controller_lifecycle_observation_query);
    } catch (...) {
    }
    if (!observed.ok()) {
      ScheduleRoute(runtime);
      return;
    }
    const auto now_ns = [&]() {
      try {
        return clock_.now();
      } catch (...) {
        return std::int64_t{-1};
      }
    }();
    const auto reconciled = ReconcileControllerLifecycle(
        runtime->snapshot.expected_controller_domain().value(), observed.value(), now_ns);
    if (!reconciled.ok()) {
      ScheduleRoute(runtime);
      return;
    }

    auto observed_status = runtime->status;
    if (!AdvanceRouteStatus(runtime, observed_status,
                            RouteSelectionTransactionPhaseDTO::kVerifying)) {
      CloseAdmission();
      return;
    }
    observed_status.controller_observation = observed.value();
    observed_status.controller_reconciliation = reconciled.value();
    observed_status.controller_transition_plan.reset();
    observed_status.reconciliation_required = false;
    observed_status.error.reset();

    if (reconciled.value().disposition ==
        ControllerLifecycleReconciliationDispositionDTO::kReconciled) {
      const bool any_dispatched =
          std::any_of(observed_status.controller_transition_results.begin(),
                      observed_status.controller_transition_results.end(), [](const auto& result) {
                        return result.dispatch_state !=
                               ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched;
                      });
      observed_status.effect = any_dispatched ? RouteSelectionEffectDTO::kEffectObserved
                                              : RouteSelectionEffectDTO::kKnownNoEffect;
      observed_status.affected_control_binding_ids.clear();
      observed_status.affected_controller_role_ids.clear();
      if (!RecordRouteStatus(runtime, std::move(observed_status))) {
        return;
      }
      ResolveAndPublishRouteConfiguration(runtime, cancellation);
      return;
    }

    const auto plan = BuildControllerLifecycleTransitionPlan(
        runtime->snapshot.expected_controller_domain().value(), observed.value(), now_ns);
    if (!plan.ok() || plan.value().steps.empty()) {
      observed_status.effect = observed_status.controller_transition_results.empty()
                                   ? RouteSelectionEffectDTO::kPartialEffectObserved
                                   : observed_status.effect;
      if (observed_status.effect == RouteSelectionEffectDTO::kNone ||
          observed_status.effect == RouteSelectionEffectDTO::kKnownNoEffect ||
          observed_status.effect == RouteSelectionEffectDTO::kEffectUncertain) {
        observed_status.effect = RouteSelectionEffectDTO::kPartialEffectObserved;
      }
      if (!RecordRouteStatus(runtime, std::move(observed_status))) {
        return;
      }
      ResolveAndPublishRouteConfiguration(runtime, cancellation);
      return;
    }

    observed_status.controller_transition_plan = plan.value();
    const auto& step = plan.value().steps.front();
    const auto phase = step.kind == ControllerLifecycleTransitionKindDTO::kDeactivate
                           ? RouteSelectionTransactionPhaseDTO::kDeactivating
                           : RouteSelectionTransactionPhaseDTO::kActivating;
    observed_status.phase = phase;
    if (!RecordRouteStatus(runtime, observed_status)) {
      return;
    }
    const auto next_effect = NextRouteEffectSequence(runtime);
    if (!next_effect.has_value() || runtime->status.remaining_duration_ns == 0) {
      CloseAdmission();
      return;
    }
    const ControllerLifecycleTransitionIntentDTO transition{
        runtime->status.acceptance.controller_lifecycle_observation_query,
        observed.value().source_sequence, 0U, step, runtime->status.remaining_duration_ns};
    auto prepared_status = runtime->status;
    if (!AdvanceRouteStatus(runtime, prepared_status, phase)) {
      CloseAdmission();
      return;
    }
    const CCLCRouteEffectAttemptDTO prepared{
        runtime->status.acceptance.route_selection_transaction_id,
        runtime->status.acceptance.route_selection_transaction_generation,
        *next_effect,
        CCLCRouteEffectKindDTO::kControllerLifecycleTransition,
        CCLCRouteEffectAttemptStateDTO::kPrepared,
        transition,
        std::nullopt};
    if (!RecordRouteEffectPrepared(runtime, prepared, std::move(prepared_status))) {
      return;
    }

    ControllerLifecycleTransitionResultDTO result{
        transition, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain,
        std::nullopt,
        coordinator_error("route_controller_transition_outcome_unknown",
                          "the controller transition raised before returning its result",
                          "transition_exception")};
    try {
      result = runtime->controller_port->ApplyTransitionStep(transition, cancellation);
    } catch (...) {
    }
    if (!result.locally_valid() || result.intent != transition) {
      result = {transition, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain,
                std::nullopt,
                coordinator_error("route_controller_transition_result_invalid",
                                  "the controller transition returned a foreign result",
                                  "transition_result_mismatch")};
    }
    auto completed_status = runtime->status;
    if (!AdvanceRouteStatus(runtime, completed_status,
                            RouteSelectionTransactionPhaseDTO::kVerifying)) {
      CloseAdmission();
      return;
    }
    completed_status.controller_transition_results.push_back(result);
    completed_status.controller_transition_plan = runtime->status.controller_transition_plan;
    switch (result.dispatch_state) {
      case ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched:
        if (completed_status.effect == RouteSelectionEffectDTO::kNone) {
          completed_status.effect = RouteSelectionEffectDTO::kKnownNoEffect;
        }
        break;
      case ControllerLifecycleTransitionDispatchStateDTO::kDispatchedKnown:
        completed_status.effect = RouteSelectionEffectDTO::kEffectObserved;
        break;
      case ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain:
        completed_status.effect = RouteSelectionEffectDTO::kEffectUncertain;
        break;
    }
    CCLCRouteEffectAttemptDTO completed = prepared;
    completed.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
    completed.result = result;
    if (!RecordRouteEffectCompleted(runtime, std::move(completed), std::move(completed_status))) {
      return;
    }
    ScheduleRoute(runtime);
  }

  void ResolveAndPublishRouteConfiguration(const std::shared_ptr<RouteRuntimeState>& runtime,
                                           std::stop_token cancellation) noexcept {
    if (cancellation.stop_requested() || !runtime->status.controller_observation.has_value() ||
        !runtime->status.controller_reconciliation.has_value()) {
      return;
    }
    Result<ActiveControlConfigurationDTO> resolved = Result<ActiveControlConfigurationDTO>::Err(
        coordinator_error("route_configuration_resolution_failed",
                          "the route configuration publisher raised during resolution",
                          "configuration_resolution_exception"));
    try {
      resolved = route_services_->configuration_publication_port
                     ->ResolveResultingActiveControlConfiguration(
                         runtime->status.acceptance, runtime->snapshot.expected_controller_domain(),
                         *runtime->status.controller_observation,
                         *runtime->status.controller_reconciliation, runtime->status.effect);
    } catch (...) {
    }
    if (!resolved.ok()) {
      CloseAdmission();
      return;
    }
    auto publishing = runtime->status;
    if (!AdvanceRouteStatus(runtime, publishing, RouteSelectionTransactionPhaseDTO::kPublishing)) {
      CloseAdmission();
      return;
    }
    publishing.resulting_active_control_configuration = resolved.value();
    if (!RecordRouteStatus(runtime, std::move(publishing))) {
      return;
    }
    PublishAndReleaseRoute(runtime, cancellation);
  }

  void PublishAndReleaseRoute(const std::shared_ptr<RouteRuntimeState>& runtime,
                              std::stop_token cancellation) noexcept {
    const bool abandon_replaced_source =
        runtime->controller_source_replaced_before_transition;
    if (cancellation.stop_requested() ||
        (!abandon_replaced_source &&
         !runtime->status.resulting_active_control_configuration.has_value())) {
      return;
    }
    if (!abandon_replaced_source) {
      Result<RouteReadinessPublicationReceiptDTO> publication =
          Result<RouteReadinessPublicationReceiptDTO>::Err(coordinator_error(
              "route_readiness_publication_failed",
              "the route readiness publication raised an exception",
              "publication_exception"));
      try {
        publication = route_services_->configuration_publication_port->PublishAndReadBack(
            runtime->status.acceptance, runtime->snapshot.expected_controller_domain(),
            *runtime->status.resulting_active_control_configuration);
      } catch (...) {
      }
      if (!publication.ok()) {
        ScheduleRoute(runtime);
        return;
      }
      const auto verified_receipt =
          VerifyRouteReadinessPublicationReceipt(runtime->status, publication.value());
      if (!verified_receipt.ok()) {
        ScheduleRoute(runtime);
        return;
      }
    }

    const auto& accepted_fence = *runtime->status.accepted_fence;
    const RouteConfigurationFenceStatusQueryDTO query{
        accepted_fence.route_selection_transaction_id,
        accepted_fence.route_selection_transaction_generation,
        accepted_fence.configuration_fence_id, accepted_fence.configuration_fence_generation,
        accepted_fence.captured_scope};
    Result<RouteConfigurationFenceStatusDTO> held =
        Result<RouteConfigurationFenceStatusDTO>::Err(coordinator_error(
            "route_fence_status_failed", "the Kernel route-fence status read raised an exception",
            "kernel_status_exception"));
    try {
      held = route_services_->kernel_route_fence_port->ObserveRouteConfigurationFenceStatus(query);
    } catch (...) {
    }
    if (!held.ok() || !held.value().locally_valid() || held.value().query != query ||
        held.value().state != RouteConfigurationFenceStateDTO::kHeld) {
      ScheduleRoute(runtime);
      return;
    }
    const auto next_effect = NextRouteEffectSequence(runtime);
    if (!next_effect.has_value()) {
      return;
    }
    const auto release_configuration = ExpectedConfiguration(
        abandon_replaced_source
            ? runtime->snapshot.value().active_control_configuration
            : runtime->status.resulting_active_control_configuration);
    if (abandon_replaced_source &&
        held.value().evidence.current_active_control_configuration !=
            release_configuration) {
      ScheduleRoute(runtime);
      return;
    }
    const RouteConfigurationFenceReleaseRequestDTO request{
        {RequestId("cclc.route.release." +
                   std::to_string(
                       runtime->status.acceptance.route_selection_transaction_generation.value()) +
                   "." + std::to_string(*next_effect)),
         held.value().evidence.execution_host_epoch.request_owner_epoch, "cclc-route-selection",
         std::nullopt},
        runtime->status.acceptance.request_id,
        runtime->status.acceptance.route_selection_transaction_id,
        runtime->status.acceptance.route_selection_transaction_generation,
        accepted_fence,
        runtime->status.status_sequence,
        ContentDigest(detail::RouteSelectionStatusReleasePreconditionDigest(runtime->status)),
        release_configuration,
        held.value().evidence};
    auto prepared_status = runtime->status;
    if (!AdvanceRouteStatus(runtime, prepared_status,
                            RouteSelectionTransactionPhaseDTO::kReleasingFence)) {
      CloseAdmission();
      return;
    }
    if (abandon_replaced_source) {
      prepared_status.effect = RouteSelectionEffectDTO::kKnownNoEffect;
      prepared_status.error = coordinator_error(
          "route_selection_controller_source_replaced",
          "the controller source changed before any controller transition",
          "controller_source_replaced_before_transition");
    }
    const CCLCRouteEffectAttemptDTO prepared{
        runtime->status.acceptance.route_selection_transaction_id,
        runtime->status.acceptance.route_selection_transaction_generation,
        *next_effect,
        CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease,
        CCLCRouteEffectAttemptStateDTO::kPrepared,
        request,
        std::nullopt};
    if (!RecordRouteEffectPrepared(runtime, prepared, std::move(prepared_status))) {
      return;
    }

    Result<RouteConfigurationFenceReleasedDTO> released =
        Result<RouteConfigurationFenceReleasedDTO>::Err(coordinator_error(
            "route_fence_release_failed", "the Kernel route-fence Release raised an exception",
            "kernel_exception"));
    try {
      released = route_services_->kernel_route_fence_port->ReleaseRouteConfigurationFence(request);
    } catch (...) {
    }
    RouteConfigurationFenceReleaseEffectResultDTO effect_result{
        request, RouteConfigurationFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld,
        std::nullopt};
    if (released.ok() && released.value().locally_valid() && released.value().request == request) {
      effect_result.disposition = RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased;
      effect_result.receipt = released.value();
    } else {
      Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO> reconciled =
          Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO>::Err(
              released.ok() ? coordinator_error("route_fence_release_invalid",
                                                "the Kernel returned a foreign Release receipt",
                                                "release_receipt_mismatch")
                            : released.error());
      try {
        reconciled = route_services_->kernel_route_fence_port
                         ->ReconcileRouteConfigurationFenceReleaseAttempt(request);
      } catch (...) {
      }
      if (!reconciled.ok() || !reconciled.value().locally_valid() ||
          reconciled.value().request != request ||
          (reconciled.value().disposition !=
               RouteConfigurationFenceReleaseAttemptDispositionDTO::kReleasedExact &&
           reconciled.value().disposition !=
               RouteConfigurationFenceReleaseAttemptDispositionDTO::kNotReleasedHeld)) {
        auto unknown = runtime->status;
        if (AdvanceRouteStatus(runtime, unknown,
                               RouteSelectionTransactionPhaseDTO::kOutcomeUnknown)) {
          unknown.reconciliation_required = true;
          unknown.error = reconciled.ok() && reconciled.value().error.has_value()
                              ? reconciled.value().error
                              : std::optional<ErrorDTO>(coordinator_error(
                                    "route_fence_release_outcome_unknown",
                                    "the prepared route-fence Release is unresolved",
                                    "release_reconciliation_inconclusive"));
          static_cast<void>(RecordRouteStatus(runtime, std::move(unknown)));
        } else {
          CloseAdmission();
        }
        runtime->unresolved_recovery_effect = prepared;
        return;
      }
      if (reconciled.value().disposition ==
          RouteConfigurationFenceReleaseAttemptDispositionDTO::kReleasedExact) {
        effect_result.disposition = RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased;
        effect_result.receipt = reconciled.value().receipt;
      } else {
        ScheduleRoute(runtime);
        return;
      }
    }

    const bool reconciled_configuration =
        !abandon_replaced_source &&
        runtime->status.resulting_active_control_configuration->state ==
            ActiveControlConfigurationStateDTO::kReconciled;
    auto terminal_status = runtime->status;
    if (!AdvanceRouteStatus(
            runtime, terminal_status,
            reconciled_configuration
                ? RouteSelectionTransactionPhaseDTO::kSucceeded
                : RouteSelectionTransactionPhaseDTO::kFailed)) {
      CloseAdmission();
      return;
    }
    terminal_status.released_fence = effect_result.receipt;
    terminal_status.remaining_duration_ns = 0;
    terminal_status.reconciliation_required = false;
    if (reconciled_configuration) {
      terminal_status.terminal_result = RouteSelectionTerminalResultDTO::kSucceeded;
      terminal_status.error.reset();
    } else if (abandon_replaced_source) {
      terminal_status.effect = RouteSelectionEffectDTO::kKnownNoEffect;
      terminal_status.terminal_result =
          RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
      terminal_status.error = coordinator_error(
          "route_selection_controller_source_replaced",
          "the controller source changed before any controller transition",
          "controller_source_replaced_before_transition");
    } else {
      terminal_status.effect = RouteSelectionEffectDTO::kPartialEffectObserved;
      terminal_status.terminal_result = RouteSelectionTerminalResultDTO::kFailedPartialEffect;
      terminal_status.error =
          coordinator_error("route_selection_unreconciled",
                            "the observed controller configuration remains externally unreconciled",
                            "external_or_partial_configuration_published");
    }
    CCLCRouteEffectAttemptDTO completed = prepared;
    completed.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
    completed.result = effect_result;
    if (!RecordRouteEffectCompleted(runtime, std::move(completed), std::move(terminal_status))) {
      return;
    }
    CompleteRoute(runtime);
  }

  void ScheduleRoute(const std::shared_ptr<RouteRuntimeState>& runtime) noexcept {
    if (runtime->scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    std::int64_t now_ns = -1;
    try {
      now_ns = clock_.now();
    } catch (...) {
      CloseAdmission();
      return;
    }
    if (now_ns < 0) {
      CloseAdmission();
      return;
    }
    if (!runtime->recovering && now_ns >= runtime->effective_deadline_ns) {
      auto status = runtime->status;
      if (AdvanceRouteStatus(runtime, status, RouteSelectionTransactionPhaseDTO::kReconciling)) {
        status.reconciliation_required = true;
        status.error = coordinator_error("route_selection_deadline_expired",
                                         "route reconciliation exceeded its fixed deadline",
                                         "deadline_expired");
        static_cast<void>(RecordRouteStatus(runtime, std::move(status)));
      } else {
        CloseAdmission();
      }
      return;
    }
    runtime->scheduled = true;
    const auto requested_deadline =
        now_ns > std::numeric_limits<std::int64_t>::max() - config_.observation_poll_period_ns
            ? std::numeric_limits<std::int64_t>::max()
            : now_ns + config_.observation_poll_period_ns;
    const auto deadline = runtime->recovering
                              ? requested_deadline
                              : std::min(runtime->effective_deadline_ns, requested_deadline);
    if (!ScheduleLifecycleCallback(deadline, [this, weak = std::weak_ptr(runtime)] {
          if (const auto state = weak.lock()) {
            state->scheduled = false;
            if (!worker_pool_.Submit([this, state](std::stop_token cancellation) {
                  ProcessRoute(state, cancellation);
                })) {
              CloseAdmission();
            }
          }
        })) {
      runtime->scheduled = false;
      CloseAdmission();
    }
  }

  void CompleteRoute(const std::shared_ptr<RouteRuntimeState>& runtime) noexcept {
    if (!runtime->reserved_domains.empty()) {
      Result<void> released = Result<void>::Err(coordinator_error(
          "route_selection_domain_release_failed", "the route domain release raised an exception",
          "reservation_exception"));
      try {
        released = domain_reservations_.release(reservation_owner(runtime->status.acceptance),
                                                runtime->reserved_domains);
      } catch (...) {
      }
      if (!released.ok()) {
        CloseAdmission();
        return;
      }
    }
    bool recovery_still_active = false;
    {
      std::lock_guard lock(state_mutex_);
      std::erase_if(active_route_transactions_,
                    [&](const auto& candidate) { return candidate == runtime; });
      if (recovery_in_progress_ && active_transactions_.empty() &&
          active_route_transactions_.empty()) {
        recovery_in_progress_ = false;
      }
      recovery_still_active = recovery_in_progress_;
    }
    {
      std::lock_guard lock(admission_mutex_);
      if (!recovery_still_active && owner_wide_containment_gate_count_ == 0U &&
          unresolved_effect_admission_gate_count_ == 0U && !admission_fault_closed_ &&
          !shutting_down_.load(std::memory_order_acquire) &&
          !accepting_.load(std::memory_order_acquire)) {
        if (!RotateLifecycleOwnerEpoch()) {
          admission_fault_closed_ = true;
          return;
        }
        ++admission_gate_generation_;
        accepting_.store(true, std::memory_order_release);
      }
    }
    state_changed_.notify_all();
  }

  [[nodiscard]] Result<void> RestoreRouteRecovery(const CCLCJournalRecoveryDTO& recovery) {
    std::vector<std::shared_ptr<RouteRuntimeState>> runtimes;
    std::vector<std::pair<CCLCDomainReservationOwnerDTO, std::vector<ControlDomainId>>>
        restored_reservations;
    try {
      runtimes.reserve(recovery.interrupted_route_selections.size());
      restored_reservations.reserve(recovery.interrupted_route_selections.size());
      for (const auto& status : recovery.interrupted_route_selections) {
        const auto unresolved =
            std::find_if(recovery.unresolved_route_effect_attempts.begin(),
                         recovery.unresolved_route_effect_attempts.end(),
                         [&](const auto& attempt) {
                           return attempt.route_selection_transaction_id ==
                                  status.acceptance.route_selection_transaction_id;
                         });
        Result<RouteSelectionCurrentSnapshot> snapshot = Result<RouteSelectionCurrentSnapshot>::Err(
            coordinator_error("route_selection_recovery_snapshot_failed",
                              "the route recovery snapshot source raised an exception",
                              "snapshot_source_exception"));
        try {
          snapshot = route_services_->current_snapshot_source->ReadCurrentRouteSelectionSnapshot(
              status.acceptance.accepted_retained_bundle_id,
              status.acceptance.accepted_graph_revision, status.acceptance.desired_configuration);
        } catch (...) {
        }
        const auto current_controller_query =
            snapshot.ok()
                ? std::optional<ControllerLifecycleObservationQueryDTO>(
                      MakeControllerLifecycleObservationQuery(
                          snapshot.value().expected_controller_domain().value()))
                : std::nullopt;
        const bool before_controller_transition =
            !status.controller_observation.has_value() &&
            !status.controller_reconciliation.has_value() &&
            !status.controller_transition_plan.has_value() &&
            status.controller_transition_results.empty() &&
            !status.resulting_active_control_configuration.has_value() &&
            (unresolved == recovery.unresolved_route_effect_attempts.end() ||
             unresolved->effect_kind !=
                 CCLCRouteEffectKindDTO::kControllerLifecycleTransition);
        const bool controller_source_replaced_before_transition =
            current_controller_query.has_value() && before_controller_transition &&
            ControllerSourceEpochOnlyChanged(
                status.acceptance.controller_lifecycle_observation_query,
                *current_controller_query);
        if (!snapshot.ok() ||
            (*current_controller_query !=
                 status.acceptance.controller_lifecycle_observation_query &&
             !controller_source_replaced_before_transition)) {
          for (const auto& restored : restored_reservations) {
            static_cast<void>(domain_reservations_.release(restored.first, restored.second));
          }
          return Result<void>::Err(
              snapshot.ok()
                  ? coordinator_error("route_selection_recovery_snapshot_mismatch",
                                      "the accepted controller closure cannot be reconstructed",
                                      "accepted_controller_query_mismatch")
                  : snapshot.error());
        }
        const auto durable_current = std::find_if(
            recovery.current_active_control_configurations.begin(),
            recovery.current_active_control_configurations.end(), [&](const auto& configuration) {
              return configuration.control_domain_id ==
                     status.acceptance.desired_configuration.control_domain_id;
            });
        const std::optional<ActiveControlConfigurationDTO> durable_configuration =
            durable_current == recovery.current_active_control_configurations.end()
                ? std::nullopt
                : std::optional<ActiveControlConfigurationDTO>(*durable_current);
        if (snapshot.value().value().active_control_configuration != durable_configuration ||
            (status.resulting_active_control_configuration.has_value() &&
             status.resulting_active_control_configuration != durable_configuration)) {
          for (const auto& restored : restored_reservations) {
            static_cast<void>(domain_reservations_.release(restored.first, restored.second));
          }
          return Result<void>::Err(coordinator_error(
              "route_selection_recovery_current_configuration_mismatch",
              "the route recovery snapshot differs from the durable current configuration",
              "durable_current_configuration_mismatch"));
        }
        std::vector<ControlDomainId> domains{
            status.acceptance.desired_configuration.control_domain_id};
        const auto owner = reservation_owner(status.acceptance);
        const auto reserved = domain_reservations_.reserve(owner, domains);
        if (!reserved.ok()) {
          for (const auto& restored : restored_reservations) {
            static_cast<void>(domain_reservations_.release(restored.first, restored.second));
          }
          return reserved;
        }
        restored_reservations.emplace_back(owner, domains);
        auto runtime = std::make_shared<RouteRuntimeState>(status, std::move(snapshot.value()),
                                                           std::nullopt, std::move(domains));
        runtime->recovering = true;
        runtime->controller_source_replaced_before_transition =
            controller_source_replaced_before_transition;
        runtime->effective_deadline_ns = clock_.now();
        const auto effect_watermark =
            std::find_if(recovery.route_effect_sequence_watermarks.begin(),
                         recovery.route_effect_sequence_watermarks.end(), [&](const auto& row) {
                           return row.route_selection_transaction_id ==
                                  status.acceptance.route_selection_transaction_id;
                         });
        if (effect_watermark == recovery.route_effect_sequence_watermarks.end()) {
          throw std::runtime_error("missing route effect watermark");
        }
        runtime->effect_sequence = effect_watermark->highest_effect_sequence;
        if (unresolved != recovery.unresolved_route_effect_attempts.end()) {
          runtime->unresolved_recovery_effect = *unresolved;
        }
        runtimes.push_back(std::move(runtime));
      }
    } catch (...) {
      for (const auto& restored : restored_reservations) {
        static_cast<void>(domain_reservations_.release(restored.first, restored.second));
      }
      return Result<void>::Err(
          coordinator_error("route_selection_recovery_capacity_unavailable",
                            "the interrupted route runtime set could not be restored",
                            "recovery_runtime_allocation_failed"));
    }

    {
      std::lock_guard lock(state_mutex_);
      active_route_transactions_.insert(active_route_transactions_.end(), runtimes.begin(),
                                        runtimes.end());
      recovery_in_progress_ = true;
    }
    for (const auto& runtime : runtimes) {
      if (!worker_pool_.Submit([this, runtime](std::stop_token cancellation) {
            ProcessRoute(runtime, cancellation);
          })) {
        CloseAdmission();
        return Result<void>::Ok();
      }
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] bool AdoptRouteRecoveryOwner(
      const std::shared_ptr<RouteRuntimeState>& runtime) noexcept {
    if (runtime->status.status_owner_epoch == config_.request_owner_epoch) {
      return true;
    }
    auto status = runtime->status;
    if (!AdvanceRouteStatus(runtime, status, RouteSelectionTransactionPhaseDTO::kReconciling)) {
      CloseAdmission();
      return false;
    }
    status.reconciliation_required = true;
    status.error = coordinator_error("route_selection_owner_replaced",
                                     "the route transaction is being reconciled by a new owner",
                                     "owner_replaced");
    status.remaining_duration_ns = 0;
    return RecordRouteStatus(runtime, std::move(status));
  }

  [[nodiscard]] Result<
      std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>>
  ObserveRecoveredRoute(const std::shared_ptr<RouteRuntimeState>& runtime) noexcept {
    if (!runtime->controller_port) {
      Result<std::shared_ptr<IControllerLifecyclePort>> resolved =
          Result<std::shared_ptr<IControllerLifecyclePort>>::Err(
              coordinator_error("route_controller_port_resolution_failed",
                                "the controller-lifecycle port resolver raised an exception",
                                "controller_port_resolver_exception"));
      try {
        resolved = route_services_->controller_lifecycle_port_resolver
                       ->ResolveExactControllerLifecyclePort(
                           runtime->status.acceptance.controller_lifecycle_observation_query);
      } catch (...) {
      }
      if (!resolved.ok() || !resolved.value()) {
        return Result<
            std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>>::
            Err(resolved.ok() ? coordinator_error("route_controller_port_resolution_failed",
                                                  "the resolver returned an empty controller port",
                                                  "controller_port_missing")
                              : resolved.error());
      }
      runtime->controller_port = std::move(resolved.value());
    }
    Result<ControllerLifecycleObservationDTO> observation =
        Result<ControllerLifecycleObservationDTO>::Err(
            coordinator_error("route_controller_observation_failed",
                              "the controller-lifecycle recovery observation raised an exception",
                              "observe_exception"));
    try {
      observation = runtime->controller_port->Observe(
          runtime->status.acceptance.controller_lifecycle_observation_query);
    } catch (...) {
    }
    if (!observation.ok()) {
      return Result<std::pair<ControllerLifecycleObservationDTO,
                              ControllerLifecycleReconciliationDTO>>::Err(observation.error());
    }
    std::int64_t now_ns = -1;
    try {
      now_ns = clock_.now();
    } catch (...) {
    }
    const auto reconciliation = ReconcileControllerLifecycle(
        runtime->snapshot.expected_controller_domain().value(), observation.value(), now_ns);
    if (!reconciliation.ok()) {
      return Result<std::pair<ControllerLifecycleObservationDTO,
                              ControllerLifecycleReconciliationDTO>>::Err(reconciliation.error());
    }
    return Result<
        std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>>::
        Ok({std::move(observation.value()), std::move(reconciliation.value())});
  }

  [[nodiscard]] Result<ActiveControlConfigurationDTO> ResolveRecoveredRouteConfiguration(
      const std::shared_ptr<RouteRuntimeState>& runtime,
      const ControllerLifecycleObservationDTO& observation,
      const ControllerLifecycleReconciliationDTO& reconciliation,
      RouteSelectionEffectDTO effect) noexcept {
    try {
      return route_services_->configuration_publication_port
          ->ResolveResultingActiveControlConfiguration(
              runtime->status.acceptance, runtime->snapshot.expected_controller_domain(),
              observation, reconciliation, effect);
    } catch (...) {
      return Result<ActiveControlConfigurationDTO>::Err(
          coordinator_error("route_configuration_resolution_failed",
                            "the route configuration recovery resolver raised an exception",
                            "configuration_resolution_exception"));
    }
  }

  void RecoverRoute(const std::shared_ptr<RouteRuntimeState>& runtime,
                    std::stop_token cancellation) noexcept {
    if (cancellation.stop_requested() || !AdoptRouteRecoveryOwner(runtime)) {
      return;
    }
    if (!runtime->unresolved_recovery_effect.has_value()) {
      if (!runtime->status.accepted_fence.has_value()) {
        auto terminal = runtime->status;
        if (!AdvanceRouteStatus(runtime, terminal, RouteSelectionTransactionPhaseDTO::kFailed)) {
          CloseAdmission();
          return;
        }
        terminal.effect = RouteSelectionEffectDTO::kKnownNoEffect;
        terminal.reconciliation_required = false;
        terminal.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
        terminal.error =
            coordinator_error("route_selection_owner_replaced_before_effect",
                              "the replaced route owner had prepared no external effect",
                              "owner_replaced_known_no_effect");
        terminal.remaining_duration_ns = 0;
        if (RecordRouteStatus(runtime, std::move(terminal))) {
          CompleteRoute(runtime);
        }
        return;
      }
      if (runtime->status.resulting_active_control_configuration.has_value()) {
        auto publishing = runtime->status;
        if (!AdvanceRouteStatus(runtime, publishing,
                                RouteSelectionTransactionPhaseDTO::kPublishing)) {
          CloseAdmission();
          return;
        }
        publishing.reconciliation_required = false;
        publishing.error.reset();
        if (!RecordRouteStatus(runtime, std::move(publishing))) {
          return;
        }
        runtime->recovering = false;
        PublishAndReleaseRoute(runtime, cancellation);
        return;
      }
      if (runtime->controller_source_replaced_before_transition &&
          runtime->status.accepted_fence.has_value()) {
        PublishAndReleaseRoute(runtime, cancellation);
        return;
      }
      ReconcileRecoveredControllerTruth(runtime, cancellation, std::nullopt);
      return;
    }

    const auto prepared = *runtime->unresolved_recovery_effect;
    switch (prepared.effect_kind) {
      case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire:
        ReconcileRecoveredRouteAcquire(runtime, prepared, cancellation);
        return;
      case CCLCRouteEffectKindDTO::kControllerLifecycleTransition:
        ReconcileRecoveredControllerTruth(runtime, cancellation, prepared);
        return;
      case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease:
        ReconcileRecoveredRouteRelease(runtime, prepared, cancellation);
        return;
    }
  }

  void ReconcileRecoveredRouteAcquire(const std::shared_ptr<RouteRuntimeState>& runtime,
                                      CCLCRouteEffectAttemptDTO prepared,
                                      std::stop_token cancellation) noexcept {
    const auto& request = std::get<RouteConfigurationFenceIntentDTO>(prepared.request);
    Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO> reconciled =
        Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO>::Err(
            coordinator_error("route_fence_acquire_reconciliation_failed",
                              "the Kernel route-fence Acquire reconciliation raised an exception",
                              "kernel_exception"));
    try {
      reconciled =
          route_services_->kernel_route_fence_port->ReconcileRouteConfigurationFenceAcquireAttempt(
              request);
    } catch (...) {
    }
    if (!reconciled.ok() || !reconciled.value().locally_valid() ||
        reconciled.value().intent != request ||
        (reconciled.value().disposition !=
             RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact &&
         reconciled.value().disposition !=
             RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen)) {
      ScheduleRoute(runtime);
      return;
    }
    RouteConfigurationFenceAcquireEffectResultDTO result{
        request, RouteConfigurationFenceAcquireEffectDispositionDTO::kProvenNotSeen, std::nullopt};
    auto successor = runtime->status;
    if (reconciled.value().disposition ==
        RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact) {
      result.disposition = RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted;
      result.acceptance = reconciled.value().acceptance;
      if (!AdvanceRouteStatus(runtime, successor, RouteSelectionTransactionPhaseDTO::kObserving)) {
        CloseAdmission();
        return;
      }
      successor.accepted_fence = result.acceptance;
      successor.reconciliation_required = false;
      if (runtime->controller_source_replaced_before_transition) {
        successor.effect = RouteSelectionEffectDTO::kKnownNoEffect;
        successor.error = coordinator_error(
            "route_selection_controller_source_replaced",
            "the controller source changed before any controller transition",
            "controller_source_replaced_before_transition");
      } else {
        successor.error.reset();
      }
    } else {
      if (!AdvanceRouteStatus(runtime, successor, RouteSelectionTransactionPhaseDTO::kFailed)) {
        CloseAdmission();
        return;
      }
      successor.effect = RouteSelectionEffectDTO::kKnownNoEffect;
      successor.reconciliation_required = false;
      successor.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
      successor.error = coordinator_error("route_fence_not_acquired",
                                          "Kernel proved that recovered Acquire had no effect",
                                          "acquire_proven_not_seen");
      successor.remaining_duration_ns = 0;
    }
    prepared.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
    prepared.result = result;
    if (!RecordRouteEffectCompleted(runtime, std::move(prepared), std::move(successor))) {
      return;
    }
    if (result.disposition == RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted) {
      if (runtime->controller_source_replaced_before_transition) {
        PublishAndReleaseRoute(runtime, cancellation);
      } else {
        ReconcileRecoveredControllerTruth(runtime, cancellation, std::nullopt);
      }
    } else {
      CompleteRoute(runtime);
    }
  }

  void ReconcileRecoveredControllerTruth(
      const std::shared_ptr<RouteRuntimeState>& runtime, std::stop_token cancellation,
      std::optional<CCLCRouteEffectAttemptDTO> unresolved_transition) noexcept {
    const auto observed = ObserveRecoveredRoute(runtime);
    if (!observed.ok()) {
      ScheduleRoute(runtime);
      return;
    }
    const auto& observation = observed.value().first;
    const auto& reconciliation = observed.value().second;
    RouteSelectionEffectDTO effect = runtime->status.effect;
    if (unresolved_transition.has_value()) {
      bool unchanged = false;
      if (runtime->status.controller_observation.has_value()) {
        const auto& prior = *runtime->status.controller_observation;
        unchanged = prior.controllers == observation.controllers &&
                    prior.active_chain_edges == observation.active_chain_edges &&
                    prior.endpoints == observation.endpoints;
      }
      effect = unchanged ? RouteSelectionEffectDTO::kKnownNoEffect
                         : RouteSelectionEffectDTO::kPartialEffectObserved;
    } else if (effect == RouteSelectionEffectDTO::kNone ||
               effect == RouteSelectionEffectDTO::kEffectUncertain) {
      effect =
          reconciliation.disposition == ControllerLifecycleReconciliationDispositionDTO::kReconciled
              ? RouteSelectionEffectDTO::kKnownNoEffect
              : RouteSelectionEffectDTO::kPartialEffectObserved;
    }
    const auto configuration =
        ResolveRecoveredRouteConfiguration(runtime, observation, reconciliation, effect);
    if (!configuration.ok()) {
      ScheduleRoute(runtime);
      return;
    }

    auto successor = runtime->status;
    if (!AdvanceRouteStatus(runtime, successor, RouteSelectionTransactionPhaseDTO::kPublishing)) {
      CloseAdmission();
      return;
    }
    successor.controller_observation = observation;
    successor.controller_reconciliation = reconciliation;
    successor.controller_transition_plan.reset();
    successor.effect = effect;
    successor.resulting_active_control_configuration = configuration.value();
    successor.reconciliation_required = false;
    successor.error.reset();
    if (reconciliation.disposition ==
        ControllerLifecycleReconciliationDispositionDTO::kReconciled) {
      successor.affected_control_binding_ids.clear();
      successor.affected_controller_role_ids.clear();
    }

    if (unresolved_transition.has_value()) {
      auto completed = std::move(*unresolved_transition);
      const auto& intent = std::get<ControllerLifecycleTransitionIntentDTO>(completed.request);
      const ControllerLifecycleTransitionResultDTO result{
          intent, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain, std::nullopt,
          coordinator_error("route_transition_reconciled_after_owner_replacement",
                            "fresh observation classified an interrupted transition",
                            "owner_replaced_after_effect_prepare")};
      completed.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
      completed.result = result;
      successor.controller_transition_results.push_back(result);
      if (!RecordRouteEffectCompleted(runtime, std::move(completed), std::move(successor))) {
        return;
      }
    } else if (!RecordRouteStatus(runtime, std::move(successor))) {
      return;
    }
    runtime->recovering = false;
    PublishAndReleaseRoute(runtime, cancellation);
  }

  void ReconcileRecoveredRouteRelease(const std::shared_ptr<RouteRuntimeState>& runtime,
                                      CCLCRouteEffectAttemptDTO prepared,
                                      std::stop_token cancellation) noexcept {
    const auto& request = std::get<RouteConfigurationFenceReleaseRequestDTO>(prepared.request);
    Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO> reconciled =
        Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO>::Err(
            coordinator_error("route_fence_release_reconciliation_failed",
                              "the Kernel route-fence Release reconciliation raised an exception",
                              "kernel_exception"));
    try {
      reconciled =
          route_services_->kernel_route_fence_port->ReconcileRouteConfigurationFenceReleaseAttempt(
              request);
    } catch (...) {
    }
    if (!reconciled.ok() || !reconciled.value().locally_valid() ||
        reconciled.value().request != request ||
        (reconciled.value().disposition !=
             RouteConfigurationFenceReleaseAttemptDispositionDTO::kReleasedExact &&
         reconciled.value().disposition !=
             RouteConfigurationFenceReleaseAttemptDispositionDTO::kNotReleasedHeld)) {
      ScheduleRoute(runtime);
      return;
    }
    if (reconciled.value().disposition ==
        RouteConfigurationFenceReleaseAttemptDispositionDTO::kNotReleasedHeld) {
      RouteConfigurationFenceReleaseEffectResultDTO result{
          request, RouteConfigurationFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld,
          std::nullopt};
      auto successor = runtime->status;
      if (!AdvanceRouteStatus(runtime, successor, RouteSelectionTransactionPhaseDTO::kPublishing)) {
        CloseAdmission();
        return;
      }
      successor.reconciliation_required = false;
      successor.error.reset();
      prepared.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
      prepared.result = result;
      if (!RecordRouteEffectCompleted(runtime, std::move(prepared), std::move(successor))) {
        return;
      }
      runtime->recovering = false;
      PublishAndReleaseRoute(runtime, cancellation);
      return;
    }

    const bool abandon_replaced_source =
        runtime->controller_source_replaced_before_transition;
    if (!abandon_replaced_source &&
        !runtime->status.resulting_active_control_configuration.has_value()) {
      CloseAdmission();
      return;
    }
    const bool reconciled_configuration =
        !abandon_replaced_source &&
        runtime->status.resulting_active_control_configuration->state ==
            ActiveControlConfigurationStateDTO::kReconciled;
    RouteConfigurationFenceReleaseEffectResultDTO result{
        request, RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased,
        reconciled.value().receipt};
    auto terminal = runtime->status;
    if (!AdvanceRouteStatus(
            runtime, terminal,
            reconciled_configuration
                ? RouteSelectionTransactionPhaseDTO::kSucceeded
                : RouteSelectionTransactionPhaseDTO::kFailed)) {
      CloseAdmission();
      return;
    }
    terminal.released_fence = result.receipt;
    terminal.reconciliation_required = false;
    terminal.remaining_duration_ns = 0;
    if (reconciled_configuration) {
      terminal.terminal_result = RouteSelectionTerminalResultDTO::kSucceeded;
      terminal.error.reset();
    } else if (abandon_replaced_source) {
      terminal.effect = RouteSelectionEffectDTO::kKnownNoEffect;
      terminal.terminal_result =
          RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
      terminal.error = coordinator_error(
          "route_selection_controller_source_replaced",
          "the controller source changed before any controller transition",
          "controller_source_replaced_before_transition");
    } else {
      terminal.effect = RouteSelectionEffectDTO::kPartialEffectObserved;
      terminal.terminal_result = RouteSelectionTerminalResultDTO::kFailedPartialEffect;
      terminal.error =
          coordinator_error("route_selection_unreconciled",
                            "the recovered route retained an unreconciled controller configuration",
                            "external_or_partial_configuration_published");
    }
    prepared.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
    prepared.result = result;
    if (RecordRouteEffectCompleted(runtime, std::move(prepared), std::move(terminal))) {
      CompleteRoute(runtime);
    }
  }

  [[nodiscard]] Result<AdmissionFacts> ReadAdmissionFacts(const ComponentLifecycleIntentDTO& intent,
                                                          bool force_full_disable) {
    const auto full_stop_facts = [&]() -> Result<AdmissionFacts> {
      try {
        const auto admission = kernel_disable_handoff_port_.ObserveDisableHandoffAdmission(
            {intent.controllable_component_id, DisableHandoffScopeRequestKindDTO::kFullStopRequired,
             std::nullopt});
        if (!admission.ok()) {
          return Result<AdmissionFacts>::Err(admission.error());
        }
        return Result<AdmissionFacts>::Ok({std::nullopt, {}, admission.value()});
      } catch (...) {
        return Result<AdmissionFacts>::Err(
            coordinator_error("component_lifecycle_full_stop_admission_failed",
                              "the Kernel Full-Stop admission observation raised an exception",
                              "kernel_admission_exception"));
      }
    };
    try {
      if (force_full_disable) {
        return full_stop_facts();
      }
      const auto snapshot =
          current_snapshot_source_.ReadCurrentComponentSnapshot(intent.controllable_component_id);
      if (snapshot.ok()) {
        AdmissionFacts result;
        result.current_snapshot = snapshot.value();
        result.catalogs.reserve(snapshot.value().provider_occurrences.size());
        bool complete_catalog_closure = true;
        for (const auto& provider : snapshot.value().provider_occurrences) {
          const auto catalog = lifecycle_plan_catalog_source_.LoadExactLifecyclePlanCatalog(
              provider.provider_manifest_id, provider.provider_manifest_revision,
              provider.lifecycle_plan_catalog_digest);
          const bool catalog_verified =
              catalog.ok() && VerifyBrandLifecyclePlanCatalog(catalog.value()).ok() &&
              catalog.value().provider_manifest_id == provider.provider_manifest_id &&
              catalog.value().provider_manifest_revision == provider.provider_manifest_revision &&
              catalog.value().catalog_digest == provider.lifecycle_plan_catalog_digest;
          if (!catalog_verified) {
            if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
              return Result<AdmissionFacts>::Err(
                  catalog.ok()
                      ? coordinator_error(
                            "component_lifecycle_catalog_mismatch",
                            "the lifecycle catalog does not match its canonical provider closure",
                            "catalog_digest_or_provider_identity_mismatch")
                      : catalog.error());
            }
            complete_catalog_closure = false;
            break;
          }
          result.catalogs.push_back(catalog.value());
        }
        if (!complete_catalog_closure) {
          result.catalogs.clear();
        }
        if (intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
          const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot.value());
          if (!expectation.ok()) {
            return full_stop_facts();
          }
          const auto admission = kernel_disable_handoff_port_.ObserveDisableHandoffAdmission(
              {intent.controllable_component_id,
               DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation.value()});
          if (!admission.ok()) {
            return full_stop_facts();
          }
          result.disable_admission = admission.value();
        }
        return Result<AdmissionFacts>::Ok(std::move(result));
      }
      if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
        return Result<AdmissionFacts>::Err(snapshot.error());
      }
    } catch (...) {
      if (intent.intent != ComponentLifecycleIntentKindDTO::kDisable) {
        return Result<AdmissionFacts>::Err(coordinator_error(
            "component_lifecycle_admission_source_failed",
            "a lifecycle admission source raised an exception", "admission_source_exception"));
      }
    }
    return full_stop_facts();
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> CommitAdmission(
      const ComponentLifecycleIntentDTO& intent, const AdmissionFacts& facts,
      std::uint64_t admission_gate_generation) {
    std::unique_lock admission_lock(admission_mutex_);
    admission_resolution_ready_.wait(admission_lock, [&] { return !admission_resolution_active_; });
    admission_resolution_active_ = true;
    ScopeExit resolution_guard([&] {
      if (!admission_lock.owns_lock()) {
        admission_lock.lock();
      }
      admission_resolution_active_ = false;
      admission_resolution_ready_.notify_all();
    });
    admission_lock.unlock();
    Result<std::optional<CCLCLifecycleRequestResultDTO>> reinspection =
        Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(coordinator_error(
            "component_lifecycle_journal_inspection_failed",
            "the lifecycle journal inspection raised an exception", "journal_exception"));
    try {
      reinspection = journal_.InspectLifecycleRequest(config_.request_owner_epoch, intent);
    } catch (...) {
    }
    admission_lock.lock();
    if (!reinspection.ok()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(reinspection.error());
    }
    if (reinspection.value().has_value()) {
      return ReplayResult(*reinspection.value());
    }
    if (!accepting_.load(std::memory_order_acquire) ||
        admission_gate_generation != admission_gate_generation_) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_admission_closed",
          "the CCLC admission gate changed before durable acceptance", "admission_gate_changed"));
    }
    if (highest_transaction_generation_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_generation_exhausted",
          "the lifecycle transaction generation is exhausted", "transaction_generation_overflow"));
    }
    const auto next_generation = highest_transaction_generation_ + 1U;
    const auto component_key = intent.controllable_component_id.value();
    const auto watermark = std::find_if(
        component_generation_watermarks_.begin(), component_generation_watermarks_.end(),
        [&](const auto& row) { return row.first == component_key; });
    const auto predecessor_value =
        facts.current_snapshot.has_value()
            ? facts.current_snapshot->component_lifecycle_generation.value()
            : (watermark == component_generation_watermarks_.end()
                   ? intent.expected_component_lifecycle_generation.value()
                   : watermark->second);
    if (watermark != component_generation_watermarks_.end() &&
        watermark->second != predecessor_value) {
      if (intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
            "component_lifecycle_snapshot_changed_during_admission",
            "the resolved Disable snapshot does not match the durable component generation",
            "disable_snapshot_watermark_mismatch_requires_promotion"));
      }
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          coordinator_error("component_lifecycle_generation_conflict",
                            "the coherent snapshot does not match the durable component generation",
                            "component_watermark_mismatch"));
    }
    admission_lock.unlock();
    std::int64_t accepted_at_ns = -1;
    try {
      accepted_at_ns = clock_.now();
    } catch (...) {
    }
    ComponentLifecycleAdmissionCandidate candidate{
        LifecycleTransactionId("lifecycle." + std::to_string(next_generation)),
        LifecycleTransactionGeneration(next_generation),
        ComponentLifecycleGeneration(predecessor_value),
        accepted_at_ns,
        config_.containment_only_disable_timeout_duration_ns,
        facts.current_snapshot,
        facts.catalogs,
        facts.disable_admission};
    const auto prepared = PrepareComponentLifecycleAdmission(intent, candidate);
    if (!prepared.ok()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(prepared.error());
    }
    if (prepared.value().acceptance.accepted_at_ns >
        std::numeric_limits<std::int64_t>::max() -
            prepared.value().acceptance.timeout_duration_ns) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_deadline_overflow",
          "the lifecycle acceptance deadline is not representable in the owner clock domain",
          "accepted_at_plus_timeout_overflow"));
    }
    const auto owner = reservation_owner(prepared.value().acceptance, intent);
    bool reservation_held = false;
    if (!prepared.value().reserved_control_domains.empty()) {
      Result<void> reserved = Result<void>::Err(coordinator_error(
          "component_lifecycle_domain_reservation_failed",
          "the lifecycle domain reservation raised an exception", "reservation_exception"));
      try {
        reserved = domain_reservations_.reserve(owner, prepared.value().reserved_control_domains);
      } catch (...) {
      }
      if (!reserved.ok()) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(reserved.error());
      }
      reservation_held = true;
    }
    ScopeExit reservation_guard([&] {
      if (reservation_held) {
        try {
          static_cast<void>(
              domain_reservations_.release(owner, prepared.value().reserved_control_domains));
        } catch (...) {
        }
      }
    });
    admission_lock.lock();
    const auto release_on_failure = [&] {
      if (reservation_held) {
        const bool relock = admission_lock.owns_lock();
        if (relock) {
          admission_lock.unlock();
        }
        try {
          static_cast<void>(
              domain_reservations_.release(owner, prepared.value().reserved_control_domains));
        } catch (...) {
        }
        reservation_held = false;
        if (relock) {
          admission_lock.lock();
        }
      }
    };

    if (facts.current_snapshot.has_value()) {
      admission_lock.unlock();
      Result<ComponentLifecycleCurrentSnapshotDTO> current =
          Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
              coordinator_error("component_lifecycle_snapshot_reread_failed",
                                "the current component snapshot reread raised an exception",
                                "snapshot_reread_exception"));
      try {
        current =
            current_snapshot_source_.ReadCurrentComponentSnapshot(intent.controllable_component_id);
      } catch (...) {
      }
      admission_lock.lock();
      if (!current.ok() || current.value() != *facts.current_snapshot) {
        release_on_failure();
        return Result<CCLCLifecycleRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_snapshot_changed_during_admission",
                              "the current component snapshot changed after domain reservation",
                              "snapshot_reread_mismatch"));
      }
    }

    std::shared_ptr<RuntimeState> runtime;
    admission_lock.unlock();
    try {
      runtime = std::make_shared<RuntimeState>(
          prepared.value().initial_status, prepared.value().qualified_lifecycle_plan_catalogs,
          prepared.value().lifecycle_mutation_eligible, prepared.value().disable_handoff_admission,
          prepared.value().reserved_control_domains);
    } catch (...) {
      release_on_failure();
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_runtime_capacity_unavailable",
          "the accepted lifecycle runtime state could not be allocated before persistence",
          "runtime_state_allocation_failed"));
    }
    admission_lock.lock();

    const bool provisional_component_watermark =
        watermark == component_generation_watermarks_.end();
    if (provisional_component_watermark) {
      if (component_generation_watermarks_.size() >= kMaxCoordinatorActiveTransactions) {
        release_on_failure();
        return Result<CCLCLifecycleRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_journal_capacity_exhausted",
                              "the bounded component lifecycle watermark table is full",
                              "component_watermark_capacity_exhausted"));
      }
      component_generation_watermarks_.emplace_back(component_key, predecessor_value);
    }
    const auto rollback_provisional_watermark = [&] {
      if (provisional_component_watermark) {
        component_generation_watermarks_.pop_back();
      }
    };

    if (!accepting_.load(std::memory_order_acquire) ||
        admission_gate_generation != admission_gate_generation_) {
      rollback_provisional_watermark();
      release_on_failure();
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          coordinator_error("component_lifecycle_admission_closed",
                            "the CCLC admission gate changed before durable persistence",
                            "admission_precommit_gate_changed"));
    }
    const auto semantic_digest =
        detail::ComponentLifecycleSemanticIntentDigest(intent);
    CellAdmissionClaimIntentDTO claim_intent{
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        config_.request_owner_epoch,
        prepared.value().acceptance.lifecycle_transaction_id.value(),
        intent.request_context.request_id,
        ContentDigest(semantic_digest),
        intent.expected_admission_generation,
        {},
        std::nullopt,
        std::nullopt};
    admission_persistence_active_ = true;
    admission_lock.unlock();
    const auto claim_attempt = journal_.RecordLifecycleClaimAttempt(
        config_.request_owner_epoch, claim_intent);
    if (!claim_attempt.ok()) {
      admission_lock.lock();
      admission_persistence_active_ = false;
      admission_resolution_ready_.notify_all();
      rollback_provisional_watermark();
      release_on_failure();
      CloseAdmissionLocked();
      return Result<CCLCLifecycleRequestResultDTO>::Err(claim_attempt.error());
    }
    Result<CellAdmissionClaimDTO> acquired =
        Result<CellAdmissionClaimDTO>::Err(coordinator_error(
            "component_lifecycle_claim_acquire_unknown",
            "Kernel lifecycle claim acquire raised an exception",
            "claim_acquire_exception"));
    try {
      acquired =
          kernel_cell_admission_port_->AcquireCellAdmissionClaim(claim_intent);
    } catch (...) {
    }
    if (!acquired.ok() || acquired.value().intent != claim_intent) {
      admission_lock.lock();
      admission_persistence_active_ = false;
      admission_resolution_ready_.notify_all();
      rollback_provisional_watermark();
      release_on_failure();
      CloseAdmissionLocked();
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          acquired.ok()
              ? coordinator_error("component_lifecycle_claim_acquire_invalid",
                                  "Kernel returned a foreign lifecycle claim",
                                  "claim_receipt_mismatch")
              : acquired.error());
    }
    const auto recorded_claim = journal_.RecordLifecycleClaimAcquired(
        config_.request_owner_epoch, acquired.value());
    if (!recorded_claim.ok()) {
      admission_lock.lock();
      admission_persistence_active_ = false;
      admission_resolution_ready_.notify_all();
      rollback_provisional_watermark();
      release_on_failure();
      CloseAdmissionLocked();
      return Result<CCLCLifecycleRequestResultDTO>::Err(recorded_claim.error());
    }
    runtime->cell_claim = acquired.value();
    Result<CCLCLifecycleRequestResultDTO> accepted =
        Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
            "component_lifecycle_journal_accept_failed",
            "the lifecycle journal acceptance raised an exception", "journal_exception"));
    try {
      accepted = journal_.AcceptOrReplayLifecycle(config_.request_owner_epoch, intent,
                                                  prepared.value().acceptance,
                                                  prepared.value().initial_status);
    } catch (...) {
    }
    admission_lock.lock();
    admission_persistence_active_ = false;
    admission_resolution_ready_.notify_all();
    if (!accepted.ok()) {
      rollback_provisional_watermark();
      release_on_failure();
      return Result<CCLCLifecycleRequestResultDTO>::Err(accepted.error());
    }
    if (accepted.value().replay_status == RequestReplayStatusDTO::kAcceptedNew) {
      // From this point the journal is authoritative. The reservation must
      // remain owned by the durable transaction even if local publication or
      // worker submission cannot complete; owner recovery will reconstruct it.
      reservation_held = false;
    }
    if (!accepted.value().locally_valid()) {
      rollback_provisional_watermark();
      release_on_failure();
      CloseAdmissionLocked();
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          coordinator_error("component_lifecycle_journal_result_invalid",
                            "the lifecycle journal returned an invalid accepted result",
                            "accepted_result_shape_invalid"));
    }
    if (accepted.value().replay_status != RequestReplayStatusDTO::kAcceptedNew) {
      rollback_provisional_watermark();
      release_on_failure();
      return ReplayResult(accepted.value());
    }
    if (!accepted.value().acceptance.has_value() || !accepted.value().status.has_value() ||
        *accepted.value().acceptance != prepared.value().acceptance ||
        *accepted.value().status != prepared.value().initial_status) {
      rollback_provisional_watermark();
      CloseAdmissionLocked();
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_journal_result_invalid",
          "the durable lifecycle acceptance differs from its exact prepared candidate",
          "accepted_result_candidate_mismatch"));
    }
    highest_transaction_generation_ = next_generation;
    const auto accepted_watermark = std::find_if(
        component_generation_watermarks_.begin(), component_generation_watermarks_.end(),
        [&](const auto& row) { return row.first == component_key; });
    accepted_watermark->second =
        accepted.value().acceptance->component_lifecycle_generation.value();

    try {
      if (runtime->permanent_containment_only ||
          (runtime->disable_admission.has_value() &&
           runtime->disable_admission->scope_resolution ==
               DisableHandoffScopeResolutionDTO::kPromotedToFull)) {
        AcquireOwnerWideContainmentGateLocked(runtime);
      }
      {
        std::lock_guard lock(state_mutex_);
        active_transactions_.push_back(runtime);
      }
      auto& initial_pool = intent.intent == ComponentLifecycleIntentKindDTO::kDisable
                               ? kernel_worker_pool_
                               : worker_pool_;
      if (!initial_pool.Submit(
              [this, runtime](std::stop_token cancellation) { Process(runtime, cancellation); })) {
        CloseAdmissionLocked();
        // Durable acceptance is already authoritative. Reporting rejection
        // here would invite a second request; owner replacement will recover.
      }
    } catch (...) {
      CloseAdmissionLocked();
      // The authoritative durable row and its domain reservation remain for
      // owner recovery. Delivery still returns the accepted result exactly.
    }
    return Result<CCLCLifecycleRequestResultDTO>::Ok(std::move(accepted.value()));
  }

  void Process(const std::shared_ptr<RuntimeState>& runtime,
               std::stop_token cancellation) noexcept {
    runtime->scheduled = false;
    if (cancellation.stop_requested() || terminal(runtime->status)) {
      return;
    }
    try {
      if (runtime->recovering) {
        ProcessRecovered(runtime, cancellation);
        return;
      }
      if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
          !runtime->lifecycle_lane_active) {
        ProcessDisable(runtime, cancellation);
        return;
      }
      if (!runtime->initialized && !Initialize(runtime)) {
        return;
      }
      RunCurrentDomain(runtime, cancellation);
    } catch (const std::exception& exception) {
      if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
        CloseAdmission();
        return;
      }
      FailBeforeEffect(
          runtime, coordinator_error("component_lifecycle_worker_exception",
                                     "the lifecycle worker caught an exception", exception.what()));
    } catch (...) {
      if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
        CloseAdmission();
        return;
      }
      FailBeforeEffect(runtime,
                       coordinator_error("component_lifecycle_worker_exception",
                                         "the lifecycle worker caught an unknown exception",
                                         "unknown_exception"));
    }
  }

  [[nodiscard]] ErrorDTO OwnerReplacementError(std::string reason) const {
    return coordinator_error("component_lifecycle_owner_replaced",
                             "the lifecycle transaction is reconciling after owner replacement",
                             std::move(reason));
  }

  void ClearResolvedOwnerReplacementMarker(ComponentLifecycleStatusDTO& status) const {
    if (status.error.has_value() && status.error->code == "component_lifecycle_owner_replaced") {
      status.error.reset();
      status.reconciliation_required = false;
    }
  }

  [[nodiscard]] bool AdoptRecoveredStatus(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->status.status_owner_epoch == config_.request_owner_epoch) {
      return true;
    }
    auto adopted = runtime->status;
    AdvanceStatus(runtime, adopted,
                  adopted.disable_handoff.has_value()
                      ? ComponentLifecycleTransactionPhaseDTO::kReconciling
                      : ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
    adopted.reconciliation_required = true;
    adopted.error = OwnerReplacementError("recovery_observation_required");
    adopted.remaining_duration_ns = 0;
    if (!adopted.locally_valid() ||
        !journal_.RecordLifecycleStatus(config_.request_owner_epoch, adopted).ok()) {
      CloseAdmission();
      return false;
    }
    runtime->status = std::move(adopted);
    return true;
  }

  void FailRecoveredBeforeEffect(const std::shared_ptr<RuntimeState>& runtime) {
    auto failed = runtime->status;
    AdvanceStatus(runtime, failed, ComponentLifecycleTransactionPhaseDTO::kFailed);
    const auto error = OwnerReplacementError("no_external_effect_was_prepared");
    bool any_observed = false;
    for (auto& domain : failed.domains) {
      any_observed = any_observed ||
                     domain.effect == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
                     domain.effect == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
      if (domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded ||
          domain.action_state == ComponentLifecycleDomainActionStateDTO::kFailed) {
        continue;
      }
      domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
      if (domain.effect == ComponentLifecycleDomainEffectDTO::kNone) {
        domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
      }
      domain.error = error;
    }
    failed.reconciliation_required = false;
    failed.terminal_result = any_observed
                                 ? ComponentLifecycleTerminalResultDTO::kFailedPartialEffect
                                 : ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect;
    failed.error = error;
    failed.remaining_duration_ns = 0;
    if (!failed.locally_valid() ||
        !CommitTerminalAndReleaseClaim(runtime, std::move(failed))) {
      return;
    }
    runtime->recovering = false;
    Complete(runtime);
  }

  void ProcessRecovered(const std::shared_ptr<RuntimeState>& runtime,
                        std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
      return;
    }
    if (runtime->unresolved_recovery_effect.has_value()) {
      const auto prepared = *runtime->unresolved_recovery_effect;
      switch (runtime->unresolved_recovery_effect->effect_kind) {
        case CCLCEffectKindDTO::kBrandLifecyclePrimitive:
          CompleteRecoveredBrandPrepareAsUncertain(runtime, cancellation);
          return;
        case CCLCEffectKindDTO::kKernelDisableHandoff:
          if (!AdoptRecoveredStatus(runtime)) {
            return;
          }
          runtime->recovering = false;
          runtime->unresolved_recovery_effect.reset();
          ReconcilePreparedDisableHandoff(runtime, *prepared.disable_handoff_request);
          return;
        case CCLCEffectKindDTO::kKernelDisableFenceRelease:
          if (!AdoptRecoveredStatus(runtime)) {
            return;
          }
          runtime->recovering = false;
          runtime->unresolved_recovery_effect.reset();
          ReconcilePreparedDisableFenceRelease(runtime, *prepared.disable_fence_release_request);
          return;
      }
    }

    if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
      if (!AdoptRecoveredStatus(runtime)) {
        return;
      }
      runtime->recovering = false;
      runtime->lifecycle_mutation_eligible = false;
      if (!runtime->status.disable_handoff.has_value()) {
        if (!RefreshDisableAdmission(runtime)) {
          // No external effect has been prepared. Keep recovery and the
          // admission gate in place while retrying only the read-only Kernel
          // admission observation under the replacement owner.
          ScheduleDisableHandoffSuccessor(runtime);
          return;
        }
        StartDisableHandoff(runtime);
      } else if (runtime->status.disable_handoff->state == DisableHandoffStateDTO::kStopping) {
        ScheduleKernelPoll(runtime);
      } else if (runtime->status.disable_handoff->state == DisableHandoffStateDTO::kReady) {
        const bool domains_resolved = std::all_of(
            runtime->status.domains.begin(), runtime->status.domains.end(), [](const auto& domain) {
              return (domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded ||
                      domain.action_state == ComponentLifecycleDomainActionStateDTO::kFailed) &&
                     domain.effect != ComponentLifecycleDomainEffectDTO::kEffectUncertain;
            });
        if (domains_resolved) {
          if (ClearRecoveredDisableReconciliationForRelease(runtime)) {
            ScheduleDisableRelease(runtime);
          }
        } else {
          ResolveDisableWithoutVendorMutation(
              runtime, OwnerReplacementError("disable_recovery_is_containment_only"));
        }
      } else {
        CloseAdmission();
      }
      return;
    }

    const auto pending_domain = std::find_if(
        runtime->status.domains.begin(), runtime->status.domains.end(), [](const auto& domain) {
          return !domain.primitive_attempts.empty() &&
                 domain.action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded &&
                 domain.action_state != ComponentLifecycleDomainActionStateDTO::kFailed;
        });
    if (pending_domain == runtime->status.domains.end()) {
      FailRecoveredBeforeEffect(runtime);
      return;
    }
    if (!AdoptRecoveredStatus(runtime)) {
      return;
    }
    ReconcileRecoveredBrandObservation(
        runtime, static_cast<std::size_t>(pending_domain - runtime->status.domains.begin()),
        cancellation);
  }

  void CompleteRecoveredBrandPrepareAsUncertain(const std::shared_ptr<RuntimeState>& runtime,
                                                std::stop_token cancellation) {
    const auto prepared = *runtime->unresolved_recovery_effect;
    const auto& intent = *prepared.lifecycle_primitive_intent;
    const auto domain = std::find_if(
        runtime->status.domains.begin(), runtime->status.domains.end(), [&](const auto& candidate) {
          return candidate.control_domain_id == intent.control_domain_id;
        });
    if (domain == runtime->status.domains.end()) {
      CloseAdmission();
      return;
    }
    const auto domain_index = static_cast<std::size_t>(domain - runtime->status.domains.begin());
    const auto error = OwnerReplacementError("owner_replaced_after_effect_prepare");
    BrandLifecyclePrimitiveResultDTO result{
        intent.lifecycle_transaction_id,
        intent.lifecycle_transaction_generation,
        intent.controllable_component_id,
        intent.control_domain_id,
        intent.control_domain_revision,
        intent.provider_instance_id,
        intent.provider_manifest_id,
        intent.provider_manifest_revision,
        intent.provider_runtime_unit_id,
        intent.provider_runtime_epoch,
        intent.lifecycle_primitive_id,
        intent.lifecycle_primitive_attempt_id,
        intent.expected_observation_generation,
        LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain,
        LifecyclePrimitiveEffectDTO::kPossible,
        std::nullopt,
        error};
    auto completed_status = runtime->status;
    AdvanceStatus(runtime, completed_status,
                  ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
    auto& completed_domain = completed_status.domains[domain_index];
    completed_domain.primitive_attempts.push_back(result);
    completed_domain.action_state = ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown;
    completed_domain.effect = ComponentLifecycleDomainEffectDTO::kEffectUncertain;
    completed_domain.error = error;
    completed_status.reconciliation_required = true;
    completed_status.error = error;
    completed_status.remaining_duration_ns = 0;
    auto completed_attempt = prepared;
    completed_attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
    completed_attempt.lifecycle_primitive_result = result;
    if (!completed_status.locally_valid() ||
        !journal_
             .RecordEffectCompleted(config_.request_owner_epoch, completed_attempt,
                                    completed_status)
             .ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(completed_status);
    runtime->unresolved_recovery_effect.reset();
    ReconcileRecoveredBrandObservation(runtime, domain_index, cancellation);
  }

  void ReconcileRecoveredBrandObservation(const std::shared_ptr<RuntimeState>& runtime,
                                          std::size_t index, std::stop_token cancellation) {
    if (cancellation.stop_requested() || index >= runtime->status.domains.size()) {
      return;
    }
    auto& domain = runtime->status.domains[index];
    if (!domain.resolved_provider_occurrence.has_value() || domain.primitive_attempts.empty()) {
      CloseAdmission();
      return;
    }
    const auto& provider = *domain.resolved_provider_occurrence;
    Result<BrandLifecyclePlanCatalogDTO> catalog = Result<BrandLifecyclePlanCatalogDTO>::Err(
        coordinator_error("component_lifecycle_recovery_catalog_unavailable",
                          "the exact lifecycle catalog could not be reloaded during recovery",
                          "catalog_source_exception"));
    Result<std::shared_ptr<IBrandLifecyclePort>> port =
        Result<std::shared_ptr<IBrandLifecyclePort>>::Err(
            coordinator_error("component_lifecycle_recovery_port_unavailable",
                              "the exact lifecycle port could not be resolved during recovery",
                              "port_resolver_exception"));
    try {
      catalog = lifecycle_plan_catalog_source_.LoadExactLifecyclePlanCatalog(
          provider.provider_manifest_id, provider.provider_manifest_revision,
          provider.lifecycle_plan_catalog_digest);
      port = lifecycle_port_resolver_.ResolveExactLifecyclePort(provider);
    } catch (...) {
      SchedulePoll(runtime);
      return;
    }
    if (!catalog.ok() || !port.ok()) {
      SchedulePoll(runtime);
      return;
    }
    if (!VerifyBrandLifecyclePlanCatalog(catalog.value()).ok() ||
        catalog.value().provider_manifest_id != provider.provider_manifest_id ||
        catalog.value().provider_manifest_revision != provider.provider_manifest_revision ||
        catalog.value().catalog_digest != provider.lifecycle_plan_catalog_digest || !port.value()) {
      CloseAdmission();
      return;
    }
    const auto& last_result = domain.primitive_attempts.back();
    const auto plan = std::find_if(
        catalog.value().plans.begin(), catalog.value().plans.end(), [&](const auto& candidate) {
          return candidate.intent == runtime->status.accepted_intent.intent &&
                 runtime->status.requested_state.state.has_value() &&
                 candidate.requested_state == *runtime->status.requested_state.state;
        });
    if (plan == catalog.value().plans.end()) {
      CloseAdmission();
      return;
    }
    const LifecyclePrimitiveTransitionDTO* transition = nullptr;
    for (const auto& candidate : plan->transitions) {
      if (candidate.lifecycle_primitive_id == last_result.lifecycle_primitive_id) {
        if (transition != nullptr) {
          CloseAdmission();
          return;
        }
        transition = &candidate;
      }
    }
    if (transition == nullptr) {
      CloseAdmission();
      return;
    }
    Result<BrandLifecycleObservationDTO> observation = Result<BrandLifecycleObservationDTO>::Err(
        coordinator_error("component_lifecycle_recovery_observation_unavailable",
                          "the exact lifecycle observation could not be read during recovery",
                          "observation_exception"));
    std::int64_t now_ns = -1;
    try {
      observation = port.value()->Observe(ObservationQuery(runtime->status, domain));
      now_ns = clock_.now();
    } catch (...) {
      SchedulePoll(runtime);
      return;
    }
    const auto minimum_observation_generation = std::max(
        last_result.expected_observation_generation, runtime->expected_observation_generation);
    if (!FreshObservation(runtime->status, domain, observation, now_ns) ||
        observation.value().observation_generation <= minimum_observation_generation ||
        (domain.reconciliation_evidence.has_value() &&
         observation.value().source_sequence <= domain.reconciliation_evidence->source_sequence)) {
      SchedulePoll(runtime);
      return;
    }
    const auto postcondition =
        EvaluateLifecyclePredicate(transition->postcondition_predicate, observation.value());
    const auto resolution = classify_uncertain_effect(*transition, domain, std::nullopt,
                                                      observation.value(), postcondition);
    auto reconciled = runtime->status;
    AdvanceStatus(runtime, reconciled, ComponentLifecycleTransactionPhaseDTO::kReconciling);
    reconciled.remaining_duration_ns = 0;
    auto& reconciled_domain = reconciled.domains[index];
    reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kReconciling;
    reconciled_domain.reconciliation_evidence =
        LifecycleReconciliationEvidenceDTO{observation.value().provider_instance_id,
                                           observation.value().provider_manifest_id,
                                           observation.value().provider_manifest_revision,
                                           observation.value().provider_runtime_unit_id,
                                           observation.value().provider_runtime_epoch,
                                           observation.value().source_sequence,
                                           observation.value().observation_generation,
                                           observation.value().observed_state,
                                           postcondition,
                                           now_ns};
    switch (resolution) {
      case UncertainEffectResolution::kRequestedEffectObserved:
        reconciled_domain.current_state = observation.value().observed_state;
        reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
        reconciled_domain.effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
        reconciled_domain.error.reset();
        reconciled.reconciliation_required = false;
        reconciled.error.reset();
        break;
      case UncertainEffectResolution::kKnownNoEffect:
        reconciled_domain.current_state = observation.value().observed_state;
        reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
        reconciled_domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
        reconciled_domain.error =
            coordinator_error("component_lifecycle_primitive_known_no_effect",
                              "fresh owner-recovery evidence proved no relevant primitive effect",
                              "state_only_observation_unchanged_after_owner_replacement");
        reconciled.reconciliation_required = false;
        reconciled.error = reconciled_domain.error;
        break;
      case UncertainEffectResolution::kPartialEffectObserved:
        reconciled_domain.current_state = observation.value().observed_state;
        reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
        reconciled_domain.effect = ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
        reconciled_domain.error = coordinator_error(
            "component_lifecycle_partial_effect_observed",
            "fresh owner-recovery evidence observed a change without the requested postcondition",
            "postcondition_not_satisfied_after_owner_replacement");
        reconciled.reconciliation_required = false;
        reconciled.error = reconciled_domain.error;
        break;
      case UncertainEffectResolution::kUnresolved:
        break;
    }
    if (!reconciled.locally_valid()) {
      CloseAdmission();
      return;
    }
    const auto recorded_reconciliation =
        journal_.RecordLifecycleStatus(config_.request_owner_epoch, reconciled);
    if (!recorded_reconciliation.ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(reconciled);
    if (resolution != UncertainEffectResolution::kUnresolved) {
      ReleaseUnresolvedEffectAdmissionGate(runtime);
    }
    if (resolution == UncertainEffectResolution::kRequestedEffectObserved) {
      if (std::all_of(runtime->status.domains.begin(), runtime->status.domains.end(),
                      [](const auto& candidate) {
                        return candidate.action_state ==
                               ComponentLifecycleDomainActionStateDTO::kSucceeded;
                      })) {
        runtime->recovering = false;
        Succeed(runtime);
      } else {
        FailRecoveredBeforeEffect(runtime);
      }
      return;
    }
    if (resolution == UncertainEffectResolution::kKnownNoEffect ||
        resolution == UncertainEffectResolution::kPartialEffectObserved) {
      runtime->recovering = false;
      FailTransaction(runtime, *runtime->status.domains[index].error);
      return;
    }
    runtime->expected_observation_generation = observation.value().observation_generation;
    SchedulePoll(runtime);
  }

  [[nodiscard]] bool ClearRecoveredDisableReconciliationForRelease(
      const std::shared_ptr<RuntimeState>& runtime) {
    if (!runtime->status.reconciliation_required &&
        runtime->status.phase == ComponentLifecycleTransactionPhaseDTO::kVerifying) {
      return true;
    }
    auto resolved = runtime->status;
    AdvanceStatus(runtime, resolved, ComponentLifecycleTransactionPhaseDTO::kVerifying);
    ClearResolvedOwnerReplacementMarker(resolved);
    resolved.reconciliation_required = false;
    resolved.remaining_duration_ns = 0;
    if (!resolved.locally_valid()) {
      CloseAdmission();
      return false;
    }
    const auto recorded_resolution =
        journal_.RecordLifecycleStatus(config_.request_owner_epoch, resolved);
    if (!recorded_resolution.ok()) {
      CloseAdmission();
      return false;
    }
    runtime->status = std::move(resolved);
    return true;
  }

  [[nodiscard]] Result<DisableHandoffRequestDTO> BuildDisableHandoffRequest(
      const std::shared_ptr<RuntimeState>& runtime, std::uint64_t effect_sequence) const {
    if (!runtime->disable_admission.has_value() || !runtime->disable_admission->locally_valid()) {
      return Result<DisableHandoffRequestDTO>::Err(coordinator_error(
          "component_lifecycle_disable_admission_missing",
          "the accepted Disable transaction has no retained Kernel admission snapshot",
          "disable_admission_absent"));
    }
    const auto& admission = *runtime->disable_admission;
    DisableHandoffRequestDTO request{
        {RequestId(
             "cclc.disable.handoff." +
             std::to_string(runtime->status.acceptance.lifecycle_transaction_generation.value()) +
             "." + std::to_string(effect_sequence)),
         admission.request_owner_epoch, "CCLC Disable handoff",
         runtime->status.accepted_intent.request_context.request_id.value()},
        runtime->status.accepted_intent.request_context.request_id,
        runtime->status.acceptance.lifecycle_transaction_id,
        runtime->status.acceptance.lifecycle_transaction_generation,
        runtime->status.accepted_intent.controllable_component_id,
        admission.scope_resolution,
        admission.current_scope,
        admission.snapshot_digest,
        admission.active_publication_generation,
        admission.active_public_pointer_digest,
        admission.graph_revision,
        admission.retained_bundle_id,
        admission.query.component_expectation.has_value()
            ? admission.query.component_expectation->component_lifecycle_generation
            : runtime->status.accepted_intent.expected_component_lifecycle_generation,
        admission.operation_resource_generation,
        admission.configuration_fence_generation,
        admission.current_active_control_configurations};
    if (!request.locally_valid()) {
      return Result<DisableHandoffRequestDTO>::Err(coordinator_error(
          "component_lifecycle_disable_handoff_request_invalid",
          "the retained admission facts cannot form an exact Disable handoff request",
          "handoff_request_construction_failed"));
    }
    return Result<DisableHandoffRequestDTO>::Ok(std::move(request));
  }

  [[nodiscard]] CCLCEffectAttemptDTO PreparedDisableHandoffAttempt(
      const DisableHandoffRequestDTO& request, std::uint64_t effect_sequence) const {
    return {CCLCRequestKindDTO::kComponentLifecycle,
            request.lifecycle_transaction_id,
            request.lifecycle_transaction_generation,
            effect_sequence,
            CCLCEffectKindDTO::kKernelDisableHandoff,
            CCLCEffectAttemptStateDTO::kPrepared,
            std::nullopt,
            std::nullopt,
            request,
            std::nullopt,
            std::nullopt,
            std::nullopt};
  }

  void ProcessDisable(const std::shared_ptr<RuntimeState>& runtime, std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
      return;
    }
    if (!runtime->status.disable_handoff.has_value()) {
      StartDisableHandoff(runtime);
      return;
    }
    switch (runtime->status.disable_handoff->state) {
      case DisableHandoffStateDTO::kStopping:
        PollDisableHandoff(runtime);
        return;
      case DisableHandoffStateDTO::kReady:
        ContinueDisableAfterReady(runtime);
        return;
      case DisableHandoffStateDTO::kFailed:
        CloseAdmission();
        return;
    }
  }

  void StartDisableHandoff(const std::shared_ptr<RuntimeState>& runtime) {
    const auto effect_sequence = NextEffectSequence(runtime->effect_sequence);
    if (!effect_sequence.ok()) {
      CloseAdmission();
      return;
    }
    const auto request = BuildDisableHandoffRequest(runtime, effect_sequence.value());
    if (!request.ok()) {
      CloseAdmission();
      return;
    }

    auto prepared_status = runtime->status;
    AdvanceStatus(runtime, prepared_status,
                  ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains);
    ClearResolvedOwnerReplacementMarker(prepared_status);
    const auto prepared_attempt =
        PreparedDisableHandoffAttempt(request.value(), effect_sequence.value());
    const auto prepared = journal_.RecordEffectPrepared(config_.request_owner_epoch,
                                                        prepared_attempt, prepared_status);
    if (!prepared.ok()) {
      CloseAdmission();
      return;
    }
    runtime->effect_sequence = effect_sequence.value();
    runtime->status = std::move(prepared_status);

    std::optional<DisableHandoffAcceptedDTO> acceptance;
    std::optional<DisableHandoffStatusDTO> status;
    try {
      const auto begun = kernel_disable_handoff_port_.BeginDisableHandoff(request.value());
      if (begun.ok() && begun.value().matches(request.value())) {
        acceptance = begun.value();
        const auto observed = kernel_disable_handoff_port_.DisableHandoffStatus(
            request.value().lifecycle_transaction_id,
            request.value().lifecycle_transaction_generation);
        if (observed.ok() &&
            handoff_pair_matches_request(request.value(), *acceptance, observed.value())) {
          status = observed.value();
        }
      }
    } catch (...) {
      // The durable prepared row makes the effect uncertain. Resolve only
      // through the exact read-only reconciliation operation below.
    }
    if (acceptance.has_value() && status.has_value()) {
      CompleteDisableHandoff(runtime, request.value(), *acceptance, *status);
      return;
    }
    ReconcilePreparedDisableHandoff(runtime, request.value());
  }

  void ReconcilePreparedDisableHandoff(const std::shared_ptr<RuntimeState>& runtime,
                                       const DisableHandoffRequestDTO& request) {
    AcquireUnresolvedEffectAdmissionGate(runtime);
    try {
      const auto reconciliation =
          kernel_disable_handoff_port_.ReconcileDisableHandoffAttempt(request);
      if (!reconciliation.ok()) {
        AcquireUnresolvedEffectAdmissionGate(runtime);
        ScheduleDisableHandoffReconciliation(runtime, request);
        return;
      }
      if (!reconciliation.value().locally_valid() || reconciliation.value().request != request) {
        CloseAdmission();
        return;
      }
      if (reconciliation.value().disposition ==
          DisableHandoffAttemptDispositionDTO::kAcceptedExact) {
        CompleteDisableHandoff(runtime, request, *reconciliation.value().acceptance,
                               *reconciliation.value().status);
        return;
      }
      if (reconciliation.value().disposition == DisableHandoffAttemptDispositionDTO::kNotSeen) {
        auto completed_status = runtime->status;
        AdvanceStatus(runtime, completed_status,
                      ComponentLifecycleTransactionPhaseDTO::kAcquiringDomains);
        ClearResolvedOwnerReplacementMarker(completed_status);
        auto completed_attempt = PreparedDisableHandoffAttempt(request, runtime->effect_sequence);
        completed_attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
        completed_attempt.disable_handoff_result = DisableHandoffEffectResultDTO{
            request, DisableHandoffEffectDispositionDTO::kProvenNotSeen, std::nullopt};
        const auto completed = journal_.RecordEffectCompleted(config_.request_owner_epoch,
                                                              completed_attempt, completed_status);
        if (!completed.ok()) {
          CloseAdmission();
          return;
        }
        runtime->status = std::move(completed_status);
        if (!RefreshDisableAdmission(runtime)) {
          ScheduleDisableHandoffSuccessor(runtime);
          return;
        }
        StartDisableHandoff(runtime);
        return;
      }
    } catch (...) {
    }
    AcquireUnresolvedEffectAdmissionGate(runtime);
    ScheduleDisableHandoffReconciliation(runtime, request);
  }

  void ScheduleDisableHandoffReconciliation(const std::shared_ptr<RuntimeState>& runtime,
                                            DisableHandoffRequestDTO request) {
    if (runtime->scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->scheduled = true;
    const auto weak = std::weak_ptr<RuntimeState>(runtime);
    if (!ScheduleLifecycleCallback(
            checked_schedule_time(clock_.now(), config_.observation_poll_period_ns),
            [this, weak, request = std::move(request)] {
              if (shutting_down_.load(std::memory_order_acquire)) {
                return;
              }
              if (const auto state = weak.lock(); state) {
                state->scheduled = false;
                if (!kernel_worker_pool_.Submit(
                        [this, state, request](std::stop_token cancellation) {
                          if (!cancellation.stop_requested()) {
                            ReconcilePreparedDisableHandoff(state, request);
                          }
                        })) {
                  CloseAdmission();
                }
              }
            })) {
      runtime->scheduled = false;
    }
  }

  void ScheduleDisableHandoffSuccessor(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->scheduled = true;
    const auto weak = std::weak_ptr<RuntimeState>(runtime);
    if (!ScheduleLifecycleCallback(
            checked_schedule_time(clock_.now(), config_.observation_poll_period_ns), [this, weak] {
              if (shutting_down_.load(std::memory_order_acquire)) {
                return;
              }
              if (const auto state = weak.lock(); state) {
                state->scheduled = false;
                if (!kernel_worker_pool_.Submit([this, state](std::stop_token cancellation) {
                      if (cancellation.stop_requested()) {
                        return;
                      }
                      if (!RefreshDisableAdmission(state)) {
                        ScheduleDisableHandoffSuccessor(state);
                        return;
                      }
                      StartDisableHandoff(state);
                    })) {
                  CloseAdmission();
                }
              }
            })) {
      runtime->scheduled = false;
    }
  }

  [[nodiscard]] bool RefreshDisableAdmission(const std::shared_ptr<RuntimeState>& runtime) {
    try {
      const bool full_stop = runtime->disable_admission.has_value() &&
                             runtime->disable_admission->query.request_kind ==
                                 DisableHandoffScopeRequestKindDTO::kFullStopRequired;
      if (full_stop) {
        const auto admission = kernel_disable_handoff_port_.ObserveDisableHandoffAdmission(
            {runtime->status.accepted_intent.controllable_component_id,
             DisableHandoffScopeRequestKindDTO::kFullStopRequired, std::nullopt});
        if (!admission.ok() || !admission.value().locally_valid()) {
          return false;
        }
        runtime->disable_admission = admission.value();
        return true;
      }
      const auto snapshot = current_snapshot_source_.ReadCurrentComponentSnapshot(
          runtime->status.accepted_intent.controllable_component_id);
      if (!snapshot.ok()) {
        return false;
      }
      const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot.value());
      if (!expectation.ok()) {
        return false;
      }
      const auto admission = kernel_disable_handoff_port_.ObserveDisableHandoffAdmission(
          {runtime->status.accepted_intent.controllable_component_id,
           DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation.value()});
      if (!admission.ok() || !admission.value().locally_valid()) {
        return false;
      }
      runtime->disable_admission = admission.value();
      return true;
    } catch (...) {
      return false;
    }
  }

  void CompleteDisableHandoff(const std::shared_ptr<RuntimeState>& runtime,
                              const DisableHandoffRequestDTO& request,
                              const DisableHandoffAcceptedDTO& acceptance,
                              const DisableHandoffStatusDTO& handoff) {
    if (!handoff_pair_matches_request(request, acceptance, handoff)) {
      CloseAdmission();
      return;
    }
    auto completed_status = runtime->status;
    AdvanceStatus(runtime, completed_status, ComponentLifecycleTransactionPhaseDTO::kContaining);
    ClearResolvedOwnerReplacementMarker(completed_status);
    completed_status.disable_handoff = handoff;
    completed_status.configuration_fence = handoff.state == DisableHandoffStateDTO::kReady
                                               ? std::optional(handoff.configuration_fence_handle)
                                               : std::nullopt;
    auto completed_attempt = PreparedDisableHandoffAttempt(request, runtime->effect_sequence);
    completed_attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
    completed_attempt.disable_handoff_result = DisableHandoffEffectResultDTO{
        request, DisableHandoffEffectDispositionDTO::kAccepted, acceptance};
    const auto completed = journal_.RecordEffectCompleted(config_.request_owner_epoch,
                                                          completed_attempt, completed_status);
    if (!completed.ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(completed_status);
    if (handoff.scope_resolution == DisableHandoffScopeResolutionDTO::kPromotedToFull) {
      AcquireOwnerWideContainmentGate(runtime);
    }
    ReleaseUnresolvedEffectAdmissionGate(runtime);
    if (handoff.state == DisableHandoffStateDTO::kReady) {
      ContinueDisableAfterReady(runtime);
    } else if (handoff.state == DisableHandoffStateDTO::kStopping) {
      ScheduleKernelPoll(runtime);
    } else {
      CloseAdmission();
    }
  }

  void PollDisableHandoff(const std::shared_ptr<RuntimeState>& runtime) {
    Result<DisableHandoffStatusDTO> observed =
        Result<DisableHandoffStatusDTO>::Err(coordinator_error(
            "component_lifecycle_disable_handoff_status_unavailable",
            "the Disable handoff status could not be observed", "status_call_failed"));
    try {
      observed = kernel_disable_handoff_port_.DisableHandoffStatus(
          runtime->status.acceptance.lifecycle_transaction_id,
          runtime->status.acceptance.lifecycle_transaction_generation);
    } catch (...) {
    }
    if (!observed.ok() || !observed.value().locally_valid() ||
        !runtime->status.disable_handoff.has_value()) {
      ScheduleKernelPoll(runtime);
      return;
    }
    const auto& prior = *runtime->status.disable_handoff;
    const auto& current = observed.value();
    if (current.lifecycle_transaction_id != prior.lifecycle_transaction_id ||
        current.lifecycle_transaction_generation != prior.lifecycle_transaction_generation ||
        current.controllable_component_id != prior.controllable_component_id ||
        current.scope_resolution != prior.scope_resolution ||
        current.component_scope_digest != prior.component_scope_digest ||
        current.configuration_fence_id != prior.configuration_fence_id ||
        current.configuration_fence_generation != prior.configuration_fence_generation ||
        current.scope != prior.scope) {
      CloseAdmission();
      return;
    }
    auto successor = runtime->status;
    AdvanceStatus(runtime, successor, ComponentLifecycleTransactionPhaseDTO::kContaining);
    successor.disable_handoff = current;
    successor.configuration_fence = current.state == DisableHandoffStateDTO::kReady
                                        ? std::optional(current.configuration_fence_handle)
                                        : std::nullopt;
    if (!journal_.RecordLifecycleStatus(config_.request_owner_epoch, successor).ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(successor);
    if (current.state == DisableHandoffStateDTO::kReady) {
      ContinueDisableAfterReady(runtime);
    } else if (current.state == DisableHandoffStateDTO::kStopping) {
      ScheduleKernelPoll(runtime);
    } else {
      CloseAdmission();
    }
  }

  void ScheduleKernelPoll(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->scheduled = true;
    const auto weak = std::weak_ptr<RuntimeState>(runtime);
    if (!ScheduleLifecycleCallback(
            checked_schedule_time(clock_.now(), config_.observation_poll_period_ns), [this, weak] {
              if (shutting_down_.load(std::memory_order_acquire)) {
                return;
              }
              if (const auto state = weak.lock(); state) {
                state->scheduled = false;
                if (!kernel_worker_pool_.Submit([this, state](std::stop_token cancellation) {
                      Process(state, cancellation);
                    })) {
                  CloseAdmission();
                }
              }
            })) {
      runtime->scheduled = false;
    }
  }

  [[nodiscard]] Result<void> ValidateDisableMutationClosure(
      const std::shared_ptr<RuntimeState>& runtime) {
    if (!runtime->lifecycle_mutation_eligible || runtime->permanent_containment_only ||
        !runtime->status.disable_handoff.has_value() ||
        runtime->status.disable_handoff->state != DisableHandoffStateDTO::kReady ||
        runtime->status.disable_handoff->scope_resolution !=
            DisableHandoffScopeResolutionDTO::kExact ||
        runtime->status.acceptance.component_lifecycle_generation.value() !=
            runtime->status.accepted_intent.expected_component_lifecycle_generation.value() + 1U) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_disable_mutation_not_eligible",
          "the contained Disable transaction is not eligible for vendor lifecycle mutation",
          "accepted_scope_or_plan_not_exact"));
    }
    const auto snapshot = current_snapshot_source_.ReadCurrentComponentSnapshot(
        runtime->status.accepted_intent.controllable_component_id);
    if (!snapshot.ok() || !snapshot.value().locally_valid() ||
        snapshot.value().component_lifecycle_generation !=
            runtime->status.acceptance.component_lifecycle_generation ||
        snapshot.value().active_publication_receipt.request.candidate_graph_revision !=
            runtime->status.accepted_intent.expected_graph_revision ||
        snapshot.value().domains.size() != runtime->status.domains.size() ||
        snapshot.value().domains.size() !=
            runtime->status.accepted_intent.expected_control_domains.size() ||
        snapshot.value().provider_occurrences.size() != runtime->status.domains.size()) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_disable_mutation_snapshot_mismatch",
          "the under-fence component snapshot does not match the accepted Disable closure",
          "under_fence_snapshot_mismatch"));
    }
    const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot.value());
    if (!expectation.ok() ||
        expectation.value().component_scope != runtime->status.disable_handoff->scope) {
      return Result<void>::Err(
          coordinator_error("component_lifecycle_disable_mutation_scope_mismatch",
                            "the under-fence component scope differs from the ready Kernel fence",
                            "under_fence_scope_mismatch"));
    }
    std::vector<BrandLifecyclePlanCatalogDTO> fresh_catalogs;
    fresh_catalogs.reserve(runtime->status.domains.size());
    std::optional<std::int64_t> later_plan_timeout_ns;
    for (std::size_t index = 0U; index < runtime->status.domains.size(); ++index) {
      const auto& domain = runtime->status.domains[index];
      if (!domain.expected_domain.has_value() || !domain.current_scope.has_value() ||
          !domain.resolved_provider_occurrence.has_value() ||
          *domain.expected_domain !=
              runtime->status.accepted_intent.expected_control_domains[index] ||
          !ExpectedDomainMatchesResolvedScope(*domain.expected_domain,
                                              snapshot.value().domains[index]) ||
          *domain.current_scope != snapshot.value().domains[index] ||
          *domain.resolved_provider_occurrence != snapshot.value().provider_occurrences[index]) {
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_disable_mutation_domain_mismatch",
            "the under-fence domain/provider closure differs from accepted Disable facts",
            "under_fence_domain_or_provider_mismatch"));
      }
      const auto catalog = lifecycle_plan_catalog_source_.LoadExactLifecyclePlanCatalog(
          snapshot.value().provider_occurrences[index].provider_manifest_id,
          snapshot.value().provider_occurrences[index].provider_manifest_revision,
          snapshot.value().provider_occurrences[index].lifecycle_plan_catalog_digest);
      if (!catalog.ok() || !VerifyBrandLifecyclePlanCatalog(catalog.value()).ok() ||
          catalog.value().provider_manifest_id !=
              snapshot.value().provider_occurrences[index].provider_manifest_id ||
          catalog.value().provider_manifest_revision !=
              snapshot.value().provider_occurrences[index].provider_manifest_revision ||
          catalog.value().catalog_digest !=
              snapshot.value().provider_occurrences[index].lifecycle_plan_catalog_digest) {
        return Result<void>::Err(
            catalog.ok()
                ? coordinator_error(
                      "component_lifecycle_disable_mutation_catalog_mismatch",
                      "the under-fence lifecycle catalog differs from the qualified closure",
                      "under_fence_catalog_mismatch")
                : catalog.error());
      }
      const auto timeout_ns = disable_plan_timeout(catalog.value());
      if (!timeout_ns.has_value()) {
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_disable_mutation_plan_unqualified",
            "the fresh under-fence lifecycle catalog has no unique qualified Disable plan",
            "under_fence_disable_plan_missing_or_ambiguous"));
      }
      if (runtime->qualified_catalogs.has_value() &&
          (runtime->qualified_catalogs->size() != runtime->status.domains.size() ||
           catalog.value() != (*runtime->qualified_catalogs)[index])) {
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_disable_mutation_catalog_mismatch",
            "the under-fence lifecycle catalog differs from the accepted qualified closure",
            "under_fence_catalog_mismatch"));
      }
      later_plan_timeout_ns = later_plan_timeout_ns.has_value()
                                  ? std::min(*later_plan_timeout_ns, *timeout_ns)
                                  : *timeout_ns;
      fresh_catalogs.push_back(catalog.value());
    }
    if (!later_plan_timeout_ns.has_value()) {
      return Result<void>::Err(
          coordinator_error("component_lifecycle_disable_mutation_plan_unqualified",
                            "the fresh under-fence lifecycle catalog closure is empty",
                            "under_fence_catalog_closure_empty"));
    }
    runtime->qualified_catalogs = std::move(fresh_catalogs);
    const auto accepted_at_ns = runtime->status.acceptance.accepted_at_ns;
    const auto later_deadline_ns =
        accepted_at_ns > std::numeric_limits<std::int64_t>::max() - *later_plan_timeout_ns
            ? std::numeric_limits<std::int64_t>::max()
            : accepted_at_ns + *later_plan_timeout_ns;
    runtime->effective_deadline_ns = std::min(runtime->effective_deadline_ns, later_deadline_ns);
    if (clock_.now() >= runtime->effective_deadline_ns) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_disable_mutation_deadline_elapsed",
          "the later-qualified Disable plan cannot widen the fixed containment deadline",
          "later_plan_timeout_elapsed"));
    }
    return Result<void>::Ok();
  }

  void ContinueDisableAfterReady(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->lifecycle_lane_active) {
      return;
    }
    runtime->lifecycle_lane_active = true;
    if (!worker_pool_.Submit([this, runtime](std::stop_token cancellation) {
          if (cancellation.stop_requested()) {
            runtime->lifecycle_lane_active = false;
            return;
          }
          try {
            if (runtime->permanent_containment_only && !runtime->status.current_state.has_value()) {
              const auto reconciled = ReconcilePromotedFullSnapshot(runtime);
              if (!reconciled.ok()) {
                // No external lifecycle effect is pending. Keep the Full-Stop
                // fence held and retry only the read-only truth acquisition;
                // this transaction can never enter vendor mutation.
                runtime->lifecycle_lane_active = false;
                SchedulePoll(runtime);
              }
              return;
            }
            const auto qualified = ValidateDisableMutationClosure(runtime);
            if (!qualified.ok()) {
              ResolveDisableWithoutVendorMutation(runtime, qualified.error());
              return;
            }
            Process(runtime, cancellation);
          } catch (const std::exception& exception) {
            ResolveDisableWithoutVendorMutation(
                runtime,
                coordinator_error("component_lifecycle_disable_qualification_exception",
                                  "the under-fence Disable qualification raised an exception",
                                  exception.what()));
          } catch (...) {
            ResolveDisableWithoutVendorMutation(
                runtime,
                coordinator_error("component_lifecycle_disable_qualification_exception",
                                  "the under-fence Disable qualification raised an exception",
                                  "unknown_exception"));
          }
        })) {
      runtime->lifecycle_lane_active = false;
      CloseAdmission();
    }
  }

  [[nodiscard]] Result<void> ReconcilePromotedFullSnapshot(
      const std::shared_ptr<RuntimeState>& runtime) {
    if (!runtime->permanent_containment_only || runtime->status.current_state.has_value() ||
        !runtime->status.disable_handoff.has_value() ||
        runtime->status.disable_handoff->scope_resolution !=
            DisableHandoffScopeResolutionDTO::kPromotedToFull ||
        runtime->status.disable_handoff->state != DisableHandoffStateDTO::kReady ||
        !runtime->status.configuration_fence.has_value() ||
        runtime->status.configuration_fence->state != ConfigurationFenceState::kHeld) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_full_stop_reconciliation_invalid",
          "the promoted Full-Stop transaction is not ready for snapshot reconciliation",
          "promoted_full_reconciliation_precondition_failed"));
    }

    const auto snapshot = current_snapshot_source_.ReadCurrentComponentSnapshot(
        runtime->status.accepted_intent.controllable_component_id);
    if (!snapshot.ok() || !snapshot.value().locally_valid() ||
        snapshot.value().controllable_component_id !=
            runtime->status.accepted_intent.controllable_component_id ||
        snapshot.value().component_lifecycle_generation !=
            runtime->status.acceptance.component_lifecycle_generation ||
        !snapshot_is_within_fence(snapshot.value(), runtime->status.configuration_fence->scope)) {
      return Result<void>::Err(
          snapshot.ok() ? coordinator_error("component_lifecycle_full_stop_snapshot_mismatch",
                                            "the fresh component snapshot is not an exact subset "
                                            "of the held Full-Stop fence",
                                            "promoted_full_snapshot_mismatch")
                        : snapshot.error());
    }

    const ErrorDTO unsatisfied{
        "component_lifecycle_disable_postcondition_not_observed",
        "Kernel containment completed but the requested Disabled lifecycle state is not observed",
        {{"reason", "containment_only_snapshot_not_disabled"}}};
    const bool disabled =
        snapshot.value().component_lifecycle_state == ComponentLifecycleStateDTO::kDisabled;
    auto reconciled = runtime->status;
    AdvanceStatus(runtime, reconciled, ComponentLifecycleTransactionPhaseDTO::kVerifying);
    reconciled.previous_state = snapshot.value().component_lifecycle_state;
    reconciled.current_state = snapshot.value().component_lifecycle_state;
    reconciled.domains.clear();
    reconciled.domains.reserve(runtime->status.accepted_intent.expected_control_domains.size() +
                               snapshot.value().domains.size());

    std::size_t expected_index = 0U;
    std::size_t current_index = 0U;
    bool all_succeeded = true;
    while (expected_index < runtime->status.accepted_intent.expected_control_domains.size() ||
           current_index < snapshot.value().domains.size()) {
      const auto& expected_rows = runtime->status.accepted_intent.expected_control_domains;
      const bool take_expected = current_index == snapshot.value().domains.size() ||
                                 (expected_index < expected_rows.size() &&
                                  expected_rows[expected_index].control_domain_id <
                                      snapshot.value().domains[current_index].control_domain_id);
      const bool take_current = expected_index == expected_rows.size() ||
                                (current_index < snapshot.value().domains.size() &&
                                 snapshot.value().domains[current_index].control_domain_id <
                                     expected_rows[expected_index].control_domain_id);

      std::optional<ExpectedControlDomainRevisionDTO> expected;
      std::optional<ResolvedLifecycleControlDomainScopeDTO> current;
      std::optional<ResolvedLifecycleProviderOccurrenceDTO> provider;
      std::optional<ControlDomainId> domain_id;
      if (take_expected) {
        expected = expected_rows[expected_index++];
        domain_id = expected->control_domain_id;
      } else if (take_current) {
        current = snapshot.value().domains[current_index];
        provider = snapshot.value().provider_occurrences[current_index++];
        domain_id = current->control_domain_id;
      } else {
        expected = expected_rows[expected_index++];
        current = snapshot.value().domains[current_index];
        provider = snapshot.value().provider_occurrences[current_index++];
        domain_id = current->control_domain_id;
      }

      const bool domain_succeeded = current.has_value() && disabled;
      all_succeeded = all_succeeded && domain_succeeded;
      reconciled.domains.push_back(
          {std::move(*domain_id),
           std::move(expected),
           std::move(current),
           std::move(provider),
           current.has_value() ? std::optional(snapshot.value().component_lifecycle_state)
                               : std::nullopt,
           current.has_value() ? std::optional(snapshot.value().component_lifecycle_state)
                               : std::nullopt,
           reconciled.requested_state,
           domain_succeeded ? ComponentLifecycleDomainActionStateDTO::kSucceeded
                            : ComponentLifecycleDomainActionStateDTO::kFailed,
           ComponentLifecycleDomainEffectDTO::kKnownNoEffect,
           {},
           std::nullopt,
           domain_succeeded ? std::nullopt : std::optional(unsatisfied)});
    }
    reconciled.reconciliation_required = false;
    reconciled.error = all_succeeded ? std::nullopt : std::optional(unsatisfied);
    if (!reconciled.locally_valid()) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_full_stop_snapshot_invalid",
          "the fresh Full-Stop snapshot cannot form a closed lifecycle reconciliation status",
          "promoted_full_status_invalid"));
    }
    const auto recorded = journal_.RecordLifecycleStatus(config_.request_owner_epoch, reconciled);
    if (!recorded.ok()) {
      return Result<void>::Err(recorded.error());
    }
    runtime->status = std::move(reconciled);
    ScheduleDisableRelease(runtime);
    return Result<void>::Ok();
  }

  void ResolveDisableWithoutVendorMutation(const std::shared_ptr<RuntimeState>& runtime,
                                           const ErrorDTO& error) {
    auto resolved = runtime->status;
    AdvanceStatus(runtime, resolved, ComponentLifecycleTransactionPhaseDTO::kVerifying);
    for (auto& domain : resolved.domains) {
      if (domain.action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded) {
        domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
        if (domain.effect == ComponentLifecycleDomainEffectDTO::kNone) {
          domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
        }
        domain.error = error;
      }
    }
    resolved.reconciliation_required = false;
    resolved.error = error;
    if (!resolved.locally_valid() ||
        !journal_.RecordLifecycleStatus(config_.request_owner_epoch, resolved).ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(resolved);
    ScheduleDisableRelease(runtime);
  }

  void ScheduleDisableRelease(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->release_scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->release_scheduled = true;
    if (!kernel_worker_pool_.Submit([this, runtime](std::stop_token cancellation) {
          runtime->release_scheduled = false;
          if (!cancellation.stop_requested()) {
            StartDisableFenceRelease(runtime);
          }
        })) {
      runtime->release_scheduled = false;
      CloseAdmission();
    }
  }

  [[nodiscard]] CCLCEffectAttemptDTO PreparedDisableFenceReleaseAttempt(
      const DisableFenceReleaseRequestDTO& request, std::uint64_t effect_sequence) const {
    return {CCLCRequestKindDTO::kComponentLifecycle,
            request.lifecycle_transaction_id,
            request.lifecycle_transaction_generation,
            effect_sequence,
            CCLCEffectKindDTO::kKernelDisableFenceRelease,
            CCLCEffectAttemptStateDTO::kPrepared,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            request,
            std::nullopt};
  }

  [[nodiscard]] Result<DisableFenceReleaseRequestDTO> BuildDisableFenceReleaseRequest(
      const std::shared_ptr<RuntimeState>& runtime, std::uint64_t effect_sequence) const {
    if (!runtime->status.disable_handoff.has_value() ||
        runtime->status.disable_handoff->state != DisableHandoffStateDTO::kReady ||
        !runtime->status.configuration_fence.has_value() ||
        runtime->status.configuration_fence->state != ConfigurationFenceState::kHeld) {
      return Result<DisableFenceReleaseRequestDTO>::Err(
          coordinator_error("component_lifecycle_disable_release_not_ready",
                            "the Disable transaction has no exact ready held fence to release",
                            "ready_fence_absent"));
    }
    const auto snapshot = current_snapshot_source_.ReadCurrentComponentSnapshot(
        runtime->status.accepted_intent.controllable_component_id);
    if (!snapshot.ok()) {
      return Result<DisableFenceReleaseRequestDTO>::Err(snapshot.error());
    }
    const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot.value());
    if (!expectation.ok()) {
      return Result<DisableFenceReleaseRequestDTO>::Err(expectation.error());
    }
    const bool full_stop = runtime->status.disable_handoff->scope_resolution ==
                           DisableHandoffScopeResolutionDTO::kPromotedToFull;
    const DisableHandoffAdmissionQueryDTO query{
        runtime->status.accepted_intent.controllable_component_id,
        full_stop ? DisableHandoffScopeRequestKindDTO::kFullStopRequired
                  : DisableHandoffScopeRequestKindDTO::kResolvedComponentScope,
        full_stop ? std::nullopt : std::optional(expectation.value())};
    const auto admission = kernel_disable_handoff_port_.ObserveDisableHandoffAdmission(query);
    if (!admission.ok()) {
      return Result<DisableFenceReleaseRequestDTO>::Err(admission.error());
    }
    const auto retained =
        journal_.LoadRetainedLifecycleStatus(runtime->status.acceptance.lifecycle_transaction_id);
    if (!retained.ok() || !retained.value().has_value() ||
        retained.value()->acceptance.lifecycle_transaction_generation !=
            runtime->status.acceptance.lifecycle_transaction_generation ||
        retained.value()->status_sequence != runtime->status.status_sequence ||
        retained.value()->disable_handoff != runtime->status.disable_handoff ||
        retained.value()->configuration_fence != runtime->status.configuration_fence) {
      return Result<DisableFenceReleaseRequestDTO>::Err(
          retained.ok()
              ? coordinator_error(
                    "component_lifecycle_disable_release_status_mismatch",
                    "the in-memory Disable release precondition differs from durable status",
                    "durable_release_precondition_mismatch")
              : retained.error());
    }
    const auto& durable_precondition = *retained.value();
    DisableFenceReleaseRequestDTO request{
        {RequestId(
             "cclc.disable.release." +
             std::to_string(runtime->status.acceptance.lifecycle_transaction_generation.value()) +
             "." + std::to_string(effect_sequence)),
         admission.value().request_owner_epoch, "CCLC Disable fence release",
         runtime->status.accepted_intent.request_context.request_id.value()},
        runtime->status.accepted_intent.request_context.request_id,
        runtime->status.acceptance.lifecycle_transaction_id,
        runtime->status.acceptance.lifecycle_transaction_generation,
        runtime->status.accepted_intent.controllable_component_id,
        *runtime->status.disable_handoff,
        durable_precondition.status_sequence,
        ContentDigest(
            detail::ComponentLifecycleStatusReleasePreconditionDigest(durable_precondition)),
        expectation.value(),
        admission.value()};
    if (!request.locally_valid()) {
      return Result<DisableFenceReleaseRequestDTO>::Err(
          coordinator_error("component_lifecycle_disable_release_request_invalid",
                            "fresh under-fence facts cannot form an exact release request",
                            "release_request_construction_failed"));
    }
    return Result<DisableFenceReleaseRequestDTO>::Ok(std::move(request));
  }

  void StartDisableFenceRelease(const std::shared_ptr<RuntimeState>& runtime) {
    const auto effect_sequence = NextEffectSequence(runtime->effect_sequence);
    if (!effect_sequence.ok()) {
      CloseAdmission();
      return;
    }
    Result<DisableFenceReleaseRequestDTO> request =
        Result<DisableFenceReleaseRequestDTO>::Err(coordinator_error(
            "component_lifecycle_disable_release_request_failed",
            "the Disable release request could not be built", "release_request_failed"));
    try {
      request = BuildDisableFenceReleaseRequest(runtime, effect_sequence.value());
    } catch (...) {
    }
    if (!request.ok()) {
      ScheduleDisableReleaseRetry(runtime);
      return;
    }
    auto prepared_status = runtime->status;
    AdvanceStatus(runtime, prepared_status, ComponentLifecycleTransactionPhaseDTO::kVerifying);
    ClearResolvedOwnerReplacementMarker(prepared_status);
    const auto prepared_attempt =
        PreparedDisableFenceReleaseAttempt(request.value(), effect_sequence.value());
    const auto persisted = journal_.RecordEffectPrepared(config_.request_owner_epoch,
                                                         prepared_attempt, prepared_status);
    if (!persisted.ok()) {
      CloseAdmission();
      return;
    }
    runtime->effect_sequence = effect_sequence.value();
    runtime->status = std::move(prepared_status);

    std::optional<DisableFenceReleasedDTO> receipt;
    try {
      const auto released = kernel_disable_handoff_port_.ReleaseDisableFence(request.value());
      if (released.ok() && released.value().matches(request.value())) {
        receipt = released.value();
      }
    } catch (...) {
    }
    if (receipt.has_value()) {
      CompleteDisableFenceRelease(runtime, request.value(), *receipt);
      return;
    }
    ReconcilePreparedDisableFenceRelease(runtime, request.value());
  }

  void ScheduleDisableReleaseRetry(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->release_retry_scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->release_retry_scheduled = true;
    const auto weak = std::weak_ptr<RuntimeState>(runtime);
    if (!ScheduleLifecycleCallback(
            checked_schedule_time(clock_.now(), config_.observation_poll_period_ns), [this, weak] {
              if (shutting_down_.load(std::memory_order_acquire)) {
                return;
              }
              if (const auto state = weak.lock(); state) {
                state->release_retry_scheduled = false;
                ScheduleDisableRelease(state);
              }
            })) {
      runtime->release_retry_scheduled = false;
    }
  }

  void ReconcilePreparedDisableFenceRelease(const std::shared_ptr<RuntimeState>& runtime,
                                            const DisableFenceReleaseRequestDTO& request) {
    AcquireUnresolvedEffectAdmissionGate(runtime);
    try {
      const auto reconciliation =
          kernel_disable_handoff_port_.ReconcileDisableFenceReleaseAttempt(request);
      if (!reconciliation.ok()) {
        AcquireUnresolvedEffectAdmissionGate(runtime);
        ScheduleDisableFenceReleaseReconciliation(runtime, request);
        return;
      }
      if (!reconciliation.value().locally_valid() || reconciliation.value().request != request) {
        CloseAdmission();
        return;
      }
      if (reconciliation.value().disposition ==
          DisableFenceReleaseAttemptDispositionDTO::kReleasedExact) {
        CompleteDisableFenceRelease(runtime, request, *reconciliation.value().release_receipt);
        return;
      }
      if (reconciliation.value().disposition ==
          DisableFenceReleaseAttemptDispositionDTO::kNotReleasedHeld) {
        auto completed_status = runtime->status;
        AdvanceStatus(runtime, completed_status, ComponentLifecycleTransactionPhaseDTO::kVerifying);
        ClearResolvedOwnerReplacementMarker(completed_status);
        auto completed_attempt =
            PreparedDisableFenceReleaseAttempt(request, runtime->effect_sequence);
        completed_attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
        completed_attempt.disable_fence_release_result = DisableFenceReleaseEffectResultDTO{
            request, DisableFenceReleaseEffectDispositionDTO::kProvenNotReleasedHeld, std::nullopt};
        if (!journal_
                 .RecordEffectCompleted(config_.request_owner_epoch, completed_attempt,
                                        completed_status)
                 .ok()) {
          CloseAdmission();
          return;
        }
        runtime->status = std::move(completed_status);
        ScheduleDisableRelease(runtime);
        return;
      }
    } catch (...) {
    }
    AcquireUnresolvedEffectAdmissionGate(runtime);
    ScheduleDisableFenceReleaseReconciliation(runtime, request);
  }

  void ScheduleDisableFenceReleaseReconciliation(const std::shared_ptr<RuntimeState>& runtime,
                                                 DisableFenceReleaseRequestDTO request) {
    if (runtime->scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->scheduled = true;
    const auto weak = std::weak_ptr<RuntimeState>(runtime);
    if (!ScheduleLifecycleCallback(
            checked_schedule_time(clock_.now(), config_.observation_poll_period_ns),
            [this, weak, request = std::move(request)] {
              if (shutting_down_.load(std::memory_order_acquire)) {
                return;
              }
              if (const auto state = weak.lock(); state) {
                state->scheduled = false;
                if (!kernel_worker_pool_.Submit(
                        [this, state, request](std::stop_token cancellation) {
                          if (!cancellation.stop_requested()) {
                            ReconcilePreparedDisableFenceRelease(state, request);
                          }
                        })) {
                  CloseAdmission();
                }
              }
            })) {
      runtime->scheduled = false;
    }
  }

  void CompleteDisableFenceRelease(const std::shared_ptr<RuntimeState>& runtime,
                                   const DisableFenceReleaseRequestDTO& request,
                                   const DisableFenceReleasedDTO& receipt) {
    if (!receipt.matches(request)) {
      CloseAdmission();
      return;
    }
    auto terminal_status = runtime->status;
    const bool all_succeeded = std::all_of(
        terminal_status.domains.begin(), terminal_status.domains.end(), [](const auto& domain) {
          return domain.action_state == ComponentLifecycleDomainActionStateDTO::kSucceeded;
        });
    const bool any_observed = std::any_of(
        terminal_status.domains.begin(), terminal_status.domains.end(), [](const auto& domain) {
          return domain.effect == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
                 domain.effect == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
        });
    AdvanceStatus(runtime, terminal_status,
                  all_succeeded ? ComponentLifecycleTransactionPhaseDTO::kSucceeded
                                : ComponentLifecycleTransactionPhaseDTO::kFailed);
    terminal_status.configuration_fence->state = ConfigurationFenceState::kReleased;
    terminal_status.disable_fence_released = receipt;
    terminal_status.reconciliation_required = false;
    terminal_status.remaining_duration_ns = 0;
    if (all_succeeded) {
      terminal_status.current_state = terminal_status.requested_state.state;
      terminal_status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
      terminal_status.error.reset();
    } else {
      terminal_status.terminal_result =
          any_observed ? ComponentLifecycleTerminalResultDTO::kFailedPartialEffect
                       : ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect;
      if (!terminal_status.error.has_value()) {
        terminal_status.error = coordinator_error(
            "component_lifecycle_disable_mutation_failed",
            "Disable containment completed without every requested lifecycle postcondition",
            "one_or_more_domains_failed");
      }
    }
    auto completed_attempt = PreparedDisableFenceReleaseAttempt(request, runtime->effect_sequence);
    completed_attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
    completed_attempt.disable_fence_release_result = DisableFenceReleaseEffectResultDTO{
        request, DisableFenceReleaseEffectDispositionDTO::kReleased, receipt};
    if (!runtime->cell_claim.has_value()) {
      CloseAdmission();
      return;
    }
    const auto& claim = *runtime->cell_claim;
    const CellAdmissionClaimReleaseDTO claim_release{
        claim.intent.owner_role,
        claim.intent.owner_epoch,
        claim.intent.owner_transaction_id,
        claim.intent.request_id,
        claim.intent.request_payload_digest,
        claim.acquired_admission_generation,
        claim.claim_generation,
        {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
         ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(
             terminal_status))}};
    if (!terminal_status.locally_valid() ||
        !journal_
             .RecordEffectCompletedAndLifecycleClaimRelease(
                 config_.request_owner_epoch, completed_attempt,
                 terminal_status, claim_release)
             .ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(terminal_status);
    runtime->cell_claim_release = claim_release;
    if (!ReleaseDurableClaim(runtime, claim_release)) {
      return;
    }
    Complete(runtime);
  }

  [[nodiscard]] bool Initialize(const std::shared_ptr<RuntimeState>& runtime) {
    if (!runtime->qualified_catalogs.has_value() ||
        runtime->qualified_catalogs->size() != runtime->status.domains.size()) {
      FailBeforeEffect(
          runtime,
          coordinator_error("component_lifecycle_catalog_closure_unqualified",
                            "the accepted lifecycle request has no executable catalog closure",
                            "qualified_catalogs_absent"));
      return false;
    }
    const auto& catalogs = *runtime->qualified_catalogs;
    std::vector<LifecycleRecoverSelectionInput> recover_inputs;
    for (std::size_t index = 0U; index < runtime->status.domains.size(); ++index) {
      if (RemainingDuration(runtime, clock_.now()) == 0) {
        FailBeforeEffect(runtime,
                         coordinator_error("component_lifecycle_deadline_expired",
                                           "the lifecycle deadline expired before observation",
                                           "deadline_exhausted"));
        return false;
      }
      auto& domain = runtime->status.domains[index];
      if (!domain.resolved_provider_occurrence.has_value()) {
        FailBeforeEffect(runtime,
                         coordinator_error("component_lifecycle_provider_missing",
                                           "a lifecycle domain has no accepted provider occurrence",
                                           "accepted_provider_absent"));
        return false;
      }
      const auto port =
          lifecycle_port_resolver_.ResolveExactLifecyclePort(*domain.resolved_provider_occurrence);
      if (!port.ok() || !port.value()) {
        FailBeforeEffect(
            runtime, port.ok()
                         ? coordinator_error("component_lifecycle_provider_unavailable",
                                             "the accepted lifecycle provider port is unavailable",
                                             "resolved_port_null")
                         : port.error());
        return false;
      }
      runtime->ports[index] = port.value();
      const auto observation = port.value()->Observe(ObservationQuery(runtime->status, domain));
      const auto observation_received_at_ns = clock_.now();
      if (RemainingDuration(runtime, observation_received_at_ns) == 0 ||
          !FreshObservation(runtime->status, domain, observation, observation_received_at_ns)) {
        FailBeforeEffect(
            runtime,
            observation.ok()
                ? coordinator_error("component_lifecycle_observation_stale",
                                    "the lifecycle provider observation is stale or mismatched",
                                    "initial_observation_not_fresh")
                : observation.error());
        return false;
      }
      runtime->observations[index] = observation.value();
      if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kRecover) {
        recover_inputs.push_back(
            {*domain.resolved_provider_occurrence, catalogs[index], observation.value()});
      }
    }

    if (!recover_inputs.empty()) {
      const auto target = SelectCommonRecoverTarget(recover_inputs);
      if (!target.ok()) {
        FailBeforeEffect(runtime, target.error());
        return false;
      }
      runtime->status.requested_state = {ComponentLifecycleRequestedStatePresenceDTO::kResolved,
                                         target.value()};
      for (auto& domain : runtime->status.domains) {
        domain.requested_state = runtime->status.requested_state;
      }
    }

    for (std::size_t index = 0U; index < runtime->status.domains.size(); ++index) {
      const auto decision = EvaluateLifecycleDomainPlan(
          runtime->status.accepted_intent.intent, *runtime->status.requested_state.state,
          catalogs[index], *runtime->observations[index]);
      if (!decision.ok()) {
        FailBeforeEffect(runtime, decision.error());
        return false;
      }
      runtime->decisions[index] = decision.value();
      if (decision.value().kind == LifecycleDomainPlanDecisionKind::kAlreadySatisfied) {
        auto& domain = runtime->status.domains[index];
        domain.current_state = runtime->status.requested_state.state;
        domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
        domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
      }
    }
    runtime->initialized = true;
    while (runtime->current_domain_index < runtime->status.domains.size() &&
           runtime->status.domains[runtime->current_domain_index].action_state ==
               ComponentLifecycleDomainActionStateDTO::kSucceeded) {
      ++runtime->current_domain_index;
    }
    if (runtime->current_domain_index == runtime->status.domains.size()) {
      Succeed(runtime);
      return false;
    }
    return true;
  }

  void RunCurrentDomain(const std::shared_ptr<RuntimeState>& runtime,
                        std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
      return;
    }
    if (runtime->verifying_domain_index.has_value()) {
      VerifyCurrentDomain(runtime, cancellation);
      return;
    }
    const auto effect_sequence = NextEffectSequence(runtime->effect_sequence);
    if (!effect_sequence.ok()) {
      FailTransaction(runtime, effect_sequence.error());
      return;
    }
    if (RemainingDuration(runtime, clock_.now()) == 0) {
      FailTransaction(runtime, coordinator_error("component_lifecycle_deadline_expired",
                                                 "the lifecycle deadline expired before dispatch",
                                                 "deadline_exhausted"));
      return;
    }
    const auto index = runtime->current_domain_index;
    const auto& transition = *runtime->decisions[index]->selected_transition;
    const auto& observation = *runtime->observations[index];
    const auto& provider = *runtime->status.domains[index].resolved_provider_occurrence;
    BrandLifecyclePrimitiveIntentDTO primitive{
        runtime->status.acceptance.lifecycle_transaction_id,
        runtime->status.acceptance.lifecycle_transaction_generation,
        runtime->status.accepted_intent.controllable_component_id,
        provider.control_domain_id,
        provider.control_domain_revision,
        provider.provider_instance_id,
        provider.provider_manifest_id,
        provider.provider_manifest_revision,
        provider.provider_runtime_unit_id,
        provider.provider_runtime_epoch,
        transition.lifecycle_primitive_id,
        LifecyclePrimitiveAttemptId(
            "attempt." +
            std::to_string(runtime->status.acceptance.lifecycle_transaction_generation.value()) +
            "." + std::to_string(effect_sequence.value())),
        observation.observation_generation,
        RemainingDuration(runtime, clock_.now())};
    if (!primitive.locally_valid()) {
      FailTransaction(runtime, coordinator_error("component_lifecycle_primitive_invalid",
                                                 "the selected lifecycle primitive is invalid",
                                                 "primitive_construction_failed"));
      return;
    }

    auto prepared_status = runtime->status;
    AdvanceStatus(runtime, prepared_status, ComponentLifecycleTransactionPhaseDTO::kExecuting);
    prepared_status.domains[index].action_state =
        ComponentLifecycleDomainActionStateDTO::kExecuting;
    CCLCEffectAttemptDTO prepared_attempt{CCLCRequestKindDTO::kComponentLifecycle,
                                          primitive.lifecycle_transaction_id,
                                          primitive.lifecycle_transaction_generation,
                                          effect_sequence.value(),
                                          CCLCEffectKindDTO::kBrandLifecyclePrimitive,
                                          CCLCEffectAttemptStateDTO::kPrepared,
                                          primitive,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt};
    const auto prepared = journal_.RecordEffectPrepared(config_.request_owner_epoch,
                                                        prepared_attempt, prepared_status);
    if (!prepared.ok()) {
      CloseAdmission();
      return;
    }
    runtime->effect_sequence = prepared_attempt.effect_sequence;
    runtime->status = std::move(prepared_status);

    BrandLifecyclePrimitiveResultDTO result = [&] {
      try {
        return runtime->ports[index]->Dispatch(primitive, cancellation);
      } catch (const std::exception& exception) {
        return UncertainDispatchResult(primitive, exception.what());
      } catch (...) {
        return UncertainDispatchResult(primitive, "unknown_dispatch_exception");
      }
    }();
    if (!result.matches(primitive)) {
      result = {primitive.lifecycle_transaction_id,
                primitive.lifecycle_transaction_generation,
                primitive.controllable_component_id,
                primitive.control_domain_id,
                primitive.control_domain_revision,
                primitive.provider_instance_id,
                primitive.provider_manifest_id,
                primitive.provider_manifest_revision,
                primitive.provider_runtime_unit_id,
                primitive.provider_runtime_epoch,
                primitive.lifecycle_primitive_id,
                primitive.lifecycle_primitive_attempt_id,
                primitive.expected_observation_generation,
                LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain,
                LifecyclePrimitiveEffectDTO::kPossible,
                std::nullopt,
                coordinator_error("component_lifecycle_adapter_result_invalid",
                                  "the lifecycle adapter returned a mismatched result",
                                  "result_identity_or_shape_mismatch")};
    }

    auto completed_status = runtime->status;
    AdvanceStatus(
        runtime, completed_status,
        result.dispatch_state == LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain ||
                result.effect == LifecyclePrimitiveEffectDTO::kPossible
            ? ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown
            : ComponentLifecycleTransactionPhaseDTO::kVerifying);
    auto& completed_domain = completed_status.domains[index];
    completed_domain.primitive_attempts.push_back(result);
    completed_domain.effect = append_domain_effect(completed_domain.effect, result);
    if (completed_domain.effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain) {
      completed_domain.action_state = ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown;
      completed_domain.error = result.error.value_or(
          coordinator_error("component_lifecycle_effect_uncertain",
                            "the lifecycle primitive effect is uncertain", "effect_possible"));
      completed_status.reconciliation_required = true;
      completed_status.error = completed_domain.error;
    } else if (result.dispatch_state == LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown &&
               result.known_result == LifecyclePrimitiveKnownResultDTO::kSucceeded) {
      completed_domain.action_state = ComponentLifecycleDomainActionStateDTO::kVerifying;
      completed_domain.error.reset();
    } else {
      completed_domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
      completed_domain.error = result.error.value_or(
          coordinator_error("component_lifecycle_primitive_failed",
                            "the lifecycle primitive did not succeed", "primitive_failed"));
      completed_status.error = completed_domain.error;
    }
    auto completed_attempt = prepared_attempt;
    completed_attempt.state = CCLCEffectAttemptStateDTO::kCompleted;
    completed_attempt.lifecycle_primitive_result = result;
    if (completed_domain.effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain) {
      AcquireUnresolvedEffectAdmissionGate(runtime);
    }
    const auto completed = journal_.RecordEffectCompleted(config_.request_owner_epoch,
                                                          completed_attempt, completed_status);
    if (!completed.ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(completed_status);

    if (runtime->status.domains[index].action_state ==
        ComponentLifecycleDomainActionStateDTO::kFailed) {
      FailTransaction(runtime, *runtime->status.domains[index].error);
      return;
    }
    runtime->verifying_domain_index = index;
    runtime->verifying_transition = transition;
    runtime->expected_observation_generation = primitive.expected_observation_generation;
    VerifyCurrentDomain(runtime, cancellation);
  }

  void VerifyCurrentDomain(const std::shared_ptr<RuntimeState>& runtime,
                           std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
      return;
    }
    const auto index = *runtime->verifying_domain_index;
    auto& domain = runtime->status.domains[index];
    const auto observation =
        runtime->ports[index]->Observe(ObservationQuery(runtime->status, domain));
    const auto now_ns = clock_.now();
    if (!FreshObservation(runtime->status, domain, observation, now_ns) ||
        (observation.ok() &&
         observation.value().observation_generation <= runtime->expected_observation_generation)) {
      if (RemainingDuration(runtime, now_ns) == 0 &&
          domain.effect != ComponentLifecycleDomainEffectDTO::kEffectUncertain) {
        FailTransaction(runtime,
                        coordinator_error(
                            "component_lifecycle_verification_timeout",
                            "no fresh post-dispatch lifecycle observation arrived before deadline",
                            "postcondition_observation_timeout"));
        return;
      }
      SchedulePoll(runtime);
      return;
    }

    if ((now_ns < 0 || now_ns >= runtime->effective_deadline_ns) &&
        domain.effect != ComponentLifecycleDomainEffectDTO::kEffectUncertain) {
      FailTransaction(runtime,
                      coordinator_error("component_lifecycle_verification_timeout",
                                        "lifecycle evidence arrived after the fixed deadline",
                                        "postcondition_evidence_late"));
      return;
    }

    if (runtime->observations[index].has_value() &&
        observation.value().source_sequence <= runtime->observations[index]->source_sequence) {
      SchedulePoll(runtime);
      return;
    }

    const auto postcondition = EvaluateLifecyclePredicate(
        runtime->verifying_transition->postcondition_predicate, observation.value());
    if (domain.effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain) {
      const auto resolution = classify_uncertain_effect(*runtime->verifying_transition, domain,
                                                        runtime->observations[index],
                                                        observation.value(), postcondition);
      auto reconciled = runtime->status;
      AdvanceStatus(runtime, reconciled, ComponentLifecycleTransactionPhaseDTO::kReconciling);
      auto& reconciled_domain = reconciled.domains[index];
      reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kReconciling;
      reconciled_domain.reconciliation_evidence =
          LifecycleReconciliationEvidenceDTO{observation.value().provider_instance_id,
                                             observation.value().provider_manifest_id,
                                             observation.value().provider_manifest_revision,
                                             observation.value().provider_runtime_unit_id,
                                             observation.value().provider_runtime_epoch,
                                             observation.value().source_sequence,
                                             observation.value().observation_generation,
                                             observation.value().observed_state,
                                             postcondition,
                                             now_ns};
      switch (resolution) {
        case UncertainEffectResolution::kRequestedEffectObserved:
          reconciled_domain.effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
          reconciled_domain.error.reset();
          reconciled.reconciliation_required = false;
          reconciled.error.reset();
          break;
        case UncertainEffectResolution::kKnownNoEffect:
          reconciled_domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
          reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
          reconciled_domain.error = coordinator_error(
              "component_lifecycle_primitive_known_no_effect",
              "fresh reconciliation proved that the prepared primitive had no relevant effect",
              "observation_unchanged_after_uncertain_dispatch");
          reconciled.reconciliation_required = false;
          reconciled.error = reconciled_domain.error;
          break;
        case UncertainEffectResolution::kPartialEffectObserved:
          reconciled_domain.effect = ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
          reconciled_domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
          reconciled_domain.error = coordinator_error(
              "component_lifecycle_partial_effect_observed",
              "fresh reconciliation observed a change without the requested postcondition",
              "postcondition_not_satisfied_after_effect");
          reconciled.reconciliation_required = false;
          reconciled.error = reconciled_domain.error;
          break;
        case UncertainEffectResolution::kUnresolved:
          break;
      }
      const auto recorded = journal_.RecordLifecycleStatus(config_.request_owner_epoch, reconciled);
      if (!recorded.ok()) {
        CloseAdmission();
        return;
      }
      runtime->status = std::move(reconciled);
      if (resolution != UncertainEffectResolution::kUnresolved) {
        ReleaseUnresolvedEffectAdmissionGate(runtime);
      }
      if (runtime->status.domains[index].action_state ==
          ComponentLifecycleDomainActionStateDTO::kFailed) {
        FailTransaction(runtime, *runtime->status.domains[index].error);
        return;
      }
      if (resolution != UncertainEffectResolution::kRequestedEffectObserved) {
        runtime->expected_observation_generation = observation.value().observation_generation;
        SchedulePoll(runtime);
        return;
      }
    } else if (postcondition != LifecyclePostconditionResultDTO::kSatisfied) {
      if (RemainingDuration(runtime, now_ns) == 0) {
        FailTransaction(
            runtime, coordinator_error(
                         "component_lifecycle_postcondition_timeout",
                         "the lifecycle primitive postcondition was not satisfied before deadline",
                         "transition_postcondition_unsatisfied"));
        return;
      }
      runtime->expected_observation_generation = observation.value().observation_generation;
      SchedulePoll(runtime);
      return;
    }

    runtime->observations[index] = observation.value();
    auto progressed = runtime->status;
    AdvanceStatus(runtime, progressed, ComponentLifecycleTransactionPhaseDTO::kVerifying);
    auto& progressed_domain = progressed.domains[index];
    progressed_domain.current_state = observation.value().observed_state;
    progressed_domain.error.reset();
    const auto next = EvaluateLifecycleDomainPlan(
        progressed.accepted_intent.intent, *progressed.requested_state.state,
        (*runtime->qualified_catalogs)[index], observation.value());
    if (!next.ok()) {
      runtime->status = std::move(progressed);
      FailTransaction(runtime, next.error());
      return;
    }
    runtime->decisions[index] = next.value();
    runtime->verifying_domain_index.reset();
    runtime->verifying_transition.reset();
    if (next.value().kind == LifecycleDomainPlanDecisionKind::kAlreadySatisfied) {
      progressed_domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
      if (progressed_domain.effect == ComponentLifecycleDomainEffectDTO::kNone) {
        progressed_domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
      } else if (progressed_domain.effect ==
                 ComponentLifecycleDomainEffectDTO::kPartialEffectObserved) {
        progressed_domain.effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
      }
      const auto recorded = journal_.RecordLifecycleStatus(config_.request_owner_epoch, progressed);
      if (!recorded.ok()) {
        CloseAdmission();
        return;
      }
      runtime->status = std::move(progressed);
      ++runtime->current_domain_index;
      while (runtime->current_domain_index < runtime->status.domains.size() &&
             runtime->status.domains[runtime->current_domain_index].action_state ==
                 ComponentLifecycleDomainActionStateDTO::kSucceeded) {
        ++runtime->current_domain_index;
      }
      if (runtime->current_domain_index == runtime->status.domains.size()) {
        Succeed(runtime);
      } else {
        RunCurrentDomain(runtime, cancellation);
      }
      return;
    }
    runtime->status = std::move(progressed);
    RunCurrentDomain(runtime, cancellation);
  }

  [[nodiscard]] BrandLifecycleObservationQueryDTO ObservationQuery(
      const ComponentLifecycleStatusDTO& status,
      const ComponentLifecycleDomainStatusDTO& domain) const {
    const auto& provider = *domain.resolved_provider_occurrence;
    return {status.acceptance.lifecycle_transaction_id,
            status.acceptance.lifecycle_transaction_generation,
            status.accepted_intent.controllable_component_id,
            provider.control_domain_id,
            provider.control_domain_revision,
            provider.provider_instance_id,
            provider.provider_manifest_id,
            provider.provider_manifest_revision,
            provider.provider_runtime_unit_id,
            provider.provider_runtime_epoch};
  }

  [[nodiscard]] BrandLifecyclePrimitiveResultDTO UncertainDispatchResult(
      const BrandLifecyclePrimitiveIntentDTO& primitive, std::string reason) const {
    return {primitive.lifecycle_transaction_id,
            primitive.lifecycle_transaction_generation,
            primitive.controllable_component_id,
            primitive.control_domain_id,
            primitive.control_domain_revision,
            primitive.provider_instance_id,
            primitive.provider_manifest_id,
            primitive.provider_manifest_revision,
            primitive.provider_runtime_unit_id,
            primitive.provider_runtime_epoch,
            primitive.lifecycle_primitive_id,
            primitive.lifecycle_primitive_attempt_id,
            primitive.expected_observation_generation,
            LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain,
            LifecyclePrimitiveEffectDTO::kPossible,
            std::nullopt,
            coordinator_error("component_lifecycle_dispatch_uncertain",
                              "the lifecycle primitive may have crossed the effect boundary",
                              std::move(reason))};
  }

  [[nodiscard]] bool FreshObservation(const ComponentLifecycleStatusDTO& status,
                                      const ComponentLifecycleDomainStatusDTO& domain,
                                      const Result<BrandLifecycleObservationDTO>& observation,
                                      std::int64_t observation_received_at_ns) const {
    if (!observation.ok() || !observation.value().matches(ObservationQuery(status, domain))) {
      return false;
    }
    return observation_received_at_ns >= status.latest_updated_at_ns &&
           observation.value().received_timestamp_ns <= observation_received_at_ns &&
           observation_received_at_ns < observation.value().fresh_until_ns;
  }

  [[nodiscard]] std::int64_t RemainingDuration(const std::shared_ptr<RuntimeState>& runtime,
                                               std::int64_t now_ns) const noexcept {
    if (now_ns <= runtime->status.acceptance.accepted_at_ns) {
      return std::max<std::int64_t>(
          0, runtime->effective_deadline_ns - runtime->status.acceptance.accepted_at_ns);
    }
    return now_ns >= runtime->effective_deadline_ns ? 0 : runtime->effective_deadline_ns - now_ns;
  }

  void AdvanceStatus(const std::shared_ptr<RuntimeState>& runtime,
                     ComponentLifecycleStatusDTO& status,
                     ComponentLifecycleTransactionPhaseDTO phase) const {
    const auto now_ns = std::max(clock_.now(), status.latest_updated_at_ns);
    ++status.status_sequence;
    status.status_owner_epoch = config_.request_owner_epoch;
    status.phase = phase;
    status.latest_updated_at_ns = now_ns;
    status.remaining_duration_ns = RemainingDuration(runtime, now_ns);
  }

  void SchedulePoll(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->scheduled || shutting_down_.load(std::memory_order_acquire)) {
      return;
    }
    runtime->scheduled = true;
    const auto weak = std::weak_ptr<RuntimeState>(runtime);
    if (!ScheduleLifecycleCallback(
            checked_schedule_time(clock_.now(), config_.observation_poll_period_ns), [this, weak] {
              if (shutting_down_.load(std::memory_order_acquire)) {
                return;
              }
              if (const auto state = weak.lock(); state) {
                state->scheduled = false;
                if (!worker_pool_.Submit([this, state](std::stop_token cancellation) {
                      Process(state, cancellation);
                    })) {
                  CloseAdmission();
                }
              }
            })) {
      runtime->scheduled = false;
    }
  }

  void FailBeforeEffect(const std::shared_ptr<RuntimeState>& runtime, const ErrorDTO& error) {
    if (std::any_of(runtime->status.domains.begin(), runtime->status.domains.end(),
                    [](const auto& domain) {
                      return !domain.primitive_attempts.empty() ||
                             domain.effect != ComponentLifecycleDomainEffectDTO::kNone;
                    })) {
      FailTransaction(runtime, error);
      return;
    }
    FailTransaction(runtime, error);
  }

  [[nodiscard]] bool CommitTerminalAndReleaseClaim(
      const std::shared_ptr<RuntimeState>& runtime,
      ComponentLifecycleStatusDTO status) {
    if (!runtime->cell_claim.has_value()) {
      CloseAdmission();
      return false;
    }
    const auto& claim = *runtime->cell_claim;
    CellAdmissionClaimReleaseDTO release{
        claim.intent.owner_role,
        claim.intent.owner_epoch,
        claim.intent.owner_transaction_id,
        claim.intent.request_id,
        claim.intent.request_payload_digest,
        claim.acquired_admission_generation,
        claim.claim_generation,
        {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
         ContentDigest(
             detail::ComponentLifecycleStatusReleasePreconditionDigest(status))}};
    const auto retained =
        journal_.RecordLifecycleTerminalAndClaimRelease(
            config_.request_owner_epoch, status, release);
    if (!retained.ok()) {
      CloseAdmission();
      return false;
    }
    runtime->status = std::move(status);
    runtime->cell_claim_release = release;
    return ReleaseDurableClaim(runtime, release);
  }

  [[nodiscard]] bool ReleaseDurableClaim(
      const std::shared_ptr<RuntimeState>& runtime,
      const CellAdmissionClaimReleaseDTO& release) {
    if (!runtime->cell_claim.has_value()) {
      CloseAdmission();
      return false;
    }
    const auto& claim = *runtime->cell_claim;
    Result<AuthoringAdmissionDTO> released =
        Result<AuthoringAdmissionDTO>::Err(coordinator_error(
            "component_lifecycle_claim_release_unknown",
            "Kernel lifecycle claim release raised an exception",
            "claim_release_exception"));
    try {
      released =
          kernel_cell_admission_port_->ReleaseCellAdmissionClaim(release);
    } catch (...) {
    }
    const CellAdmissionRecoveryReceiptQueryDTO query{claim.intent, claim, release};
    Result<CellAdmissionRecoveryReceiptDTO> receipt =
        Result<CellAdmissionRecoveryReceiptDTO>::Err(coordinator_error(
            "component_lifecycle_claim_release_unknown",
            "Kernel lifecycle release receipt raised an exception",
            "claim_receipt_exception"));
    try {
      receipt =
          kernel_cell_admission_port_->GetCellAdmissionRecoveryReceipt(query);
    } catch (...) {
    }
    if (!released.ok() || !receipt.ok() ||
        receipt.value().status !=
            CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact ||
        !journal_
             .RecordLifecycleClaimReleaseReceipt(
                 config_.request_owner_epoch, release, receipt.value())
             .ok()) {
      CloseAdmission();
      return false;
    }
    return true;
  }

  void FailTransaction(const std::shared_ptr<RuntimeState>& runtime, const ErrorDTO& error) {
    if (terminal(runtime->status)) {
      return;
    }
    const bool uncertain = std::any_of(
        runtime->status.domains.begin(), runtime->status.domains.end(), [](const auto& domain) {
          return domain.effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain;
        });
    if (uncertain) {
      auto status = runtime->status;
      AdvanceStatus(runtime, status, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
      status.reconciliation_required = true;
      status.error = error;
      for (auto& domain : status.domains) {
        if (domain.effect == ComponentLifecycleDomainEffectDTO::kEffectUncertain) {
          domain.action_state = ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown;
          domain.error = error;
        }
      }
      if (journal_.RecordLifecycleStatus(config_.request_owner_epoch, status).ok()) {
        runtime->status = std::move(status);
        SchedulePoll(runtime);
      } else {
        CloseAdmission();
      }
      return;
    }

    if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
        runtime->status.disable_handoff.has_value() &&
        runtime->status.disable_handoff->state == DisableHandoffStateDTO::kReady &&
        runtime->status.configuration_fence.has_value() &&
        runtime->status.configuration_fence->state == ConfigurationFenceState::kHeld) {
      ResolveDisableWithoutVendorMutation(runtime, error);
      return;
    }

    auto status = runtime->status;
    AdvanceStatus(runtime, status, ComponentLifecycleTransactionPhaseDTO::kFailed);
    bool any_effect = false;
    for (auto& domain : status.domains) {
      any_effect = any_effect ||
                   domain.effect == ComponentLifecycleDomainEffectDTO::kEffectObserved ||
                   domain.effect == ComponentLifecycleDomainEffectDTO::kPartialEffectObserved;
      if (domain.action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded) {
        domain.action_state = ComponentLifecycleDomainActionStateDTO::kFailed;
        if (domain.effect == ComponentLifecycleDomainEffectDTO::kNone) {
          domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
        }
        domain.error = error;
      }
    }
    status.terminal_result = any_effect ? ComponentLifecycleTerminalResultDTO::kFailedPartialEffect
                                        : ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect;
    status.error = error;
    status.remaining_duration_ns = 0;
    if (!status.locally_valid() ||
        !CommitTerminalAndReleaseClaim(runtime, std::move(status))) {
      return;
    }
    Complete(runtime);
  }

  void Succeed(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable) {
      ScheduleDisableRelease(runtime);
      return;
    }
    auto status = runtime->status;
    AdvanceStatus(runtime, status, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
    status.current_state = status.requested_state.state;
    status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
    status.error.reset();
    status.reconciliation_required = false;
    status.remaining_duration_ns = 0;
    if (!status.locally_valid() ||
        !CommitTerminalAndReleaseClaim(runtime, std::move(status))) {
      return;
    }
    Complete(runtime);
  }

  void Complete(const std::shared_ptr<RuntimeState>& runtime) {
    if (!runtime->reserved_domains.empty()) {
      const auto released = domain_reservations_.release(
          reservation_owner(runtime->status.acceptance, runtime->status.accepted_intent),
          runtime->reserved_domains);
      if (!released.ok()) {
        CloseAdmission();
        return;
      }
    }
    bool recovery_still_active = false;
    {
      std::lock_guard lock(state_mutex_);
      std::erase_if(active_transactions_,
                    [&](const auto& candidate) { return candidate == runtime; });
      if (recovery_in_progress_ && active_transactions_.empty() &&
          active_route_transactions_.empty()) {
        recovery_in_progress_ = false;
      }
      recovery_still_active = recovery_in_progress_;
    }
    {
      std::lock_guard lock(admission_mutex_);
      if (runtime->owner_wide_containment_gate) {
        runtime->owner_wide_containment_gate = false;
        if (owner_wide_containment_gate_count_ == 0U) {
          CloseAdmissionLocked();
          return;
        }
        --owner_wide_containment_gate_count_;
      }
      if (runtime->unresolved_effect_admission_gate) {
        runtime->unresolved_effect_admission_gate = false;
        if (unresolved_effect_admission_gate_count_ == 0U) {
          CloseAdmissionLocked();
          return;
        }
        --unresolved_effect_admission_gate_count_;
      }
      if (!recovery_still_active && owner_wide_containment_gate_count_ == 0U &&
          unresolved_effect_admission_gate_count_ == 0U && !admission_fault_closed_ &&
          !shutting_down_.load(std::memory_order_acquire) &&
          !accepting_.load(std::memory_order_acquire)) {
        if (!RotateLifecycleOwnerEpoch()) {
          admission_fault_closed_ = true;
          return;
        }
        ++admission_gate_generation_;
        accepting_.store(true, std::memory_order_release);
      }
    }
    {
      std::lock_guard lock(schedule_mutex_);
      std::erase_if(scheduled_handles_, [](const auto& scheduled) {
        return scheduled.second->load(std::memory_order_acquire);
      });
    }
    state_changed_.notify_all();
  }

  void Shutdown() noexcept {
    if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    accepting_.store(false, std::memory_order_release);
    std::vector<std::pair<ScheduleHandle, std::shared_ptr<std::atomic<bool>>>> pending_handles;
    bool scheduler_lane_used = false;
    {
      std::unique_lock lock(schedule_mutex_);
      schedule_state_changed_.wait(lock, [&] { return schedule_publications_in_flight_ == 0U; });
      pending_handles.swap(scheduled_handles_);
      scheduler_lane_used = scheduler_lane_used_;
    }
    for (const auto& scheduled : pending_handles) {
      if (!scheduled.second->load(std::memory_order_acquire)) {
        CancelScheduledHandleOrTerminate(scheduled.first);
      }
    }
    if (scheduler_lane_used) {
      bool drained = false;
      try {
        drained = scheduler_.drain_for(SchedulerPriority::Lifecycle, std::chrono::seconds(5));
      } catch (...) {
        std::terminate();
      }
      if (!drained) {
        // Returning would leave callbacks that capture this owner alive. A
        // failed lifetime proof is a process-fatal shutdown invariant.
        std::terminate();
      }
    }
    worker_pool_.Stop();
    kernel_worker_pool_.Stop();
  }

  void CloseAdmissionLocked() noexcept {
    admission_fault_closed_ = true;
    accepting_.store(false, std::memory_order_release);
    ++admission_gate_generation_;
  }

  void AcquireOwnerWideContainmentGateLocked(
      const std::shared_ptr<RuntimeState>& runtime) noexcept {
    if (runtime->owner_wide_containment_gate) {
      return;
    }
    runtime->owner_wide_containment_gate = true;
    ++owner_wide_containment_gate_count_;
    accepting_.store(false, std::memory_order_release);
    ++admission_gate_generation_;
  }

  void AcquireOwnerWideContainmentGate(const std::shared_ptr<RuntimeState>& runtime) noexcept {
    std::lock_guard lock(admission_mutex_);
    AcquireOwnerWideContainmentGateLocked(runtime);
  }

  void AcquireUnresolvedEffectAdmissionGate(const std::shared_ptr<RuntimeState>& runtime) noexcept {
    std::lock_guard lock(admission_mutex_);
    if (runtime->unresolved_effect_admission_gate) {
      return;
    }
    runtime->unresolved_effect_admission_gate = true;
    ++unresolved_effect_admission_gate_count_;
    accepting_.store(false, std::memory_order_release);
    ++admission_gate_generation_;
  }

  void ReleaseUnresolvedEffectAdmissionGate(const std::shared_ptr<RuntimeState>& runtime) noexcept {
    bool recovery_still_active = false;
    {
      std::lock_guard lock(state_mutex_);
      recovery_still_active = recovery_in_progress_;
    }
    std::lock_guard lock(admission_mutex_);
    if (!runtime->unresolved_effect_admission_gate) {
      return;
    }
    runtime->unresolved_effect_admission_gate = false;
    if (unresolved_effect_admission_gate_count_ == 0U) {
      CloseAdmissionLocked();
      return;
    }
    --unresolved_effect_admission_gate_count_;
    if (!recovery_still_active && unresolved_effect_admission_gate_count_ == 0U &&
        owner_wide_containment_gate_count_ == 0U && !admission_fault_closed_ &&
        !shutting_down_.load(std::memory_order_acquire) &&
        !accepting_.load(std::memory_order_acquire)) {
      ++admission_gate_generation_;
      accepting_.store(true, std::memory_order_release);
    }
  }

  void CancelScheduledHandleOrTerminate(ScheduleHandle handle) noexcept {
    try {
      scheduler_.cancel(handle);
    } catch (...) {
      std::terminate();
    }
  }

  void FinishSchedulePublication() noexcept {
    try {
      std::lock_guard lock(schedule_mutex_);
      if (schedule_publications_in_flight_ == 0U) {
        std::terminate();
      }
      --schedule_publications_in_flight_;
      schedule_state_changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  [[nodiscard]] bool ScheduleLifecycleCallback(TimePoint deadline, std::function<void()> callback) {
    const auto fired = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard lock(schedule_mutex_);
      if (shutting_down_.load(std::memory_order_acquire)) {
        return false;
      }
      ++schedule_publications_in_flight_;
    }

    ScheduleHandle handle = 0U;
    try {
      handle = scheduler_.schedule(
          deadline,
          [fired, callback = std::move(callback)] {
            fired->store(true, std::memory_order_release);
            callback();
          },
          SchedulerPriority::Lifecycle);
    } catch (...) {
      FinishSchedulePublication();
      throw;
    }

    bool cancel = false;
    std::exception_ptr publication_error;
    try {
      std::lock_guard lock(schedule_mutex_);
      scheduler_lane_used_ = true;
      std::erase_if(scheduled_handles_, [](const auto& scheduled) {
        return scheduled.second->load(std::memory_order_acquire);
      });
      if (!fired->load(std::memory_order_acquire)) {
        if (shutting_down_.load(std::memory_order_acquire)) {
          cancel = true;
        } else {
          scheduled_handles_.emplace_back(handle, fired);
        }
      }
    } catch (...) {
      publication_error = std::current_exception();
      cancel = !fired->load(std::memory_order_acquire);
    }
    if (cancel) {
      // Keep the publication barrier raised until the accepted scheduler
      // handle is no longer able to start a future callback.
      CancelScheduledHandleOrTerminate(handle);
    }
    FinishSchedulePublication();
    if (publication_error) {
      std::rethrow_exception(publication_error);
    }
    return !cancel;
  }

  const ComponentLifecycleCoordinatorConfig config_;
  ICCLCJournal& journal_;
  CCLCDomainReservationTable& domain_reservations_;
  const ICCLCCurrentComponentSnapshotSource& current_snapshot_source_;
  const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source_;
  const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver_;
  ICCLCKernelDisableHandoffPort& kernel_disable_handoff_port_;
  IClock& clock_;
  IScheduler& scheduler_;
  IKernelCellAdmissionPort* kernel_cell_admission_port_;
  std::optional<ComponentLifecycleCoordinatorRouteServices> route_services_;
  FixedWorkerPool worker_pool_;
  FixedWorkerPool kernel_worker_pool_;

  mutable std::mutex state_mutex_;
  std::condition_variable state_changed_;
  bool started_ = false;
  bool starting_ = false;
  std::vector<std::shared_ptr<RuntimeState>> active_transactions_;
  struct RouteRuntimeState;
  std::vector<std::shared_ptr<RouteRuntimeState>> active_route_transactions_;
  bool recovery_in_progress_ = false;
  std::atomic<bool> accepting_{false};
  std::atomic<bool> shutting_down_{false};

  std::mutex admission_mutex_;
  std::condition_variable admission_resolution_ready_;
  bool admission_resolution_active_ = false;
  bool admission_persistence_active_ = false;
  std::uint64_t highest_transaction_generation_ = 0U;
  std::uint64_t highest_route_transaction_generation_ = 0U;
  std::uint64_t admission_gate_generation_ = 0U;
  std::size_t owner_wide_containment_gate_count_ = 0U;
  std::size_t unresolved_effect_admission_gate_count_ = 0U;
  bool admission_fault_closed_ = false;
  std::vector<std::pair<std::string, std::uint64_t>> component_generation_watermarks_;
  std::vector<std::pair<std::string, std::uint64_t>> active_configuration_generation_watermarks_;
  std::mutex schedule_mutex_;
  std::condition_variable schedule_state_changed_;
  std::size_t schedule_publications_in_flight_ = 0U;
  bool scheduler_lane_used_ = false;
  std::vector<std::pair<ScheduleHandle, std::shared_ptr<std::atomic<bool>>>> scheduled_handles_;
};

ComponentLifecycleCoordinator::ComponentLifecycleCoordinator(
    ComponentLifecycleCoordinatorConfig config, ICCLCJournal& journal,
    CCLCDomainReservationTable& domain_reservations,
    const ICCLCCurrentComponentSnapshotSource& current_snapshot_source,
    const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source,
    const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver,
    ICCLCKernelDisableHandoffPort& kernel_disable_handoff_port, IClock& clock,
    IScheduler& scheduler, IKernelCellAdmissionPort* kernel_cell_admission_port,
    std::optional<ComponentLifecycleCoordinatorRouteServices> route_services)
    : impl_(std::make_unique<Impl>(std::move(config), journal, domain_reservations,
                                   current_snapshot_source, lifecycle_plan_catalog_source,
                                   lifecycle_port_resolver, kernel_disable_handoff_port, clock,
                                   scheduler, kernel_cell_admission_port,
                                   std::move(route_services))) {}

ComponentLifecycleCoordinator::~ComponentLifecycleCoordinator() = default;

Result<void> ComponentLifecycleCoordinator::Start() {
  return impl_->Start();
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::SubmitLifecycle(
    const ComponentLifecycleIntentDTO& intent) {
  return impl_->SubmitLifecycle(intent);
}

Result<CCLCRouteSelectionRequestResultDTO> ComponentLifecycleCoordinator::SubmitRouteSelection(
    const RouteSelectionIntentDTO& intent) {
  return impl_->SubmitRouteSelection(intent);
}

Result<std::optional<RouteSelectionStatusDTO>>
ComponentLifecycleCoordinator::LoadRouteSelectionStatus(
    const RouteSelectionTransactionId& route_selection_transaction_id) {
  return impl_->LoadRouteSelectionStatus(route_selection_transaction_id);
}

Result<std::optional<ComponentLifecycleStatusDTO>>
ComponentLifecycleCoordinator::LoadLifecycleStatus(
    const LifecycleTransactionId& lifecycle_transaction_id) {
  return impl_->LoadLifecycleStatus(lifecycle_transaction_id);
}

bool ComponentLifecycleCoordinator::accepting_new_requests() const noexcept {
  return impl_->accepting_new_requests();
}

RequestOwnerEpoch ComponentLifecycleCoordinator::request_owner_epoch() const {
  return impl_->request_owner_epoch();
}

void ComponentLifecycleCoordinator::CloseAdmission() noexcept {
  impl_->CloseAdmission();
}

bool ComponentLifecycleCoordinator::DrainFor(std::chrono::nanoseconds timeout) {
  return impl_->DrainFor(timeout);
}

}  // namespace manyforge::core
