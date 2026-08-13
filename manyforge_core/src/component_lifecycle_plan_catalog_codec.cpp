#include <algorithm>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::int64_t kMinimumSafeInteger = -9'007'199'254'740'991LL;
constexpr std::int64_t kMaximumSafeInteger = 9'007'199'254'740'991LL;

[[nodiscard]] bool portable_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxLifecycleIdentityBytes) {
    return false;
  }
  const auto alphanumeric = [](unsigned char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](char character) {
    return alphanumeric(static_cast<unsigned char>(character)) || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] bool exact_catalog_identities(const BrandLifecyclePlanCatalogDTO& catalog) noexcept {
  if (!portable_identifier(catalog.provider_manifest_id.value()) ||
      !content_identity::is_sha256_digest(catalog.provider_manifest_revision.value())) {
    return false;
  }
  return std::all_of(catalog.plans.begin(), catalog.plans.end(), [](const auto& plan) {
    const auto predicate_ids_valid = [](const LifecyclePredicateDTO& predicate) {
      return std::all_of(
          predicate.fact_predicates.begin(), predicate.fact_predicates.end(),
          [](const auto& fact) { return portable_identifier(fact.lifecycle_fact_id.value()); });
    };
    if (!predicate_ids_valid(plan.requested_postcondition) ||
        (plan.recover_target_eligibility.has_value() &&
         !predicate_ids_valid(*plan.recover_target_eligibility))) {
      return false;
    }
    return std::all_of(plan.transitions.begin(), plan.transitions.end(), [&](const auto& row) {
      return portable_identifier(row.lifecycle_primitive_id.value()) &&
             predicate_ids_valid(row.eligibility_predicate) &&
             predicate_ids_valid(row.postcondition_predicate);
    });
  });
}

template <typename Value>
[[nodiscard]] Result<Value> reject(std::string code, std::string message, std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] std::string_view intent_token(ComponentLifecycleIntentKindDTO intent) {
  switch (intent) {
    case ComponentLifecycleIntentKindDTO::kPrepare:
      return "prepare";
    case ComponentLifecycleIntentKindDTO::kEnable:
      return "enable";
    case ComponentLifecycleIntentKindDTO::kDisable:
      return "disable";
    case ComponentLifecycleIntentKindDTO::kRecover:
      return "recover";
  }
  throw content_identity::CanonicalValueError("unknown lifecycle intent");
}

[[nodiscard]] std::string_view state_token(ComponentLifecycleStateDTO state) {
  switch (state) {
    case ComponentLifecycleStateDTO::kUndeclared:
      return "undeclared";
    case ComponentLifecycleStateDTO::kDeclared:
      return "declared";
    case ComponentLifecycleStateDTO::kPrepared:
      return "prepared";
    case ComponentLifecycleStateDTO::kActive:
      return "active";
    case ComponentLifecycleStateDTO::kDegraded:
      return "degraded";
    case ComponentLifecycleStateDTO::kFaulted:
      return "faulted";
    case ComponentLifecycleStateDTO::kDisabling:
      return "disabling";
    case ComponentLifecycleStateDTO::kDisabled:
      return "disabled";
  }
  throw content_identity::CanonicalValueError("unknown lifecycle state");
}

