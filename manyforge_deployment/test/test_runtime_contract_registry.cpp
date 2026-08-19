#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>

namespace manyforge::deployment {
namespace {

TEST(RuntimeContractRegistry, PackageAndEmbeddedOffersAreOneExactValue) {
  const auto& registry = build_qualified_runtime_contract_registry();
  ASSERT_EQ(registry.contracts().size(), 3U);
  ASSERT_EQ(registry.package_manifest_set().offers.size(),
            registry.contracts().size());
  ASSERT_TRUE(
      core::VerifyRuntimeContractManifestSet(registry.package_manifest_set())
          .ok());

  for (const auto& row : registry.contracts()) {
    const auto package_offer = std::find_if(
        registry.package_manifest_set().offers.begin(),
        registry.package_manifest_set().offers.end(),
        [&](const auto& offer) {
          return offer.runtime_implementation_id ==
                 row.offer().runtime_implementation_id;
        });
    ASSERT_NE(package_offer, registry.package_manifest_set().offers.end());
    EXPECT_EQ(*package_offer, row.offer());

    const auto embedded_projection =
        detail::runtime_contract_offer_projection(row.offer());
    const auto embedded =
        detail::decode_runtime_contract_offer_projection(
            embedded_projection, "test.runtime_contract_offer");
    EXPECT_EQ(embedded, row.offer());
    const auto package_bytes = core::EncodeRuntimeContractOffer(*package_offer);
    const auto embedded_bytes = core::EncodeRuntimeContractOffer(embedded);
    ASSERT_TRUE(package_bytes.ok());
    ASSERT_TRUE(embedded_bytes.ok());
    EXPECT_EQ(package_bytes.value(), embedded_bytes.value());
    const auto decoded_package =
        core::DecodeRuntimeContractOffer(package_bytes.value());
    ASSERT_TRUE(decoded_package.ok());
    EXPECT_EQ(decoded_package.value(), embedded);
  }
}

TEST(RuntimeContractRegistry,
     ExplicitMappingsCoverEveryMaintainedRuntimeOfferAndFamily) {
  const auto& registry = build_qualified_runtime_contract_registry();
  std::vector<std::string> families;
  for (const auto& row : registry.contracts()) {
    ASSERT_TRUE(std::is_sorted(row.offer().provided_api_contract_ids.begin(),
                               row.offer().provided_api_contract_ids.end()));
    ASSERT_FALSE(row.runtime_role_api_mappings().empty());
    ASSERT_FALSE(row.source_endpoint_api_mappings().empty());
    ASSERT_EQ(row.offer().provided_api_contract_ids.size(),
              row.runtime_role_api_mappings().size() +
                  row.source_endpoint_api_mappings().size());
    families.push_back(row.offer().runtime_implementation_id.value());
    for (const auto& mapping : row.runtime_role_api_mappings()) {
      EXPECT_FALSE(mapping.semantic_owner_id.empty());
    }
    for (const auto& mapping : row.source_endpoint_api_mappings()) {
      EXPECT_FALSE(mapping.semantic_owner_id.empty());
    }
  }
  std::sort(families.begin(), families.end());
  EXPECT_EQ(std::adjacent_find(families.begin(), families.end()),
            families.end());
}

TEST(RuntimeContractRegistry,
     ExactAndStrictSupersetOffersPassWhileMissingOfferFactsFail) {
  const auto& registry = build_qualified_runtime_contract_registry();
  const auto row = std::find_if(
      registry.contracts().begin(), registry.contracts().end(),
      [](const auto& candidate) {
        return !candidate.offer().provided_capabilities.empty();
      });
  ASSERT_NE(row, registry.contracts().end());

  const auto exact = core::MakeRuntimeContractRequirement(
      row->offer().runtime_implementation_id,
      row->offer().provided_api_contract_ids,
      row->offer().provided_capabilities);
  ASSERT_TRUE(exact.ok());
  EXPECT_TRUE(
      core::CheckRuntimeContractCompatibility(row->offer(), exact.value())
          .ok());

  auto superset_apis = row->offer().provided_api_contract_ids;
  superset_apis.emplace_back("manyforge.api.registry_test_extra");
  const auto superset = core::MakeRuntimeContractOffer(
      row->offer().runtime_implementation_id, std::move(superset_apis),
      row->offer().provided_capabilities);
  ASSERT_TRUE(superset.ok());
  EXPECT_TRUE(
      core::CheckRuntimeContractCompatibility(superset.value(), exact.value())
          .ok());

  auto missing_apis = row->offer().provided_api_contract_ids;
  missing_apis.pop_back();
  const auto missing_api_offer = core::MakeRuntimeContractOffer(
      row->offer().runtime_implementation_id, std::move(missing_apis),
      row->offer().provided_capabilities);
  ASSERT_TRUE(missing_api_offer.ok());
  EXPECT_FALSE(core::CheckRuntimeContractCompatibility(
                   missing_api_offer.value(), exact.value())
                   .ok());

  auto missing_capabilities = row->offer().provided_capabilities;
  missing_capabilities.pop_back();
  const auto missing_capability_offer = core::MakeRuntimeContractOffer(
      row->offer().runtime_implementation_id,
      row->offer().provided_api_contract_ids,
      std::move(missing_capabilities));
  ASSERT_TRUE(missing_capability_offer.ok());
  EXPECT_FALSE(core::CheckRuntimeContractCompatibility(
                   missing_capability_offer.value(), exact.value())
                   .ok());

  const auto wrong_family_requirement = core::MakeRuntimeContractRequirement(
      core::RuntimeImplementationFamilyId("manyforge.wrong.family"),
      row->offer().provided_api_contract_ids,
      row->offer().provided_capabilities);
  ASSERT_TRUE(wrong_family_requirement.ok());
  EXPECT_FALSE(core::CheckRuntimeContractCompatibility(
                   row->offer(), wrong_family_requirement.value())
                   .ok());
}

}  // namespace
}  // namespace manyforge::deployment
