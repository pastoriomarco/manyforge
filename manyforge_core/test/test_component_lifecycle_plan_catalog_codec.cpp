#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::string_view kManifestRevisionOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kManifestRevisionTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

[[nodiscard]] LifecycleFactValueDTO Token(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, std::move(value)};
}

[[nodiscard]] LifecycleFactValueDTO Boolean(bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt, std::nullopt};
}

[[nodiscard]] LifecyclePredicateDTO Predicate(ComponentLifecycleStateDTO state,
                                              LifecycleFactId fact_id,
                                              LifecycleFactValueDTO expected_value) {
  return {{state}, {{std::move(fact_id), std::move(expected_value)}}};
}

[[nodiscard]] LifecycleIntentPlanDTO PreparePlan() {
  const auto eligibility = Predicate(ComponentLifecycleStateDTO::kDeclared,
                                     LifecycleFactId("vendor.mode"), Token("idle"));
  const auto postcondition = Predicate(ComponentLifecycleStateDTO::kPrepared,
                                       LifecycleFactId("vendor.ready"), Boolean(true));
  return {ComponentLifecycleIntentKindDTO::kPrepare,
          ComponentLifecycleStateDTO::kPrepared,
          std::nullopt,
          postcondition,
          5'000'000'000LL,
          {{LifecyclePrimitiveId("prepare.enable"), eligibility, postcondition}}};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO Catalog(
    std::string manifest_id = "provider.a",
    std::string revision = std::string(kManifestRevisionOne)) {
  auto result =
      MakeBrandLifecyclePlanCatalog(ProviderManifestId(std::move(manifest_id)),
                                    ProviderManifestRevision(std::move(revision)), {PreparePlan()});
  EXPECT_TRUE(result.ok());
  return result.value();
}

[[nodiscard]] ProviderManifestDTO ManifestFor(const BrandLifecyclePlanCatalogDTO& catalog) {
  const auto semantic =
      ComputeBrandLifecyclePlanCatalogSemanticDigest(catalog.provider_manifest_id, catalog.plans);
  EXPECT_TRUE(semantic.ok());
  std::vector<ProviderCapabilityOfferDTO> capabilities;
  const auto add_capability = [&](std::string id) {
    const auto* definition = find_builtin_capability_definition(id);
    EXPECT_NE(definition, nullptr);
    capabilities.push_back(
        {CapabilityId(std::move(id)),
         CapabilityDefinitionRevision(std::string(definition->capability_definition_revision)),
         {}});
  };
  add_capability("component.lifecycle.observe");
  for (const auto& plan : catalog.plans) {
    add_capability("component.lifecycle." + std::string(to_string(plan.intent)));
  }
  return ProviderManifestDTO{
      catalog.provider_manifest_id,
      catalog.provider_manifest_revision,
      std::move(capabilities),
      {},
      ProviderLifecyclePlanCatalogDeclarationDTO{
          SchemaId(std::string(kBrandLifecyclePlanCatalogApiVersion)), semantic.value()}};
}

[[nodiscard]] BuildOwnedLifecyclePlanCatalogEntryDTO EntryFor(
    BrandLifecyclePlanCatalogDTO catalog) {
  auto manifest = ManifestFor(catalog);
  return {std::move(manifest), std::move(catalog)};
}

TEST(ComponentLifecyclePlanCatalogCodec, EmitsOneExactCanonicalArtifactAndDigest) {
  const auto catalog = Catalog();
  ASSERT_TRUE(VerifyBrandLifecyclePlanCatalog(catalog).ok());
  const auto encoded = EncodeBrandLifecyclePlanCatalog(catalog);
  ASSERT_TRUE(encoded.ok());

  EXPECT_EQ(catalog.catalog_digest.value(),
            "sha256:ebbcd36265b0b73072c18bc241c687c5eaca49f5c45753329a170d0e4aff3687");
  EXPECT_EQ(encoded.value(),
            "{\"api_version\":\"manyforge.brand_lifecycle_plan_catalog.v1\","
            "\"catalog_digest\":\"sha256:"
            "ebbcd36265b0b73072c18bc241c687c5eaca49f5c45753329a170d0e4aff3687\","
            "\"plans\":[{\"intent\":\"prepare\",\"requested_postcondition\":{\"allowed_states\":["
            "\"prepared\"],\"fact_predicates\":[{\"expected_value\":{\"boolean_value\":true,"
            "\"kind\":\"boolean\"},\"lifecycle_fact_id\":\"vendor.ready\"}]},"
            "\"requested_state\":\"prepared\",\"timeout_duration_ns\":5000000000,"
            "\"transitions\":[{\"eligibility_predicate\":{\"allowed_states\":[\"declared\"],"
            "\"fact_predicates\":[{\"expected_value\":{\"kind\":\"token\",\"token_value\":"
            "\"idle\"},\"lifecycle_fact_id\":\"vendor.mode\"}]},\"lifecycle_primitive_id\":"
            "\"prepare.enable\",\"postcondition_predicate\":{\"allowed_states\":[\"prepared\"],"
            "\"fact_predicates\":[{\"expected_value\":{\"boolean_value\":true,\"kind\":"
            "\"boolean\"},\"lifecycle_fact_id\":\"vendor.ready\"}]}}]}],"
            "\"provider_manifest_id\":\"provider.a\",\"provider_manifest_revision\":"
            "\"sha256:1111111111111111111111111111111111111111111111111111111111111111\"}");
}

TEST(ComponentLifecyclePlanCatalogCodec, SemanticChangesAndTransitionOrderRotateDigest) {
  auto changed_timeout = PreparePlan();
  changed_timeout.timeout_duration_ns += 1;
  const auto first = Catalog();
  const auto changed = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("provider.a"), ProviderManifestRevision(std::string(kManifestRevisionOne)),
      {changed_timeout});
  ASSERT_TRUE(changed.ok());
  EXPECT_NE(first.catalog_digest, changed.value().catalog_digest);

  auto ordered = PreparePlan();
  auto second_transition = ordered.transitions.front();
  second_transition.lifecycle_primitive_id = LifecyclePrimitiveId("prepare.select_mode");
  ordered.transitions.push_back(second_transition);
  auto reversed = ordered;
  std::reverse(reversed.transitions.begin(), reversed.transitions.end());
  const auto ordered_catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("provider.a"), ProviderManifestRevision(std::string(kManifestRevisionTwo)),
      {ordered});
  const auto reversed_catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("provider.a"), ProviderManifestRevision(std::string(kManifestRevisionTwo)),
      {reversed});
  ASSERT_TRUE(ordered_catalog.ok());
  ASSERT_TRUE(reversed_catalog.ok());
  EXPECT_NE(ordered_catalog.value().catalog_digest, reversed_catalog.value().catalog_digest);
  const auto ordered_semantic = ComputeBrandLifecyclePlanCatalogSemanticDigest(
      ordered_catalog.value().provider_manifest_id, ordered_catalog.value().plans);
  const auto reversed_semantic = ComputeBrandLifecyclePlanCatalogSemanticDigest(
      reversed_catalog.value().provider_manifest_id, reversed_catalog.value().plans);
  ASSERT_TRUE(ordered_semantic.ok());
  ASSERT_TRUE(reversed_semantic.ok());
  EXPECT_NE(ordered_semantic.value(), reversed_semantic.value());
}