[[nodiscard]] CanonicalValue safe_integer(std::int64_t value) {
  if (value < kMinimumSafeInteger || value > kMaximumSafeInteger) {
    throw content_identity::CanonicalValueError("integer is outside the I-JSON safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue fact_value(const LifecycleFactValueDTO& value) {
  CanonicalValue::Object members;
  switch (value.kind) {
    case LifecycleFactValueKindDTO::kUnknown:
      members.emplace_back("kind", CanonicalValue("unknown"));
      break;
    case LifecycleFactValueKindDTO::kBoolean:
      members.emplace_back("kind", CanonicalValue("boolean"));
      members.emplace_back("boolean_value", CanonicalValue(*value.boolean_value));
      break;
    case LifecycleFactValueKindDTO::kInteger:
      members.emplace_back("kind", CanonicalValue("integer"));
      members.emplace_back("integer_value", safe_integer(*value.integer_value));
      break;
    case LifecycleFactValueKindDTO::kToken:
      members.emplace_back("kind", CanonicalValue("token"));
      members.emplace_back("token_value", CanonicalValue(*value.token_value));
      break;
  }
  return CanonicalValue(std::move(members));
}

[[nodiscard]] CanonicalValue predicate(const LifecyclePredicateDTO& value) {
  CanonicalValue::Array states;
  states.reserve(value.allowed_states.size());
  for (const auto state : value.allowed_states) {
    states.emplace_back(std::string(state_token(state)));
  }

  CanonicalValue::Array facts;
  facts.reserve(value.fact_predicates.size());
  for (const auto& fact : value.fact_predicates) {
    facts.emplace_back(CanonicalValue::Object{
        {"lifecycle_fact_id", CanonicalValue(fact.lifecycle_fact_id.value())},
        {"expected_value", fact_value(fact.expected_value)}});
  }
  return CanonicalValue(
      CanonicalValue::Object{{"allowed_states", CanonicalValue(std::move(states))},
                             {"fact_predicates", CanonicalValue(std::move(facts))}});
}

[[nodiscard]] CanonicalValue transition(const LifecyclePrimitiveTransitionDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"lifecycle_primitive_id", CanonicalValue(value.lifecycle_primitive_id.value())},
      {"eligibility_predicate", predicate(value.eligibility_predicate)},
      {"postcondition_predicate", predicate(value.postcondition_predicate)}});
}

[[nodiscard]] CanonicalValue plan(const LifecycleIntentPlanDTO& value) {
  CanonicalValue::Array transitions;
  transitions.reserve(value.transitions.size());
  for (const auto& row : value.transitions) {
    transitions.emplace_back(transition(row));
  }

  CanonicalValue::Object members{
      {"intent", CanonicalValue(std::string(intent_token(value.intent)))},
      {"requested_state", CanonicalValue(std::string(state_token(value.requested_state)))},
      {"requested_postcondition", predicate(value.requested_postcondition)},
      {"timeout_duration_ns", safe_integer(value.timeout_duration_ns)},
      {"transitions", CanonicalValue(std::move(transitions))}};
  if (value.recover_target_eligibility.has_value()) {
    members.emplace_back("recover_target_eligibility",
                         predicate(*value.recover_target_eligibility));
  }
  return CanonicalValue(std::move(members));
}

[[nodiscard]] CanonicalValue::Array plans_value(const std::vector<LifecycleIntentPlanDTO>& plans) {
  CanonicalValue::Array result;
  result.reserve(plans.size());
  for (const auto& row : plans) {
    result.emplace_back(plan(row));
  }
  return result;
}

[[nodiscard]] CanonicalValue content_value(const BrandLifecyclePlanCatalogDTO& catalog) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kBrandLifecyclePlanCatalogApiVersion))},
      {"provider_manifest_id", CanonicalValue(catalog.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(catalog.provider_manifest_revision.value())},
      {"plans", CanonicalValue(plans_value(catalog.plans))}});
}

[[nodiscard]] CanonicalValue semantic_content_value(
    const ProviderManifestId& provider_manifest_id,
    const std::vector<LifecycleIntentPlanDTO>& plans) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kBrandLifecyclePlanCatalogApiVersion))},
      {"provider_manifest_id", CanonicalValue(provider_manifest_id.value())},
      {"plans", CanonicalValue(plans_value(plans))}});
}

[[nodiscard]] ContentDigest computed_digest(const BrandLifecyclePlanCatalogDTO& catalog) {
  return ContentDigest(content_identity::framed_sha256(kBrandLifecyclePlanCatalogIdentityDomain,
                                                       kBrandLifecyclePlanCatalogApiVersion,
                                                       content_value(catalog)));
}

}  // namespace

