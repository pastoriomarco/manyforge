#include <manyforge_roboplan/retained_native_brand_lifecycle_port.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ErrorDTO port_error(std::string code,
                                        std::string message,
                                        std::string reason) {
  return {std::move(code), std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] std::vector<core::LifecycleFactDTO>
predicate_facts(const core::LifecyclePredicateDTO &predicate) {
  std::vector<core::LifecycleFactDTO> facts;
  facts.reserve(predicate.fact_predicates.size());
  for (const auto &fact : predicate.fact_predicates) {
    facts.push_back({fact.lifecycle_fact_id, fact.expected_value});
  }
  return facts;
}

void augment_unambiguous_next_facts(
    const core::BrandLifecyclePlanCatalogDTO &catalog,
    const core::ComponentLifecycleStateDTO state,
    std::vector<core::LifecycleFactDTO> &facts) {
  std::map<core::LifecycleFactId,
           std::optional<core::LifecycleFactValueDTO>> candidates;
  for (const auto &plan : catalog.plans) {
    for (const auto &transition : plan.transitions) {
      if (!std::binary_search(
              transition.eligibility_predicate.allowed_states.begin(),
              transition.eligibility_predicate.allowed_states.end(), state)) {
        continue;
      }
      for (const auto &predicate :
           transition.eligibility_predicate.fact_predicates) {
        const auto existing = std::lower_bound(
            facts.begin(), facts.end(), predicate.lifecycle_fact_id,
            [](const core::LifecycleFactDTO &candidate,
               const core::LifecycleFactId &id) {
              return candidate.lifecycle_fact_id < id;
            });
        if (existing != facts.end() &&
            existing->lifecycle_fact_id == predicate.lifecycle_fact_id) {
          continue;
        }
        const auto [found, inserted] = candidates.emplace(
            predicate.lifecycle_fact_id, predicate.expected_value);
        if (!inserted && found->second.has_value() &&
            *found->second != predicate.expected_value) {
          found->second.reset();
        }
      }
    }
  }
  for (auto &[id, value] : candidates) {
    if (value.has_value()) {
      facts.push_back({std::move(id), std::move(*value)});
    }
  }
  std::sort(facts.begin(), facts.end(),
            [](const auto &left, const auto &right) {
              return left.lifecycle_fact_id < right.lifecycle_fact_id;
            });
}

[[nodiscard]] bool exact_occurrence(
    const core::ResolvedLifecycleProviderOccurrenceDTO &occurrence,
    const core::BrandLifecycleObservationQueryDTO &query) {
  return occurrence.control_domain_id == query.control_domain_id &&
         occurrence.control_domain_revision == query.control_domain_revision &&
         occurrence.provider_instance_id == query.provider_instance_id &&
         occurrence.provider_manifest_id == query.provider_manifest_id &&
         occurrence.provider_manifest_revision ==
             query.provider_manifest_revision &&
         occurrence.provider_runtime_unit_id ==
             query.provider_runtime_unit_id &&
         occurrence.provider_runtime_epoch == query.provider_runtime_epoch;
}

[[nodiscard]] bool exact_occurrence(
    const core::ResolvedLifecycleProviderOccurrenceDTO &occurrence,
    const core::BrandLifecyclePrimitiveIntentDTO &intent) {
  return occurrence.control_domain_id == intent.control_domain_id &&
         occurrence.control_domain_revision == intent.control_domain_revision &&
         occurrence.provider_instance_id == intent.provider_instance_id &&
         occurrence.provider_manifest_id == intent.provider_manifest_id &&
         occurrence.provider_manifest_revision ==
             intent.provider_manifest_revision &&
         occurrence.provider_runtime_unit_id ==
             intent.provider_runtime_unit_id &&
         occurrence.provider_runtime_epoch == intent.provider_runtime_epoch;
}

} // namespace

struct RetainedNativeBrandLifecyclePort::State final {
  State(core::ResolvedLifecycleProviderOccurrenceDTO occurrence_in,
        core::BrandLifecyclePlanCatalogDTO catalog_in,
        core::IClock &clock_in, const core::Duration freshness_ns_in,
        std::vector<core::LifecycleFactDTO> facts_in,
        core::ComponentLifecycleStateDTO observed_state_in,
        std::function<core::Result<void>()> verify_engine_in,
        std::function<core::Result<bool>()> route_ready_in)
      : occurrence(std::move(occurrence_in)),
        catalog(std::move(catalog_in)), clock(&clock_in),
        freshness_ns(freshness_ns_in),
        observed_state(observed_state_in),
        facts(std::move(facts_in)),
        verify_engine(std::move(verify_engine_in)),
        route_ready(std::move(route_ready_in)) {}

  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
  core::BrandLifecyclePlanCatalogDTO catalog;
  core::IClock *clock;
  core::Duration freshness_ns;
  core::ComponentLifecycleStateDTO observed_state;
  std::vector<core::LifecycleFactDTO> facts;
  std::function<core::Result<void>()> verify_engine;
  std::function<core::Result<bool>()> route_ready;
  std::uint64_t source_sequence{1U};
  std::uint64_t observation_generation{1U};
  std::mutex mutex;
};

