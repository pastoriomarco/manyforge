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
        std::vector<core::LifecycleFactDTO> facts_in)
      : occurrence(std::move(occurrence_in)),
        catalog(std::move(catalog_in)), clock(&clock_in),
        freshness_ns(freshness_ns_in),
        observed_state(core::ComponentLifecycleStateDTO::kDeclared),
        facts(std::move(facts_in)) {}

  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
  core::BrandLifecyclePlanCatalogDTO catalog;
  core::IClock *clock;
  core::Duration freshness_ns;
  core::ComponentLifecycleStateDTO observed_state;
  std::vector<core::LifecycleFactDTO> facts;
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
    const core::Duration observation_freshness_ns) {
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
      observation_freshness_ns >
          core::kMaxLifecycleTransactionTimeoutNs) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        port_error("native_brand_lifecycle_profile_invalid",
                   "the retained lifecycle profile is not an exact bounded "
                   "provider occurrence",
                   "profile_or_occurrence_invalid"));
  }

  const core::LifecyclePredicateDTO *initial = nullptr;
  for (const auto &plan : verified_catalog.plans) {
    for (const auto &transition : plan.transitions) {
      if (!std::binary_search(
              transition.eligibility_predicate.allowed_states.begin(),
              transition.eligibility_predicate.allowed_states.end(),
              core::ComponentLifecycleStateDTO::kDeclared)) {
        continue;
      }
      if (initial != nullptr && *initial != transition.eligibility_predicate) {
        return core::Result<
            std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
            port_error("native_brand_lifecycle_initial_state_ambiguous",
                       "the retained lifecycle profile has multiple declared "
                       "initial predicates",
                       "initial_predicate_ambiguous"));
      }
      initial = &transition.eligibility_predicate;
    }
  }
  if (initial == nullptr) {
    return core::Result<
        std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Err(
        port_error("native_brand_lifecycle_initial_state_missing",
                   "the retained lifecycle profile has no declared initial "
                   "predicate",
                   "initial_predicate_missing"));
  }
  auto facts = predicate_facts(*initial);
  augment_unambiguous_next_facts(
      verified_catalog, core::ComponentLifecycleStateDTO::kDeclared, facts);
  auto state = std::make_shared<State>(
      std::move(provider_occurrence), std::move(verified_catalog), clock,
      observation_freshness_ns, std::move(facts));
  return core::Result<
      std::shared_ptr<RetainedNativeBrandLifecyclePort>>::Ok(
      std::shared_ptr<RetainedNativeBrandLifecyclePort>(
          new RetainedNativeBrandLifecyclePort(std::move(state))));
}

core::Result<core::BrandLifecycleObservationDTO>
RetainedNativeBrandLifecyclePort::Observe(
    const core::BrandLifecycleObservationQueryDTO &query) {
  std::lock_guard<std::mutex> lock(state_->mutex);
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

  core::BrandLifecycleObservationDTO observation{
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
      state_->source_sequence,
      state_->observation_generation,
      0,
      0,
      1,
      state_->observed_state,
      state_->facts};
  const core::LifecyclePrimitiveTransitionDTO *selected = nullptr;
  for (const auto &plan : state_->catalog.plans) {
    for (const auto &transition : plan.transitions) {
      if (transition.lifecycle_primitive_id != intent.lifecycle_primitive_id ||
          core::EvaluateLifecyclePredicate(transition.eligibility_predicate,
                                           observation) !=
              core::LifecyclePostconditionResultDTO::kSatisfied) {
        continue;
      }
      if (selected != nullptr) {
        return result(
            core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
            core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
            port_error("native_brand_lifecycle_primitive_ambiguous",
                       "the lifecycle primitive resolves to multiple current "
                       "transitions",
                       "primitive_transition_ambiguous"));
      }
      selected = &transition;
    }
  }
  if (selected == nullptr ||
      selected->postcondition_predicate.allowed_states.size() != 1U ||
      state_->observation_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
    return result(
        core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
        core::LifecyclePrimitiveEffectDTO::kNone, std::nullopt,
        port_error("native_brand_lifecycle_primitive_ineligible",
                   "the lifecycle primitive has no sole eligible transition",
                   "primitive_not_eligible"));
  }

  state_->observed_state =
      selected->postcondition_predicate.allowed_states.front();
  state_->facts = predicate_facts(selected->postcondition_predicate);
  augment_unambiguous_next_facts(state_->catalog, state_->observed_state,
                                 state_->facts);
  ++state_->observation_generation;
  return result(core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
                core::LifecyclePrimitiveEffectDTO::kObserved,
                core::LifecyclePrimitiveKnownResultDTO::kSucceeded,
                std::nullopt);
}

} // namespace manyforge::roboplan_backend