Result<ContentDigest> ComputeBrandLifecyclePlanCatalogSemanticDigest(
    const ProviderManifestId& provider_manifest_id,
    const std::vector<LifecycleIntentPlanDTO>& plans) {
  BrandLifecyclePlanCatalogDTO validation{
      provider_manifest_id,
      ProviderManifestRevision(
          "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
      ContentDigest("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
      plans};
  if (!validation.locally_valid() || !exact_catalog_identities(validation)) {
    return reject<ContentDigest>(
        "brand_lifecycle_plan_catalog_invalid",
        "the lifecycle plan catalog semantics are not closed and canonical",
        "catalog_semantics_not_locally_valid");
  }
  try {
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kBrandLifecyclePlanCatalogSemanticIdentityDomain, kBrandLifecyclePlanCatalogApiVersion,
        semantic_content_value(provider_manifest_id, plans))));
  } catch (const std::exception&) {
    return reject<ContentDigest>(
        "brand_lifecycle_plan_catalog_not_canonical",
        "the lifecycle plan catalog semantics cannot be represented canonically",
        "catalog_semantics_not_canonical");
  }
}

Result<void> VerifyBrandLifecyclePlanCatalogManifestAnchor(
    const ProviderManifestDTO& provider_manifest, const BrandLifecyclePlanCatalogDTO& catalog) {
  const auto verified = VerifyBrandLifecyclePlanCatalog(catalog);
  if (!verified.ok()) {
    return verified;
  }
  if (provider_manifest.provider_manifest_id != catalog.provider_manifest_id ||
      provider_manifest.provider_manifest_revision != catalog.provider_manifest_revision ||
      !provider_manifest.lifecycle_plan_catalog.has_value() ||
      provider_manifest.lifecycle_plan_catalog->catalog_api_version.value() !=
          kBrandLifecyclePlanCatalogApiVersion ||
      !content_identity::is_sha256_digest(
          provider_manifest.lifecycle_plan_catalog->semantic_digest.value())) {
    return Result<void>::Err(
        ErrorDTO{"brand_lifecycle_plan_catalog_manifest_anchor_mismatch",
                 "the provider manifest does not select the exact lifecycle catalog",
                 {{"reason", "provider_manifest_anchor_mismatch"}}});
  }
  const auto semantic =
      ComputeBrandLifecyclePlanCatalogSemanticDigest(catalog.provider_manifest_id, catalog.plans);
  if (!semantic.ok()) {
    return Result<void>::Err(semantic.error());
  }
  if (!content_identity::digest_equal(
          semantic.value().value(),
          provider_manifest.lifecycle_plan_catalog->semantic_digest.value())) {
    return Result<void>::Err(
        ErrorDTO{"brand_lifecycle_plan_catalog_semantic_digest_mismatch",
                 "the lifecycle catalog does not match its provider-manifest anchor",
                 {{"reason", "catalog_semantic_digest_mismatch"}}});
  }
  std::set<std::string> expected_capabilities{"component.lifecycle.observe"};
  for (const auto& plan : catalog.plans) {
    expected_capabilities.insert("component.lifecycle." + std::string(intent_token(plan.intent)));
  }
  std::set<std::string> actual_capabilities;
  for (const auto& offer : provider_manifest.offered_capabilities) {
    if (!offer.capability_id.value().starts_with("component.lifecycle.")) {
      continue;
    }
    const auto* definition = find_builtin_capability_definition(offer.capability_id);
    if (definition == nullptr ||
        offer.capability_definition_revision.value() !=
            definition->capability_definition_revision ||
        !actual_capabilities.insert(offer.capability_id.value()).second) {
      return Result<void>::Err(
          ErrorDTO{"brand_lifecycle_plan_catalog_manifest_capability_mismatch",
                   "the provider manifest lifecycle capability closure is invalid",
                   {{"reason", "lifecycle_capability_definition_mismatch"}}});
    }
  }
  if (actual_capabilities != expected_capabilities) {
    return Result<void>::Err(
        ErrorDTO{"brand_lifecycle_plan_catalog_manifest_capability_mismatch",
                 "the provider manifest does not offer the exact catalog intents",
                 {{"reason", "lifecycle_capability_set_mismatch"}}});
  }
  return Result<void>::Ok();
}