RetainedNativeBrandLifecyclePort::RetainedNativeBrandLifecyclePort(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}

core::Result<std::shared_ptr<RetainedNativeBrandLifecyclePort>>
RetainedNativeBrandLifecyclePort::Create(
    core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
    core::BrandLifecyclePlanCatalogDTO verified_catalog, core::IClock &clock,
    const core::Duration observation_freshness_ns,
    std::function<core::Result<void>()> verify_engine,
    std::function<core::Result<bool>()> route_ready,
    const std::optional<core::ComponentLifecycleStateDTO> retained_state,
    bool allow_closed_active_during_route_recovery) {
  const auto verified =
      core::VerifyBrandLifecyclePlanCatalog(verified_catalog);
  if (!provider_occurrence.locally_valid() || !verified.ok() ||
      verified_catalog.provider_manifest_id !=
          provider_occurrence.provider_manifest_id ||
      verified_catalog.provider_manifest_revision !=
          provider_occurrence.provider_manifest_revision ||
      verified_catalog.catalog_digest !=
          provider_occurrence.lifecycle_plan_catalog_digest ||
      observation_freshness_ns <= 0 ||
      !verify_engine || !route_ready ||
      observation_freshness_ns >
          core::kMaxLifecycleTransactionTimeoutNs) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        port_error("native_brand_lifecycle_profile_invalid",
                   "the retained lifecycle profile is not an exact bounded "
                   "provider occurrence",
                   "profile_or_occurrence_invalid"));
  }

  const auto engine = verify_engine();
  if (!engine.ok()) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        engine.error());
  }
  const auto ready = route_ready();
  if (!ready.ok()) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        ready.error());
  }
  // Without a retained CCLC fact the constructed engine is only Prepared:
  // route readiness never implies that a component was enabled.
  const auto initial_state =
      retained_state.value_or(core::ComponentLifecycleStateDTO::kPrepared);
  const bool closed_active_recovery = allow_closed_active_during_route_recovery &&
      initial_state == core::ComponentLifecycleStateDTO::kActive && !ready.value();
  if (!closed_active_recovery &&
      initial_state == core::ComponentLifecycleStateDTO::kActive && !ready.value()) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        port_error("native_brand_lifecycle_retained_state_mismatch",
                   "a retained Active component has no current native route",
                   "retained_active_route_not_ready"));
  }
  const core::LifecyclePredicateDTO *initial = nullptr;
  for (const auto &plan : verified_catalog.plans) {
    if (plan.requested_state == initial_state) {
      if (initial != nullptr && *initial != plan.requested_postcondition) {
        return core::Result<
            std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
            port_error("native_brand_lifecycle_initial_state_ambiguous",
                       "the retained lifecycle profile has multiple actual "
                       "initial predicates", "initial_predicate_ambiguous"));
      }
      initial = &plan.requested_postcondition;
    }
  }
  if (initial == nullptr) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        port_error("native_brand_lifecycle_initial_state_missing",
                   "the retained lifecycle profile has no actual initial "
                   "predicate",
                   "initial_predicate_missing"));
  }
  auto facts = predicate_facts(*initial);
  augment_unambiguous_next_facts(verified_catalog, initial_state, facts);
  auto state = std::make_shared<State>(
      std::move(provider_occurrence), std::move(verified_catalog), clock,
      observation_freshness_ns, std::move(facts), initial_state,
      std::move(verify_engine), std::move(route_ready));
  return core::Result<
      std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Ok(
      std::shared_ptr<RetainedNativeBrandLifecyclePort>(
          new RetainedNativeBrandLifecyclePort(std::move(state))));
}

