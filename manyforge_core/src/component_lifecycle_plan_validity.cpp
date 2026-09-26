#include <algorithm>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <set>
#include <string_view>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId &value) noexcept {
  return !value.empty() && value.value().size() <= kMaxLifecycleIdentityBytes;
}

[[nodiscard]] bool portable_token(const std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxLifecycleFactTokenBytes) {
    return false;
  }
  const auto alphanumeric = [](const unsigned char character) noexcept {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return alphanumeric(byte) || character == '.' || character == '_' ||
           character == '-';
  });
}

template <typename Value, typename Less>
[[nodiscard]] bool sorted_unique(const std::vector<Value> &values,
                                 Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(),
                            [&](const Value &left, const Value &right) {
                              return !less(left, right) && !less(right, left);
                            }) == values.end();
}

[[nodiscard]] bool known(const ComponentLifecycleIntentKindDTO value) noexcept {
  switch (value) {
  case ComponentLifecycleIntentKindDTO::kPrepare:
  case ComponentLifecycleIntentKindDTO::kEnable:
  case ComponentLifecycleIntentKindDTO::kDisable:
  case ComponentLifecycleIntentKindDTO::kRecover:
    return true;
  }
  return false;
}

[[nodiscard]] bool known(const ComponentLifecycleStateDTO value) noexcept {
  switch (value) {
  case ComponentLifecycleStateDTO::kUndeclared:
  case ComponentLifecycleStateDTO::kDeclared:
  case ComponentLifecycleStateDTO::kPrepared:
  case ComponentLifecycleStateDTO::kActive:
  case ComponentLifecycleStateDTO::kDegraded:
  case ComponentLifecycleStateDTO::kFaulted:
  case ComponentLifecycleStateDTO::kDisabling:
  case ComponentLifecycleStateDTO::kDisabled:
    return true;
  }
  return false;
}

} // namespace

bool LifecycleFactValueDTO::locally_valid() const noexcept {
  switch (kind) {
  case LifecycleFactValueKindDTO::kUnknown:
    return !boolean_value.has_value() && !integer_value.has_value() &&
           !token_value.has_value();
  case LifecycleFactValueKindDTO::kBoolean:
    return boolean_value.has_value() && !integer_value.has_value() &&
           !token_value.has_value();
  case LifecycleFactValueKindDTO::kInteger:
    return !boolean_value.has_value() && integer_value.has_value() &&
           !token_value.has_value();
  case LifecycleFactValueKindDTO::kToken:
    return !boolean_value.has_value() && !integer_value.has_value() &&
           token_value.has_value() && portable_token(*token_value);
  }
  return false;
}

bool LifecycleFactDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_fact_id) && value.locally_valid();
}

bool LifecycleFactPredicateDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_fact_id) && expected_value.locally_valid() &&
         expected_value.kind != LifecycleFactValueKindDTO::kUnknown;
}

bool LifecyclePredicateDTO::locally_valid() const noexcept {
  if (allowed_states.empty() ||
      allowed_states.size() >
          static_cast<std::size_t>(ComponentLifecycleStateDTO::kDisabled) + 1U ||
      fact_predicates.size() > kMaxLifecyclePredicateFacts ||
      !std::is_sorted(allowed_states.begin(), allowed_states.end()) ||
      std::adjacent_find(allowed_states.begin(), allowed_states.end()) !=
          allowed_states.end() ||
      std::any_of(allowed_states.begin(), allowed_states.end(),
                  [](const ComponentLifecycleStateDTO state) {
                    return !known(state);
                  })) {
    return false;
  }
  const auto less = [](const LifecycleFactPredicateDTO &left,
                       const LifecycleFactPredicateDTO &right) {
    return left.lifecycle_fact_id < right.lifecycle_fact_id;
  };
  return sorted_unique(fact_predicates, less) &&
         std::all_of(fact_predicates.begin(), fact_predicates.end(),
                     [](const LifecycleFactPredicateDTO &predicate) {
                       return predicate.locally_valid();
                     });
}

bool LifecyclePrimitiveTransitionDTO::locally_valid() const noexcept {
  return bounded_identity(lifecycle_primitive_id) &&
         eligibility_predicate.locally_valid() &&
         postcondition_predicate.locally_valid();
}

bool LifecycleIntentPlanDTO::locally_valid() const noexcept {
  if (!known(intent) || !known(requested_state) ||
      !requested_postcondition.locally_valid() || timeout_duration_ns <= 0 ||
      timeout_duration_ns > kMaxLifecycleTransactionTimeoutNs ||
      transitions.size() > kMaxLifecycleTransitionsPerPlan ||
      (recover_target_eligibility.has_value() &&
       !recover_target_eligibility->locally_valid())) {
    return false;
  }
  const bool target_valid = [&]() {
    switch (intent) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return requested_state == ComponentLifecycleStateDTO::kPrepared &&
             !recover_target_eligibility.has_value();
    case ComponentLifecycleIntentKindDTO::kEnable:
      return requested_state == ComponentLifecycleStateDTO::kActive &&
             !recover_target_eligibility.has_value();
    case ComponentLifecycleIntentKindDTO::kDisable:
      return requested_state == ComponentLifecycleStateDTO::kDisabled &&
             !recover_target_eligibility.has_value();
    case ComponentLifecycleIntentKindDTO::kRecover:
      return recover_target_eligibility.has_value() &&
             (requested_state == ComponentLifecycleStateDTO::kDeclared ||
              requested_state == ComponentLifecycleStateDTO::kPrepared ||
              requested_state == ComponentLifecycleStateDTO::kDisabled);
    }
    return false;
  }();
  if (!target_valid) {
    return false;
  }
  std::set<LifecyclePrimitiveId> primitive_ids;
  return std::all_of(transitions.begin(), transitions.end(),
                     [&](const LifecyclePrimitiveTransitionDTO &transition) {
                       return transition.locally_valid() &&
                              primitive_ids.insert(
                                  transition.lifecycle_primitive_id).second;
                     });
}

bool BrandLifecyclePlanCatalogDTO::locally_valid() const noexcept {
  if (!bounded_identity(provider_manifest_id) ||
      !bounded_identity(provider_manifest_revision) ||
      !bounded_identity(catalog_digest) || plans.empty() ||
      plans.size() > kMaxLifecycleCatalogPlans) {
    return false;
  }
  const auto less = [](const LifecycleIntentPlanDTO &left,
                       const LifecycleIntentPlanDTO &right) {
    if (left.intent != right.intent) {
      return left.intent < right.intent;
    }
    return left.requested_state < right.requested_state;
  };
  if (!sorted_unique(plans, less)) {
    return false;
  }
  std::size_t transition_count = 0U;
  std::size_t recover_count = 0U;
  for (const auto &plan : plans) {
    if (!plan.locally_valid() ||
        transition_count >
            kMaxLifecycleTransitionsPerCatalog - plan.transitions.size()) {
      return false;
    }
    transition_count += plan.transitions.size();
    if (plan.intent == ComponentLifecycleIntentKindDTO::kRecover &&
        ++recover_count > 3U) {
      return false;
    }
  }
  return true;
}

} // namespace manyforge::core