TEST(ComponentLifecyclePlanCatalogCodec, RejectsInvalidDigestAndUnsafeJsonInteger) {
  auto catalog = Catalog();
  catalog.catalog_digest =
      ContentDigest("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  EXPECT_FALSE(VerifyBrandLifecyclePlanCatalog(catalog).ok());
  EXPECT_FALSE(EncodeBrandLifecyclePlanCatalog(catalog).ok());

  auto unsafe = PreparePlan();
  unsafe.transitions.front().eligibility_predicate.fact_predicates.front().expected_value = {
      LifecycleFactValueKindDTO::kInteger, std::nullopt, std::numeric_limits<std::int64_t>::max(),
      std::nullopt};
  EXPECT_FALSE(MakeBrandLifecyclePlanCatalog(
                   ProviderManifestId("provider.a"),
                   ProviderManifestRevision(std::string(kManifestRevisionOne)), {unsafe})
                   .ok());
  EXPECT_FALSE(MakeBrandLifecyclePlanCatalog(ProviderManifestId("provider.a"),
                                             ProviderManifestRevision("not-a-content-digest"),
                                             {PreparePlan()})
                   .ok());
  EXPECT_FALSE(MakeBrandLifecyclePlanCatalog(
                   ProviderManifestId("provider/invalid"),
                   ProviderManifestRevision(std::string(kManifestRevisionOne)), {PreparePlan()})
                   .ok());
}

TEST(BuildOwnedLifecyclePlanCatalogSource, RejectsAmbiguityAndRequiresExactTriple) {
  const auto first = Catalog("provider.a", std::string(kManifestRevisionOne));
  const auto second = Catalog("provider.b", std::string(kManifestRevisionTwo));
  auto source = BuildOwnedLifecyclePlanCatalogSource::Make({EntryFor(second), EntryFor(first)});
  ASSERT_TRUE(source.ok());

  const auto loaded = source.value().LoadExactLifecyclePlanCatalog(
      first.provider_manifest_id, first.provider_manifest_revision, first.catalog_digest);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value(), first);
  EXPECT_FALSE(source.value()
                   .LoadExactLifecyclePlanCatalog(first.provider_manifest_id,
                                                  first.provider_manifest_revision,
                                                  second.catalog_digest)
                   .ok());
  EXPECT_FALSE(source.value()
                   .LoadExactLifecyclePlanCatalog(ProviderManifestId("provider.missing"),
                                                  first.provider_manifest_revision,
                                                  first.catalog_digest)
                   .ok());
  EXPECT_FALSE(BuildOwnedLifecyclePlanCatalogSource::Make({EntryFor(first), EntryFor(first)}).ok());

  auto stale_anchor = EntryFor(first);
  stale_anchor.provider_manifest.lifecycle_plan_catalog->semantic_digest = second.catalog_digest;
  EXPECT_FALSE(BuildOwnedLifecyclePlanCatalogSource::Make({std::move(stale_anchor)}).ok());

  auto missing_capability = EntryFor(first);
  missing_capability.provider_manifest.offered_capabilities.pop_back();
  EXPECT_FALSE(BuildOwnedLifecyclePlanCatalogSource::Make({std::move(missing_capability)}).ok());

  auto empty = BuildOwnedLifecyclePlanCatalogSource::Make({});
  ASSERT_TRUE(empty.ok());
  EXPECT_FALSE(empty.value()
                   .LoadExactLifecyclePlanCatalog(first.provider_manifest_id,
                                                  first.provider_manifest_revision,
                                                  first.catalog_digest)
                   .ok());
}

TEST(BuildOwnedLifecyclePlanCatalogSource, ImmutableReadsAreConcurrent) {
  const auto catalog = Catalog();
  auto source = BuildOwnedLifecyclePlanCatalogSource::Make({EntryFor(catalog)});
  ASSERT_TRUE(source.ok());
  std::atomic<bool> valid{true};
  std::vector<std::jthread> readers;
  for (std::size_t index = 0; index < 8U; ++index) {
    readers.emplace_back([&] {
      for (std::size_t iteration = 0; iteration < 100U; ++iteration) {
        const auto loaded = source.value().LoadExactLifecyclePlanCatalog(
            catalog.provider_manifest_id, catalog.provider_manifest_revision,
            catalog.catalog_digest);
        if (!loaded.ok() || loaded.value() != catalog) {
          valid.store(false, std::memory_order_release);
        }
      }
    });
  }
  readers.clear();
  EXPECT_TRUE(valid.load(std::memory_order_acquire));
}

}  // namespace
}  // namespace manyforge::core