core::Result<core::BrandLifecycleObservationDTO>
RetainedNativeBrandLifecyclePort::Observe(
    const core::BrandLifecycleObservationQueryDTO &query) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto engine = state_->verify_engine();
  if (!engine.ok()) {
    return core::Result<core::BrandLifecycleObservationDTO>::Err(
        engine.error());
  }
  const auto route = state_->route_ready();
  if (!route.ok()) {
    return core::Result<core::BrandLifecycleObservationDTO>::Err(
        route.error());
  }
  // Active native readiness requires a current route. Command eligibility
  // is the Kernel's separate fact and is not observed here.
  if (state_->observed_state == core::ComponentLifecycleStateDTO::kActive &&
      !route.value()) {
    return core::Result<core::BrandLifecycleObservationDTO>::Err(port_error(
        "native_brand_lifecycle_route_state_changed",
        "an Active native provider no longer has a current route",
        "active_route_not_ready"));
  }
  const auto now = state_->clock->now();
  if (!query.locally_valid() || !exact_occurrence(state_->occurrence, query) ||
      now < 0 || now > std::numeric_limits<core::TimePoint>::max() -
                            state_->freshness_ns ||
      state_->source_sequence == std::numeric_limits<std::uint64_t>::max()) {
    return core::Result<core::BrandLifecycleObservationDTO>::Err(port_error(
        "native_brand_lifecycle_observation_invalid",
        "the lifecycle observation query or clock state is invalid",
        "query_clock_or_sequence_invalid"));
  }
  const auto sequence = state_->source_sequence++;
  return core::Result<core::BrandLifecycleObservationDTO>::Ok(
      {query.lifecycle_transaction_id,
       query.lifecycle_transaction_generation,
       query.controllable_component_id,
       query.control_domain_id,
       query.control_domain_revision,
       query.provider_instance_id,
       query.provider_manifest_id,
       query.provider_manifest_revision,
       query.provider_runtime_unit_id,
       query.provider_runtime_epoch,
       sequence,
       state_->observation_generation,
       now,
       now,
       now + state_->freshness_ns,
       state_->observed_state,
       state_->facts});
}

core::BrandLifecyclePrimitiveResultDTO
RetainedNativeBrandLifecyclePort::Dispatch(
    const core::BrandLifecyclePrimitiveIntentDTO &intent,
    const std::stop_token cancellation) {
  const auto result = [&](const core::LifecyclePrimitiveDispatchStateDTO state,
                          const core::LifecyclePrimitiveEffectDTO effect,
                          std::optional<core::LifecyclePrimitiveKnownResultDTO>
                              known,
                          std::optional<core::ErrorDTO> error) {
    return core::BrandLifecyclePrimitiveResultDTO{
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
        state,
        effect,
        known,
        std::move(error)};
  };

  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto engine = state_->verify_engine();
  if (!engine.ok()) {
    return result(core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
                  core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
                  engine.error());
  }
  if (!intent.locally_valid() ||
      !exact_occurrence(state_->occurrence, intent) ||
      cancellation.stop_requested() ||
      intent.expected_observation_generation !=
          state_->observation_generation) {
    return result(
        core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
        core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
        port_error("native_brand_lifecycle_dispatch_rejected",
                   "the lifecycle primitive was not dispatched",
                   cancellation.stop_requested()
                       ? "cancelled"
                       : "intent_or_observation_generation_invalid"));
  }
  const auto target = [&]() -> std::optional<core::ComponentLifecycleStateDTO> {
    if (intent.lifecycle_primitive_id ==
        core::LifecyclePrimitiveId("native.control.prepare")) {
      return core::ComponentLifecycleStateDTO::kPrepared;
    }
    if (intent.lifecycle_primitive_id ==
        core::LifecyclePrimitiveId("native.control.enable")) {
      return core::ComponentLifecycleStateDTO::kActive;
    }
    if (intent.lifecycle_primitive_id ==
        core::LifecyclePrimitiveId("native.control.disable")) {
      return core::ComponentLifecycleStateDTO::kDisabled;
    }
    return std::nullopt;
  }();
  const auto plan = std::find_if(state_->catalog.plans.begin(),
                                 state_->catalog.plans.end(),
                                 [&](const auto& candidate) {
    return target && candidate.requested_state == *target &&
           candidate.transitions.size() == 1U &&
           candidate.transitions.front().lifecycle_primitive_id ==
               intent.lifecycle_primitive_id;
  });
  if (plan == state_->catalog.plans.end() ||
      !std::binary_search(
          plan->transitions.front().eligibility_predicate.allowed_states.begin(),
          plan->transitions.front().eligibility_predicate.allowed_states.end(),
          state_->observed_state)) {
    return result(core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
                  core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
                  port_error("native_brand_lifecycle_transition_ineligible",
                             "native lifecycle transition is not eligible from current state",
                             "state_or_primitive_mismatch"));
  }
  // The native provider's readiness is its constructed engine (verified
  // above) and, for Active, its current routes. Command eligibility is not
  // changed here: CCLC publishes the complete component result to the
  // Kernel only after every domain's postcondition holds.
  if (*target == core::ComponentLifecycleStateDTO::kActive) {
    const auto ready = state_->route_ready();
    if (!ready.ok()) {
      return result(core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
                    core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
                    ready.error());
    }
    if (!ready.value()) {
      return result(core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
                    core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
                    port_error("native_lifecycle_route_not_current",
                               "Enable requires a current native route",
                               "route_not_current"));
    }
  }
  state_->observed_state = *target;
  state_->facts = predicate_facts(plan->requested_postcondition);
  augment_unambiguous_next_facts(state_->catalog, *target, state_->facts);
  ++state_->observation_generation;
  return result(core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
                core::LifecyclePrimitiveEffectDTO::kObserved,
                core::LifecyclePrimitiveKnownResultDTO::kSucceeded,
                std::nullopt);
}

} // namespace manyforge::roboplan_backend
