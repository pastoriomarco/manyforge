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
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core {
namespace {

inline constexpr std::size_t kMaxLifecycleWorkerCount = 64U;
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

// Preserve only the current failed primitive's cause. A successful, unknown, or
// foreign result is not evidence for this reconciliation failure.
[[nodiscard]] ErrorDTO with_failed_primitive_details(
    ErrorDTO error, const ComponentLifecycleStatusDTO& status,
    const ComponentLifecycleDomainStatusDTO& domain, const LifecyclePrimitiveId& primitive_id) {
  if (domain.primitive_attempts.empty() || !domain.resolved_provider_occurrence.has_value()) {
    return error;
  }
  const auto& result = domain.primitive_attempts.back();
  const auto& provider = *domain.resolved_provider_occurrence;
  if (!result.locally_valid() ||
      result.known_result != LifecyclePrimitiveKnownResultDTO::kFailed || !result.error ||
      result.lifecycle_transaction_id != status.acceptance.lifecycle_transaction_id ||
      result.lifecycle_transaction_generation !=
          status.acceptance.lifecycle_transaction_generation ||
      result.controllable_component_id != status.accepted_intent.controllable_component_id ||
      result.control_domain_id != domain.control_domain_id ||
      result.control_domain_revision != provider.control_domain_revision ||
      result.provider_instance_id != provider.provider_instance_id ||
      result.provider_manifest_id != provider.provider_manifest_id ||
      result.provider_manifest_revision != provider.provider_manifest_revision ||
      result.provider_runtime_unit_id != provider.provider_runtime_unit_id ||
      result.provider_runtime_epoch != provider.provider_runtime_epoch ||
      result.lifecycle_primitive_id != primitive_id) {
    return error;
  }
  const auto bounded = [](std::string value) {
    if (value.size() > kMaxLifecycleErrorDetailValueBytes) {
      auto end = kMaxLifecycleErrorDetailValueBytes;
      while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
        --end;
      }
      value.resize(end);
    }
    return value;
  };
  const auto& cause = *result.error;
  error.details.emplace("primitive_code", bounded(cause.code));
  const auto reason = cause.details.find("reason");
  if (reason != cause.details.end()) {
    error.details.emplace("primitive_reason", bounded(reason->second));
  }
  const auto response = cause.details.find("controller_response_message");
  error.details.emplace(
      "primitive_message",
      bounded(response != cause.details.end() && !response->second.empty() ? response->second
                                                                         : cause.message));
  for (const auto* key : {"controller_name", "switch_action", "controller_service"}) {
    const auto detail = cause.details.find(key);
    if (detail != cause.details.end()) {
      error.details.emplace(key, bounded(detail->second));
    }
  }
  error.details.emplace("primitive_attempt_id",
                        bounded(result.lifecycle_primitive_attempt_id.value()));
  return error;
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
  return accepted.source_runtime_epoch != current.source_runtime_epoch && normalized == current;
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

  [[nodiscard]] bool IsIdle() {
    std::lock_guard lock(mutex_);
    return queued_.empty() && running_ == 0U;
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
      // The task owns accepted runtime/product captures. Release those tails
      // outside the pool mutex before exposing idle/completed accounting.
      task = {};
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
         observation_poll_period_ns > 0 &&
         observation_poll_period_ns <= kMaxLifecycleTransactionTimeoutNs &&
         route_selection_timeout_duration_ns > 0 &&
         route_selection_timeout_duration_ns <= kMaxRouteSelectionTimeoutNs &&
         (!quiescent_predecessor ||
          (!quiescent_predecessor->empty() &&
           quiescent_predecessor->value().size() <= kMaxLifecycleIdentityBytes));
}

bool ComponentLifecycleCoordinatorRouteServices::locally_valid() const noexcept {
  return current_snapshot_source != nullptr && controller_lifecycle_port_resolver != nullptr &&
         kernel_route_fence_port != nullptr && configuration_publication_port != nullptr;
}

class ComponentLifecycleCoordinator::Impl final {
  struct RuntimeState;
  struct PendingProductExchange final {
    std::shared_ptr<const ComponentLifecycleProductContext> previous;
    std::shared_ptr<const ComponentLifecycleProductContext> successor;
    RequestOwnerEpoch owner;
  };

  struct ProductControl final {
    explicit ProductControl(std::shared_ptr<const ComponentLifecycleProductContext> initial)
        : current(std::move(initial)) {}
    std::mutex mutex;
    std::condition_variable ready;
    std::shared_ptr<const ComponentLifecycleProductContext> current;
    std::size_t operation_pins{};
    bool exchange_pending{};
    std::optional<PendingProductExchange> pending;
  };

  struct ProductLease final {
    explicit ProductLease(std::shared_ptr<ProductControl> control) : control(std::move(control)) {}
    ~ProductLease() {
      if (!armed) return;
      std::lock_guard lock(control->mutex);
      --control->operation_pins;
      control->ready.notify_all();
    }
    std::shared_ptr<ProductControl> control;
    bool armed{};
  };

 public:
  Impl(ComponentLifecycleCoordinatorConfig config, ICCLCJournal& journal,
       CCLCDomainReservationTable& domain_reservations,
       std::shared_ptr<const ComponentLifecycleProductContext> product_context,
       IKernelCellAdmissionPort& kernel_cell_admission_port, IClock& clock, IScheduler& scheduler)
      : config_(std::move(config)),
        request_epoch_(std::make_shared<const RequestOwnerEpoch>(config_.request_owner_epoch)),
        journal_(journal),
        domain_reservations_(domain_reservations),
        product_control_(std::make_shared<ProductControl>(std::move(product_context))),
        kernel_cell_admission_port_(kernel_cell_admission_port),
        clock_(clock),
        scheduler_(scheduler),
        worker_pool_(kMaxCoordinatorActiveTransactions) {}

  ~Impl() { Shutdown(); }

  [[nodiscard]] Result<ComponentLifecycleProductPin> AcquireProductPin() const {
    // Allocate before taking the short exclusion: an unarmed lease never
    // reenters the mutex if allocation or a refusal destroys it.
    auto lease = std::make_shared<ProductLease>(product_control_);
    std::lock_guard lock(admission_mutex_);
    if (product_control_->exchange_pending) {
      return Result<ComponentLifecycleProductPin>::Err(
          coordinator_error("cclc_product_exchange_pending", "CCLC is updating its configuration",
                            "product_exchange_pending"));
    }
    if (shutting_down_.load(std::memory_order_acquire)) {
      return Result<ComponentLifecycleProductPin>::Err(
          coordinator_error("cclc_product_owner_closed", "CCLC product owner is shutting down",
                            "product_owner_closed"));
    }
    ++product_control_->operation_pins;
    lease->armed = true;
    return Result<ComponentLifecycleProductPin>::Ok(
        ComponentLifecycleProductPin(product_control_->current, std::move(lease)));
  }

  [[nodiscard]] Result<IoOutputLocalResynchronizationProofDTO> ReadIoOutputLocalResynchronization(
      const RuntimeParticipantBindingDTO& captured) const {
    using Reply = Result<IoOutputLocalResynchronizationProofDTO>;
    const auto refuse = [] {
      return Reply::Err(coordinator_error(
          "io_output_local_resynchronization_unproved",
          "Actual completed local Disable then Enable is not retained for this exact output",
          "io_output_local_resynchronization_unproved"));
    };
    const auto valid = verify_runtime_participant_binding(captured);
    if (!valid.ok() || !captured.io_output_binding) return refuse();
    const auto& output = *captured.io_output_binding;
    auto pin = AcquireProductPin();
    if (!pin.ok()) return Reply::Err(pin.error());
    const auto& context = pin.value().context();
    const auto current = context.current_snapshot_source.ReadCurrentComponentSnapshot(
        captured.controllable_component_id);
    if (!current.ok()) return Reply::Err(current.error());
    const auto& snapshot = current.value();
    if (!VerifyComponentLifecycleCurrentSnapshot(snapshot).ok() || !context.identity ||
        snapshot.controllable_component_id != captured.controllable_component_id ||
        snapshot.component_lifecycle_state != ComponentLifecycleStateDTO::kActive ||
        snapshot.active_publication_receipt != context.identity->publication ||
        std::count(context.identity->configured_components.begin(),
                   context.identity->configured_components.end(),
                   captured.controllable_component_id) != 1 ||
        snapshot.active_publication_receipt.request.candidate_retained_bundle_id !=
            captured.retained_bundle_id ||
        snapshot.active_publication_receipt.request.candidate_graph_revision !=
            captured.graph_revision ||
        snapshot.component_lifecycle_generation.value() <=
            output.component_lifecycle_generation.value() ||
        std::count(snapshot.provider_occurrences.begin(), snapshot.provider_occurrences.end(),
                   output.provider) != 1 ||
        std::any_of(snapshot.domains.begin(), snapshot.domains.end(),
                    [](const auto& domain) { return domain.provider() == nullptr; }))
      return refuse();
    const auto history =
        journal_.LoadRetainedComponentLifecycleHistory(captured.controllable_component_id);
    if (!history.ok()) return Reply::Err(history.error());
    const auto qualified = [&](const ComponentLifecycleStatusDTO& status,
                               ComponentLifecycleIntentKindDTO intent,
                               ComponentLifecycleStateDTO state, std::string_view primitive) {
      if (!status.locally_valid() || status.selected_bring_up || status.apply_bring_up ||
          status.accepted_intent.intent != intent ||
          status.accepted_intent.controllable_component_id != captured.controllable_component_id ||
          status.accepted_intent.expected_graph_revision != captured.graph_revision ||
          status.phase != ComponentLifecycleTransactionPhaseDTO::kSucceeded ||
          status.terminal_result != ComponentLifecycleTerminalResultDTO::kSucceeded ||
          status.current_state != state || status.reconciliation_required || status.error ||
          status.acceptance.component_lifecycle_generation.value() <=
              output.component_lifecycle_generation.value())
        return false;
      const auto domain =
          std::find_if(status.domains.begin(), status.domains.end(), [&](const auto& row) {
            return row.control_domain_id == output.provider.control_domain_id;
          });
      if (domain == status.domains.end() || !domain->current_scope ||
          !domain->current_scope->provider() ||
          domain->resolved_provider_occurrence != output.provider ||
          domain->current_state != state ||
          domain->action_state != ComponentLifecycleDomainActionStateDTO::kSucceeded ||
          domain->current_scope->provider()->provider.signal_configuration_digest !=
              output.signal_configuration_digest ||
          std::find(domain->current_scope->resource_ids().begin(),
                    domain->current_scope->resource_ids().end(),
                    *output.signal.output_resource) == domain->current_scope->resource_ids().end())
        return false;
      // An already-satisfied label is not proof that a local restart happened.
      return std::any_of(
          domain->primitive_attempts.begin(), domain->primitive_attempts.end(),
          [&](const auto& attempt) {
            return attempt.lifecycle_primitive_id.value() == primitive &&
                   attempt.provider_instance_id == output.provider.provider_instance_id &&
                   attempt.provider_manifest_id == output.provider.provider_manifest_id &&
                   attempt.provider_manifest_revision ==
                       output.provider.provider_manifest_revision &&
                   attempt.provider_runtime_unit_id == output.provider.provider_runtime_unit_id &&
                   attempt.provider_runtime_epoch == output.provider.provider_runtime_epoch &&
                   attempt.dispatch_state == LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown &&
                   attempt.effect == LifecyclePrimitiveEffectDTO::kObserved &&
                   attempt.known_result == LifecyclePrimitiveKnownResultDTO::kSucceeded;
          });
    };
    const ComponentLifecycleStatusDTO* enabled = nullptr;
    const ComponentLifecycleStatusDTO* disabled = nullptr;
    for (const auto& status : history.value()) {
      if (status.acceptance.component_lifecycle_generation ==
              snapshot.component_lifecycle_generation &&
          qualified(status, ComponentLifecycleIntentKindDTO::kEnable,
                    ComponentLifecycleStateDTO::kActive, "io.provider.enable"))
        enabled = &status;
    }
    if (!enabled) return refuse();
    for (const auto& status : history.value()) {
      if (status.acceptance.component_lifecycle_generation.value() <
              enabled->acceptance.component_lifecycle_generation.value() &&
          status.latest_updated_at_ns <= enabled->accepted_at_ns &&
          qualified(status, ComponentLifecycleIntentKindDTO::kDisable,
                    ComponentLifecycleStateDTO::kDisabled, "io.provider.disable") &&
          (!disabled || status.acceptance.component_lifecycle_generation.value() >
                            disabled->acceptance.component_lifecycle_generation.value()))
        disabled = &status;
    }
    if (!disabled) return refuse();
    const auto after = context.current_snapshot_source.ReadCurrentComponentSnapshot(
        captured.controllable_component_id);
    if (!after.ok()) return Reply::Err(after.error());
    if (after.value() != snapshot) return refuse();
    return Reply::Ok({captured.retained_bundle_id, captured.graph_revision,
                      captured.controllable_component_id, output.provider,
                      disabled->acceptance.lifecycle_transaction_id,
                      enabled->acceptance.lifecycle_transaction_id,
                      disabled->acceptance.component_lifecycle_generation,
                      enabled->acceptance.component_lifecycle_generation});
  }

  [[nodiscard]] Result<CCLCComponentReadDTO> ReadCurrentComponentView(
      const ControllableComponentId& component,
      const std::function<Result<ComponentLifecycleTargetViewDTO>()>& read_target,
      const std::function<Result<ComponentLifecycleReadinessTargetFactsDTO>()>& read_target_facts) const {
    using ReadResult = Result<CCLCComponentReadDTO>;
    auto pin = AcquireProductPin();
    if (!pin.ok()) return ReadResult::Err(pin.error());
    const auto& context = pin.value().context();
    const auto owner = CurrentOwnerEpoch();
    const auto now = clock_.now();
    if (owner.empty() || now < 0 || !read_target || !read_target_facts)
      return ReadResult::Err(coordinator_error("cclc_product_read_unavailable",
          "the current component owner read is unavailable", "owner_read_unavailable"));
    auto current = context.current_snapshot_source.ReadCurrentComponentSnapshot(component);
    if (!current.ok()) return ReadResult::Err(current.error());
    auto verified = VerifyComponentLifecycleCurrentSnapshot(current.value());
    if (!verified.ok()) return ReadResult::Err(verified.error());
    if (current.value().controllable_component_id != component || !context.identity ||
        current.value().active_publication_receipt != context.identity->publication ||
        std::count(context.identity->configured_components.begin(),
                   context.identity->configured_components.end(), component) != 1)
      return ReadResult::Err(coordinator_error("cclc_product_read_unavailable",
          "the component does not belong to this exact current product", "product_identity_mismatch"));
    auto target_facts = read_target_facts();
    if (!target_facts.ok()) return ReadResult::Err(target_facts.error());
    auto target = read_target();
    if (!target.ok()) return ReadResult::Err(target.error());
    if (target.value().target != target_facts.value().facts.target)
      return ReadResult::Err(coordinator_error("component_lifecycle_facts_stale",
          "the component target changed during its read", "target_read_changed"));
    const auto route_membership = ReadComponentRouteMembership(component);
    std::vector<RouteSelectionStatusDTO> route_statuses;
    route_statuses.reserve(route_membership.size());
    for (const auto& route : route_membership) {
      auto status = journal_.LoadRetainedRouteSelectionStatus(
          route->original_acceptance.route_selection_transaction_id);
      if (!status.ok()) return ReadResult::Err(status.error());
      if (!status.value() || status.value()->acceptance != route->original_acceptance)
        return ReadResult::Err(coordinator_error("route_selection_status_unavailable",
            "the current route pursuit has no exact durable status", "route_status_unavailable"));
      route_statuses.push_back(std::move(*status.value()));
    }
    const auto admission = kernel_cell_admission_port_.GetAuthoringAdmission();
    std::uint64_t gate_generation;
    const char* owner_admission_closed_reason = nullptr;
    {
      std::lock_guard lock(admission_mutex_);
      gate_generation = admission_gate_generation_;
      if (!accepting_.load(std::memory_order_acquire))
        owner_admission_closed_reason = "cclc_not_accepting";
      else if (admission_fault_closed_.load(std::memory_order_acquire))
        owner_admission_closed_reason = "cclc_admission_fault_closed";
      else if (admission_resolution_active_)
        owner_admission_closed_reason = "cclc_admission_resolution_active";
      else if (admission_persistence_active_)
        owner_admission_closed_reason = "cclc_admission_persistence_active";
      else if (owner_wide_containment_gate_count_ != 0U)
        owner_admission_closed_reason = "cclc_owner_containment_pending";
      else if (unresolved_effect_admission_gate_count_ != 0U)
        owner_admission_closed_reason = "cclc_effect_outcome_unresolved";
    }
    const bool owner_admission_open = owner_admission_closed_reason == nullptr;
    CCLCComponentReadDTO result{current.value(), std::move(target.value()),
        {owner, now, std::nullopt, std::nullopt, false, std::nullopt, {},
         {std::nullopt, std::nullopt, std::nullopt}}};
    auto& readiness = result.readiness;
    const auto block = [&](const ErrorDTO& error) {
      if (!readiness.error) readiness.error = error;
    };
    if (target_facts.value().blocker) block(*target_facts.value().blocker);
    if (owner_admission_closed_reason != nullptr)
      block(coordinator_error("cclc_admission_closed", "CCLC admission is closed",
                              owner_admission_closed_reason));
    if (!admission.authoring_allowed || !admission.admission_generation.nonzero())
      block(coordinator_error("authoring_not_ready", "cell admission is closed", "cell_admission_closed"));
    switch (result.snapshot.component_lifecycle_state) {
      case ComponentLifecycleStateDTO::kDeclared:
      case ComponentLifecycleStateDTO::kDisabled:
        readiness.next_intent = ComponentLifecycleIntentKindDTO::kPrepare;
        readiness.requested_state = ComponentLifecycleStateDTO::kPrepared;
        break;
      case ComponentLifecycleStateDTO::kPrepared:
        readiness.next_intent = ComponentLifecycleIntentKindDTO::kEnable;
        readiness.requested_state = ComponentLifecycleStateDTO::kActive;
        break;
      case ComponentLifecycleStateDTO::kActive:
        break;
      default:
        block(coordinator_error("invalid_state_transition",
            "the current component state has no ordinary activation step", "activation_state_not_eligible"));
        break;
    }
    // Capture owning readers from the existing route publisher, never the
    // inventory's original static readers. The same captured association is
    // checked before and after the complete projection.
    std::optional<ComponentLifecycleReadinessSourceView> sources;
    std::optional<ErrorDTO> source_association_error;
    const auto source_unavailable = [&](const ErrorDTO& error) {
      source_association_error = error;
      block(error);
    };
    if (!route_statuses.empty()) {
      // Pending or uncertain route work owns the association. A surviving old
      // reader is not evidence for the candidate, even before fence acquisition.
      const auto error = std::find_if(route_statuses.begin(), route_statuses.end(),
          [](const auto& status) { return status.error.has_value(); });
      source_unavailable(error != route_statuses.end() ? *error->error :
          coordinator_error("route_selection_pending",
              "the current component route pursuit has not settled", "route_pursuit_pending"));
    } else if (context.capture_readiness_sources) {
      auto captured = context.capture_readiness_sources(result.snapshot);
      if (!captured.ok()) source_unavailable(captured.error());
      else if (!captured.value().validate_current)
        source_unavailable(coordinator_error("cclc_product_read_unavailable", "the source association has no current readback",
                                "source_readback_unavailable"));
      else {
        sources = std::move(captured.value());
        auto checked = sources->validate_current();
        if (!checked.ok()) { source_unavailable(checked.error()); sources.reset(); }
      }
    } else {
      source_unavailable(coordinator_error("cclc_product_read_unavailable", "the current route-source view is not composed",
                              "route_source_view_unavailable"));
    }
    readiness.domains.reserve(result.snapshot.provider_occurrences.size());
    const bool active = result.snapshot.component_lifecycle_state == ComponentLifecycleStateDTO::kActive;
    for (const auto& provider : result.snapshot.provider_occurrences) {
      ComponentLifecycleDomainReadinessDTO row{provider, std::nullopt, std::nullopt, std::nullopt};
      if (source_association_error) {
        // A still-live predecessor callback is not current-route evidence.
        row.error = source_association_error;
        readiness.domains.push_back(std::move(row));
        continue;
      }
      auto health = context.lifecycle_port_resolver.ReadExactLifecycleHealth(provider, now);
      if (!health.ok()) row.error = health.error();
      else {
        row.health = std::move(health.value());
        if (!row.health->locally_valid()) {
          row.error = coordinator_error("component_lifecycle_observation_invalid",
              "the exact provider health projection is invalid", "health_projection_invalid");
          row.health.reset();
        }
        else {
          row.has_activation_step = row.health->has_activation_step;
          // Match the follower's existing cause interpretation. A diagnostic
          // alone is not an additional lifecycle predicate; availability is
          // decided by the same qualified catalog below.
          auto cause = LifecycleBlockingCauseClassDTO::kNone;
          for (const auto& fact : row.health->facts) {
            if (fact.lifecycle_fact_id.value() != kLifecycleBlockingCauseFactId) continue;
            cause = fact.value.kind == LifecycleFactValueKindDTO::kToken && fact.value.token_value
                        ? parse_lifecycle_blocking_cause_class(*fact.value.token_value)
                              .value_or(LifecycleBlockingCauseClassDTO::kUnknown)
                        : LifecycleBlockingCauseClassDTO::kUnknown;
            break;
          }
          const auto recovered = target_facts.value().facts.target
                                     ? target_facts.value().facts.target->recovered_cause
                                     : std::nullopt;
          if (lifecycle_cause_requires_intervention(cause) && recovered != cause) {
            row.error = row.health->diagnostic.value_or(coordinator_error(
                "intervention_required", "the current lifecycle source requires intervention",
                "intervention_required"));
            if (row.error->details.size() < 64U)
              row.error->details.emplace("cause_class", std::string(to_string(cause)));
          }
          auto catalog = context.lifecycle_plan_catalog_source.LoadExactLifecyclePlanCatalog(
              provider.provider_manifest_id, provider.provider_manifest_revision,
              provider.lifecycle_plan_catalog_digest);
          if (!row.error && !catalog.ok()) row.error = catalog.error();
          else if (!row.error && (readiness.next_intent || active)) {
            auto decision = EvaluateLifecycleDomainPlan(
                active ? ComponentLifecycleIntentKindDTO::kEnable : *readiness.next_intent,
                active ? ComponentLifecycleStateDTO::kActive : *readiness.requested_state,
                catalog.value(), provider, *row.health);
            if (!decision.ok()) row.error = decision.error();
            else if (active && decision.value().kind != LifecycleDomainPlanDecisionKind::kAlreadySatisfied)
              row.error = coordinator_error("component_lifecycle_transition_missing",
                  "the existing Active postcondition is not satisfied", "active_postcondition_not_satisfied");
            else if (!active) row.decision_kind = decision.value().kind;
          }
        }
      }
      if (row.error) block(*row.error);
      readiness.domains.push_back(std::move(row));
    }
    if (!context.measured_state_inventory) {
      readiness.measured_state.error = source_association_error.has_value()
          ? *source_association_error
          : coordinator_error("measured_source_unavailable",
              "the qualified robot inventory is not composed", "measured_inventory_unavailable");
    } else {
      const auto& robots = context.measured_state_inventory->robots();
      const auto robot = std::find_if(robots.begin(), robots.end(),
          [&](const auto& requirement) { return requirement.component_id == component; });
      readiness.measured_state.required = robot != robots.end();
      if (robot != robots.end()) {
        readiness.measured_state.available = false;
        if (!sources || !sources->measured_sources)
          readiness.measured_state.error = coordinator_error("measured_source_unavailable",
              "the exact current component source view is unavailable", "current_measured_sources_unavailable");
        else if (sources->measured_sources->component_id != component)
          readiness.measured_state.error = coordinator_error("measured_source_unavailable",
              "the captured source view belongs to another component", "measured_component_mismatch");
        else {
          auto measured = context.measured_state_inventory->ReadComponent(*sources->measured_sources, now);
          if (!measured.ok()) readiness.measured_state.error = measured.error();
          else readiness.measured_state.available = true;
        }
      } else if (sources && sources->measured_sources) {
        readiness.measured_state.required.reset();
        readiness.measured_state.error = coordinator_error("measured_source_unavailable",
            "the source view does not belong to the qualified robot inventory", "measured_membership_mismatch");
      }
    }
    // Measured state is command readiness, not a lifecycle transition
    // prerequisite.  Keep its exact warning in the component view; motion
    // admission consumes that verdict separately.  In particular, a
    // configured commanded-state simulator can Prepare/Enable from its
    // authored hardware state while playback is stopped.
    if (sources) {
      auto checked = sources->validate_current();
      if (!checked.ok()) return ReadResult::Err(checked.error());
    }
    // A positive sampled verdict must not outlive its actual owner, target,
    // publication, route or admission generation. No retry/probe is issued.
    for (std::size_t index = 0U; index < route_membership.size(); ++index) {
      auto status = journal_.LoadRetainedRouteSelectionStatus(
          route_membership[index]->original_acceptance.route_selection_transaction_id);
      if (!status.ok()) return ReadResult::Err(status.error());
      if (!status.value() || *status.value() != route_statuses[index])
        return ReadResult::Err(coordinator_error("component_lifecycle_facts_stale",
            "the component route status changed during its read", "route_status_changed"));
    }
    auto latest = context.current_snapshot_source.ReadCurrentComponentSnapshot(component);
    if (!latest.ok()) return ReadResult::Err(latest.error());
    auto latest_facts = read_target_facts();
    if (!latest_facts.ok()) return ReadResult::Err(latest_facts.error());
    // Keep the existing short-circuit order and sample each owner/admission
    // value at most once. Diagnostics consume these same samples only.
    const bool snapshot_changed = latest.value() != result.snapshot;
    const bool target_facts_changed =
        !snapshot_changed && latest_facts.value() != target_facts.value();
    const auto latest_owner = !snapshot_changed && !target_facts_changed
                                  ? std::optional(CurrentOwnerEpoch())
                                  : std::nullopt;
    const bool owner_changed = latest_owner && *latest_owner != owner;
    const auto latest_admission = latest_owner && !owner_changed
                                      ? std::optional(kernel_cell_admission_port_.GetAuthoringAdmission())
                                      : std::nullopt;
    const bool admission_changed = latest_admission && *latest_admission != admission;
    if (snapshot_changed || target_facts_changed || owner_changed || admission_changed) {
      auto error = coordinator_error("component_lifecycle_facts_stale",
          "the current component context changed during its read", "component_read_changed");
      const auto token = [](bool value) { return value ? "true" : "false"; };
      error.details.emplace("controllable_component_id", component.value());
      error.details.emplace("snapshot_changed", token(snapshot_changed));
      error.details.emplace("target_facts_changed",
                            snapshot_changed ? "not_checked" : token(target_facts_changed));
      error.details.emplace("owner_changed", latest_owner ? token(owner_changed) : "not_checked");
      error.details.emplace("admission_changed",
                            latest_admission ? token(admission_changed) : "not_checked");
      error.details.emplace("snapshot_digest_before", result.snapshot.snapshot_digest.value());
      error.details.emplace("snapshot_digest_after", latest.value().snapshot_digest.value());
      error.details.emplace("component_generation_before",
                            std::to_string(result.snapshot.component_lifecycle_generation.value()));
      error.details.emplace("component_generation_after",
                            std::to_string(latest.value().component_lifecycle_generation.value()));
      error.details.emplace("owner_epoch_before", owner.value());
      error.details.emplace("owner_epoch_after", latest_owner ? latest_owner->value() : "not_checked");
      error.details.emplace("admission_generation_before",
                            std::to_string(admission.admission_generation.value()));
      error.details.emplace("admission_generation_after",
                            latest_admission ? std::to_string(latest_admission->admission_generation.value())
                                             : "not_checked");
      error.details.emplace("authoring_allowed_before", token(admission.authoring_allowed));
      error.details.emplace("authoring_allowed_after",
                            latest_admission ? token(latest_admission->authoring_allowed) : "not_checked");
      return ReadResult::Err(std::move(error));
    }
    {
      std::lock_guard lock(admission_mutex_);
      if (!ComponentRouteMembershipMatches(component, route_membership) ||
          admission_gate_generation_ != gate_generation ||
          product_control_->current.get() != &context ||
          shutting_down_.load(std::memory_order_acquire) || product_control_->exchange_pending ||
          owner_admission_open != (accepting_.load(std::memory_order_acquire) &&
              !admission_fault_closed_.load(std::memory_order_acquire) &&
              !admission_resolution_active_ && !admission_persistence_active_ &&
              owner_wide_containment_gate_count_ == 0U && unresolved_effect_admission_gate_count_ == 0U))
        return ReadResult::Err(coordinator_error("component_lifecycle_facts_stale",
            "the current owner changed during its read", "owner_read_changed"));
    }
    readiness.ready = !readiness.error;
    return ReadResult::Ok(std::move(result));
  }

  [[nodiscard]] Result<std::shared_ptr<const ComponentLifecycleProductContext>>
  ReadPinnedProductContext(const ComponentLifecycleProductPin& pin) const {
    using ContextResult = Result<std::shared_ptr<const ComponentLifecycleProductContext>>;
    std::lock_guard lock(admission_mutex_);
    const auto lease = std::static_pointer_cast<ProductLease>(pin.lease_);
    if (!lease || !lease->armed || lease->control != product_control_ ||
        pin.context_ != product_control_->current) {
      return ContextResult::Err(coordinator_error(
          "cclc_product_pin_foreign", "the product pin does not belong to this current owner",
          "product_pin_foreign"));
    }
    return ContextResult::Ok(pin.context_);
  }

  [[nodiscard]] bool WithdrawProductExchange(
      const std::shared_ptr<const ComponentLifecycleProductContext>& previous,
      const std::shared_ptr<const ComponentLifecycleProductContext>& successor,
      const RequestOwnerEpoch& captured_owner) {
    std::optional<PendingProductExchange> withdrawn;
    {
      std::unique_lock lock(admission_mutex_, std::try_to_lock);
      if (!lock.owns_lock()) return false;
      if (product_control_->pending && product_control_->pending->previous == previous &&
          product_control_->pending->successor == successor &&
          product_control_->pending->owner == captured_owner) {
        withdrawn = std::move(product_control_->pending);
        product_control_->pending.reset();
        product_control_->exchange_pending = false;
        admission_resolution_ready_.notify_all();
      }
    }
    return true;
  }

  void CloseProductExchangeAdmission() noexcept {
    // A failed external proof must close admission immediately even while an
    // original worker owns the admission exclusion. It must never wait for
    // that worker, nor lose the sticky fault before a later withdrawal.
    admission_fault_closed_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    std::unique_lock lock(admission_mutex_, std::try_to_lock);
    if (lock.owns_lock()) CloseAdmissionLocked();
  }

  [[nodiscard]] Result<ComponentLifecycleProductExchangeObservation> TryExchangeProductContext(
      const std::shared_ptr<const ComponentLifecycleProductContext>& previous,
      const std::shared_ptr<const ComponentLifecycleProductContext>& successor,
      const RequestOwnerEpoch& expected_owner,
      const ActiveBundlePublicationReceiptDTO& expected_previous_publication) {
    using ExchangeResult = Result<ComponentLifecycleProductExchangeObservation>;
    // Owning locals are declared before exclusions: withdrawing a pending
    // value or replacing the current root never runs a product destructor
    // under admission/state/schedule locks.
    std::optional<PendingProductExchange> withdrawn;
    std::shared_ptr<const ComponentLifecycleProductContext> retired;
    std::vector<std::pair<ScheduleHandle, std::shared_ptr<std::atomic<bool>>>> polls;
    std::vector<std::pair<std::string, std::uint64_t>> previous_watermarks;
    std::vector<std::pair<std::string, std::uint64_t>> successor_watermarks;
    const auto observation = [&](ComponentLifecycleProductExchangePhase phase,
                                 std::string reason) -> ExchangeResult {
      if (!previous || !previous->identity || !previous->identity->publication.valid())
        return ExchangeResult::Err(coordinator_error("cclc_product_exchange_refused",
                                                     "prepared predecessor cannot be observed",
                                                     "prepared_product_identity_invalid"));
      return ExchangeResult::Ok({phase, expected_owner, previous->identity->publication,
                                 phase == ComponentLifecycleProductExchangePhase::kCompleted
                                     ? successor->identity->publication
                                     : previous->identity->publication,
                                 std::move(reason)});
    };
    const auto reject = [&](std::string reason, bool uncertain = false) -> ExchangeResult {
      if (uncertain) CloseProductExchangeAdmission();
      std::unique_lock lock(admission_mutex_, std::try_to_lock);
      if (!lock.owns_lock()) {
        if (!uncertain)
          return observation(ComponentLifecycleProductExchangePhase::kPending,
                             "product_owner_exclusion_busy");
        return ExchangeResult::Err(coordinator_error("cclc_product_exchange_refused",
                                                     "CCLC product exchange could not be qualified",
                                                     std::move(reason)));
      }
      withdrawn = std::move(product_control_->pending);
      product_control_->pending.reset();
      product_control_->exchange_pending = false;
      admission_resolution_ready_.notify_all();
      return ExchangeResult::Err(coordinator_error("cclc_product_exchange_refused",
                                                   "CCLC product exchange could not be qualified",
                                                   std::move(reason)));
    };
    bool scheduler_lane_used = false;
    {
      std::unique_lock admission_lock(admission_mutex_, std::try_to_lock);
      if (!admission_lock.owns_lock())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_owner_exclusion_busy");
      const auto refuse_locked = [&](std::string reason) -> ExchangeResult {
        withdrawn = std::move(product_control_->pending);
        product_control_->pending.reset();
        product_control_->exchange_pending = false;
        admission_resolution_ready_.notify_all();
        return ExchangeResult::Err(coordinator_error("cclc_product_exchange_refused",
                                                     "CCLC product exchange could not be qualified",
                                                     std::move(reason)));
      };
      if (!previous || !successor || previous == successor || !previous->identity ||
          !successor->identity || !previous->identity->publication.valid() ||
          !successor->identity->publication.valid() ||
          previous->identity->publication != expected_previous_publication ||
          (successor->route_services && !successor->route_services->locally_valid()) ||
          !successor->identity->publication.request.expected_previous_publication_generation ||
          *successor->identity->publication.request.expected_previous_publication_generation !=
              previous->identity->publication.publication_generation ||
          successor->identity->publication.publication_generation.value() <=
              previous->identity->publication.publication_generation.value()) {
        return refuse_locked("prepared_product_identity_invalid");
      }
      const auto& membership = successor->identity->configured_components;
      for (const auto& component : membership) {
        if (component.empty() || component.value().size() > kMaxLifecycleIdentityBytes ||
            std::count(membership.begin(), membership.end(), component) != 1)
          return refuse_locked("prepared_product_membership_invalid");
      }
      if (CurrentOwnerEpoch() != expected_owner || expected_owner.empty())
        return refuse_locked("product_owner_changed");
      if (shutting_down_.load(std::memory_order_acquire) || admission_fault_closed_ ||
          unresolved_effect_admission_gate_count_ != 0U)
        return refuse_locked("product_owner_fault_or_uncertainty");
      // Exact completed replay observes the current slot; it never repeats
      // publication, worker work or an exchange.
      if (product_control_->current == successor)
        return observation(ComponentLifecycleProductExchangePhase::kCompleted,
                           "product_exchange_completed");
      if (product_control_->current != previous)
        return refuse_locked("product_predecessor_changed");
      if (product_control_->pending && (product_control_->pending->previous != previous ||
                                        product_control_->pending->successor != successor ||
                                        product_control_->pending->owner != expected_owner))
        return refuse_locked("product_exchange_request_conflict");
      {
        std::unique_lock state_lock(state_mutex_, std::try_to_lock);
        if (!state_lock.owns_lock())
          return observation(ComponentLifecycleProductExchangePhase::kPending,
                             "product_state_exclusion_busy");
        if (!started_ || starting_ || recovery_in_progress_)
          return refuse_locked("product_owner_not_current");
      }
      if (!product_control_->pending)
        product_control_->pending.emplace(
            PendingProductExchange{previous, successor, expected_owner});
      product_control_->exchange_pending = true;
      // This is only an ingress/read-pass barrier. Accepted contexts hold
      // lifetime, not counted leases: all work/tail predicates below are also
      // required, independently of operation_pins.
      if (product_control_->operation_pins != 0U || admission_resolution_active_ ||
          admission_persistence_active_)
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_operations_pending");
      std::unique_lock state_lock(state_mutex_, std::try_to_lock);
      if (!state_lock.owns_lock())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_state_exclusion_busy");
      std::unique_lock schedule_lock(schedule_mutex_, std::try_to_lock);
      if (!schedule_lock.owns_lock())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_schedule_exclusion_busy");
      if (!active_transactions_.empty() || !active_route_transactions_.empty() ||
          !worker_pool_.IsIdle() || schedule_publications_in_flight_ != 0U)
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_work_pending");
      if (owner_wide_containment_gate_count_ != 0U || !AdmissionIsOpen())
        return refuse_locked("product_admission_not_settled");
      polls = scheduled_handles_;
      scheduler_lane_used = scheduler_lane_used_;
      try {
        previous_watermarks = component_generation_watermarks_;
      } catch (...) {
        return refuse_locked("product_generation_proof_exception");
      }
    }
    // Active sets are empty and workers/publication producers idle. Cancel
    // only leftover weak terminal polls, retaining every tracked handle until
    // the existing Lifecycle lane reports no already-dequeued callback.
    try {
      for (const auto& poll : polls) {
        if (!poll.second->load(std::memory_order_acquire)) scheduler_.cancel(poll.first);
      }
      if (scheduler_lane_used &&
          !scheduler_.drain_for(SchedulerPriority::Lifecycle, std::chrono::nanoseconds::zero()))
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_callbacks_pending");
    } catch (...) {
      return reject("product_callback_completion_unknown", true);
    }
    if (!worker_pool_.IsIdle())
      return observation(ComponentLifecycleProductExchangePhase::kPending,
                         "product_worker_tail_pending");
    Result<void> durable = Result<void>::Err(coordinator_error(
        "cclc_product_journal_quiescence_failed", "journal quiescence read did not complete",
        "journal_quiescence_exception"));
    try {
      durable = journal_.ReadQuiescentOwner(expected_owner);
    } catch (...) {
    }
    // At this point memory is settled. A durable residual claim/effect/status
    // is a blocker, not an indefinitely pending preparation; an unreadable
    // proof closes admission and retains the original current product.
    if (!durable.ok()) return reject("product_journal_not_quiescent", true);
    // Apply adoption can advance durable facts while this retained owner
    // still holds the predecessor cache. Operation pins and the settled work
    // barrier exclude the live facade writers; reads cannot fence an outside
    // uncoordinated journal writer.
    try {
      successor_watermarks = previous_watermarks;
      auto membership = successor->identity->configured_components;
      std::sort(membership.begin(), membership.end());
      using GenerationProof = std::pair<ComponentLifecycleCurrentFactReadbackDTO,
                                        std::optional<ComponentLifecycleCurrentSnapshotDTO>>;
      const auto read_component =
          [&](const ControllableComponentId& component) -> Result<GenerationProof> {
        auto fact = journal_.LoadCurrentComponentLifecycleFact(component);
        if (!fact.ok()) return Result<GenerationProof>::Err(fact.error());
        if (!fact.value().locally_valid() || fact.value().controllable_component_id != component)
          return Result<GenerationProof>::Err(coordinator_error(
              "cclc_product_exchange_refused", "current fact changed component scope",
              "product_generation_facts_mismatch"));
        std::optional<ComponentLifecycleCurrentSnapshotDTO> snapshot;
        if (fact.value().durable_generation) {
          auto current = successor->current_snapshot_source.ReadCurrentComponentSnapshot(component);
          if (!current.ok()) return Result<GenerationProof>::Err(current.error());
          if (!VerifyComponentLifecycleCurrentSnapshot(current.value()).ok() ||
              current.value().controllable_component_id != component ||
              current.value().active_publication_receipt != successor->identity->publication ||
              current.value().component_lifecycle_generation != *fact.value().durable_generation)
            return Result<GenerationProof>::Err(coordinator_error(
                "cclc_product_exchange_refused", "snapshot differs from exact successor facts",
                "product_generation_snapshot_mismatch"));
          snapshot.emplace(std::move(current.value()));
        }
        return Result<GenerationProof>::Ok({std::move(fact.value()), std::move(snapshot)});
      };
      std::vector<GenerationProof> proofs;
      proofs.reserve(membership.size());
      for (const auto& component : membership) {
        auto proof = read_component(component);
        if (!proof.ok()) return reject("product_generation_proof_failed");
        const auto& generation = proof.value().first.durable_generation;
        auto floor = std::find_if(successor_watermarks.begin(), successor_watermarks.end(),
                                  [&](const auto& row) { return row.first == component.value(); });
        if (floor != successor_watermarks.end() &&
            (!generation || generation->value() < floor->second))
          return reject("product_generation_floor_regressed");
        if (generation) {
          if (floor == successor_watermarks.end()) {
            if (successor_watermarks.size() >= kMaxCoordinatorActiveTransactions)
              return reject("product_generation_watermark_capacity_exhausted");
            successor_watermarks.emplace_back(component.value(), generation->value());
          } else {
            floor->second = generation->value();
          }
        }
        proofs.push_back(std::move(proof.value()));
      }
      for (std::size_t index = 0U; index < membership.size(); ++index) {
        auto repeated = read_component(membership[index]);
        if (!repeated.ok() || repeated.value() != proofs[index])
          return reject("product_generation_proof_changed");
      }
      durable = journal_.ReadQuiescentOwner(expected_owner);
    } catch (...) {
      return reject("product_generation_proof_exception");
    }
    if (!durable.ok()) return reject("product_journal_not_quiescent", true);
    {
      std::unique_lock admission_lock(admission_mutex_, std::try_to_lock);
      if (!admission_lock.owns_lock())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_owner_exclusion_busy");
      std::unique_lock state_lock(state_mutex_, std::try_to_lock);
      if (!state_lock.owns_lock())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_state_exclusion_busy");
      std::unique_lock schedule_lock(schedule_mutex_, std::try_to_lock);
      if (!schedule_lock.owns_lock())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_schedule_exclusion_busy");
      if (CurrentOwnerEpoch() != expected_owner || product_control_->current != previous ||
          !product_control_->pending || product_control_->pending->previous != previous ||
          product_control_->pending->successor != successor ||
          product_control_->pending->owner != expected_owner || admission_fault_closed_ ||
          shutting_down_.load(std::memory_order_acquire) || recovery_in_progress_ ||
          unresolved_effect_admission_gate_count_ != 0U ||
          owner_wide_containment_gate_count_ != 0U || !AdmissionIsOpen() ||
          component_generation_watermarks_ != previous_watermarks) {
        withdrawn = std::move(product_control_->pending);
        product_control_->pending.reset();
        product_control_->exchange_pending = false;
        admission_resolution_ready_.notify_all();
        return ExchangeResult::Err(
            coordinator_error("cclc_product_exchange_refused",
                              "product or owner changed during exchange qualification",
                              "product_exchange_recheck_failed"));
      }
      if (product_control_->operation_pins != 0U || admission_resolution_active_ ||
          admission_persistence_active_ || !active_transactions_.empty() ||
          !active_route_transactions_.empty() || schedule_publications_in_flight_ != 0U ||
          !worker_pool_.IsIdle())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_completion_recheck_pending");
      // The zero-duration drain already proved that all snapshotted callbacks
      // returned. Final publication/worker checks exclude a new producer.
      std::erase_if(scheduled_handles_, [&](const auto& item) {
        return std::any_of(polls.begin(), polls.end(),
                           [&](const auto& poll) { return item.first == poll.first; });
      });
      if (!scheduled_handles_.empty())
        return observation(ComponentLifecycleProductExchangePhase::kPending,
                           "product_new_callbacks_pending");
      retired = std::move(product_control_->current);
      component_generation_watermarks_.swap(successor_watermarks);
      product_control_->current = successor;
      withdrawn = std::move(product_control_->pending);
      product_control_->pending.reset();
      product_control_->exchange_pending = false;
      admission_resolution_ready_.notify_all();
    }
    return observation(ComponentLifecycleProductExchangePhase::kCompleted,
                       "product_exchange_completed");
  }

  [[nodiscard]] Result<void> Start() {
    auto startup_pin = AcquireProductPin();
    if (!startup_pin.ok()) return Result<void>::Err(startup_pin.error());
    const auto product_context = startup_pin.value().context_;
    if (!config_.locally_valid() || (product_context->route_services.has_value() &&
                                     !product_context->route_services->locally_valid())) {
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
                                           config_.request_membership_capacity,
                                           config_.quiescent_predecessor);
    } catch (...) {
    }
    const bool current_owner_interrupted =
        recovered.ok() &&
        std::any_of(recovered.value().interrupted_transactions.begin(),
                    recovered.value().interrupted_transactions.end(), [&](const auto& status) {
                      return status.status_owner_epoch == recovered.value().request_owner_epoch;
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
    request_epoch_.store(
        std::make_shared<const RequestOwnerEpoch>(recovered.value().request_owner_epoch),
        std::memory_order_release);
    for (auto& row : recovered.value().cell_claims) {
      Result<CellAdmissionRecoveryReceiptDTO> receipt =
          Result<CellAdmissionRecoveryReceiptDTO>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "Kernel predecessor claim receipt raised an exception", "claim_receipt_exception"));
      try {
        receipt = kernel_cell_admission_port_.GetCellAdmissionRecoveryReceipt(
            {row.intent, row.claim, std::nullopt});
      } catch (...) {
      }
      std::optional<CellAdmissionClaimDTO> recovered_claim = row.claim;
      if (!recovered_claim.has_value() && receipt.ok() &&
          receipt.value().status == CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
          receipt.value().matched_claim.has_value() &&
          receipt.value().matched_claim->intent == row.intent) {
        recovered_claim = receipt.value().matched_claim;
        if (!journal_.RecordCellClaimAcquired(CurrentOwnerEpoch(), *recovered_claim).ok()) {
          Shutdown();
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "recovered lifecycle claim could not be journaled", "claim_recovery_journal_failed"));
        }
        row.claim = recovered_claim;
      }
      if (!recovered_claim.has_value()) {
        if (!receipt.ok() ||
            receipt.value().status != CellAdmissionRecoveryReceiptStatusDTO::kUnknown ||
            !journal_.RecordCellClaimNotAcquired(CurrentOwnerEpoch(), row.intent).ok()) {
          Shutdown();
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_unknown",
              "lifecycle acquire attempt could not be proven absent", "claim_acquire_unresolved"));
        }
        continue;
      }
      if (row.transaction_accepted && !row.release_intent.has_value()) {
        if (!receipt.ok() ||
            receipt.value().status != CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
            receipt.value().matched_claim != recovered_claim) {
          Shutdown();
          return Result<void>::Err(
              coordinator_error("component_lifecycle_claim_recovery_unknown",
                                "accepted lifecycle transaction no longer has its exact held claim",
                                "accepted_claim_not_held"));
        }
        continue;
      }
      std::optional<CellAdmissionClaimReleaseDTO> recovery_release = row.release_intent;
      if (!row.transaction_accepted) {
        if (!recovery_release)
          recovery_release = CellAdmissionClaimReleaseDTO{
              row.intent.owner_role,
              row.intent.owner_epoch,
              row.intent.owner_transaction_id,
              row.intent.request_id,
              row.intent.request_payload_digest,
              recovered_claim->acquired_admission_generation,
              recovered_claim->claim_generation,
              {CellAdmissionSettlementKindDTO::kProvenNoEffect, row.intent.request_payload_digest}};
        if (!journal_.RecordUnacceptedCellClaimRelease(CurrentOwnerEpoch(), *recovery_release)
                 .ok()) {
          Shutdown();
          return Result<void>::Err(
              coordinator_error("component_lifecycle_claim_recovery_unknown",
                                "unaccepted lifecycle claim release could not be journaled",
                                "claim_release_journal_failed"));
        }
      }
      if (receipt.ok() &&
          receipt.value().status == CellAdmissionRecoveryReceiptStatusDTO::kHeldExact) {
        try {
          static_cast<void>(
              kernel_cell_admission_port_.ReleaseCellAdmissionClaim(*recovery_release));
          receipt = kernel_cell_admission_port_.GetCellAdmissionRecoveryReceipt(
              {row.intent, row.claim, std::nullopt});
        } catch (...) {
        }
      }
      if (!receipt.ok() ||
          (receipt.value().status != CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact &&
           receipt.value().status != CellAdmissionRecoveryReceiptStatusDTO::kReconciledExact) ||
          !journal_
               .RecordCellClaimReleaseReceipt(CurrentOwnerEpoch(), *recovery_release,
                                              receipt.value())
               .ok()) {
        Shutdown();
        return Result<void>::Err(coordinator_error(
            "component_lifecycle_claim_recovery_unknown",
            "predecessor lifecycle release could not be proven exact", "claim_release_unresolved"));
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
    if (!recovered.value().interrupted_route_selections.empty() &&
        !product_context->route_services.has_value()) {
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
            if (status.selected_bring_up || status.apply_bring_up || domain.current_scope.has_value()) {
              domains.push_back(domain.control_domain_id);
            }
          }
          const auto owner = reservation_owner(status.acceptance, status.accepted_intent);
          // Q98 delegated rows restore observation-only global closure;
          // their exact parent claim/fence is never copied into a child owner.
          if (status.apply_bring_up) domains.clear();
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
          auto runtime = std::make_shared<RuntimeState>(product_context, status, std::nullopt,
                                                        std::move(domains), std::nullopt);
          const auto recovered_claim =
              std::find_if(recovered.value().cell_claims.begin(),
                           recovered.value().cell_claims.end(), [&](const auto& row) {
                             return row.intent.owner_transaction_id ==
                                    status.acceptance.lifecycle_transaction_id.value();
                           });
          if (!status.apply_bring_up &&
              (recovered_claim == recovered.value().cell_claims.end() ||
               !recovered_claim->claim.has_value())) {
            Shutdown();
            return Result<void>::Err(
                coordinator_error("component_lifecycle_claim_recovery_missing",
                                  "interrupted lifecycle transaction has no exact Kernel claim",
                                  "claim_recovery_missing"));
          }
          if (!status.apply_bring_up) {
            runtime->cell_claim = recovered_claim->claim;
            runtime->cell_claim_release = recovered_claim->release_intent;
          }
          // A delegated child never reconstructs its parent's claim as an
          // independently releasable child claim. Manager owns its settlement.
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
          runtime->owner_wide_containment_gate = runtime->permanent_containment_only;
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
        bool submitted = false;
        try {
          submitted = worker_pool_.Submit(
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
      const auto restored = RestoreRouteRecovery(recovered.value(), product_context);
      if (!restored.ok()) {
        Shutdown();
        return restored;
      }
      return Result<void>::Ok();
    }
    if (recovered.value().interrupted_transactions.empty()) {
      if (!ReconcileLifecycleOwnerEpoch(CurrentOwnerEpoch())) {
        Shutdown();
        return Result<void>::Err(
            coordinator_error("component_lifecycle_owner_epoch_reconciliation_failed",
                              "Kernel lifecycle-owner epoch could not be rotated",
                              "owner_epoch_reconciliation_failed"));
      }
      accepting_.store(true, std::memory_order_release);
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleProductPin* product_pin = nullptr,
      const ComponentLifecycleTargetExpectationDTO* expected_target = nullptr) {
    return ResolveAdmission(
        [&] { return SubmitLifecycleResolved(intent, product_pin, expected_target); });
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycleResolved(
      const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleProductPin* product_pin,
      const ComponentLifecycleTargetExpectationDTO* expected_target) {
    bool known_unstarted = false;
    auto result = SubmitLifecycleAttempt(intent, known_unstarted, product_pin, expected_target);
    if (!result.ok() && known_unstarted) {
      // The same gate covers preflight, acceptance and rejection: neither
      // a duplicate nor a namespace checkpoint can cross this decision.
      auto error = result.error();
      error.details["effect"] = "not_started";
      auto retained =
          journal_.RecordLifecycleRejection(CurrentOwnerEpoch(), intent, error, std::nullopt);
      result = retained.ok() ? ReplayResult(retained.value()) : retained;
    }
    if (!result.ok() && result.error().code == "request_ledger_full") {
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          CheckpointFullRequestNamespace(intent.request_context.request_owner_epoch));
    }
    return result;
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycleAttempt(
      const ComponentLifecycleIntentDTO& intent, bool& known_unstarted,
      const ComponentLifecycleProductPin* product_pin,
      const ComponentLifecycleTargetExpectationDTO* expected_target) {
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
      inspected = journal_.InspectLifecycleRequest(CurrentOwnerEpoch(), intent);
    } catch (...) {
    }
    if (!inspected.ok()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(inspected.error());
    }
    if (inspected.value().has_value()) {
      return ReplayResult(*inspected.value());
    }
    if (intent.close_incident_recovery)
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_close_incident_mismatch",
          "Close incident recovery requires the exact open incident journal path",
          "close_incident_mismatch"));
    // Replay is a journal read. A transient exchange refusal must not bind an
    // unstarted request to a durable rejection, and an existing whole-pass pin
    // remains usable while new pins are excluded.
    std::optional<ComponentLifecycleProductPin> owned_pin;
    if (product_pin == nullptr) {
      auto acquired = AcquireProductPin();
      if (!acquired.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(acquired.error());
      owned_pin.emplace(std::move(acquired.value()));
      product_pin = &*owned_pin;
    }
    const auto context = ReadPinnedProductContext(*product_pin);
    if (!context.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(context.error());
    // Exact journal replay precedes this private fresh-target expectation.
    if (expected_target != nullptr &&
        (!expected_target->locally_valid() ||
         expected_target->controllable_component_id != intent.controllable_component_id)) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          coordinator_error("component_lifecycle_target_expectation_invalid",
                            "the private target expectation does not match the "
                            "lifecycle component",
                            "target_expectation_invalid"));
    }
    known_unstarted = true;
    std::uint64_t admission_gate_generation = 0U;
    {
      std::lock_guard lock(admission_mutex_);
      if (!AdmissionIsOpen()) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_admission_closed",
                              "the CCLC coordinator is not accepting new lifecycle work",
                              "coordinator_not_accepting"));
      }
      admission_gate_generation = admission_gate_generation_;
    }
    if (!AdmissionIsOpen()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_admission_closed",
          "the CCLC coordinator is not accepting new lifecycle work", "coordinator_not_accepting"));
    }

    for (std::size_t attempt = 0U; attempt < kMaximumAdmissionResolutionAttempts; ++attempt) {
      const auto facts = ReadAdmissionFacts(intent, context.value());
      if (!facts.ok()) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(facts.error());
      }
      const auto admitted = CommitAdmission(intent, facts.value(), admission_gate_generation,
                                            known_unstarted, expected_target);
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

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) {
    if (!intent.locally_valid()) {
      return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_intent_invalid",
                            "lifecycle result query is invalid", "intent_invalid"));
    }
    try {
      return ResolveAdmission([&] {
        auto result = journal_.ReadLifecycleRequestResult(intent);
        if (result.ok() && result.value() &&
            result.value()->replay_status == RequestReplayStatusDTO::kRequestLedgerFull) {
          return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
              UnstartedCapacityDeferral());
        }
        return result;
      });
    } catch (...) {
      return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_journal_inspection_failed",
                            "lifecycle result lookup failed", "journal_exception"));
    }
  }

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadSelectedComponentBringUpRequestResult(const ComponentLifecycleIntentDTO& intent,
                                            const ContentDigest& stable_digest) {
    if (!intent.locally_valid() || !IsExactLowercaseSha256Digest(stable_digest.value())) {
      return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_intent_invalid",
                            "selected bring-up result query is invalid", "intent_invalid"));
    }
    try {
      return journal_.ReadSelectedComponentBringUpRequestResult(intent, stable_digest);
    } catch (...) {
      return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_journal_inspection_failed",
                            "selected bring-up result lookup failed", "journal_exception"));
    }
  }

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadApplyComponentBringUpRequestResult(const ComponentLifecycleIntentDTO& intent,
                                         const ContentDigest& stable_digest) {
    if (!intent.locally_valid() || !IsExactLowercaseSha256Digest(stable_digest.value())) {
      return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_intent_invalid",
                            "delegated Apply result query is invalid", "intent_invalid"));
    }
    try {
      return journal_.ReadApplyComponentBringUpRequestResult(intent, stable_digest);
    } catch (...) {
      return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_journal_inspection_failed",
                            "delegated Apply result lookup failed", "journal_exception"));
    }
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptApplyComponentBringUp(
      const ComponentLifecycleIntentDTO& intent, const ApplyComponentBringUpEvidenceDTO& evidence,
      const std::optional<ComponentLifecycleCurrentSnapshotDTO>& predecessor,
      const std::vector<BrandLifecyclePlanCatalogDTO>& catalogs,
      const std::vector<ResolvedLifecycleControlDomainScopeDTO>& domains,
      const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& providers,
      ComponentLifecycleCoordinatorApplyBringUpServices services) {
    return ResolveAdmission([&]() -> Result<CCLCLifecycleRequestResultDTO> {
      try {
        // Retained results do not need current authority or installation.
        const auto replay = ReadApplyComponentBringUpRequestResult(
            intent, evidence.original_request_payload_digest);
        if (!replay.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(replay.error());
        if (replay.value()) return ReplayResult(*replay.value());
        const auto inspected = journal_.InspectApplyComponentBringUpRequest(
            CurrentOwnerEpoch(), intent, evidence.original_request_payload_digest);
        if (!inspected.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(inspected.error());
        if (inspected.value()) return ReplayResult(*inspected.value());
        auto acquired = AcquireProductPin();
        if (!acquired.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(acquired.error());
        const auto context = ReadPinnedProductContext(acquired.value());
        if (!context.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(context.error());
        if (!services.with_original_live_parent || !services.active_publication_reader ||
            !services.expected_publication.valid() ||
            !services.expected_previous_publication.valid() ||
            !services.expected_publication.expected_previous_publication_generation ||
            *services.expected_publication.expected_previous_publication_generation !=
                services.expected_previous_publication.publication_generation ||
            services.expected_previous_publication.publication_generation.value() ==
                std::numeric_limits<std::uint64_t>::max() ||
            services.expected_publication.candidate_projection_revision !=
                evidence.candidate_projection_revision ||
            !std::holds_alternative<NamedApplyPublicationTransactionDTO>(
                services.expected_publication.transaction_ref)) {
          return Result<CCLCLifecycleRequestResultDTO>::Err(ApplyError("services_invalid"));
        }
        const auto& publication_parent = std::get<NamedApplyPublicationTransactionDTO>(
            services.expected_publication.transaction_ref);
        if (publication_parent.apply_id != evidence.parent_authority.parent.owner.apply_id ||
            publication_parent.apply_generation !=
                evidence.parent_authority.parent.owner.apply_generation ||
            (predecessor &&
             predecessor->active_publication_receipt != services.expected_previous_publication))
          return Result<CCLCLifecycleRequestResultDTO>::Err(
              ApplyError("publication_parent_mismatch"));
        // The immutable request contains the original owner's observation,
        // never a caller-selected first seed. Read outside coordinator locks;
        // the journal repeats this full W/R selection inside acceptance CAS.
        std::optional<ApplyComponentQualifiedPredecessorDTO> qualified_predecessor;
        if (predecessor) {
          if (!VerifyComponentLifecycleCurrentSnapshot(*predecessor).ok())
            return Result<CCLCLifecycleRequestResultDTO>::Err(
                ApplyError("predecessor_snapshot_invalid"));
          qualified_predecessor = ApplyComponentQualifiedPredecessorDTO{
              predecessor->component_lifecycle_generation, predecessor->snapshot_digest};
        }
        const auto selected_generation = journal_.ReadApplyComponentGenerationSelection(
            CurrentOwnerEpoch(), intent.controllable_component_id, qualified_predecessor);
        if (!selected_generation.ok())
          return Result<CCLCLifecycleRequestResultDTO>::Err(selected_generation.error());
        if (!intent.apply_generation_selection ||
            *intent.apply_generation_selection != selected_generation.value())
          return Result<CCLCLifecycleRequestResultDTO>::Err(
              ApplyError("component_generation_selection_changed"));
        std::uint64_t next_generation = 0U;
        std::uint64_t gate = 0U;
        bool new_watermark = false;
        {
          std::lock_guard lock(admission_mutex_);
          if (!AdmissionIsOpen() ||
              intent.request_context.request_owner_epoch != CurrentOwnerEpoch())
            return Result<CCLCLifecycleRequestResultDTO>::Err(
                ApplyError("original_owner_required"));
          if (highest_transaction_generation_ == std::numeric_limits<std::uint64_t>::max())
            return Result<CCLCLifecycleRequestResultDTO>::Err(
                ApplyError("transaction_generation_overflow"));
          next_generation = highest_transaction_generation_ + 1U;
          gate = admission_gate_generation_;
          const auto watermark =
              std::find_if(component_generation_watermarks_.begin(),
                           component_generation_watermarks_.end(), [&](const auto& row) {
                             return row.first == intent.controllable_component_id.value();
                           });
          new_watermark = watermark == component_generation_watermarks_.end();
          if ((!new_watermark &&
               (!selected_generation.value().predecessor_generation ||
                watermark->second !=
                    selected_generation.value().predecessor_generation->value())) ||
              (new_watermark && selected_generation.value().predecessor_generation) ||
              (new_watermark &&
               component_generation_watermarks_.size() >= kMaxCoordinatorActiveTransactions))
            return Result<CCLCLifecycleRequestResultDTO>::Err(
                ApplyError("component_watermark_mismatch"));
        }
        {
          std::lock_guard lock(state_mutex_);
          if (!started_ || active_transactions_.size() + active_route_transactions_.size() >=
                               kMaxCoordinatorActiveTransactions)
            return Result<CCLCLifecycleRequestResultDTO>::Err(
                ApplyError("runtime_capacity_unavailable"));
        }
        const ComponentLifecycleAdmissionCandidate candidate{
            LifecycleTransactionId("apply-lifecycle." + std::to_string(next_generation)),
            LifecycleTransactionGeneration(next_generation),
            intent.expected_component_lifecycle_generation,
            clock_.now(),
            predecessor,
            catalogs,
            selected_generation.value()};
        const auto prepared =
            PrepareApplyComponentBringUpAdmission(intent, candidate, evidence, domains, providers);
        if (!prepared.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(prepared.error());
        auto runtime =
            std::make_shared<RuntimeState>(context.value(), prepared.value().initial_status,
                                           prepared.value().qualified_lifecycle_plan_catalogs,
                                           std::vector<ControlDomainId>{}, predecessor);
        runtime->permanent_containment_only = false;
        runtime->apply_services = std::move(services);
        runtime->apply_candidate_domains = domains;
        runtime->apply_candidate_providers = providers;
        runtime->apply_domain_catalogs.resize(runtime->status.domains.size());
        std::pair<std::string, std::uint64_t> new_row{
            intent.controllable_component_id.value(),
            prepared.value().acceptance.component_lifecycle_generation.value()};
        // Q98: the verified original parent owns cell/scope exclusion.
        // A delegated child adds no conflicting domain reservation owner.
        bool durable = false;
        bool persistence_invoked = false;
        Result<CCLCLifecycleRequestResultDTO> accepted =
            Result<CCLCLifecycleRequestResultDTO>::Err(ApplyError("acceptance_not_invoked"));
        const auto pinned = WithApplyParent(runtime, [&]() -> Result<void> {
          const auto head =
              runtime->apply_services->active_publication_reader->ReadActiveBundlePublication(
                  runtime->apply_services->expected_previous_publication.publication_generation);
          if (!head.ok()) return Result<void>::Err(head.error());
          if (head.value() != runtime->apply_services->expected_previous_publication)
            return Result<void>::Err(ApplyError("previous_publication_head_mismatch"));
          if (qualified_predecessor) {
            // The original parent pin spans this fresh owner read through
            // journal acceptance. A sealed numeric predecessor is not proof.
            const auto actual =
                context.value()->current_snapshot_source.ReadCurrentComponentSnapshot(
                    intent.controllable_component_id);
            if (!actual.ok()) return Result<void>::Err(actual.error());
            if (!predecessor || actual.value() != *predecessor ||
                actual.value().component_lifecycle_generation !=
                    qualified_predecessor->generation ||
                actual.value().snapshot_digest != qualified_predecessor->snapshot_digest)
              return Result<void>::Err(ApplyError("predecessor_snapshot_changed"));
          } else {
            // A present real snapshot cannot be erased to force the initial
            // absence grammar. Read failure itself grants no absence proof;
            // genuine absence remains part of the original host parent join.
            const auto actual =
                context.value()->current_snapshot_source.ReadCurrentComponentSnapshot(
                    intent.controllable_component_id);
            if (actual.ok())
              return Result<void>::Err(ApplyError("predecessor_snapshot_missing"));
          }
          {
            std::lock_guard lock(admission_mutex_);
            if (!AdmissionIsOpen() || gate != admission_gate_generation_)
              return Result<void>::Err(ApplyError("admission_gate_changed"));
            admission_persistence_active_ = true;
          }
          ScopeExit persistence_finished([&] {
            std::lock_guard lock(admission_mutex_);
            admission_persistence_active_ = false;
            admission_resolution_ready_.notify_all();
          });
          persistence_invoked = true;
          accepted = journal_.AcceptOrReplayApplyComponentBringUp(
              CurrentOwnerEpoch(), intent, prepared.value().acceptance,
              prepared.value().initial_status, evidence.original_request_payload_digest);
          durable = accepted.ok() &&
                    accepted.value().replay_status == RequestReplayStatusDTO::kAcceptedNew;
          if (!accepted.ok()) return Result<void>::Err(accepted.error());
          return Result<void>::Ok();
        });
        if (!durable) {
          if (persistence_invoked && !accepted.ok()) CloseAdmission();
          if (!pinned.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(pinned.error());
          return accepted.ok() ? ReplayResult(accepted.value()) : accepted;
        }
        if (!accepted.value().locally_valid() ||
            accepted.value().acceptance != std::optional(prepared.value().acceptance) ||
            accepted.value().status != std::optional(prepared.value().initial_status)) {
          CloseAdmission();
          return Result<CCLCLifecycleRequestResultDTO>::Err(
              ApplyError("accepted_candidate_mismatch"));
        }
        {
          std::lock_guard lock(admission_mutex_);
          highest_transaction_generation_ = next_generation;
          if (new_watermark)
            component_generation_watermarks_.push_back(std::move(new_row));
          else
            for (auto& row : component_generation_watermarks_)
              if (row.first == intent.controllable_component_id.value())
                row.second = prepared.value().acceptance.component_lifecycle_generation.value();
        }
        {
          std::lock_guard lock(state_mutex_);
          active_transactions_.push_back(runtime);
        }
        if (!pinned.ok()) {
          runtime->apply_stopped = true;
          CloseAdmission();
        }
        // No worker submission: real installation must arrive via Continue.
        return accepted;
      } catch (...) {
        CloseAdmission();
        return Result<CCLCLifecycleRequestResultDTO>::Err(ApplyError("acceptance_exception"));
      }
    });
  }

  [[nodiscard]] Result<void> ValidateInstalledApplyCandidate(
      const std::shared_ptr<RuntimeState>& runtime, const ComponentLifecycleIntentDTO& intent,
      const ContentDigest& digest, const ComponentLifecycleStatusDTO& retained_status,
      const std::vector<ResolvedLifecycleControlDomainScopeDTO>& domains,
      const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& providers,
      const std::vector<BrandLifecyclePortRegistrationDTO>& registrations) const {
    if (!retained_status.locally_valid() || !retained_status.apply_bring_up ||
        retained_status.acceptance != runtime->original_acceptance ||
        !runtime->original_apply_parent ||
        retained_status.apply_bring_up->parent_authority != *runtime->original_apply_parent)
      return Result<void>::Err(ApplyError("original_runtime_required"));
    if (retained_status.accepted_intent != intent ||
        retained_status.apply_bring_up->original_request_payload_digest != digest ||
        domains.size() != runtime->apply_candidate_domains.size() ||
        providers != runtime->apply_candidate_providers || registrations.size() != providers.size())
      return Result<void>::Err(ApplyError("installed_candidate_mismatch"));
    for (std::size_t index = 0U; index < domains.size(); ++index) {
      auto qualified = runtime->apply_candidate_domains[index];
      const bool same_configuration = std::visit(
          [&](auto& before, const auto& installed) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(before)>,
                                          std::decay_t<decltype(installed)>>) {
              return false;
            } else {
              if (before.active_control_configuration &&
                  before.active_control_configuration != installed.active_control_configuration)
                return false;
              before.active_control_configuration = installed.active_control_configuration;
              return true;
            }
          },
          qualified.payload, domains[index].payload);
      if (!same_configuration)
        return Result<void>::Err(ApplyError("installed_configuration_mismatch"));
      // Configuration can now exist because this is actual installation;
      // every other qualified scope fact must remain exact.
      if (!domains[index].locally_valid() || qualified != domains[index] ||
          registrations[index].provider_occurrence != providers[index] ||
          !registrations[index].port || !providers[index].locally_valid() ||
          providers[index].control_domain_id != domains[index].control_domain_id ||
          providers[index].control_domain_revision != domains[index].control_domain_revision)
        return Result<void>::Err(ApplyError("installed_registration_mismatch"));
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] Result<InstalledProviderCandidateFactsDTO> ReadInstalledApplyCandidateFacts(
      const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest,
      const std::vector<ResolvedLifecycleControlDomainScopeDTO>& domains,
      const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& providers,
      const std::vector<BrandLifecyclePortRegistrationDTO>& registrations) {
    using Reply = Result<InstalledProviderCandidateFactsDTO>;
    try {
      const auto owner = CurrentOwnerEpoch();
      const auto retained = ReadApplyComponentBringUpRequestResult(intent, digest);
      if (!retained.ok()) return Reply::Err(retained.error());
      if (!retained.value() || !retained.value()->status)
        return Reply::Err(ApplyError("accepted_runtime_absent"));
      const auto& status = *retained.value()->status;
      std::shared_ptr<RuntimeState> runtime;
      {
        std::lock_guard lock(state_mutex_);
        const auto found = std::find_if(
            active_transactions_.begin(), active_transactions_.end(),
            [&](const auto& row) { return row->original_acceptance == status.acceptance; });
        if (found != active_transactions_.end()) runtime = *found;
      }
      // Accepted candidate vectors/services are immutable after insertion.
      // Do not read mutable RuntimeState::status or enter ResolveAdmission:
      // the caller already holds the original host installation guard.
      if (!runtime || !runtime->apply_services || !runtime->original_apply_parent ||
          !AdmissionIsOpen() || shutting_down_.load(std::memory_order_acquire) ||
          runtime->apply_continued.load(std::memory_order_acquire) ||
          runtime->apply_stopped.load(std::memory_order_acquire) ||
          intent.request_context.request_owner_epoch != owner ||
          status.status_owner_epoch != owner ||
          status.phase != ComponentLifecycleTransactionPhaseDTO::kAccepted ||
          status.reconciliation_required || status.error)
        return Reply::Err(ApplyError("original_runtime_required"));
      const auto valid = ValidateInstalledApplyCandidate(runtime, intent, digest, status, domains,
                                                         providers, registrations);
      if (!valid.ok()) return Reply::Err(valid.error());
      const auto& publication = runtime->apply_services->expected_publication;
      InstalledProviderCandidateFactsDTO facts{
          publication.candidate_retained_bundle_id,
          publication.candidate_graph_revision,
          intent.controllable_component_id,
          runtime->original_acceptance.component_lifecycle_generation,
          domains,
          providers,
          InstalledApplyChildCandidateSourceDTO{owner, runtime->original_apply_parent->parent,
                                                runtime->original_acceptance, digest}};
      const auto after = ReadApplyComponentBringUpRequestResult(intent, digest);
      if (!after.ok()) return Reply::Err(after.error());
      if (!after.value() || after.value()->status != retained.value()->status ||
          CurrentOwnerEpoch() != owner || !AdmissionIsOpen() ||
          shutting_down_.load(std::memory_order_acquire) ||
          runtime->apply_continued.load(std::memory_order_acquire) ||
          runtime->apply_stopped.load(std::memory_order_acquire))
        return Reply::Err(ApplyError("original_runtime_changed"));
      return Reply::Ok(std::move(facts));
    } catch (...) {
      return Reply::Err(ApplyError("candidate_fact_source_failed"));
    }
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> ContinueApplyComponentBringUp(
      const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest,
      const std::vector<ResolvedLifecycleControlDomainScopeDTO>& domains,
      const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& providers,
      const std::vector<BrandLifecyclePortRegistrationDTO>& registrations,
      const std::vector<ControlDomainId>& retired,
      ICCLCComponentCommandReadinessPort* installed_readiness_port) {
    return ResolveAdmission([&]() -> Result<CCLCLifecycleRequestResultDTO> {
      std::shared_ptr<RuntimeState> runtime;
      try {
        const auto retained = ReadApplyComponentBringUpRequestResult(intent, digest);
        if (!retained.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(retained.error());
        if (!retained.value() || !retained.value()->status)
          return Result<CCLCLifecycleRequestResultDTO>::Err(ApplyError("accepted_runtime_absent"));
        {
          std::lock_guard lock(state_mutex_);
          const auto match = std::find_if(
              active_transactions_.begin(), active_transactions_.end(), [&](const auto& value) {
                // Readback marks its returned acceptance as exact replay; the
                // stored status retains the original immutable acceptance.
                return value->original_acceptance == retained.value()->status->acceptance;
              });
          if (match != active_transactions_.end()) runtime = *match;
        }
        if (!runtime || !runtime->apply_services || runtime->recovering ||
            intent.request_context.request_owner_epoch != CurrentOwnerEpoch())
          return Result<CCLCLifecycleRequestResultDTO>::Err(
              ApplyError("original_runtime_required"));
        if (runtime->apply_continued || runtime->apply_stopped)
          return Result<CCLCLifecycleRequestResultDTO>::Err(
              ApplyError("continuation_already_consumed"));
        const auto continued = WithApplyParent(runtime, [&]() -> Result<void> {
          auto* readiness_port = installed_readiness_port != nullptr
                                     ? installed_readiness_port
                                     : runtime->product_context->component_command_readiness_port;
          if (readiness_port == nullptr)
            return Result<void>::Err(ApplyError("installed_readiness_port_absent"));
          {
            std::lock_guard lock(admission_mutex_);
            if (!AdmissionIsOpen()) return Result<void>::Err(ApplyError("admission_closed"));
          }
          std::vector<std::shared_ptr<RuntimeState>> siblings;
          {
            std::lock_guard lock(state_mutex_);
            siblings = active_transactions_;
          }
          // This check runs inside the same original-parent pin that spans
          // installation observation and scheduling. Positive Holding is
          // published only after actual postconditions/readiness and journal
          // persistence; a callback mutex alone does not serialize effects.
          for (const auto& sibling : siblings) {
            if (sibling == runtime || !sibling->original_apply_parent) continue;
            if (sibling->original_apply_parent != runtime->original_apply_parent)
              return Result<void>::Err(ApplyError("foreign_apply_runtime_active"));
            if (sibling->original_acceptance.lifecycle_transaction_generation.value() <
                    runtime->original_acceptance.lifecycle_transaction_generation.value() &&
                (!sibling->apply_holding.load(std::memory_order_acquire) ||
                 sibling->apply_stopped.load(std::memory_order_acquire)))
              return Result<void>::Err(ApplyError("previous_apply_child_not_holding"));
          }
          const auto installation =
              ValidateInstalledApplyCandidate(runtime, intent, digest, *retained.value()->status,
                                              domains, providers, registrations);
          if (!installation.ok()) return installation;
          std::vector<ControlDomainId> expected_retired;
          for (const auto& domain : runtime->status.domains)
            if (!domain.expected_domain) expected_retired.push_back(domain.control_domain_id);
          if (retired != expected_retired)
            return Result<void>::Err(ApplyError("retired_installation_mismatch"));
          // The original-live parent pins this installed port through provider
          // work and Holding publication. It is never used by ordinary requests.
          runtime->apply_readiness_port = readiness_port;
          auto installed = runtime->status;
          AdvanceStatus(runtime, installed, ComponentLifecycleTransactionPhaseDTO::kVerifying);
          std::size_t candidate_index = 0U;
          for (std::size_t index = 0U; index < installed.domains.size(); ++index) {
            auto& domain = installed.domains[index];
            if (!domain.expected_domain) {
              domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
              continue;
            }
            domain.current_scope = domains[candidate_index];
            domain.resolved_provider_occurrence = providers[candidate_index];
            domain.action_state = ComponentLifecycleDomainActionStateDTO::kReserved;
            runtime->ports[index] = registrations[candidate_index].port;
            runtime->apply_domain_catalogs[index] =
                (*runtime->qualified_catalogs)[candidate_index++];
          }
          // Consume only after exact installation joins. Observation is already
          // outside the no-effect admission phase; a lost/late result retains
          // the parent and reservations and cannot be retried through Continue.
          runtime->apply_continued = true;
          const auto initialized = InitializeApply(runtime, installed);
          if (!initialized.ok()) return initialized;
          if (!installed.locally_valid())
            return Result<void>::Err(ApplyError("installed_status_invalid"));
          const auto recorded = journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), installed);
          if (!recorded.ok()) return Result<void>::Err(recorded.error());
          runtime->status = std::move(installed);
          runtime->initialized = true;
          runtime->command_eligibility_closed = true;
          return Result<void>::Ok();
        });
        if (!continued.ok()) {
          if (runtime->apply_continued) StopApply(runtime, continued.error());
          return Result<CCLCLifecycleRequestResultDTO>::Err(continued.error());
        }
        const auto answer = journal_.ReadApplyComponentBringUpRequestResult(intent, digest);
        if (!worker_pool_.Submit(
                [this, runtime](std::stop_token cancellation) { Process(runtime, cancellation); }))
          StopApply(runtime, ApplyError("worker_submission_failed"));
        if (!answer.ok()) return Result<CCLCLifecycleRequestResultDTO>::Err(answer.error());
        return answer.value() ? ReplayResult(*answer.value())
                              : Result<CCLCLifecycleRequestResultDTO>::Err(
                                    ApplyError("accepted_runtime_absent"));
      } catch (...) {
        if (runtime && runtime->apply_continued)
          StopApply(runtime, ApplyError("continuation_exception"));
        else
          CloseAdmission();
        return Result<CCLCLifecycleRequestResultDTO>::Err(ApplyError("continuation_exception"));
      }
    });
  }

  [[nodiscard]] static ErrorDTO ApplyError(std::string reason) {
    return coordinator_error("component_lifecycle_apply_bring_up_refused",
                             "the original delegated Apply continuation could not be proven",
                             std::move(reason));
  }

  [[nodiscard]] Result<void> WithApplyParent(const std::shared_ptr<RuntimeState>& runtime,
                                             const std::function<Result<void>()>& action) {
    if (!runtime->apply_services || !runtime->apply_services->with_original_live_parent ||
        runtime->recovering || runtime->apply_stopped ||
        runtime->status.accepted_intent.request_context.request_owner_epoch !=
            CurrentOwnerEpoch() ||
        runtime->status.status_owner_epoch != CurrentOwnerEpoch())
      return Result<void>::Err(ApplyError("original_live_parent_required"));
    const auto pinned_status = runtime->status;
    bool invoked = false;
    bool repeated = false;
    Result<void> result = Result<void>::Err(ApplyError("live_parent_action_not_invoked"));
    try {
      const auto held = runtime->apply_services->with_original_live_parent(pinned_status, [&] {
        if (std::exchange(invoked, true)) {
          repeated = true;
          return Result<void>::Err(ApplyError("live_parent_action_repeated"));
        }
        const auto& claim = pinned_status.apply_bring_up->parent_authority.parent_claim;
        const auto receipt = kernel_cell_admission_port_.GetCellAdmissionRecoveryReceipt(
            {claim.intent, claim, std::nullopt});
        if (!receipt.ok()) return result = Result<void>::Err(receipt.error());
        if (!ValidateCellAdmissionRecoveryReceipt(receipt.value()).ok() ||
            receipt.value().status != CellAdmissionRecoveryReceiptStatusDTO::kHeldExact ||
            receipt.value().matched_claim != std::optional(claim))
          return result = Result<void>::Err(ApplyError("parent_claim_not_held_exact"));
        return result = action();
      });
      if (!held.ok()) return held;
      if (repeated) return Result<void>::Err(ApplyError("live_parent_action_repeated"));
      return result;
    } catch (...) {
      return Result<void>::Err(ApplyError("live_parent_exception"));
    }
  }

  static void UpdateApplyInitialAggregate(ComponentLifecycleStatusDTO& status) {
    if (!status.apply_bring_up || status.apply_bring_up->recovery ||
        status.apply_bring_up->predecessor_occurrence ||
        status.apply_bring_up->successor_occurrence_floor ||
        std::any_of(status.apply_bring_up->selected_scope.domains.begin(),
                    status.apply_bring_up->selected_scope.domains.end(), [](const auto& domain) {
                      return domain.predecessor_control_domain_revision.has_value();
                    }))
      return;
    status.previous_state.reset();
    const auto observed = status.domains.front().current_state;
    status.current_state =
        std::all_of(status.domains.begin(), status.domains.end(),
                    [&](const auto& domain) { return domain.current_state == observed; })
            ? observed
            : std::nullopt;
  }

  [[nodiscard]] Result<void> InitializeApply(const std::shared_ptr<RuntimeState>& runtime,
                                             ComponentLifecycleStatusDTO& installed) {
    for (std::size_t index = 0U; index < installed.domains.size(); ++index) {
      auto& domain = installed.domains[index];
      if (!domain.expected_domain) continue;
      if (RemainingDuration(runtime, clock_.now()) == 0)
        return Result<void>::Err(ApplyError("installed_observation_deadline_expired"));
      const auto& catalog = *runtime->apply_domain_catalogs[index];
      const auto& provider = *domain.resolved_provider_occurrence;
      if (!VerifyBrandLifecyclePlanCatalog(catalog).ok() ||
          catalog.provider_manifest_id != provider.provider_manifest_id ||
          catalog.provider_manifest_revision != provider.provider_manifest_revision ||
          catalog.catalog_digest != provider.lifecycle_plan_catalog_digest)
        return Result<void>::Err(ApplyError("installed_catalog_mismatch"));
      const auto observed = runtime->ports[index]->Observe(ObservationQuery(installed, domain));
      if (!FreshObservation(installed, domain, observed, clock_.now()) ||
          RemainingDuration(runtime, clock_.now()) == 0)
        return Result<void>::Err(observed.ok() ? ApplyError("installed_observation_not_fresh")
                                               : observed.error());
      runtime->observations[index] = observed.value();
      if (!domain.previous_state) domain.previous_state = observed.value().observed_state;
      domain.current_state = observed.value().observed_state;
      const auto decision =
          EvaluateLifecycleDomainPlan(installed.accepted_intent.intent,
                                      *installed.requested_state.state, catalog, observed.value());
      if (!decision.ok()) return Result<void>::Err(decision.error());
      runtime->decisions[index] = decision.value();
      if (decision.value().kind == LifecycleDomainPlanDecisionKind::kAlreadySatisfied) {
        domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
        domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
      }
    }
    UpdateApplyInitialAggregate(installed);
    runtime->current_domain_index = 0U;
    while (runtime->current_domain_index < installed.domains.size() &&
           (!installed.domains[runtime->current_domain_index].expected_domain ||
            installed.domains[runtime->current_domain_index].action_state ==
                ComponentLifecycleDomainActionStateDTO::kSucceeded))
      ++runtime->current_domain_index;
    return Result<void>::Ok();
  }

  void StopApply(const std::shared_ptr<RuntimeState>& runtime, const ErrorDTO& error) {
    if (runtime->apply_stopped || terminal(runtime->status)) return;
    runtime->apply_stopped = true;
    auto status = runtime->status;
    AdvanceStatus(runtime, status, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
    status.reconciliation_required = true;
    status.terminal_result.reset();
    status.error = error;
    if (status.locally_valid() && journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), status).ok())
      runtime->status = std::move(status);
    CloseAdmission();
    // No polling can resume effects after this refusal. Replacement may only
    // expose/reconcile the retained row; Manager retains its parent authority.
  }

  void SucceedApply(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->apply_stopped) return;
    if (runtime->apply_readiness_port == nullptr) {
      StopApply(runtime, ApplyError("installed_readiness_port_absent"));
      return;
    }
    if (RemainingDuration(runtime, clock_.now()) == 0) {
      StopApply(runtime, ApplyError("parent_publication_deadline_expired"));
      return;
    }
    if (!runtime->apply_holding) {
      const ComponentCommandReadinessPublicationDTO publication{
          ComponentReadinessAuthorityDTO::kParentApplyFence,
          std::nullopt,
          runtime->status.apply_bring_up->parent_authority.held_fence_query.configuration_fence_id,
          runtime->status.accepted_intent.controllable_component_id,
          runtime->status.acceptance.component_lifecycle_generation,
          false,
          runtime->status.apply_bring_up->parent_authority.held_fence_query.expected_scope};
      const auto readback =
          runtime->apply_readiness_port->PublishComponentCommandReadiness(publication);
      if (!readback.ok() || readback.value().command_eligible ||
          readback.value().controllable_component_id != publication.controllable_component_id ||
          readback.value().component_lifecycle_generation !=
              publication.component_lifecycle_generation) {
        StopApply(runtime,
                  readback.ok() ? ApplyError("holding_readiness_mismatch") : readback.error());
        return;
      }
      auto holding = runtime->status;
      AdvanceStatus(runtime, holding, ComponentLifecycleTransactionPhaseDTO::kVerifying);
      holding.current_state = holding.requested_state.state;
      if (!holding.locally_valid() ||
          !journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), holding).ok()) {
        StopApply(runtime, ApplyError("holding_journal_failed"));
        return;
      }
      runtime->status = std::move(holding);
      runtime->apply_holding = true;
    }
    const auto& services = *runtime->apply_services;
    const auto head = services.active_publication_reader->ReadActiveBundlePublication(
        ActiveBundlePublicationGeneration(
            services.expected_previous_publication.publication_generation.value() + 1U));
    if (!head.ok()) {
      SchedulePoll(runtime);
      return;
    }
    if (!head.value().valid() || head.value().request != services.expected_publication) {
      StopApply(runtime, ApplyError("parent_publication_head_mismatch"));
      return;
    }
    if (RemainingDuration(runtime, clock_.now()) == 0) {
      StopApply(runtime, ApplyError("parent_publication_evidence_late"));
      return;
    }
    auto succeeded = runtime->status;
    AdvanceStatus(runtime, succeeded, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
    succeeded.current_state = succeeded.requested_state.state;
    succeeded.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
    succeeded.remaining_duration_ns = 0;
    succeeded.error.reset();
    succeeded.reconciliation_required = false;
    if (!succeeded.locally_valid() ||
        !journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), succeeded).ok()) {
      StopApply(runtime, ApplyError("terminal_journal_failed"));
      return;
    }
    runtime->status = std::move(succeeded);
    Complete(runtime);
    // Parent claim/fence are exclusively Manager-owned; no child release.
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

  [[nodiscard]] Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO& intent) {
    if (!intent.locally_valid()) {
      return Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(coordinator_error(
          "route_selection_intent_invalid", "route result query is invalid", "intent_invalid"));
    }
    try {
      return ResolveAdmission([&] {
        auto result = journal_.ReadRouteSelectionRequestResult(intent);
        if (result.ok() && result.value() &&
            result.value()->replay_status == RequestReplayStatusDTO::kRequestLedgerFull) {
          return Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(
              UnstartedCapacityDeferral());
        }
        return result;
      });
    } catch (...) {
      return Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(
          coordinator_error("component_lifecycle_journal_inspection_failed",
                            "route result lookup failed", "journal_exception"));
    }
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent) {
    return ResolveAdmission([&] { return SubmitRouteSelectionResolved(intent); });
  }

  [[nodiscard]] Result<
      std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>>
  ObserveRouteConfiguration(
      const RetainedBundleId& retained_bundle_id, const GraphRevision& graph_revision,
      const DesiredControlConfigurationDTO& desired_configuration,
      const std::optional<ExpectedActiveControlConfigurationDTO>& expected_current) {
    using Readback =
        std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>;
    auto acquired = AcquireProductPin();
    if (!acquired.ok()) return Result<Readback>::Err(acquired.error());
    const auto context = ReadPinnedProductContext(acquired.value());
    if (!context.ok()) return Result<Readback>::Err(context.error());
    if (!context.value()->route_services ||
        context.value()->route_services->current_snapshot_source == nullptr ||
        context.value()->route_services->controller_lifecycle_port_resolver == nullptr) {
      return Result<Readback>::Err(coordinator_error(
          "route_observation_unavailable", "controller observation services are not composed",
          "route_services_missing"));
    }
    try {
      // Observation never needs route-selection permission: a Declared
      // component (or one without lifecycle facts) is still observable.
      auto snapshot =
          context.value()
              ->route_services->current_snapshot_source->ReadCurrentRouteConfigurationObservation(
                  retained_bundle_id, graph_revision, desired_configuration);
      if (!snapshot.ok()) return Result<Readback>::Err(snapshot.error());
      if (expected_current) {
        const auto& current = snapshot.value().value().active_control_configuration;
        const ExpectedActiveControlConfigurationDTO actual{
            current ? ExpectedActiveControlConfigurationPresenceDTO::kPresent
                    : ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
            current ? std::optional(current->active_control_configuration_id) : std::nullopt,
            current ? std::optional(current->active_control_configuration_generation)
                    : std::nullopt};
        if (!expected_current->locally_valid() || *expected_current != actual) {
          return Result<Readback>::Err(coordinator_error(
              "route_observation_configuration_changed",
              "retained controller configuration changed", "current_configuration_mismatch"));
        }
      }
      std::shared_ptr<IControllerLifecyclePort> port;
      return ObserveRouteController(*context.value(), snapshot.value().expected_controller_domain(),
                                    port);
    } catch (...) {
      return Result<Readback>::Err(coordinator_error(
          "route_selection_snapshot_failed", "the route snapshot source raised an exception",
          "snapshot_source_exception"));
    }
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteRecovery(
      const RouteSelectionIntentDTO& intent, const ActiveControlConfigurationDTO& predecessor) {
    return ResolveAdmission([&] { return SubmitRouteSelectionResolved(intent, &predecessor); });
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelectionResolved(
      const RouteSelectionIntentDTO& intent,
      const ActiveControlConfigurationDTO* recovery_predecessor = nullptr) {
    bool known_unstarted = false;
    auto result = SubmitRouteSelectionAttempt(intent, known_unstarted, recovery_predecessor);
    if (!result.ok() && known_unstarted) {
      auto error = result.error();
      error.details["effect"] = "not_started";
      auto retained =
          journal_.RecordRouteSelectionRejection(CurrentOwnerEpoch(), intent, error, std::nullopt);
      result = retained.ok() ? ReplayRouteResult(retained.value()) : retained;
    }
    if (!result.ok() && result.error().code == "request_ledger_full") {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          CheckpointFullRequestNamespace(intent.request_context.request_owner_epoch));
    }
    return result;
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelectionAttempt(
      const RouteSelectionIntentDTO& intent, bool& known_unstarted,
      const ActiveControlConfigurationDTO* recovery_predecessor) {
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
    auto acquired = AcquireProductPin();
    if (!acquired.ok()) {
      // Retained requests remain journal observations during exchange, while
      // a new request is refused before touching product services or the ledger.
      if (acquired.error().code == "cclc_product_exchange_pending") {
        try {
          const auto retained = journal_.InspectRouteSelectionRequest(CurrentOwnerEpoch(), intent);
          if (!retained.ok())
            return Result<CCLCRouteSelectionRequestResultDTO>::Err(retained.error());
          if (retained.value()) return ReplayRouteResult(*retained.value());
        } catch (...) {
          return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
              "route_selection_journal_inspection_failed",
              "the route journal inspection raised an exception", "journal_exception"));
        }
      }
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(acquired.error());
    }
    const auto context = ReadPinnedProductContext(acquired.value());
    if (!context.ok()) return Result<CCLCRouteSelectionRequestResultDTO>::Err(context.error());
    if (!context.value()->route_services.has_value()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_unsupported", "the CCLC route owner is not composed",
                            "route_services_unavailable"));
    }

    Result<std::optional<CCLCRouteSelectionRequestResultDTO>> inspected =
        Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(coordinator_error(
            "route_selection_journal_inspection_failed",
            "the route journal inspection raised an exception", "journal_exception"));
    try {
      inspected = journal_.InspectRouteSelectionRequest(CurrentOwnerEpoch(), intent);
    } catch (...) {
    }
    if (!inspected.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(inspected.error());
    }
    if (inspected.value().has_value()) {
      return ReplayRouteResult(*inspected.value());
    }
    known_unstarted = true;
    if (!intent.expected_admission_generation) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
          "route_selection_intent_invalid", "new route requests require admission generation",
          "admission_generation_required"));
    }

    std::uint64_t admission_gate_generation = 0U;
    {
      std::lock_guard lock(admission_mutex_);
      if (!AdmissionIsOpen()) {
        return Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
            "route_selection_admission_closed",
            "the CCLC coordinator is not accepting new route work", "coordinator_not_accepting"));
      }
      admission_gate_generation = admission_gate_generation_;
    }
    return CommitRouteAdmission(intent, admission_gate_generation, known_unstarted,
                                recovery_predecessor, context.value());
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

  [[nodiscard]] bool AdmissionIsOpen() const noexcept {
    // A completion tail may publish its old raw accepting value after a new
    // exchange proof closes admission outside the exclusion. The sticky fault
    // always dominates that value, including fresh admission and commit checks.
    return accepting_.load(std::memory_order_acquire) &&
           !admission_fault_closed_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool accepting_new_requests() const noexcept { return AdmissionIsOpen(); }

  [[nodiscard]] bool admission_fault_closed() const noexcept {
    std::lock_guard lock(admission_mutex_);
    return admission_fault_closed_;
  }

  [[nodiscard]] RequestOwnerEpoch request_owner_epoch() const { return CurrentOwnerEpoch(); }

  [[nodiscard]] RequestOwnerEpoch CurrentOwnerEpoch() const {
    return *request_epoch_.load(std::memory_order_acquire);
  }

  void CloseAdmission() noexcept {
    std::unique_lock lock(admission_mutex_);
    CloseAdmissionLocked();
    admission_resolution_ready_.wait(lock, [&] { return !admission_persistence_active_; });
  }

  [[nodiscard]] Result<void> ValidateRetirementScopeLocked(
      const CCLCLifecycleTargetRetirementScopeDTO& scope) const {
    if (!scope.locally_valid() || !started_ || shutting_down_.load(std::memory_order_acquire) ||
        product_control_->exchange_pending || CurrentOwnerEpoch() != scope.request_owner_epoch ||
        !product_control_->current || !product_control_->current->identity)
      return Result<void>::Err(coordinator_error("component_lifecycle_target_retirement_scope_stale",
          "Close scope has no exact current product owner", "retirement_scope_stale"));
    const auto& publication = product_control_->current->identity->publication;
    if (publication.publication_generation != scope.publication_generation ||
        publication.durable_public_pointer_content_digest != scope.active_public_pointer_digest)
      return Result<void>::Err(coordinator_error("component_lifecycle_target_retirement_scope_stale",
          "Close scope names another immutable publication", "retirement_publication_changed"));
    return Result<void>::Ok();
  }

  [[nodiscard]] Result<CCLCLifecycleTargetRetirementOfferDTO> ReadLifecycleTargetRetirementOffer() {
    using Offer = Result<CCLCLifecycleTargetRetirementOfferDTO>;
    auto pin = AcquireProductPin();
    if (!pin.ok()) return Offer::Err(pin.error());
    std::unique_ptr<ICellCloseAdmissionLease> lease;
    std::unique_lock admission_lock(admission_mutex_);
    if (!product_control_->current || !product_control_->current->identity)
      return Offer::Err(coordinator_error("component_lifecycle_target_retirement_scope_unavailable",
          "Close needs the actual immutable publication", "publication_identity_unavailable"));
    const auto& publication = product_control_->current->identity->publication;
    const CCLCLifecycleTargetRetirementScopeDTO scope{CurrentOwnerEpoch(), publication.publication_generation,
        publication.durable_public_pointer_content_digest};
    const auto scoped = ValidateRetirementScopeLocked(scope);
    if (!scoped.ok()) return Offer::Err(scoped.error());
    auto acquired = kernel_cell_admission_port_.AcquireCellCloseAdmissionLease();
    if (!acquired.ok()) return Offer::Err(acquired.error());
    lease = std::move(acquired.value());
    const auto before = lease->ValidatePrerequisites();
    if (!before.ok()) return Offer::Err(before.error());
    auto targets = journal_.LoadLifecycleTargetsForClose(scope.request_owner_epoch);
    if (!targets.ok()) return Offer::Err(targets.error());
    const auto after = lease->ValidatePrerequisites();
    if (!after.ok()) return Offer::Err(after.error());
    return Offer::Ok({scope, std::move(targets.value())});
  }

  [[nodiscard]] Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>> ReadLifecycleTargetRetirementResult(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) {
    // Original immutable membership reads do not borrow the current product,
    // change targets, dispatch work or require a current owner namespace.
    return journal_.ReadLifecycleTargetRetirementResult(intent);
  }

  [[nodiscard]] Result<CCLCLifecycleTargetRetirementResultDTO> RetireLifecycleTargetsForClose(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) {
    using Retired = Result<CCLCLifecycleTargetRetirementResultDTO>;
    // Exact replay/conflict/expiry belongs to the original request before any
    // current publication or Kernel fact. A lost reply cannot retire successors.
    auto original = journal_.ReadLifecycleTargetRetirementResult(intent);
    if (!original.ok()) return Retired::Err(original.error());
    if (original.value()) return Retired::Ok(std::move(*original.value()));
    auto pin = AcquireProductPin();
    if (!pin.ok()) return Retired::Err(pin.error());
    std::unique_ptr<ICellCloseAdmissionLease> lease;
    std::unique_lock admission_lock(admission_mutex_);
    const auto scoped = ValidateRetirementScopeLocked(intent.scope);
    if (!scoped.ok()) return Retired::Err(scoped.error());
    auto acquired = kernel_cell_admission_port_.AcquireCellCloseAdmissionLease();
    if (!acquired.ok()) return Retired::Err(acquired.error());
    lease = std::move(acquired.value());
    const auto prepared = lease->PrepareCommandEligibilityClosure(intent);
    if (!prepared.ok()) return Retired::Err(prepared.error());
    // Every allocation-bearing error value is prepared before retirement.
    auto join_unknown = Retired::Err(coordinator_error(
        "component_lifecycle_target_retirement_outcome_unknown",
        "the durable retirement has no exact Kernel eligibility closure",
        "eligibility_closure_unproved"));
    // The same SQL transaction compares all targets, retains the complete
    // typed result, and checks this admission lease at acceptance/precommit.
    // There is no lifecycle dispatch and no state/submission lock over SQL.
    auto retired = journal_.RetireLifecycleTargetsForClose(CurrentOwnerEpoch(), intent, *lease);
    if (!retired.ok()) {
      if (retired.error().code == "persistence_outcome_unknown") {
        static_cast<void>(lease->CloseCommandEligibilityForUnknownRetirement(intent));
        CloseAdmissionLocked();
      }
      return retired;
    }
    if (retired.value().replay_status == RequestReplayStatusDTO::kReplayedExact)
      return retired;
    if (!lease->CloseCommandEligibilityAfterRetirement(intent, retired.value())) {
      static_cast<void>(lease->CloseCommandEligibilityForUnknownRetirement(intent));
      CloseAdmissionLocked();
      return join_unknown;
    }
    return retired;
  }

  [[nodiscard]] Result<void> ReadLifecycleOwnerSettlement(
      const CCLCLifecycleTargetRetirementScopeDTO& scope) {
    auto pin = AcquireProductPin();
    if (!pin.ok()) return Result<void>::Err(pin.error());
    std::unique_ptr<ICellCloseAdmissionLease> lease;
    {
      std::unique_lock admission_lock(admission_mutex_);
      const auto scoped = ValidateRetirementScopeLocked(scope);
      if (!scoped.ok()) return scoped;
      auto acquired = kernel_cell_admission_port_.AcquireCellCloseAdmissionLease();
      if (!acquired.ok()) return Result<void>::Err(acquired.error());
      lease = std::move(acquired.value());
    }
    const auto before = lease->ValidatePrerequisites();
    if (!before.ok()) return before;
    const auto settled = ReadLifecycleOwnerSettlement();
    if (!settled.ok()) return settled; // Existing exact memory/durable settlement proof.
    const auto after = lease->ValidatePrerequisites();
    if (!after.ok()) return after;
    // Like the existing settlement front/recheck, never wait for CCLC
    // while retaining Kernel admission: Offer/Retire use CCLC -> Kernel.
    std::unique_lock admission_lock(admission_mutex_, std::try_to_lock);
    if (!admission_lock.owns_lock())
      return Result<void>::Err(coordinator_error("component_lifecycle_owner_settlement_unknown",
          "the lifecycle owner has no complete current settlement proof",
          "original_work_settlement_unproven"));
    return ValidateRetirementScopeLocked(scope);
  }

  [[nodiscard]] Result<void> ReadLifecycleOwnerSettlement() {
    const auto unknown = [] {
      return Result<void>::Err(
          coordinator_error("component_lifecycle_owner_settlement_unknown",
                            "the lifecycle owner has no complete current settlement proof",
                            "original_work_settlement_unproven"));
    };
    const auto memory_settled = [&] {
      return started_ && !starting_ && !recovery_in_progress_ &&
             !shutting_down_.load(std::memory_order_acquire) && !admission_fault_closed_ &&
             owner_wide_containment_gate_count_ == 0U &&
             unresolved_effect_admission_gate_count_ == 0U && !admission_resolution_active_ &&
             !admission_persistence_active_ && active_transactions_.empty() &&
             active_route_transactions_.empty() && schedule_publications_in_flight_ == 0U &&
             worker_pool_.IsIdle() &&
             std::all_of(
                 scheduled_handles_.begin(), scheduled_handles_.end(),
                 [](const auto& poll) { return poll.second->load(std::memory_order_acquire); });
    };
    std::optional<RequestOwnerEpoch> owner;
    std::uint64_t gate_generation = 0U;
    bool admission_open = false;
    {
      std::unique_lock admission_lock(admission_mutex_, std::try_to_lock);
      if (!admission_lock.owns_lock()) return unknown();
      std::unique_lock state_lock(state_mutex_, std::try_to_lock);
      if (!state_lock.owns_lock()) return unknown();
      std::unique_lock schedule_lock(schedule_mutex_, std::try_to_lock);
      if (!schedule_lock.owns_lock() || !memory_settled()) return unknown();
      owner = CurrentOwnerEpoch();
      gate_generation = admission_gate_generation_;
      admission_open = AdmissionIsOpen();
    }
    // Existing worker/journal mutex reads are non-real-time operations. The
    // front exclusions never wait, and no new work/drain/cancel is performed.
    Result<void> durable = unknown();
    try {
      durable = journal_.ReadQuiescentOwner(*owner);
    } catch (...) {
    }
    if (!durable.ok()) return unknown();
    {
      std::unique_lock admission_lock(admission_mutex_, std::try_to_lock);
      if (!admission_lock.owns_lock()) return unknown();
      std::unique_lock state_lock(state_mutex_, std::try_to_lock);
      if (!state_lock.owns_lock()) return unknown();
      std::unique_lock schedule_lock(schedule_mutex_, std::try_to_lock);
      if (!schedule_lock.owns_lock() || CurrentOwnerEpoch() != *owner ||
          admission_gate_generation_ != gate_generation || AdmissionIsOpen() != admission_open ||
          !memory_settled())
        return unknown();
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] {
      return std::max(std::chrono::nanoseconds::zero(),
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          deadline - std::chrono::steady_clock::now()));
    };
    for (;;) {
      {
        std::unique_lock lock(admission_mutex_);
        if (!admission_resolution_ready_.wait_until(lock, deadline, [&] {
              return !admission_resolution_active_ && !admission_persistence_active_;
            })) return false;
      }
      if (!worker_pool_.DrainFor(remaining())) return false;
      {
        std::unique_lock lock(state_mutex_);
        if (!state_changed_.wait_until(lock, deadline, [&] {
              return active_transactions_.empty() && active_route_transactions_.empty();
            })) return false;
      }
      // Complete removes its active row before its final admission reconciliation.
      // Wait for that actual worker return rather than treating the removal as
      // proof that every genuine-fault producer has finished.
      if (!worker_pool_.DrainFor(remaining())) return false;
      std::vector<std::pair<ScheduleHandle, std::shared_ptr<std::atomic<bool>>>> polls;
      bool scheduler_lane_used = false;
      {
        std::unique_lock schedule_lock(schedule_mutex_);
        if (!schedule_state_changed_.wait_until(schedule_lock, deadline, [&] {
              return schedule_publications_in_flight_ == 0U;
            })) return false;
      }
      {
        // Exclude acceptance publication while taking the terminal-work snapshot.
        // Direct callers must still be stopped by a lifetime-handoff caller.
        std::lock_guard admission_lock(admission_mutex_);
        std::lock_guard state_lock(state_mutex_);
        std::lock_guard schedule_lock(schedule_mutex_);
        if (admission_resolution_active_ || admission_persistence_active_ ||
            !active_transactions_.empty() || !active_route_transactions_.empty() ||
            schedule_publications_in_flight_ != 0U || !worker_pool_.IsIdle()) {
          if (std::chrono::steady_clock::now() >= deadline) return false;
          continue;
        }
        polls = scheduled_handles_;
        scheduler_lane_used = scheduler_lane_used_;
      }
      // Only terminal runtimes remain. Their queued polls hold weak pointers;
      // cancelling those polls does not cancel active work or require a fake
      // clock to advance just to retire an already-completed owner.
      try {
        for (const auto& poll : polls) {
          if (!poll.second->load(std::memory_order_acquire)) scheduler_.cancel(poll.first);
        }
      } catch (...) {
        // Keep the handles tracked for ordinary shutdown if cancellation fails.
        return false;
      }
      {
        std::lock_guard lock(schedule_mutex_);
        std::erase_if(scheduled_handles_, [&](const auto& item) {
          return std::any_of(polls.begin(), polls.end(), [&](const auto& poll) {
            return item.first == poll.first;
          });
        });
      }
      try {
        if (scheduler_lane_used &&
            !scheduler_.drain_for(SchedulerPriority::Lifecycle, remaining())) return false;
      } catch (...) {
        return false;
      }
      // A callback already dequeued before cancel may have submitted a terminal
      // no-op worker. Both callback and worker must return before the final read.
      if (!worker_pool_.DrainFor(remaining())) return false;
      {
        std::lock_guard admission_lock(admission_mutex_);
        std::lock_guard state_lock(state_mutex_);
        std::lock_guard schedule_lock(schedule_mutex_);
        if (!admission_resolution_active_ && !admission_persistence_active_ &&
            active_transactions_.empty() && active_route_transactions_.empty() &&
            schedule_publications_in_flight_ == 0U &&
            std::none_of(scheduled_handles_.begin(), scheduled_handles_.end(),
                         [](const auto& poll) {
                           return !poll.second->load(std::memory_order_acquire);
                         }) && worker_pool_.IsIdle()) return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) return false;
    }
  }

 private:
  template <typename Function>
  std::invoke_result_t<Function> ResolveAdmission(Function&& function) {
    std::unique_lock lock(admission_mutex_);
    admission_resolution_ready_.wait(lock, [&] { return !admission_resolution_active_; });
    admission_resolution_active_ = true;
    ScopeExit resolved([&] {
      lock.lock();
      admission_resolution_active_ = false;
      admission_resolution_ready_.notify_all();
    });
    lock.unlock();
    return function();
  }

  static ErrorDTO UnstartedCapacityDeferral() {
    return {"cclc_request_admission_deferred",
            "CCLC is finishing accepted work before retiring its full request namespace",
            {{"reason", "request_namespace_busy"}, {"effect", "not_started"}}};
  }

  [[nodiscard]] bool ReconcileLifecycleOwnerEpoch(const RequestOwnerEpoch& epoch) noexcept {
    const auto probe_digest =
        ContentDigest("sha256:0000000000000000000000000000000000000000000000000000000000000000");
    const auto reconciliation_digest = CellAdmissionOwnerEpochReconciliationEvidenceDigest(
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, epoch);
    if (!reconciliation_digest.ok()) {
      return false;
    }
    const CellAdmissionClaimIntentDTO probe{CellAdmissionClaimKindDTO::kComponentLifecycle,
                                            CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
                                            epoch,
                                            "cclc-owner-epoch-probe",
                                            RequestId("cclc-owner-epoch-probe"),
                                            probe_digest,
                                            AdmissionGeneration(1U),
                                            {},
                                            std::nullopt,
                                            std::nullopt};
    for (std::size_t attempt = 0; attempt != kMaximumAdmissionResolutionAttempts; ++attempt) {
      Result<CellAdmissionRecoveryReceiptDTO> receipt =
          Result<CellAdmissionRecoveryReceiptDTO>::Err(coordinator_error(
              "component_lifecycle_owner_epoch_reconciliation_failed",
              "Kernel lifecycle-owner receipt raised an exception", "recovery_receipt_exception"));
      try {
        receipt = kernel_cell_admission_port_.GetCellAdmissionRecoveryReceipt(
            {probe, std::nullopt, std::nullopt});
      } catch (...) {
      }
      if (!receipt.ok()) {
        return false;
      }
      const auto matches = [&](const CellAdmissionRecoveryReceiptDTO& value) {
        return value.kernel_owner_epoch == receipt.value().kernel_owner_epoch &&
               std::find(value.current_owner_epochs.begin(), value.current_owner_epochs.end(),
                         CellAdmissionCurrentOwnerEpochDTO{
                             CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, epoch}) !=
                   value.current_owner_epochs.end();
      };
      // A lost reconcile response must not rebuild a different claim inventory
      // under the same digest. The current authoritative owner mapping proves
      // that this exact namespace was already installed.
      if (matches(receipt.value())) return true;
      auto owners = receipt.value().current_owner_epochs;
      std::erase_if(owners, [](const auto& owner) {
        return owner.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner;
      });
      owners.push_back({CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, epoch});
      std::sort(owners.begin(), owners.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.owner_role < rhs.owner_role; });
      const CellAdmissionReconcileDTO reconcile{receipt.value().kernel_owner_epoch,
                                                receipt.value().held_claims, std::move(owners),
                                                reconciliation_digest.value()};
      try {
        const auto applied = kernel_cell_admission_port_.ReconcileCellAdmissionClaims(reconcile);
        if (applied.ok()) return true;
        const auto reason = applied.error().details.find("reason");
        if (applied.error().code == "authoring_not_ready" &&
            reason != applied.error().details.end() &&
            reason->second == "reconcile_inventory_not_exact") {
          // Another owner's claim changed after our read. Kernel explicitly
          // made no change; only this refusal permits a fresh inventory.
          continue;
        }
      } catch (...) {
      }
      try {
        const auto observed = kernel_cell_admission_port_.GetCellAdmissionRecoveryReceipt(
            {probe, std::nullopt, std::nullopt});
        return observed.ok() && matches(observed.value());
      } catch (...) {
        return false;
      }
    }
    return false;
  }

  [[nodiscard]] ErrorDTO CheckpointFullRequestNamespace(const RequestOwnerEpoch& predecessor) {
    std::unique_lock admission_lock(admission_mutex_);
    auto refreshed = [] {
      return ErrorDTO{
          "cclc_request_namespace_refreshed",
          "CCLC retired a full settled request namespace; refresh before a new submission",
          {{"reason", "settled_request_namespace_checkpoint"}, {"effect", "not_started"}}};
    };
    // This call is reachable only after exact inspection found this request
    // unseen in a full namespace. A concurrent checkpoint may already have
    // retired it; no effectful retry was made.
    if (CurrentOwnerEpoch() != predecessor) return refreshed();
    if (!AdmissionIsOpen() || admission_persistence_active_ ||
        owner_wide_containment_gate_count_ != 0U || unresolved_effect_admission_gate_count_ != 0U) {
      return replay_error(RequestReplayStatusDTO::kRequestLedgerFull);
    }
    admission_lock.unlock();
    {
      std::lock_guard lock(state_mutex_);
      if (recovery_in_progress_ || !active_transactions_.empty() ||
          !active_route_transactions_.empty()) {
        return UnstartedCapacityDeferral();
      }
    }
    {
      std::lock_guard lock(schedule_mutex_);
      if (schedule_publications_in_flight_ != 0U ||
          std::any_of(scheduled_handles_.begin(), scheduled_handles_.end(), [](const auto& item) {
            return !item.second->load(std::memory_order_acquire);
          })) {
        return UnstartedCapacityDeferral();
      }
    }
    if (!worker_pool_.IsIdle()) {
      return UnstartedCapacityDeferral();
    }
    admission_lock.lock();
    if (!AdmissionIsOpen()) {
      return replay_error(RequestReplayStatusDTO::kRequestLedgerFull);
    }
    accepting_.store(false, std::memory_order_release);
    admission_lock.unlock();
    try {
      const auto checkpoint = journal_.BeginOwnerEpoch(
          config_.request_owner_epoch, clock_.now(), config_.request_membership_capacity,
          predecessor, CCLCOwnerEpochTransitionPurpose::kLiveCapacityCheckpoint);
      if (checkpoint.ok() && checkpoint.value().locally_valid() &&
          ReconcileLifecycleOwnerEpoch(checkpoint.value().request_owner_epoch)) {
        admission_lock.lock();
        request_epoch_.store(
            std::make_shared<const RequestOwnerEpoch>(checkpoint.value().request_owner_epoch),
            std::memory_order_release);
        if (!admission_fault_closed_ && !shutting_down_.load(std::memory_order_acquire)) {
          accepting_.store(true, std::memory_order_release);
        }
        return refreshed();
      }
    } catch (...) {
    }
    admission_lock.lock();
    CloseAdmissionLocked();
    return coordinator_error("component_lifecycle_owner_epoch_reconciliation_failed",
                             "CCLC request namespace checkpoint could not be proven complete",
                             "owner_epoch_reconciliation_failed");
  }

  struct AdmissionFacts final {
    std::shared_ptr<const ComponentLifecycleProductContext> product_context;
    std::optional<ComponentLifecycleCurrentSnapshotDTO> current_snapshot;
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs;
  };

  struct RuntimeState final {
    RuntimeState(std::shared_ptr<const ComponentLifecycleProductContext> context,
                 ComponentLifecycleStatusDTO initial_status,
                 std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> accepted_catalogs,
                 std::vector<ControlDomainId> accepted_reserved_domains,
                 std::optional<ComponentLifecycleCurrentSnapshotDTO> accepted_component_snapshot)
        : product_context(std::move(context)),
          status(std::move(initial_status)),
          original_acceptance(status.acceptance),
          original_apply_parent(status.apply_bring_up
                                    ? std::optional(status.apply_bring_up->parent_authority)
                                    : std::nullopt),
          qualified_catalogs(std::move(accepted_catalogs)),
          reserved_domains(std::move(accepted_reserved_domains)),
          accepted_snapshot(std::move(accepted_component_snapshot)),
          effective_deadline_ns(deadline_at(status)),
          ports(status.domains.size()),
          observations(status.domains.size()),
          decisions(status.domains.size()) {
      permanent_containment_only =
          status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
          std::none_of(status.domains.begin(), status.domains.end(),
                       [](const auto& domain) { return domain.current_scope.has_value(); });
    }

    // First member: exact product outlives ports, snapshots and all worker tails.
    const std::shared_ptr<const ComponentLifecycleProductContext> product_context;
    ComponentLifecycleStatusDTO status;
    const ComponentLifecycleAcceptedDTO original_acceptance;
    const std::optional<ApplyComponentBringUpAuthorityDTO> original_apply_parent;
    std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> qualified_catalogs;
    std::vector<ControlDomainId> reserved_domains;
    std::optional<ComponentLifecycleCurrentSnapshotDTO> accepted_snapshot;
    std::int64_t effective_deadline_ns;
    std::vector<std::shared_ptr<IBrandLifecyclePort>> ports;
    std::vector<std::optional<BrandLifecycleObservationDTO>> observations;
    std::vector<std::optional<LifecycleDomainPlanDecision>> decisions;
    std::optional<ComponentLifecycleCoordinatorApplyBringUpServices> apply_services;
    /// Nonowning installed port; only the original-live Apply pin grants use.
    ICCLCComponentCommandReadinessPort* apply_readiness_port = nullptr;
    std::vector<ResolvedLifecycleControlDomainScopeDTO> apply_candidate_domains;
    std::vector<ResolvedLifecycleProviderOccurrenceDTO> apply_candidate_providers;
    std::vector<std::optional<BrandLifecyclePlanCatalogDTO>> apply_domain_catalogs;
    std::atomic<bool> apply_continued{false};
    std::atomic<bool> apply_holding{false};
    std::atomic<bool> apply_stopped{false};
    std::size_t current_domain_index = 0U;
    std::uint64_t effect_sequence = 0U;
    std::optional<std::size_t> verifying_domain_index;
    std::optional<LifecyclePrimitiveTransitionDTO> verifying_transition;
    std::optional<CCLCEffectAttemptDTO> unresolved_recovery_effect;
    std::optional<CellAdmissionClaimDTO> cell_claim;
    std::optional<CellAdmissionClaimReleaseDTO> cell_claim_release;
    std::uint64_t expected_observation_generation = 0U;
    bool initialized = false;
    /// The Kernel closed this component's eligibility for this transaction;
    /// every terminal settlement must then publish its complete result.
    bool command_eligibility_closed = false;
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
        std::shared_ptr<const ComponentLifecycleProductContext> context,
        RouteSelectionStatusDTO initial_status, RouteSelectionCurrentSnapshot accepted_snapshot,
        std::optional<RouteConfigurationFenceAdmissionSnapshotDTO> accepted_fence_admission,
        std::vector<ControlDomainId> accepted_reserved_domains)
        : product_context(std::move(context)),
          original_acceptance(initial_status.acceptance),
          status(std::move(initial_status)),
          snapshot(std::move(accepted_snapshot)),
          fence_admission(std::move(accepted_fence_admission)),
          reserved_domains(std::move(accepted_reserved_domains)),
          effective_deadline_ns(status.accepted_at_ns + status.acceptance.timeout_duration_ns) {}

    const std::shared_ptr<const ComponentLifecycleProductContext> product_context;
    // Immutable accepted identity for read-only membership; worker status is
    // never read under the membership mutex as if that protected its writes.
    const RouteSelectionAcceptedDTO original_acceptance;
    RouteSelectionStatusDTO status;
    RouteSelectionCurrentSnapshot snapshot;
    std::optional<RouteConfigurationFenceAdmissionSnapshotDTO> fence_admission;
    std::vector<ControlDomainId> reserved_domains;
    std::shared_ptr<IControllerLifecyclePort> controller_port;
    std::optional<CellAdmissionClaimDTO> cell_claim;
    std::optional<CellAdmissionClaimReleaseDTO> cell_claim_release;
    std::uint64_t effect_sequence{};
    std::optional<CCLCRouteEffectAttemptDTO> unresolved_recovery_effect;
    std::int64_t effective_deadline_ns{};
    bool recovering{};
    bool scheduled{};
    bool controller_source_replaced_before_transition{};
  };

  [[nodiscard]] std::vector<std::shared_ptr<RouteRuntimeState>>
  ReadComponentRouteMembership(const ControllableComponentId& component) const {
    std::vector<std::shared_ptr<RouteRuntimeState>> result;
    std::lock_guard lock(state_mutex_);
    for (const auto& route : active_route_transactions_) {
      const auto& expectations = route->original_acceptance.component_expectations;
      if (std::any_of(expectations.begin(), expectations.end(), [&](const auto& expected) {
            return expected.controllable_component_id == component;
          })) result.push_back(route);
    }
    return result;
  }

  [[nodiscard]] bool ComponentRouteMembershipMatches(
      const ControllableComponentId& component,
      const std::vector<std::shared_ptr<RouteRuntimeState>>& captured) const {
    std::lock_guard lock(state_mutex_);
    std::size_t index = 0U;
    for (const auto& route : active_route_transactions_) {
      const auto& expectations = route->original_acceptance.component_expectations;
      if (!std::any_of(expectations.begin(), expectations.end(), [&](const auto& expected) {
            return expected.controllable_component_id == component;
          })) continue;
      if (index >= captured.size() || captured[index++].get() != route.get()) return false;
    }
    return index == captured.size();
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> ReplayResult(
      const CCLCLifecycleRequestResultDTO& result) const {
    if (!result.locally_valid()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
          "component_lifecycle_journal_result_invalid",
          "the lifecycle journal returned an invalid request result", "journal_result_invalid"));
    }
    if (result.rejection) return Result<CCLCLifecycleRequestResultDTO>::Err(*result.rejection);
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
    if (result.rejection) return Result<CCLCRouteSelectionRequestResultDTO>::Err(*result.rejection);
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

  template <typename RecordRejection>
  [[nodiscard]] Result<CellAdmissionClaimDTO> AcquireDurableCellClaim(
      const CellAdmissionClaimIntentDTO& intent, RecordRejection&& record_rejection,
      const ActiveControlConfigurationDTO* recovery_predecessor = nullptr,
      const SelectedComponentBringUpOfferDTO* selected_offer = nullptr,
      const ComponentLifecycleIntentDTO* local_output_recovery = nullptr) {
    // This submit owns the persistence flag. Closing must not wait for itself.
    const auto close = [&] {
      std::lock_guard lock(admission_mutex_);
      CloseAdmissionLocked();
    };
    try {
      auto attempt = selected_offer ? journal_.RecordSelectedCellClaimAttempt(
                                          CurrentOwnerEpoch(), intent,
                                          selected_offer->original_request_payload_digest)
                                    : journal_.RecordCellClaimAttempt(CurrentOwnerEpoch(), intent);
      if (!attempt.ok()) {
        close();
        return Result<CellAdmissionClaimDTO>::Err(attempt.error());
      }
      auto acquired = selected_offer
                          ? kernel_cell_admission_port_.AcquireSelectedComponentBringUpClaim(
                                intent, *selected_offer)
                      : recovery_predecessor
                          ? kernel_cell_admission_port_.AcquireRouteRecoveryCellAdmissionClaim(
                                intent, *recovery_predecessor)
                      : local_output_recovery
                          ? kernel_cell_admission_port_.AcquireIoOutputRecoveryLifecycleClaim(
                                intent, *local_output_recovery)
                          : kernel_cell_admission_port_.AcquireCellAdmissionClaim(intent);
      if (!acquired.ok() || acquired.value().intent != intent) {
        const bool unstarted =
            !acquired.ok() && (acquired.error().code == "stale_generation" ||
                               acquired.error().code == "authoring_not_ready" ||
                               acquired.error().code == "invalid_state_transition" ||
                               acquired.error().code == "invalid_request");
        auto error = acquired.ok() ? coordinator_error("component_lifecycle_claim_acquire_invalid",
                                                       "Kernel returned a foreign CCLC claim",
                                                       "claim_receipt_mismatch")
                                   : acquired.error();
        if (unstarted) {
          error.details["effect"] = "not_started";
          const auto recorded = record_rejection(error);
          if (recorded.ok() && recorded.value().rejection == error &&
              !recorded.value().acceptance && !recorded.value().status) {
            return Result<CellAdmissionClaimDTO>::Err(std::move(error));
          }
          if (!recorded.ok()) error = recorded.error();
        }
        close();
        return Result<CellAdmissionClaimDTO>::Err(std::move(error));
      }
      auto retained = journal_.RecordCellClaimAcquired(CurrentOwnerEpoch(), acquired.value());
      if (!retained.ok()) {
        close();
        return Result<CellAdmissionClaimDTO>::Err(retained.error());
      }
      return acquired;
    } catch (...) {
      close();
      return Result<CellAdmissionClaimDTO>::Err(coordinator_error(
          "component_lifecycle_claim_acquire_unknown", "CCLC claim acquisition outcome is unknown",
          "claim_acquire_exception"));
    }
  }

  [[nodiscard]] static CellAdmissionClaimReleaseDTO TerminalCellClaimRelease(
      const CellAdmissionClaimDTO& claim, std::string digest) {
    return {claim.intent.owner_role,
            claim.intent.owner_epoch,
            claim.intent.owner_transaction_id,
            claim.intent.request_id,
            claim.intent.request_payload_digest,
            claim.acquired_admission_generation,
            claim.claim_generation,
            {CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
             ContentDigest(std::move(digest))}};
  }

  [[nodiscard]] Result<void> RequireSettledNativeApply() {
    try {
      const auto pending = journal_.ReadUnsettledNativeApplyPlan();
      if (!pending.ok()) return Result<void>::Err(pending.error());
      if (pending.value().has_value()) {
        auto error = coordinator_error(
            "component_lifecycle_native_apply_unsettled",
            "a planned native Apply has not been adopted; ordinary lifecycle and route changes remain closed",
            "native_apply_plan_pending");
        error.details["apply_id"] = pending.value()->apply_id.value();
        error.details["apply_generation"] =
            std::to_string(pending.value()->apply_generation.value());
        return Result<void>::Err(std::move(error));
      }
      return Result<void>::Ok();
    } catch (...) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_native_apply_status_unavailable",
          "CCLC could not verify whether the native Apply plan settled",
          "native_apply_plan_query_failed"));
    }
  }

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> CommitRouteAdmission(
      const RouteSelectionIntentDTO& intent, std::uint64_t admission_gate_generation,
      bool& known_unstarted, const ActiveControlConfigurationDTO* recovery_predecessor,
      const std::shared_ptr<const ComponentLifecycleProductContext>& product_context) {
    std::unique_lock admission_lock(admission_mutex_);

    if (!AdmissionIsOpen() || admission_gate_generation != admission_gate_generation_) {
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
      reinspection = journal_.InspectRouteSelectionRequest(CurrentOwnerEpoch(), intent);
    } catch (...) {
    }
    if (!reinspection.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(reinspection.error());
    }
    if (reinspection.value().has_value()) {
      return ReplayRouteResult(*reinspection.value());
    }
    const auto native_apply = RequireSettledNativeApply();
    if (!native_apply.ok()) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(native_apply.error());
    }

    Result<RouteSelectionCurrentSnapshot> snapshot =
        Result<RouteSelectionCurrentSnapshot>::Err(coordinator_error(
            "route_selection_snapshot_failed", "the route snapshot source raised an exception",
            "snapshot_source_exception"));
    try {
      snapshot = product_context->route_services->current_snapshot_source
                     ->ReadCurrentRouteSelectionSnapshot(intent.expected_retained_bundle_id,
                                                         intent.expected_graph_revision,
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

    if (recovery_predecessor) {
      const auto& old = *recovery_predecessor;
      const auto& desired = intent.desired_configuration;
      if (!old.locally_valid() || old.state != ActiveControlConfigurationStateDTO::kReconciled ||
          journal_current.value() != old || intent.program_node_evidence ||
          desired.control_domain_id != old.control_domain_id ||
          desired.control_domain_revision != old.control_domain_revision ||
          desired.controller_topology_id != old.controller_topology_id ||
          desired.topology_revision != old.topology_revision ||
          desired.activation_set_id != old.activation_set_id ||
          desired.binding_routes.size() != old.binding_routes.size() ||
          !std::equal(desired.binding_routes.begin(), desired.binding_routes.end(),
                      old.binding_routes.begin(), [](const auto& wanted, const auto& saved) {
                        return wanted.control_binding_id == saved.control_binding_id &&
                               wanted.control_binding_revision == saved.control_binding_revision &&
                               wanted.control_route_id == saved.control_route_id;
                      })) {
        return Result<CCLCRouteSelectionRequestResultDTO>::Err(
            coordinator_error("route_recovery_predecessor_mismatch",
                              "replacement recovery must restore the exact durable saved route",
                              "route_recovery_predecessor_mismatch"));
      }
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
      fence_snapshot = product_context->route_services->kernel_route_fence_port
                           ->ObserveRouteConfigurationFenceAdmission(fence_query);
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
    const auto prepared =
        PrepareRouteSelectionAdmission(intent, candidate, recovery_predecessor != nullptr);
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
      reread = product_context->route_services->current_snapshot_source
                   ->ReadCurrentRouteSelectionSnapshot(intent.expected_retained_bundle_id,
                                                       intent.expected_graph_revision,
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
          product_context, prepared.value().initial_status, std::move(snapshot.value()),
          fence_snapshot.value(), prepared.value().reserved_control_domains);
    } catch (...) {
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(
          coordinator_error("route_selection_runtime_capacity_unavailable",
                            "the route runtime state could not be allocated before persistence",
                            "runtime_state_allocation_failed"));
    }

    admission_lock.lock();
    if (!AdmissionIsOpen() || admission_gate_generation != admission_gate_generation_ ||
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

    CellAdmissionClaimIntentDTO claim_intent{
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        CurrentOwnerEpoch(),
        transaction_id.value(),
        intent.request_context.request_id,
        ContentDigest(detail::RouteSelectionSemanticIntentDigest(intent)),
        *intent.expected_admission_generation,
        {},
        {},
        {},
        intent.program_node_evidence};
    if (recovery_predecessor) {
      claim_intent.claim_kind = CellAdmissionClaimKindDTO::kInterventionOrRecovery;
      claim_intent.scope.configurations = {
          {recovery_predecessor->control_domain_id,
           recovery_predecessor->active_control_configuration_id,
           recovery_predecessor->active_control_configuration_generation}};
      claim_intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
          claim_intent.owner_role, claim_intent.owner_epoch, claim_intent.owner_transaction_id,
          claim_intent.request_payload_digest, std::nullopt};
    }
    admission_persistence_active_ = true;
    known_unstarted = false;
    admission_lock.unlock();
    const auto acquired = AcquireDurableCellClaim(
        claim_intent,
        [&](const ErrorDTO& error) {
          return journal_.RecordRouteSelectionRejection(CurrentOwnerEpoch(), intent, error,
                                                        claim_intent);
        },
        recovery_predecessor);
    if (!acquired.ok()) {
      admission_lock.lock();
      admission_persistence_active_ = false;
      admission_resolution_ready_.notify_all();
      rollback_watermark();
      return Result<CCLCRouteSelectionRequestResultDTO>::Err(acquired.error());
    }
    runtime->cell_claim = acquired.value();
    Result<CCLCRouteSelectionRequestResultDTO> accepted =
        Result<CCLCRouteSelectionRequestResultDTO>::Err(coordinator_error(
            "route_selection_journal_accept_failed",
            "the route journal acceptance raised an exception", "journal_exception"));
    try {
      accepted = journal_.AcceptOrReplayRouteSelection(CurrentOwnerEpoch(), intent,
                                                       prepared.value().acceptance,
                                                       prepared.value().initial_status);
    } catch (...) {
    }
    admission_lock.lock();
    admission_persistence_active_ = false;
    admission_resolution_ready_.notify_all();
    if (!accepted.ok()) {
      rollback_watermark();
      CloseAdmissionLocked();
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
    status.status_owner_epoch = CurrentOwnerEpoch();
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
      if (status.terminal_result && runtime->cell_claim) {
        const auto release = TerminalCellClaimRelease(
            *runtime->cell_claim, detail::RouteSelectionStatusReleasePreconditionDigest(status));
        recorded =
            journal_.RecordRouteTerminalAndClaimRelease(CurrentOwnerEpoch(), status, release);
        if (recorded.ok()) runtime->cell_claim_release = release;
      } else {
        recorded = journal_.RecordRouteSelectionStatus(CurrentOwnerEpoch(), status);
      }
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
      recorded = journal_.RecordRouteEffectPrepared(CurrentOwnerEpoch(), attempt, status);
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
      if (status.terminal_result && runtime->cell_claim) {
        const auto release = TerminalCellClaimRelease(
            *runtime->cell_claim, detail::RouteSelectionStatusReleasePreconditionDigest(status));
        recorded = journal_.RecordRouteEffectCompletedAndClaimRelease(CurrentOwnerEpoch(), attempt,
                                                                      status, release);
        if (recorded.ok()) runtime->cell_claim_release = release;
      } else {
        recorded = journal_.RecordRouteEffectCompleted(CurrentOwnerEpoch(), attempt, status);
      }
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
      accepted = runtime->product_context->route_services->kernel_route_fence_port
                     ->AcquireRouteConfigurationFence(request);
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
        reconciled = runtime->product_context->route_services->kernel_route_fence_port
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
    const auto readback = ObserveRouteController(*runtime->product_context,
                                                 runtime->snapshot.expected_controller_domain(),
                                                 runtime->controller_port);
    if (!readback.ok()) {
      if (!runtime->controller_port) {
        CloseAdmission();
      } else {
        ScheduleRoute(runtime);
      }
      return;
    }
    const auto& [observed, reconciled] = readback.value();

    auto observed_status = runtime->status;
    if (!AdvanceRouteStatus(runtime, observed_status,
                            RouteSelectionTransactionPhaseDTO::kVerifying)) {
      CloseAdmission();
      return;
    }
    observed_status.controller_observation = observed;
    observed_status.controller_reconciliation = reconciled;
    observed_status.controller_transition_plan.reset();
    observed_status.reconciliation_required = false;
    observed_status.error.reset();

    // The durable private-recovery claim authorizes reconciliation of observed
    // truth, never a controller transition. A mismatch must remain a mismatch;
    // only ordinary route selection may repair it by changing controllers.
    const bool observation_only_recovery =
        runtime->cell_claim && runtime->cell_claim->intent.claim_kind ==
                                   CellAdmissionClaimKindDTO::kInterventionOrRecovery;
    const bool configuration_reconciled =
        reconciled.disposition == ControllerLifecycleReconciliationDispositionDTO::kReconciled;
    if (configuration_reconciled || observation_only_recovery) {
      const bool any_dispatched =
          std::any_of(observed_status.controller_transition_results.begin(),
                      observed_status.controller_transition_results.end(), [](const auto& result) {
                        return result.dispatch_state !=
                               ControllerLifecycleTransitionDispatchStateDTO::kNotDispatched;
                      });
      observed_status.effect = !configuration_reconciled
                                   ? RouteSelectionEffectDTO::kPartialEffectObserved
                               : any_dispatched ? RouteSelectionEffectDTO::kEffectObserved
                                                : RouteSelectionEffectDTO::kKnownNoEffect;
      if (configuration_reconciled) {
        observed_status.affected_control_binding_ids.clear();
        observed_status.affected_controller_role_ids.clear();
      }
      if (!RecordRouteStatus(runtime, std::move(observed_status))) {
        return;
      }
      ResolveAndPublishRouteConfiguration(runtime, cancellation);
      return;
    }

    std::int64_t now_ns = -1;
    try {
      now_ns = clock_.now();
    } catch (...) {
    }
    const auto plan = BuildControllerLifecycleTransitionPlan(
        runtime->snapshot.expected_controller_domain().value(), observed, now_ns);
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
        observed.source_sequence, 0U, step, runtime->status.remaining_duration_ns};
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
      resolved = runtime->product_context->route_services->configuration_publication_port
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
    const bool abandon_replaced_source = runtime->controller_source_replaced_before_transition;
    if (cancellation.stop_requested() ||
        (!abandon_replaced_source &&
         !runtime->status.resulting_active_control_configuration.has_value())) {
      return;
    }
    if (!abandon_replaced_source) {
      Result<RouteReadinessPublicationReceiptDTO> publication =
          Result<RouteReadinessPublicationReceiptDTO>::Err(coordinator_error(
              "route_readiness_publication_failed",
              "the route readiness publication raised an exception", "publication_exception"));
      try {
        publication =
            runtime->product_context->route_services->configuration_publication_port
                ->PublishAndReadBack(runtime->status.acceptance,
                                     runtime->snapshot.expected_controller_domain(),
                                     *runtime->status.resulting_active_control_configuration);
      } catch (...) {
      }
      if (!publication.ok()) {
        RecordRoutePublicationFailure(runtime, publication.error());
        return;
      }
      const auto verified_receipt =
          VerifyRouteReadinessPublicationReceipt(runtime->status, publication.value());
      if (!verified_receipt.ok()) {
        RecordRoutePublicationFailure(runtime, verified_receipt.error());
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
      held = runtime->product_context->route_services->kernel_route_fence_port
                 ->ObserveRouteConfigurationFenceStatus(query);
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
        abandon_replaced_source ? runtime->snapshot.value().active_control_configuration
                                : runtime->status.resulting_active_control_configuration);
    if (abandon_replaced_source &&
        held.value().evidence.current_active_control_configuration != release_configuration) {
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
      prepared_status.error =
          coordinator_error("route_selection_controller_source_replaced",
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
      released = runtime->product_context->route_services->kernel_route_fence_port
                     ->ReleaseRouteConfigurationFence(request);
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
        reconciled = runtime->product_context->route_services->kernel_route_fence_port
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
        !abandon_replaced_source && runtime->status.resulting_active_control_configuration->state ==
                                        ActiveControlConfigurationStateDTO::kReconciled;
    auto terminal_status = runtime->status;
    if (!AdvanceRouteStatus(runtime, terminal_status,
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
      terminal_status.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
      terminal_status.error =
          coordinator_error("route_selection_controller_source_replaced",
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

  void RecordRoutePublicationFailure(const std::shared_ptr<RouteRuntimeState>& runtime,
                                     const ErrorDTO& error) noexcept {
    // Keep retry semantics, but expose and persist the actual failure once per
    // change. Identical retries must not create a status/journal flood.
    if (runtime->status.error != error) {
      auto failed = runtime->status;
      if (!AdvanceRouteStatus(runtime, failed, RouteSelectionTransactionPhaseDTO::kPublishing)) {
        CloseAdmission();
        return;
      }
      failed.error = error;
      if (!RecordRouteStatus(runtime, std::move(failed))) return;
    }
    ScheduleRoute(runtime);
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
    if (runtime->cell_claim) {
      if (!runtime->cell_claim_release ||
          !ReleaseDurableClaim(runtime, *runtime->cell_claim_release)) {
        CloseAdmission();
        return;
      }
    } else if (runtime->status.accepted_intent.expected_admission_generation) {
      CloseAdmission();
      return;
    }
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
          !shutting_down_.load(std::memory_order_acquire) && !AdmissionIsOpen()) {
        if (!ReconcileLifecycleOwnerEpoch(CurrentOwnerEpoch())) {
          admission_fault_closed_ = true;
          return;
        }
        ++admission_gate_generation_;
        accepting_.store(true, std::memory_order_release);
      }
    }
    state_changed_.notify_all();
  }

  [[nodiscard]] Result<void> RestoreRouteRecovery(
      const CCLCJournalRecoveryDTO& recovery,
      const std::shared_ptr<const ComponentLifecycleProductContext>& product_context) {
    std::vector<std::shared_ptr<RouteRuntimeState>> runtimes;
    std::vector<std::pair<CCLCDomainReservationOwnerDTO, std::vector<ControlDomainId>>>
        restored_reservations;
    try {
      runtimes.reserve(recovery.interrupted_route_selections.size());
      restored_reservations.reserve(recovery.interrupted_route_selections.size());
      for (const auto& status : recovery.interrupted_route_selections) {
        const auto unresolved =
            std::find_if(recovery.unresolved_route_effect_attempts.begin(),
                         recovery.unresolved_route_effect_attempts.end(), [&](const auto& attempt) {
                           return attempt.route_selection_transaction_id ==
                                  status.acceptance.route_selection_transaction_id;
                         });
        Result<RouteSelectionCurrentSnapshot> snapshot = Result<RouteSelectionCurrentSnapshot>::Err(
            coordinator_error("route_selection_recovery_snapshot_failed",
                              "the route recovery snapshot source raised an exception",
                              "snapshot_source_exception"));
        try {
          snapshot =
              product_context->route_services->current_snapshot_source
                  ->ReadCurrentRouteSelectionSnapshot(status.acceptance.accepted_retained_bundle_id,
                                                      status.acceptance.accepted_graph_revision,
                                                      status.acceptance.desired_configuration);
        } catch (...) {
        }
        const auto current_controller_query =
            snapshot.ok() ? std::optional<ControllerLifecycleObservationQueryDTO>(
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
             unresolved->effect_kind != CCLCRouteEffectKindDTO::kControllerLifecycleTransition);
        const bool controller_source_replaced_before_transition =
            current_controller_query.has_value() && before_controller_transition &&
            ControllerSourceEpochOnlyChanged(
                status.acceptance.controller_lifecycle_observation_query,
                *current_controller_query);
        if (!snapshot.ok() || (*current_controller_query !=
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
        auto runtime = std::make_shared<RouteRuntimeState>(
            product_context, status, std::move(snapshot.value()), std::nullopt, std::move(domains));
        const auto claim = std::find_if(
            recovery.cell_claims.begin(), recovery.cell_claims.end(), [&](const auto& row) {
              return row.intent.owner_transaction_id ==
                     status.acceptance.route_selection_transaction_id.value();
            });
        if (status.accepted_intent.expected_admission_generation) {
          if (claim == recovery.cell_claims.end() || !claim->claim) {
            for (const auto& restored : restored_reservations)
              static_cast<void>(domain_reservations_.release(restored.first, restored.second));
            return Result<void>::Err(coordinator_error(
                "component_lifecycle_claim_recovery_missing",
                "interrupted route has no exact Kernel claim", "claim_recovery_missing"));
          }
          runtime->cell_claim = claim->claim;
          runtime->cell_claim_release = claim->release_intent;
        } else if (claim != recovery.cell_claims.end()) {
          for (const auto& restored : restored_reservations)
            static_cast<void>(domain_reservations_.release(restored.first, restored.second));
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_claim_recovery_missing",
              "legacy route cannot gain a retrospective claim", "legacy_route_claim_conflict"));
        }
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
    if (runtime->status.status_owner_epoch == CurrentOwnerEpoch()) {
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
  ObserveRouteController(const ComponentLifecycleProductContext& context,
                         const ControllerLifecycleExpectedDomain& expected,
                         std::shared_ptr<IControllerLifecyclePort>& port) noexcept {
    const auto query = MakeControllerLifecycleObservationQuery(expected.value());
    if (!port) {
      Result<std::shared_ptr<IControllerLifecyclePort>> resolved =
          Result<std::shared_ptr<IControllerLifecyclePort>>::Err(
              coordinator_error("route_controller_port_resolution_failed",
                                "the controller-lifecycle port resolver raised an exception",
                                "controller_port_resolver_exception"));
      try {
        resolved = context.route_services->controller_lifecycle_port_resolver
                       ->ResolveExactControllerLifecyclePort(query);
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
      port = std::move(resolved.value());
    }
    Result<ControllerLifecycleObservationDTO> observation =
        Result<ControllerLifecycleObservationDTO>::Err(coordinator_error(
            "route_controller_observation_failed",
            "the controller-lifecycle observation raised an exception", "observe_exception"));
    try {
      observation = port->Observe(query);
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
    const auto reconciliation =
        ReconcileControllerLifecycle(expected.value(), observation.value(), now_ns);
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
      return runtime->product_context->route_services->configuration_publication_port
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
      reconciled = runtime->product_context->route_services->kernel_route_fence_port
                       ->ReconcileRouteConfigurationFenceAcquireAttempt(request);
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
        successor.error =
            coordinator_error("route_selection_controller_source_replaced",
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
    const auto observed = ObserveRouteController(*runtime->product_context,
                                                 runtime->snapshot.expected_controller_domain(),
                                                 runtime->controller_port);
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
      reconciled = runtime->product_context->route_services->kernel_route_fence_port
                       ->ReconcileRouteConfigurationFenceReleaseAttempt(request);
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

    const bool abandon_replaced_source = runtime->controller_source_replaced_before_transition;
    if (!abandon_replaced_source &&
        !runtime->status.resulting_active_control_configuration.has_value()) {
      CloseAdmission();
      return;
    }
    const bool reconciled_configuration =
        !abandon_replaced_source && runtime->status.resulting_active_control_configuration->state ==
                                        ActiveControlConfigurationStateDTO::kReconciled;
    RouteConfigurationFenceReleaseEffectResultDTO result{
        request, RouteConfigurationFenceReleaseEffectDispositionDTO::kReleased,
        reconciled.value().receipt};
    auto terminal = runtime->status;
    if (!AdvanceRouteStatus(runtime, terminal,
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
      terminal.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
      terminal.error =
          coordinator_error("route_selection_controller_source_replaced",
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

  [[nodiscard]] Result<AdmissionFacts> ReadAdmissionFacts(
      const ComponentLifecycleIntentDTO& intent,
      const std::shared_ptr<const ComponentLifecycleProductContext>& product_context) {
    try {
      const auto snapshot = product_context->current_snapshot_source.ReadCurrentComponentSnapshot(
          intent.controllable_component_id);
      if (!snapshot.ok()) return Result<AdmissionFacts>::Err(snapshot.error());
      {
        AdmissionFacts result;
        result.product_context = product_context;
        result.current_snapshot = snapshot.value();
        result.catalogs.reserve(snapshot.value().provider_occurrences.size());
        for (const auto& provider : snapshot.value().provider_occurrences) {
          const auto catalog =
              product_context->lifecycle_plan_catalog_source.LoadExactLifecyclePlanCatalog(
                  provider.provider_manifest_id, provider.provider_manifest_revision,
                  provider.lifecycle_plan_catalog_digest);
          const bool catalog_verified =
              catalog.ok() && VerifyBrandLifecyclePlanCatalog(catalog.value()).ok() &&
              catalog.value().provider_manifest_id == provider.provider_manifest_id &&
              catalog.value().provider_manifest_revision == provider.provider_manifest_revision &&
              catalog.value().catalog_digest == provider.lifecycle_plan_catalog_digest;
          if (!catalog_verified) {
            return Result<AdmissionFacts>::Err(
                catalog.ok()
                    ? coordinator_error(
                          "component_lifecycle_catalog_mismatch",
                          "the lifecycle catalog does not match its canonical provider closure",
                          "catalog_digest_or_provider_identity_mismatch")
                    : catalog.error());
          }
          result.catalogs.push_back(catalog.value());
        }
        return Result<AdmissionFacts>::Ok(std::move(result));
      }
    } catch (...) {
      return Result<AdmissionFacts>::Err(coordinator_error(
          "component_lifecycle_admission_source_failed",
          "a lifecycle admission source raised an exception", "admission_source_exception"));
    }
  }

  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> CommitAdmission(
      const ComponentLifecycleIntentDTO& intent, const AdmissionFacts& facts,
      std::uint64_t admission_gate_generation, bool& known_unstarted,
      const ComponentLifecycleTargetExpectationDTO* expected_target) {
    known_unstarted = false;
    std::unique_lock admission_lock(admission_mutex_);
    admission_lock.unlock();
    Result<std::optional<CCLCLifecycleRequestResultDTO>> reinspection =
        Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(coordinator_error(
            "component_lifecycle_journal_inspection_failed",
            "the lifecycle journal inspection raised an exception", "journal_exception"));
    try {
      reinspection = journal_.InspectLifecycleRequest(CurrentOwnerEpoch(), intent);
    } catch (...) {
    }
    admission_lock.lock();
    if (!reinspection.ok()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(reinspection.error());
    }
    if (reinspection.value().has_value()) {
      return ReplayResult(*reinspection.value());
    }
    known_unstarted = true;
    admission_lock.unlock();
    const auto native_apply = RequireSettledNativeApply();
    admission_lock.lock();
    if (!native_apply.ok()) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(native_apply.error());
    }
    if (!AdmissionIsOpen() || admission_gate_generation != admission_gate_generation_) {
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
        facts.current_snapshot,
        facts.catalogs};
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
        current = facts.product_context->current_snapshot_source.ReadCurrentComponentSnapshot(
            intent.controllable_component_id);
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
          facts.product_context, prepared.value().initial_status,
          prepared.value().qualified_lifecycle_plan_catalogs,
          prepared.value().reserved_control_domains, facts.current_snapshot);
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

    if (!AdmissionIsOpen() || admission_gate_generation != admission_gate_generation_) {
      rollback_provisional_watermark();
      release_on_failure();
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          coordinator_error("component_lifecycle_admission_closed",
                            "the CCLC admission gate changed before durable persistence",
                            "admission_precommit_gate_changed"));
    }
    const auto semantic_digest = detail::ComponentLifecycleSemanticIntentDigest(intent);
    // An ordinary lifecycle claim is whole-cell and exclusive (idle cell, no
    // other claim), so it carries no resource/configuration scope; the
    // journal persists and recovers it in exactly that form.
    CellAdmissionClaimIntentDTO claim_intent{
        CellAdmissionClaimKindDTO::kComponentLifecycle,
        CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
        CurrentOwnerEpoch(),
        prepared.value().acceptance.lifecycle_transaction_id.value(),
        intent.request_context.request_id,
        ContentDigest(semantic_digest),
        intent.expected_admission_generation,
        {},
        std::nullopt,
        std::nullopt,
        std::nullopt};
    bool persistence_owned = false;
    ScopeExit persistence_guard([&]() noexcept {
      if (!persistence_owned) return;
      try {
        if (!admission_lock.owns_lock()) admission_lock.lock();
        // Any exception after accounting began leaves the durable claim/result
        // untouched and closes ingress before releasing this caller's guard.
        CloseAdmissionLocked();
        admission_persistence_active_ = false;
        persistence_owned = false;
        admission_resolution_ready_.notify_all();
        rollback_provisional_watermark();
        release_on_failure();
      } catch (...) {
        std::terminate();
      }
    });
    admission_persistence_active_ = true;
    persistence_owned = true;
    known_unstarted = false;
    admission_lock.unlock();
    const bool local_output_recovery =
        facts.current_snapshot && !facts.current_snapshot->domains.empty() &&
        std::all_of(facts.current_snapshot->domains.begin(), facts.current_snapshot->domains.end(),
                    [](const auto& domain) { return domain.provider() != nullptr; }) &&
        (intent.intent == ComponentLifecycleIntentKindDTO::kDisable ||
         intent.intent == ComponentLifecycleIntentKindDTO::kPrepare ||
         intent.intent == ComponentLifecycleIntentKindDTO::kEnable);
    const auto acquired = AcquireDurableCellClaim(
        claim_intent,
        [&](const ErrorDTO& error) {
          return journal_.RecordLifecycleRejection(CurrentOwnerEpoch(), intent, error,
                                                   claim_intent);
        },
        nullptr, nullptr, local_output_recovery ? &intent : nullptr);
    if (!acquired.ok()) {
      admission_lock.lock();
      admission_persistence_active_ = false;
      persistence_owned = false;
      admission_resolution_ready_.notify_all();
      rollback_provisional_watermark();
      release_on_failure();
      return Result<CCLCLifecycleRequestResultDTO>::Err(acquired.error());
    }
    runtime->cell_claim = acquired.value();
    Result<CCLCLifecycleRequestResultDTO> accepted =
        Result<CCLCLifecycleRequestResultDTO>::Err(coordinator_error(
            "component_lifecycle_journal_accept_failed",
            "the lifecycle journal acceptance raised an exception", "journal_exception"));
    try {
      accepted = expected_target != nullptr
                     ? journal_.AcceptOrReplayTargetLifecycle(
                           CurrentOwnerEpoch(), intent, prepared.value().acceptance,
                           prepared.value().initial_status, *expected_target, acquired.value())
                     : journal_.AcceptOrReplayLifecycle(CurrentOwnerEpoch(), intent,
                                                        prepared.value().acceptance,
                                                        prepared.value().initial_status);
    } catch (...) {
    }
    if (expected_target != nullptr && accepted.ok() && accepted.value().locally_valid() &&
        accepted.value().replay_status == RequestReplayStatusDTO::kAcceptedNew &&
        accepted.value().rejection.has_value() && !accepted.value().acceptance.has_value() &&
        !accepted.value().status.has_value()) {
      // The journal atomically retained this no-admission refusal and the
      // original acquired claim's proven-no-effect settlement. Keep the
      // admission persistence guard until the actual Kernel receipt is joined.
      const auto& claim = acquired.value();
      const CellAdmissionClaimReleaseDTO release{
          claim.intent.owner_role,
          claim.intent.owner_epoch,
          claim.intent.owner_transaction_id,
          claim.intent.request_id,
          claim.intent.request_payload_digest,
          claim.acquired_admission_generation,
          claim.claim_generation,
          {CellAdmissionSettlementKindDTO::kProvenNoEffect, claim.intent.request_payload_digest}};
      bool released = false;
      try {
        // This caller owns admission_persistence_active_. The common helper
        // must not wait on that same guard to close admission on failure.
        released = ReleaseDurableClaim(runtime, release, false);
      } catch (...) {
        // The accounted caller closes under the existing mutex below.
      }
      admission_lock.lock();
      if (!released) CloseAdmissionLocked();
      admission_persistence_active_ = false;
      persistence_owned = false;
      admission_resolution_ready_.notify_all();
      rollback_provisional_watermark();
      release_on_failure();
      if (!released) {
        return Result<CCLCLifecycleRequestResultDTO>::Err(
            coordinator_error("component_lifecycle_claim_release_unknown",
                              "the refused target lifecycle claim has no exact "
                              "Kernel release receipt",
                              "target_refusal_claim_release_unresolved"));
      }
      // No accepted transaction, worker, component watermark or transaction
      // generation is minted for a stale target. Replay retains this refusal.
      return accepted;
    }
    admission_lock.lock();
    admission_persistence_active_ = false;
    persistence_owned = false;
    admission_resolution_ready_.notify_all();
    if (!accepted.ok()) {
      rollback_provisional_watermark();
      CloseAdmissionLocked();
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
      if (runtime->permanent_containment_only) {
        AcquireOwnerWideContainmentGateLocked(runtime);
      }
      {
        std::lock_guard lock(state_mutex_);
        active_transactions_.push_back(runtime);
      }
      // Disable is an idle lifecycle mutation under the same exact cell
      // claim as Prepare and Enable; it never invokes a Kernel Stop.
      if (!worker_pool_.Submit(
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
      if (runtime->status.apply_bring_up) {
        if (!runtime->apply_continued || runtime->apply_stopped) return;
        const auto pinned = WithApplyParent(runtime, [&]() -> Result<void> {
          if (runtime->apply_holding ||
              runtime->current_domain_index == runtime->status.domains.size())
            SucceedApply(runtime);
          else
            RunCurrentDomain(runtime, cancellation);
          return Result<void>::Ok();
        });
        if (!pinned.ok()) StopApply(runtime, pinned.error());
        return;
      }
      if (!runtime->initialized && !Initialize(runtime)) {
        return;
      }
      RunCurrentDomain(runtime, cancellation);
    } catch (const std::exception& exception) {
      FailBeforeEffect(
          runtime, coordinator_error("component_lifecycle_worker_exception",
                                     "the lifecycle worker caught an exception", exception.what()));
    } catch (...) {
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
    if (runtime->status.status_owner_epoch == CurrentOwnerEpoch()) {
      return true;
    }
    auto adopted = runtime->status;
    AdvanceStatus(runtime, adopted, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown);
    adopted.reconciliation_required = true;
    adopted.error = OwnerReplacementError("recovery_observation_required");
    adopted.remaining_duration_ns = 0;
    if (!adopted.locally_valid() ||
        !journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), adopted).ok()) {
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
    if (!failed.locally_valid() || !PublishTerminalComponentReadiness(runtime, failed).ok()) {
      CloseAdmission();
      return;
    }
    if (!CommitTerminalAndReleaseClaim(runtime, std::move(failed))) {
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
    if (runtime->status.selected_bring_up || runtime->status.apply_bring_up) {
      // The recorded claim/fence describes the predecessor owner's attempt.
      // Replacement can expose it, never resume retirement or bring-up from it.
      // Keep the claim and selected fence for explicit reconciliation.
      if (AdoptRecoveredStatus(runtime)) {
        AcquireUnresolvedEffectAdmissionGate(runtime);
      }
      return;
    }
    if (runtime->unresolved_recovery_effect.has_value()) {
      CompleteRecoveredBrandPrepareAsUncertain(runtime, cancellation);
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
    const auto domain_index =
        static_cast<std::size_t>(pending_domain - runtime->status.domains.begin());
    if (!AdoptRecoveredStatus(runtime)) {
      return;
    }
    ReconcileRecoveredBrandObservation(runtime, domain_index, cancellation);
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
        !journal_.RecordEffectCompleted(CurrentOwnerEpoch(), completed_attempt, completed_status)
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
      catalog =
          runtime->product_context->lifecycle_plan_catalog_source.LoadExactLifecyclePlanCatalog(
              provider.provider_manifest_id, provider.provider_manifest_revision,
              provider.lifecycle_plan_catalog_digest);
      port = runtime->product_context->lifecycle_port_resolver.ResolveExactLifecyclePort(provider);
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
            with_failed_primitive_details(
                coordinator_error("component_lifecycle_primitive_known_no_effect",
                                  "fresh owner-recovery evidence proved no relevant primitive effect",
                                  "state_only_observation_unchanged_after_owner_replacement"),
                runtime->status, domain, transition->lifecycle_primitive_id);
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
        journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), reconciled);
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

  [[nodiscard]] bool Initialize(const std::shared_ptr<RuntimeState>& runtime) {
    // The whole-cell claim is held now. A preclaim snapshot reread alone
    // cannot authorize a provider effect across the claim-acquisition gap.
    if (runtime->status.accepted_intent.intent == ComponentLifecycleIntentKindDTO::kDisable &&
        runtime->accepted_snapshot.has_value()) {
      // Acceptance advances the durable component-generation watermark and
      // retains the pre-effect lifecycle state at that new generation. The
      // snapshot captured before acceptance is therefore expected to differ
      // by exactly this generation step once read back from the journal.
      auto expected = *runtime->accepted_snapshot;
      const auto predecessor_generation =
          expected.component_lifecycle_generation.value();
      const auto accepted_generation =
          runtime->status.acceptance.component_lifecycle_generation.value();
      if (predecessor_generation == std::numeric_limits<std::uint64_t>::max() ||
          accepted_generation != predecessor_generation + 1U) {
        FailBeforeEffect(
            runtime,
            coordinator_error(
                "component_lifecycle_snapshot_generation_invalid",
                "Disable acceptance did not reserve the exact successor "
                "component generation",
                "accepted_generation_not_successor"));
        return false;
      }
      expected.component_lifecycle_generation =
          runtime->status.acceptance.component_lifecycle_generation;
      const auto expected_digest =
          ComputeComponentLifecycleCurrentSnapshotDigest(expected);
      if (!expected_digest.ok()) {
        FailBeforeEffect(
            runtime,
            coordinator_error(
                "component_lifecycle_snapshot_generation_invalid",
                "the accepted Disable snapshot cannot be canonically "
                "recomputed",
                "accepted_snapshot_digest_failed"));
        return false;
      }
      expected.snapshot_digest = expected_digest.value();
      Result<ComponentLifecycleCurrentSnapshotDTO> current =
          Result<ComponentLifecycleCurrentSnapshotDTO>::Err(
              coordinator_error("component_lifecycle_snapshot_reread_failed",
                                "the claimed component snapshot reread raised an exception",
                                "claimed_snapshot_reread_exception"));
      try {
        current = runtime->product_context->current_snapshot_source.ReadCurrentComponentSnapshot(
            runtime->status.accepted_intent.controllable_component_id);
      } catch (...) {
      }
      if (!current.ok() || current.value() != expected) {
        auto error = coordinator_error(
            "component_lifecycle_snapshot_changed_under_claim",
            "the component changed before its claimed lifecycle effect",
            "claimed_snapshot_mismatch");
        if (!current.ok()) {
          error.details.emplace("readback_error_code", current.error().code);
        } else {
          const auto &observed = current.value();
          std::string mismatches;
          const auto note = [&](const std::string_view name,
                                const bool equal) {
            if (equal) return;
            if (!mismatches.empty()) mismatches.push_back(',');
            mismatches.append(name);
          };
          note("component_id", expected.controllable_component_id ==
                                   observed.controllable_component_id);
          note("component_generation", expected.component_lifecycle_generation ==
                                          observed.component_lifecycle_generation);
          note("component_state", expected.component_lifecycle_state ==
                                      observed.component_lifecycle_state);
          note("active_publication", expected.active_publication_receipt ==
                                         observed.active_publication_receipt);
          note("domains", expected.domains == observed.domains);
          note("provider_occurrences", expected.provider_occurrences ==
                                           observed.provider_occurrences);
          note("snapshot_digest", expected.snapshot_digest ==
                                      observed.snapshot_digest);
          error.details.emplace("mismatched_fields", std::move(mismatches));
          error.details.emplace(
              "expected_component_generation",
              std::to_string(expected.component_lifecycle_generation.value()));
          error.details.emplace(
              "observed_component_generation",
              std::to_string(observed.component_lifecycle_generation.value()));
        }
        FailBeforeEffect(runtime, std::move(error));
        return false;
      }
    }
    // Close command eligibility before any adapter work. The held claim
    // already excludes commands; closing first makes every later failure
    // settle command-closed unless a verified result reopens it.
    if (runtime->product_context->component_command_readiness_port != nullptr &&
        !runtime->command_eligibility_closed) {
      Result<ComponentCommandReadinessDTO> closed =
          Result<ComponentCommandReadinessDTO>::Err(coordinator_error(
              "component_lifecycle_readiness_close_failed",
              "the Kernel could not close component command eligibility",
              "close_exception"));
      try {
        closed = runtime->product_context->component_command_readiness_port
                     ->CloseComponentCommandEligibility(
                         runtime->status.acceptance.lifecycle_transaction_id,
                         runtime->status.accepted_intent.controllable_component_id);
      } catch (...) {
      }
      if (!closed.ok() || closed.value().command_eligible ||
          closed.value().controllable_component_id !=
              runtime->status.accepted_intent.controllable_component_id) {
        FailBeforeEffect(runtime, closed.ok()
                                      ? coordinator_error(
                                            "component_lifecycle_readiness_close_failed",
                                            "the Kernel did not read back a closed component",
                                            "close_readback_mismatch")
                                      : closed.error());
        return false;
      }
      runtime->command_eligibility_closed = true;
    }
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
      const auto port = runtime->product_context->lifecycle_port_resolver.ResolveExactLifecyclePort(
          *domain.resolved_provider_occurrence);
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
      // Q127: only activation admission consumes endpoint conflict facts.
      // Observe remains usable for Disable/Recover/Close, and unrelated
      // health-source errors retain their existing admission semantics.
      const auto intent = runtime->status.accepted_intent.intent;
      if (intent == ComponentLifecycleIntentKindDTO::kPrepare ||
          intent == ComponentLifecycleIntentKindDTO::kEnable) {
        const auto health = runtime->product_context->lifecycle_port_resolver
                                .ReadExactLifecycleHealth(
                                    *domain.resolved_provider_occurrence,
                                    observation_received_at_ns);
        if (!health.ok() &&
            (health.error().code == "duplicate_component_instance" ||
             health.error().code == "component_instance_graph_unsettled")) {
          FailBeforeEffect(runtime, health.error());
          return false;
        }
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
                                          std::nullopt};
    const auto prepared =
        journal_.RecordEffectPrepared(CurrentOwnerEpoch(), prepared_attempt, prepared_status);
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
    const auto completed =
        journal_.RecordEffectCompleted(CurrentOwnerEpoch(), completed_attempt, completed_status);
    if (!completed.ok()) {
      CloseAdmission();
      return;
    }
    runtime->status = std::move(completed_status);

    if (runtime->status.apply_bring_up &&
        runtime->status.phase == ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown) {
      StopApply(runtime, runtime->status.error.value_or(ApplyError("dispatch_outcome_unknown")));
      return;
    }

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
          reconciled_domain.error = with_failed_primitive_details(
              coordinator_error(
                  "component_lifecycle_primitive_known_no_effect",
                  "fresh reconciliation proved that the prepared primitive had no relevant effect",
                  "observation_unchanged_after_uncertain_dispatch"),
              runtime->status, domain, runtime->verifying_transition->lifecycle_primitive_id);
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
      const auto recorded = journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), reconciled);
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
    UpdateApplyInitialAggregate(progressed);
    progressed_domain.error.reset();
    const auto next = EvaluateLifecycleDomainPlan(
        progressed.accepted_intent.intent, *progressed.requested_state.state,
        runtime->status.apply_bring_up ? *runtime->apply_domain_catalogs[index]
                                     : (*runtime->qualified_catalogs)[index], observation.value());
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
      const auto recorded = journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), progressed);
      if (!recorded.ok()) {
        CloseAdmission();
        return;
      }
      runtime->status = std::move(progressed);
      ++runtime->current_domain_index;
      while (runtime->current_domain_index < runtime->status.domains.size() &&
             (runtime->status.domains[runtime->current_domain_index].action_state ==
                  ComponentLifecycleDomainActionStateDTO::kSucceeded ||
              (runtime->status.apply_bring_up &&
               !runtime->status.domains[runtime->current_domain_index].expected_domain))) {
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
    status.status_owner_epoch = CurrentOwnerEpoch();
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

  /// Publish the complete component result to the Kernel before durable
  /// settlement. Eligibility opens only for a verified Active result; a known
  /// no-effect failure keeps its predecessor state; any other failure is
  /// closed. Nothing is published when this transaction never closed the
  /// component (a preflight refusal leaves the predecessor unchanged).
  [[nodiscard]] Result<void> PublishTerminalComponentReadiness(
      const std::shared_ptr<RuntimeState>& runtime, const ComponentLifecycleStatusDTO& status) {
    if (runtime->product_context->component_command_readiness_port == nullptr ||
        (!runtime->command_eligibility_closed && !runtime->recovering)) {
      return Result<void>::Ok();
    }
    const bool provider_component =
        std::any_of(status.domains.begin(), status.domains.end(), [](const auto& domain) {
          return domain.current_scope && domain.current_scope->provider();
        });
    std::optional<ComponentLifecycleCurrentSnapshotDTO> provider_snapshot;
    std::vector<BrandLifecycleObservationDTO> provider_observations;
    if (provider_component) {
      try {
        // Read the actual durable membership/generation; do not project the
        // not-yet-committed terminal aggregate state into this snapshot.
        const auto current =
            runtime->product_context->current_snapshot_source.ReadCurrentComponentSnapshot(
                status.accepted_intent.controllable_component_id);
        if (!current.ok()) return Result<void>::Err(current.error());
        const auto verified = VerifyComponentLifecycleCurrentSnapshot(current.value());
        if (!verified.ok()) return Result<void>::Err(verified.error());
        if (current.value().controllable_component_id !=
                status.accepted_intent.controllable_component_id ||
            current.value().component_lifecycle_generation !=
                status.acceptance.component_lifecycle_generation)
          return Result<void>::Err(coordinator_error(
              "component_lifecycle_readiness_publication_failed",
              "actual provider snapshot does not match the held lifecycle generation",
              "provider_snapshot_identity_mismatch"));
        provider_snapshot = current.value();
        for (const auto& observation : runtime->observations) {
          if (observation && std::any_of(provider_snapshot->domains.begin(),
                                         provider_snapshot->domains.end(), [&](const auto& domain) {
                                           return domain.provider() &&
                                                  domain.control_domain_id ==
                                                      observation->control_domain_id;
                                         }))
            provider_observations.push_back(*observation);
        }
      } catch (...) {
        return Result<void>::Err(
            coordinator_error("component_lifecycle_readiness_publication_failed",
                              "actual provider publication proof could not be captured",
                              "provider_snapshot_read_exception"));
      }
    }
    // Retirement and terminal publication share the existing CCLC exclusion.
    // SQL completes outside Kernel state; this local exclusion ends before the
    // caller's failure path may call CloseAdmission on the same mutex.
    std::unique_lock retirement_lock(admission_mutex_);
    const auto retained =
        journal_.ReadLifecycleTargetRetention(status.accepted_intent.controllable_component_id);
    if (!retained.ok()) return Result<void>::Err(retained.error());
    if (retained.value() && (!retained.value()->locally_valid() ||
                             retained.value()->target.controllable_component_id !=
                                 status.accepted_intent.controllable_component_id)) {
      return Result<void>::Err(
          coordinator_error("component_lifecycle_target_retirement_proof_invalid",
                            "terminal readiness has no valid exact retained target proof",
                            "retirement_proof_invalid"));
    }
    const bool closed_by_retirement =
        retained.value() &&
        retained.value()->retirement_reason == LifecycleTargetRetirementReasonDTO::kClose;
    const bool eligible =
        !closed_by_retirement && status.terminal_result.has_value() &&
        *status.terminal_result != ComponentLifecycleTerminalResultDTO::kFailedPartialEffect &&
        status.current_state == ComponentLifecycleStateDTO::kActive;
    const ComponentCommandReadinessPublicationDTO publication{
        ComponentReadinessAuthorityDTO::kLifecycleClaim,
        status.acceptance.lifecycle_transaction_id,
        std::nullopt,
        status.accepted_intent.controllable_component_id,
        status.acceptance.component_lifecycle_generation, eligible};
    Result<ComponentCommandReadinessDTO> published =
        Result<ComponentCommandReadinessDTO>::Err(coordinator_error(
            "component_lifecycle_readiness_publication_failed",
            "the Kernel component readiness publication raised an exception",
            "publication_exception"));
    try {
      published = provider_snapshot
                      ? runtime->product_context->component_command_readiness_port
                            ->PublishComponentCommandReadiness(
                                publication, *provider_snapshot, provider_observations)
                      : runtime->product_context->component_command_readiness_port
                            ->PublishComponentCommandReadiness(publication);
    } catch (...) {
    }
    if (!published.ok() && !runtime->command_eligibility_closed) {
      // Recovery after owner replacement may no longer hold a claim in the
      // new Kernel. Settle only on a verified closed readback; otherwise the
      // claim is retained and admission closes.
      Result<ComponentCommandReadinessDTO> readback =
          Result<ComponentCommandReadinessDTO>::Err(published.error());
      try {
        readback = runtime->product_context->component_command_readiness_port
                       ->ReadComponentCommandReadiness(publication.controllable_component_id);
      } catch (...) {
      }
      if (readback.ok() && !readback.value().command_eligible &&
          readback.value().controllable_component_id == publication.controllable_component_id) {
        return Result<void>::Ok();
      }
      return Result<void>::Err(published.error());
    }
    if (!published.ok()) {
      return Result<void>::Err(published.error());
    }
    if (published.value().controllable_component_id != publication.controllable_component_id ||
        published.value().component_lifecycle_generation !=
            publication.component_lifecycle_generation ||
        published.value().command_eligible != eligible) {
      return Result<void>::Err(coordinator_error(
          "component_lifecycle_readiness_publication_failed",
          "the Kernel component readiness readback differs from the lifecycle result",
          "readiness_readback_mismatch"));
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] bool CommitTerminalAndReleaseClaim(const std::shared_ptr<RuntimeState>& runtime,
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
         ContentDigest(detail::ComponentLifecycleStatusReleasePreconditionDigest(status))}};
    const auto retained =
        journal_.RecordLifecycleTerminalAndClaimRelease(CurrentOwnerEpoch(), status, release);
    if (!retained.ok()) {
      CloseAdmission();
      return false;
    }
    runtime->status = std::move(status);
    runtime->cell_claim_release = release;
    return ReleaseDurableClaim(runtime, release);
  }

  template <typename Runtime>
  [[nodiscard]] bool ReleaseDurableClaim(const std::shared_ptr<Runtime>& runtime,
                                         const CellAdmissionClaimReleaseDTO& release,
                                         bool close_admission_on_failure = true) {
    if (!runtime->cell_claim.has_value()) {
      if (close_admission_on_failure) CloseAdmission();
      return false;
    }
    const auto& claim = *runtime->cell_claim;
    try {
      // The exact retained receipt below is authoritative even if this reply
      // is lost after the Kernel has durably released the claim.
      static_cast<void>(kernel_cell_admission_port_.ReleaseCellAdmissionClaim(release));
    } catch (...) {
    }
    const CellAdmissionRecoveryReceiptQueryDTO query{claim.intent, claim, release};
    Result<CellAdmissionRecoveryReceiptDTO> receipt =
        Result<CellAdmissionRecoveryReceiptDTO>::Err(coordinator_error(
            "component_lifecycle_claim_release_unknown",
            "Kernel lifecycle release receipt raised an exception", "claim_receipt_exception"));
    try {
      receipt = kernel_cell_admission_port_.GetCellAdmissionRecoveryReceipt(query);
    } catch (...) {
    }
    if (!receipt.ok() ||
        receipt.value().status != CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact ||
        !journal_.RecordCellClaimReleaseReceipt(CurrentOwnerEpoch(), release, receipt.value())
             .ok()) {
      if (close_admission_on_failure) CloseAdmission();
      return false;
    }
    return true;
  }

  void FailTransaction(const std::shared_ptr<RuntimeState>& runtime, const ErrorDTO& error) {
    if (runtime->status.apply_bring_up && !runtime->recovering) {
      StopApply(runtime, error);
      return;
    }
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
      if (journal_.RecordLifecycleStatus(CurrentOwnerEpoch(), status).ok()) {
        runtime->status = std::move(status);
        SchedulePoll(runtime);
      } else {
        CloseAdmission();
      }
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
    if (!status.locally_valid()) {
      return;
    }
    // Release after a known failure requires verified containment: the
    // Kernel must read back the settled result. Otherwise keep the claim.
    if (!PublishTerminalComponentReadiness(runtime, status).ok()) {
      CloseAdmission();
      return;
    }
    if (!CommitTerminalAndReleaseClaim(runtime, std::move(status))) {
      return;
    }
    Complete(runtime);
  }

  void Succeed(const std::shared_ptr<RuntimeState>& runtime) {
    if (runtime->status.apply_bring_up && !runtime->recovering) {
      SucceedApply(runtime);
      return;
    }
    auto status = runtime->status;
    AdvanceStatus(runtime, status, ComponentLifecycleTransactionPhaseDTO::kSucceeded);
    status.current_state = status.requested_state.state;
    status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
    status.error.reset();
    status.reconciliation_required = false;
    status.remaining_duration_ns = 0;
    if (!status.locally_valid()) {
      return;
    }
    // Success requires the verified Kernel result. Otherwise this is a
    // known failure after effects, which settles command-closed.
    if (auto published = PublishTerminalComponentReadiness(runtime, status); !published.ok()) {
      FailTransaction(runtime, published.error());
      return;
    }
    if (!CommitTerminalAndReleaseClaim(runtime, std::move(status))) {
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
          !shutting_down_.load(std::memory_order_acquire) && !AdmissionIsOpen()) {
        if (!ReconcileLifecycleOwnerEpoch(CurrentOwnerEpoch())) {
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
  }

  void CloseAdmissionLocked() noexcept {
    admission_fault_closed_ = true;
    accepting_.store(false, std::memory_order_release);
    ++admission_gate_generation_;
    // Sticky faults stop fresh work even if an exchange attempt is between
    // its external proof reads. The next attempt withdraws its prepared root.
    product_control_->exchange_pending = false;
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
        !shutting_down_.load(std::memory_order_acquire) && !AdmissionIsOpen()) {
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
            ScopeExit finished([&] { fired->store(true, std::memory_order_release); });
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
  // One synchronized namespace value; effects keep their exact persisted
  // identities, and rotation is permitted only at the ordinary quiescent gate.
  std::atomic<std::shared_ptr<const RequestOwnerEpoch>> request_epoch_;
  ICCLCJournal& journal_;
  CCLCDomainReservationTable& domain_reservations_;
  // Reuses the existing admission exclusion and wait notifications. The
  // accounting shell can outlive Impl without retaining any owner authority.
  const std::shared_ptr<ProductControl> product_control_;
  IKernelCellAdmissionPort& kernel_cell_admission_port_;
  IClock& clock_;
  IScheduler& scheduler_;
  FixedWorkerPool worker_pool_;

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

  std::mutex& admission_mutex_ = product_control_->mutex;
  std::condition_variable& admission_resolution_ready_ = product_control_->ready;
  bool admission_resolution_active_ = false;
  bool admission_persistence_active_ = false;
  std::uint64_t highest_transaction_generation_ = 0U;
  std::uint64_t highest_route_transaction_generation_ = 0U;
  std::uint64_t admission_gate_generation_ = 0U;
  std::size_t owner_wide_containment_gate_count_ = 0U;
  std::size_t unresolved_effect_admission_gate_count_ = 0U;
  std::atomic<bool> admission_fault_closed_{false};
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
    std::shared_ptr<const ComponentLifecycleProductContext> product_context,
    IKernelCellAdmissionPort& kernel_cell_admission_port, IClock& clock, IScheduler& scheduler)
    : impl_(product_context
                ? std::make_unique<Impl>(std::move(config), journal, domain_reservations,
                                         std::move(product_context), kernel_cell_admission_port,
                                         clock, scheduler)
                : nullptr) {
  if (!impl_) throw std::invalid_argument("CCLC product context is absent");
}

ComponentLifecycleCoordinator::ComponentLifecycleCoordinator(
    ComponentLifecycleCoordinatorConfig config, ICCLCJournal& journal,
    CCLCDomainReservationTable& domain_reservations,
    const ICCLCCurrentComponentSnapshotSource& current_snapshot_source,
    const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source,
    const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver,
    IKernelCellAdmissionPort& kernel_cell_admission_port, IClock& clock, IScheduler& scheduler,
    std::optional<ComponentLifecycleCoordinatorRouteServices> route_services,
    ICCLCComponentCommandReadinessPort* component_command_readiness_port)
    : ComponentLifecycleCoordinator(
          std::move(config), journal, domain_reservations,
          std::make_shared<const ComponentLifecycleProductContext>(ComponentLifecycleProductContext{
              current_snapshot_source, lifecycle_plan_catalog_source, lifecycle_port_resolver,
              std::move(route_services), component_command_readiness_port}),
          kernel_cell_admission_port, clock, scheduler) {}

ComponentLifecycleCoordinator::~ComponentLifecycleCoordinator() = default;

Result<void> ComponentLifecycleCoordinator::Start() {
  return impl_->Start();
}

Result<IoOutputLocalResynchronizationProofDTO>
ComponentLifecycleCoordinator::ReadIoOutputLocalResynchronization(
    const RuntimeParticipantBindingDTO& captured_binding) const {
  return impl_->ReadIoOutputLocalResynchronization(captured_binding);
}

Result<CCLCComponentReadDTO> ComponentLifecycleCoordinator::ReadCurrentComponentView(
    const ControllableComponentId& component,
    const std::function<Result<ComponentLifecycleTargetViewDTO>()>& read_target,
    const std::function<Result<ComponentLifecycleReadinessTargetFactsDTO>()>& read_target_facts) const {
  return impl_->ReadCurrentComponentView(component, read_target, read_target_facts);
}

Result<CCLCLifecycleTargetRetirementOfferDTO> ComponentLifecycleCoordinator::ReadLifecycleTargetRetirementOffer() const {
  return impl_->ReadLifecycleTargetRetirementOffer();
}
Result<CCLCLifecycleTargetRetirementResultDTO> ComponentLifecycleCoordinator::RetireLifecycleTargetsForClose(
    const CCLCLifecycleTargetRetirementIntentDTO& intent) {
  return impl_->RetireLifecycleTargetsForClose(intent);
}
Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>> ComponentLifecycleCoordinator::ReadLifecycleTargetRetirementResult(
    const CCLCLifecycleTargetRetirementIntentDTO& intent) const {
  return impl_->ReadLifecycleTargetRetirementResult(intent);
}
Result<void> ComponentLifecycleCoordinator::ReadLifecycleOwnerSettlement(
    const CCLCLifecycleTargetRetirementScopeDTO& scope) const {
  return impl_->ReadLifecycleOwnerSettlement(scope);
}

Result<ComponentLifecycleProductPin> ComponentLifecycleCoordinator::AcquireProductPin() const {
  return impl_->AcquireProductPin();
}

bool ComponentLifecycleCoordinator::WithdrawProductExchange(
    const std::shared_ptr<const ComponentLifecycleProductContext>& previous,
    const std::shared_ptr<const ComponentLifecycleProductContext>& successor,
    const RequestOwnerEpoch& captured_owner) {
  return impl_->WithdrawProductExchange(previous, successor, captured_owner);
}

void ComponentLifecycleCoordinator::CloseProductExchangeAdmission() noexcept {
  impl_->CloseProductExchangeAdmission();
}

Result<ComponentLifecycleProductExchangeObservation>
ComponentLifecycleCoordinator::TryExchangeProductContext(
    const std::shared_ptr<const ComponentLifecycleProductContext>& expected_previous,
    const std::shared_ptr<const ComponentLifecycleProductContext>& prepared_successor,
    const RequestOwnerEpoch& expected_owner,
    const ActiveBundlePublicationReceiptDTO& expected_previous_publication) {
  try {
    return impl_->TryExchangeProductContext(expected_previous, prepared_successor, expected_owner,
                                            expected_previous_publication);
  } catch (...) {
    // A pending request cannot silently survive failed accounting/allocation.
    // Actual context truth is retained; no rollback, owner rotation or retry.
    impl_->CloseProductExchangeAdmission();
    return Result<ComponentLifecycleProductExchangeObservation>::Err(coordinator_error(
        "cclc_product_exchange_unknown", "CCLC product exchange raised an exception",
        "product_exchange_exception"));
  }
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::SubmitLifecycle(
    const ComponentLifecycleIntentDTO& intent) {
  return impl_->SubmitLifecycle(intent);
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::SubmitLifecycle(
    const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleProductPin& product_pin) {
  return impl_->SubmitLifecycle(intent, &product_pin);
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::SubmitTargetLifecycle(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleTargetExpectationDTO& expected_target) {
  return impl_->SubmitLifecycle(intent, nullptr, &expected_target);
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::SubmitTargetLifecycle(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleTargetExpectationDTO& expected_target,
    const ComponentLifecycleProductPin& product_pin) {
  return impl_->SubmitLifecycle(intent, &product_pin, &expected_target);
}

Result<void> ComponentLifecycleCoordinator::ReadLifecycleOwnerSettlement() const {
  return impl_->ReadLifecycleOwnerSettlement();
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
ComponentLifecycleCoordinator::ReadLifecycleRequestResult(
    const ComponentLifecycleIntentDTO& intent) {
  return impl_->ReadLifecycleRequestResult(intent);
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
ComponentLifecycleCoordinator::ReadSelectedComponentBringUpRequestResult(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& stable_digest) {
  return impl_->ReadSelectedComponentBringUpRequestResult(intent, stable_digest);
}

Result<std::optional<CCLCLifecycleRequestResultDTO>>
ComponentLifecycleCoordinator::ReadApplyComponentBringUpRequestResult(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& stable_digest) {
  return impl_->ReadApplyComponentBringUpRequestResult(intent, stable_digest);
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::AcceptApplyComponentBringUp(
    const ComponentLifecycleIntentDTO& intent, const ApplyComponentBringUpEvidenceDTO& evidence,
    const std::optional<ComponentLifecycleCurrentSnapshotDTO>& genuine_predecessor,
    const std::vector<BrandLifecyclePlanCatalogDTO>& candidate_catalogs,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& qualified_candidate_domains,
    const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& candidate_provider_occurrences,
    ComponentLifecycleCoordinatorApplyBringUpServices services) {
  return impl_->AcceptApplyComponentBringUp(intent, evidence, genuine_predecessor,
                                            candidate_catalogs, qualified_candidate_domains,
                                            candidate_provider_occurrences, std::move(services));
}

Result<InstalledProviderCandidateFactsDTO>
ComponentLifecycleCoordinator::ReadInstalledApplyCandidateFacts(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& digest,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& domains,
    const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& providers,
    const std::vector<BrandLifecyclePortRegistrationDTO>& registrations) {
  return impl_->ReadInstalledApplyCandidateFacts(intent, digest, domains, providers, registrations);
}

Result<CCLCLifecycleRequestResultDTO> ComponentLifecycleCoordinator::ContinueApplyComponentBringUp(
    const ComponentLifecycleIntentDTO& intent, const ContentDigest& stable_digest,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& installed_candidate_domains,
    const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& installed_candidate_providers,
    const std::vector<BrandLifecyclePortRegistrationDTO>& installed_candidate_ports,
    const std::vector<ControlDomainId>& retired_known_no_effect_domains,
    ICCLCComponentCommandReadinessPort* installed_candidate_readiness_port) {
  return impl_->ContinueApplyComponentBringUp(
      intent, stable_digest, installed_candidate_domains, installed_candidate_providers,
      installed_candidate_ports, retired_known_no_effect_domains,
      installed_candidate_readiness_port);
}

Result<CCLCRouteSelectionRequestResultDTO> ComponentLifecycleCoordinator::SubmitRouteSelection(
    const RouteSelectionIntentDTO& intent) {
  return impl_->SubmitRouteSelection(intent);
}

Result<CCLCRouteSelectionRequestResultDTO> ComponentLifecycleCoordinator::SubmitRouteRecovery(
    const RouteSelectionIntentDTO& intent, const ActiveControlConfigurationDTO& predecessor) {
  return impl_->SubmitRouteRecovery(intent, predecessor);
}

Result<std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>>
ComponentLifecycleCoordinator::ObserveRouteConfiguration(
    const RetainedBundleId& retained_bundle_id, const GraphRevision& graph_revision,
    const DesiredControlConfigurationDTO& desired_configuration,
    std::optional<ExpectedActiveControlConfigurationDTO> expected_current) {
  return impl_->ObserveRouteConfiguration(retained_bundle_id, graph_revision, desired_configuration,
                                          expected_current);
}

Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
ComponentLifecycleCoordinator::ReadRouteSelectionRequestResult(
    const RouteSelectionIntentDTO& intent) {
  return impl_->ReadRouteSelectionRequestResult(intent);
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

bool ComponentLifecycleCoordinator::admission_fault_closed() const noexcept {
  return impl_->admission_fault_closed();
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