Result<BrandLifecyclePlanCatalogDTO> MakeBrandLifecyclePlanCatalog(
    ProviderManifestId provider_manifest_id, ProviderManifestRevision provider_manifest_revision,
    std::vector<LifecycleIntentPlanDTO> plans) {
  BrandLifecyclePlanCatalogDTO catalog{std::move(provider_manifest_id),
                                       std::move(provider_manifest_revision),
                                       ContentDigest("pending"), std::move(plans)};
  if (!catalog.locally_valid() || !exact_catalog_identities(catalog)) {
    return reject<BrandLifecyclePlanCatalogDTO>(
        "brand_lifecycle_plan_catalog_invalid",
        "the lifecycle plan catalog is not a closed canonical catalog",
        "catalog_not_locally_valid");
  }
  try {
    catalog.catalog_digest = computed_digest(catalog);
  } catch (const std::exception&) {
    return reject<BrandLifecyclePlanCatalogDTO>(
        "brand_lifecycle_plan_catalog_not_canonical",
        "the lifecycle plan catalog cannot be represented canonically",
        "catalog_content_not_canonical");
  }
  return Result<BrandLifecyclePlanCatalogDTO>::Ok(std::move(catalog));
}

Result<void> VerifyBrandLifecyclePlanCatalog(const BrandLifecyclePlanCatalogDTO& catalog) {
  if (!catalog.locally_valid() || !exact_catalog_identities(catalog) ||
      !content_identity::is_sha256_digest(catalog.catalog_digest.value())) {
    return Result<void>::Err(
        ErrorDTO{"brand_lifecycle_plan_catalog_invalid",
                 "the lifecycle plan catalog shape or digest spelling is invalid",
                 {{"reason", "catalog_not_locally_valid"}}});
  }
  try {
    const auto expected = computed_digest(catalog);
    if (!content_identity::digest_equal(expected.value(), catalog.catalog_digest.value())) {
      return Result<void>::Err(
          ErrorDTO{"brand_lifecycle_plan_catalog_digest_mismatch",
                   "the lifecycle plan catalog digest does not match its canonical content",
                   {{"reason", "catalog_digest_mismatch"}}});
    }
  } catch (const std::exception&) {
    return Result<void>::Err(
        ErrorDTO{"brand_lifecycle_plan_catalog_not_canonical",
                 "the lifecycle plan catalog cannot be represented canonically",
                 {{"reason", "catalog_content_not_canonical"}}});
  }
  return Result<void>::Ok();
}

Result<std::string> EncodeBrandLifecyclePlanCatalog(const BrandLifecyclePlanCatalogDTO& catalog) {
  const auto verified = VerifyBrandLifecyclePlanCatalog(catalog);
  if (!verified.ok()) {
    return Result<std::string>::Err(verified.error());
  }
  try {
    return Result<std::string>::Ok(
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kBrandLifecyclePlanCatalogApiVersion))},
            {"provider_manifest_id", CanonicalValue(catalog.provider_manifest_id.value())},
            {"provider_manifest_revision",
             CanonicalValue(catalog.provider_manifest_revision.value())},
            {"catalog_digest", CanonicalValue(catalog.catalog_digest.value())},
            {"plans", CanonicalValue(plans_value(catalog.plans))}})));
  } catch (const std::exception&) {
    return reject<std::string>("brand_lifecycle_plan_catalog_not_canonical",
                               "the lifecycle plan catalog cannot be encoded canonically",
                               "catalog_encoding_failed");
  }
}

}  // namespace manyforge::core
