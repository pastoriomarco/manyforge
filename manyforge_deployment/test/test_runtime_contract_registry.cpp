#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_capability_declarations.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {
namespace {

// DX753: archived identities test refusal without restoring Native admission APIs.
constexpr std::string_view kRetiredNativeImplementationId = "manyforge.native_execution_host";
constexpr std::string_view kRetiredNativeProductContractId = "manyforge.native_execution_runtime_unit";

TEST(RuntimeContractRegistry, OldSealedRosProfileRefusesWithRecompileGuidance) {
  const auto &contracts = runtime_contract_declaration_registry().contracts();
  const auto current = std::find_if(contracts.begin(), contracts.end(), [](const auto &row) {
    return row.runtime_profile_id().value() == "manyforge.ros2_control_execution_runtime";
  });
  ASSERT_NE(current, contracts.end());
  try {
    static_cast<void>(detail::require_runtime_contract(current->runtime_profile_id(),
        core::RuntimeProfileRevision("sha256:0f916fb469d27a221a97cf72737e006c4c4fc566752ea477a141847a0012fef5"),
        current->runtime_product_id(), current->runtime_product_revision()));
    FAIL() << "old sealed ROS profile must not enter the current build contract";
  } catch (const RetainedBundleError &error) {
    EXPECT_NE(std::string(error.what()).find("recompile"), std::string::npos);
  }
}

TEST(RuntimeContractRegistry, PackageAndEmbeddedOffersAreOneExactValue) {
  const auto &registry = runtime_contract_declaration_registry();
  ASSERT_EQ(registry.contracts().size(), 2U);
  ASSERT_EQ(registry.package_manifest_set().offers.size(),
            registry.contracts().size());
  ASSERT_TRUE(
      core::VerifyRuntimeContractManifestSet(registry.package_manifest_set())
          .ok());

  for (const auto &row : registry.contracts()) {
    const auto package_offer = std::find_if(
        registry.package_manifest_set().offers.begin(),
        registry.package_manifest_set().offers.end(), [&](const auto &offer) {
          return offer.runtime_implementation_id ==
                 row.offer().runtime_implementation_id;
        });
    ASSERT_NE(package_offer, registry.package_manifest_set().offers.end());
    EXPECT_EQ(*package_offer, row.offer());

    const auto embedded_projection =
        detail::runtime_contract_offer_projection(row.offer());
    const auto embedded = detail::decode_runtime_contract_offer_projection(
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
  const auto &registry = runtime_contract_declaration_registry();
  std::vector<std::string> families;
  for (const auto &row : registry.contracts()) {
    ASSERT_TRUE(std::is_sorted(row.offer().provided_api_contract_ids.begin(),
                               row.offer().provided_api_contract_ids.end()));
    ASSERT_FALSE(row.runtime_role_api_mappings().empty());
    ASSERT_FALSE(row.source_endpoint_api_mappings().empty());
    ASSERT_EQ(row.offer().provided_api_contract_ids.size(),
              row.runtime_role_api_mappings().size() +
                  row.source_endpoint_api_mappings().size());
    families.push_back(row.offer().runtime_implementation_id.value());
    for (const auto &mapping : row.runtime_role_api_mappings()) {
      EXPECT_FALSE(mapping.semantic_owner_id.empty());
    }
    for (const auto &mapping : row.source_endpoint_api_mappings()) {
      EXPECT_FALSE(mapping.semantic_owner_id.empty());
    }
  }
  std::sort(families.begin(), families.end());
  EXPECT_EQ(std::adjacent_find(families.begin(), families.end()),
            families.end());
}

TEST(RuntimeContractRegistry,
     Ros2ControlOfferRetainsCanonicalLogicalSceneCapabilitiesAfterNativeRetirement) {
  const auto &registry = runtime_contract_declaration_registry();
  const auto check_offer = [&](const auto &offer, std::size_t expected_size) {
    ASSERT_EQ(offer.provided_capabilities.size(), expected_size);
    EXPECT_TRUE(std::is_sorted(
        offer.provided_capabilities.begin(), offer.provided_capabilities.end(),
        [](const auto &left, const auto &right) {
          if (left.capability_id != right.capability_id) {
            return left.capability_id < right.capability_id;
          }
          return left.capability_definition_revision <
                 right.capability_definition_revision;
        }));
    for (const auto capability_id :
         runtime_contract_capabilities::kLogicalSceneServices) {
      const auto *descriptor =
          core::find_builtin_capability_definition(capability_id);
      ASSERT_NE(descriptor, nullptr);
      const core::RuntimeCapabilityContractRefDTO expected{
          core::CapabilityId(std::string(capability_id)),
          core::CapabilityDefinitionRevision(
              std::string(descriptor->capability_definition_revision))};
      EXPECT_NE(std::find(offer.provided_capabilities.begin(),
                          offer.provided_capabilities.end(), expected),
                offer.provided_capabilities.end())
          << capability_id;
    }
  };

  const auto native = std::find_if(
      registry.contracts().begin(), registry.contracts().end(),
      [](const auto &row) {
        return row.offer().runtime_implementation_id.value() ==
               kRetiredNativeImplementationId;
      });
  const auto ros2_control = std::find_if(
      registry.contracts().begin(), registry.contracts().end(),
      [](const auto &row) {
        return row.offer().runtime_implementation_id.value() ==
               ros2_control_execution_runtime_contract::kImplementationId;
      });
  EXPECT_EQ(native, registry.contracts().end());
  ASSERT_NE(ros2_control, registry.contracts().end());
  check_offer(ros2_control->offer(), 42U);
  const auto has_operation_scale = [](const auto &offer) {
    return std::any_of(offer.provided_capabilities.begin(),
                       offer.provided_capabilities.end(), [](const auto &ref) {
                         return ref.capability_id.value() ==
                                "execution.runtime_scale.operation_scoped";
                       });
  };
  EXPECT_FALSE(has_operation_scale(ros2_control->offer()));
}

TEST(RuntimeContractRegistry,
     RealRosOfferCarriesExactIoServicesAndMissingServiceStillRefuses) {
  const auto &rows = runtime_contract_declaration_registry().contracts();
  const auto ros = std::find_if(rows.begin(), rows.end(), [](const auto &row) {
    return row.offer().runtime_implementation_id.value() ==
           ros2_control_execution_runtime_contract::kImplementationId;
  });
  ASSERT_NE(ros, rows.end());
  std::vector<core::RuntimeCapabilityContractRefDTO> services;
  for (const auto id : runtime_contract_capabilities::kIoSignalServices) {
    const auto *builtin = core::find_builtin_capability_definition(id);
    ASSERT_NE(builtin, nullptr);
    const core::RuntimeCapabilityContractRefDTO expected{
        core::CapabilityId(std::string(id)),
        core::CapabilityDefinitionRevision(
            std::string(builtin->capability_definition_revision))};
    services.push_back(expected);
    EXPECT_NE(std::find(ros->offer().provided_capabilities.begin(),
                        ros->offer().provided_capabilities.end(), expected),
              ros->offer().provided_capabilities.end());
    for (const auto &other : rows) {
      if (&other != &*ros) {
        EXPECT_EQ(std::find(other.offer().provided_capabilities.begin(),
                            other.offer().provided_capabilities.end(), expected),
                  other.offer().provided_capabilities.end());
      }
    }
  }
  const auto required = core::MakeRuntimeContractRequirement(
      ros->offer().runtime_implementation_id, {}, services);
  ASSERT_TRUE(required.ok());
  EXPECT_TRUE(core::CheckRuntimeContractCompatibility(ros->offer(), required.value()).ok());
  auto missing = ros->offer().provided_capabilities;
  std::erase_if(missing, [&](const auto &row) {
    return row.capability_id == services.front().capability_id;
  });
  const auto incomplete = core::MakeRuntimeContractOffer(
      ros->offer().runtime_implementation_id,
      ros->offer().provided_api_contract_ids, std::move(missing));
  ASSERT_TRUE(incomplete.ok());
  const auto refused =
      core::CheckRuntimeContractCompatibility(incomplete.value(), required.value());
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "required_capability_missing");
  EXPECT_EQ(refused.error().details.at("capability_id"),
            services.front().capability_id.value());
}

TEST(RuntimeContractRegistry, RetiredNativeProductIsUnavailableBeforeAdmission) {
  const auto &contracts = runtime_contract_declaration_registry().contracts();
  EXPECT_EQ(std::find_if(contracts.begin(), contracts.end(), [](const auto &row) {
              return row.runtime_profile_id().value() ==
                     "manyforge.native_execution_runtime";
            }), contracts.end());
  EXPECT_THROW(static_cast<void>(detail::require_runtime_contract(
      core::RuntimeProfileId("manyforge.native_execution_runtime"),
      core::RuntimeProfileRevision("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
      core::ProductContractId(std::string(kRetiredNativeProductContractId)),
      core::ProductContractRevision("sha256:0000000000000000000000000000000000000000000000000000000000000000"))), RetainedBundleError);
}

TEST(RuntimeContractRegistry,
     ExactAndStrictSupersetOffersPassWhileMissingOfferFactsFail) {
  const auto &registry = runtime_contract_declaration_registry();
  const auto row =
      std::find_if(registry.contracts().begin(), registry.contracts().end(),
                   [](const auto &candidate) {
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
      row->offer().provided_api_contract_ids, std::move(missing_capabilities));
  ASSERT_TRUE(missing_capability_offer.ok());
  EXPECT_FALSE(core::CheckRuntimeContractCompatibility(
                   missing_capability_offer.value(), exact.value())
                   .ok());

  auto diagnostic_capabilities = row->offer().provided_capabilities;
  ASSERT_FALSE(diagnostic_capabilities.empty());
  diagnostic_capabilities.front().capability_definition_revision =
      core::CapabilityDefinitionRevision(
          "sha256:"
          "0000000000000000000000000000000000000000000000000000000000000000");
  const auto wrong_family_requirement = core::MakeRuntimeContractRequirement(
      core::RuntimeImplementationFamilyId("manyforge.wrong.family"),
      row->offer().provided_api_contract_ids,
      std::move(diagnostic_capabilities));
  ASSERT_TRUE(wrong_family_requirement.ok());
  EXPECT_TRUE(core::CheckRuntimeContractCompatibility(
                  row->offer(), wrong_family_requirement.value())
                  .ok());
}

} // namespace
} // namespace manyforge::deployment
