#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kProviderManifestSchemaRevision =
    "sha256:b242431032bf183f81718ae1b7b8501ee5f705e52d455732e0052215565bf4cc";
constexpr std::string_view kEmptyProviderContractSchemaRevision =
    "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270";
constexpr std::string_view kExecutionCapabilityRevision =
    "sha256:7e5958db0906b84cb4e5c68ab0ef896f74dc0ba49a9ceb4d918b386abaf9f28c";
constexpr std::string_view kLifecycleObserveCapabilityRevision =
    "sha256:deb6815e681e8879c172e1a7dcd03a55154bc599efaa35167c9d23f54d6b9104";
constexpr std::string_view kLifecycleSemanticDigest =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static_assert(!std::is_aggregate_v<QualifiedProviderManifest>);
static_assert(!std::is_default_constructible_v<QualifiedProviderManifest>);
static_assert(core::kMaxProviderManifestCapabilities == 256U);
static_assert(core::kMaxProviderManifestContracts == 256U);
static_assert(core::kMaxProviderManifestEndpointRoles == 256U);

[[nodiscard]] CanonicalValue string_set(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (auto &value : values) {
    result.emplace_back(std::move(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue provider_contract_ref(
    std::string role_id = "motion_stop",
    std::string schema_id = "manyforge.value_schema.provider_contract.empty.v1",
    std::string schema_revision =
        std::string(kEmptyProviderContractSchemaRevision)) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue(std::move(role_id))},
      {"contract_schema_id", CanonicalValue(std::move(schema_id))},
      {"contract_schema_revision", CanonicalValue(std::move(schema_revision))},
  });
}

[[nodiscard]] CanonicalValue
command_support(std::string kind = "motion_controlled_stop",
                std::vector<std::string> roles = {"stop_command"},
                std::optional<std::string> registered_id = std::nullopt) {
  CanonicalValue::Object fields{
      {"command_kind", CanonicalValue(std::move(kind))},
      {"command_endpoint_role_ids", string_set(std::move(roles))},
  };
  if (registered_id.has_value()) {
    fields.emplace_back("registered_command_kind_id",
                        CanonicalValue(std::move(*registered_id)));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue
evidence_support(std::string kind = "motion_rest",
                 std::vector<std::string> roles = {"joint_state"},
                 std::optional<std::string> registered_id = std::nullopt) {
  CanonicalValue::Object fields{
      {"evidence_kind", CanonicalValue(std::move(kind))},
      {"evidence_endpoint_role_ids", string_set(std::move(roles))},
  };
  if (registered_id.has_value()) {
    fields.emplace_back("registered_evidence_kind_id",
                        CanonicalValue(std::move(*registered_id)));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue contract_declaration(
    CanonicalValue contract_ref = provider_contract_ref(),
    std::optional<CanonicalValue> command = command_support(),
    std::optional<CanonicalValue> evidence = evidence_support()) {
  CanonicalValue::Object fields{
      {"provider_contract_ref", std::move(contract_ref)},
  };
  if (command.has_value()) {
    fields.emplace_back("stop_command_support", std::move(*command));
  }
  if (evidence.has_value()) {
    fields.emplace_back("stop_evidence_support", std::move(*evidence));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue
manifest_identity_value(std::vector<std::string> capability_roles = {"execute"},
                        CanonicalValue::Array contracts = CanonicalValue::Array{
                            contract_declaration()}) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kProviderManifestApiVersion))},
      {"provider_manifest_id", CanonicalValue("xarm_trajectory_provider")},
      {"offered_capabilities",
       CanonicalValue(
           CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
               {"capability_id",
                CanonicalValue("execution.trajectory.execute")},
               {"capability_definition_revision",
                CanonicalValue(std::string(kExecutionCapabilityRevision))},
               {"required_endpoint_role_ids",
                string_set(std::move(capability_roles))},
           })})},
      {"provider_contracts", CanonicalValue(std::move(contracts))},
  });
}

[[nodiscard]] std::string document_from_identity(CanonicalValue identity) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/provider_manifest");
  preimage.push_back('\0');
  preimage.append(core::kProviderManifestApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("provider_manifest_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string revision_from_document(std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto *revision =
      parsed.root().find_object_member("provider_manifest_revision");
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto *text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] core::ProviderManifestCatalogRecordRefDTO
reference_for(std::string_view manifest_id, std::string_view document,
              const core::ContentAddressedStoreKey &store_key) {
  return {
      core::ProviderManifestId(std::string(manifest_id)),
      core::ProviderManifestRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kProviderManifestApiVersion)),
      core::SchemaRevision(std::string(kProviderManifestSchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      store_key,
  };
}

[[nodiscard]] QualifiedProviderManifest
qualify(CanonicalValue identity,
        const core::ContentAddressedStoreKey &store_key =
            core::ContentAddressedStoreKey("sha256/provider-manifest")) {
  const auto document = document_from_identity(std::move(identity));
  return resolve_and_qualify_provider_manifest(
      reference_for("xarm_trajectory_provider", document, store_key), store_key,
      document);
}

TEST(ProviderManifestCodec,
     QualifiesExactManifestAndCanonicalizesEveryRoleSet) {
  auto identity = manifest_identity_value(
      {"execute_feedback", "execute"},
      CanonicalValue::Array{contract_declaration(
          provider_contract_ref(),
          command_support("motion_controlled_stop",
                          {"stop_result", "stop_command"}),
          evidence_support("motion_rest",
                           {"joint_state_secondary", "joint_state"}))});
  const auto document = document_from_identity(identity);
  const auto qualified = qualify(std::move(identity));
  const auto &manifest = qualified.value();
  EXPECT_EQ(qualified.reference(),
            reference_for(
                "xarm_trajectory_provider", document,
                core::ContentAddressedStoreKey("sha256/provider-manifest")));

  EXPECT_EQ(manifest.provider_manifest_id,
            core::ProviderManifestId("xarm_trajectory_provider"));
  ASSERT_EQ(manifest.offered_capabilities.size(), 1U);
  EXPECT_EQ(manifest.offered_capabilities.front().required_endpoint_role_ids,
            (std::vector<core::EndpointRoleId>{
                core::EndpointRoleId("execute"),
                core::EndpointRoleId("execute_feedback")}));
  ASSERT_EQ(manifest.provider_contracts.size(), 1U);
  ASSERT_TRUE(
      manifest.provider_contracts.front().stop_command_support.has_value());
  EXPECT_EQ(
      manifest.provider_contracts.front()
          .stop_command_support->command_endpoint_role_ids,
      (std::vector<core::EndpointRoleId>{core::EndpointRoleId("stop_command"),
                                         core::EndpointRoleId("stop_result")}));
  EXPECT_EQ(canonical_json(qualified.normalized_root()), document);
}

TEST(ProviderManifestCodec,
     AllowsFocusedDirectPortsAndAnEmptyContractCollection) {
  const auto direct = qualify(manifest_identity_value(
      {},
      CanonicalValue::Array{contract_declaration(
          provider_contract_ref(), command_support("logical_cancel", {}),
          evidence_support("logical_cancellation_no_continuing_effect", {}))}));
  EXPECT_TRUE(direct.value()
                  .offered_capabilities.front()
                  .required_endpoint_role_ids.empty());
  EXPECT_TRUE(direct.value()
                  .provider_contracts.front()
                  .stop_command_support->command_endpoint_role_ids.empty());

  const auto no_contracts =
      qualify(manifest_identity_value({"execute"}, CanonicalValue::Array{}));
  EXPECT_TRUE(no_contracts.value().provider_contracts.empty());
}

TEST(ProviderManifestCodec,
     LifecycleCapabilitiesRequireOneExactSemanticCatalogAnchor) {
  auto anchored = manifest_identity_value({"execute"}, CanonicalValue::Array{});
  auto root = anchored.as_object();
  auto offered = std::find_if(root.begin(), root.end(), [](const auto &member) {
    return member.first == "offered_capabilities";
  });
  ASSERT_NE(offered, root.end());
  auto rows = std::get<CanonicalValue::Array>(offered->second.storage());
  rows.insert(
      rows.begin(),
      CanonicalValue(CanonicalValue::Object{
          {"capability_id", CanonicalValue("component.lifecycle.observe")},
          {"capability_definition_revision",
           CanonicalValue(std::string(kLifecycleObserveCapabilityRevision))},
          {"required_endpoint_role_ids", string_set({})},
      }));
  offered->second = CanonicalValue(std::move(rows));
  root.emplace_back(
      "lifecycle_plan_catalog",
      CanonicalValue(CanonicalValue::Object{
          {"catalog_api_version",
           CanonicalValue("manyforge.brand_lifecycle_plan_catalog.v1")},
          {"semantic_digest",
           CanonicalValue(std::string(kLifecycleSemanticDigest))},
      }));

  const auto qualified = qualify(CanonicalValue(root));
  ASSERT_TRUE(qualified.value().lifecycle_plan_catalog.has_value());
  EXPECT_EQ(
      qualified.value().lifecycle_plan_catalog->catalog_api_version.value(),
      "manyforge.brand_lifecycle_plan_catalog.v1");
  EXPECT_EQ(qualified.value().lifecycle_plan_catalog->semantic_digest.value(),
            kLifecycleSemanticDigest);

  auto missing_anchor = root;
  missing_anchor.erase(
      std::remove_if(missing_anchor.begin(), missing_anchor.end(),
                     [](const auto &member) {
                       return member.first == "lifecycle_plan_catalog";
                     }),
      missing_anchor.end());
  EXPECT_THROW(qualify(CanonicalValue(std::move(missing_anchor))),
               ProviderManifestCodecError);

  auto declaration_without_capability =
      manifest_identity_value({"execute"}, CanonicalValue::Array{}).as_object();
  declaration_without_capability.emplace_back(
      "lifecycle_plan_catalog",
      CanonicalValue(CanonicalValue::Object{
          {"catalog_api_version",
           CanonicalValue("manyforge.brand_lifecycle_plan_catalog.v1")},
          {"semantic_digest",
           CanonicalValue(std::string(kLifecycleSemanticDigest))},
      }));
  EXPECT_THROW(
      qualify(CanonicalValue(std::move(declaration_without_capability))),
      ProviderManifestCodecError);
}

TEST(ProviderManifestCodec, RequiresExactBuiltinCapabilityDefinitions) {
  auto unknown = manifest_identity_value();
  auto root = unknown.as_object();
  auto offered = std::find_if(root.begin(), root.end(), [](const auto &member) {
    return member.first == "offered_capabilities";
  });
  ASSERT_NE(offered, root.end());
  auto rows = std::get<CanonicalValue::Array>(offered->second.storage());
  auto row = rows.front().as_object();
  auto capability_id =
      std::find_if(row.begin(), row.end(), [](const auto &member) {
        return member.first == "capability_id";
      });
  ASSERT_NE(capability_id, row.end());
  capability_id->second = CanonicalValue("execution.trajectory.unknown");
  rows.front() = CanonicalValue(std::move(row));
  offered->second = CanonicalValue(std::move(rows));
  EXPECT_THROW(qualify(CanonicalValue(std::move(root))),
               ProviderManifestCodecError);

  auto stale = manifest_identity_value();
  root = stale.as_object();
  offered = std::find_if(root.begin(), root.end(), [](const auto &member) {
    return member.first == "offered_capabilities";
  });
  rows = std::get<CanonicalValue::Array>(offered->second.storage());
  row = rows.front().as_object();
  auto revision = std::find_if(row.begin(), row.end(), [](const auto &member) {
    return member.first == "capability_definition_revision";
  });
  ASSERT_NE(revision, row.end());
  revision->second = CanonicalValue(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  rows.front() = CanonicalValue(std::move(row));
  offered->second = CanonicalValue(std::move(rows));
  EXPECT_THROW(qualify(CanonicalValue(std::move(root))),
               ProviderManifestCodecError);
}

TEST(ProviderManifestCodec,
     RequiresExactQualifiedProviderContractValueSchemas) {
  const auto unknown_schema = manifest_identity_value(
      {"execute"},
      CanonicalValue::Array{contract_declaration(provider_contract_ref(
          "motion_stop",
          "manyforge.value_schema.provider_contract.unknown.v1"))});
  EXPECT_THROW(qualify(unknown_schema), ProviderManifestCodecError);

  const auto stale_schema = manifest_identity_value(
      {"execute"},
      CanonicalValue::Array{contract_declaration(provider_contract_ref(
          "motion_stop", "manyforge.value_schema.provider_contract.empty.v1",
          "sha256:"
          "000000000000000000000000000000000000000000000000000000000000000"
          "0"))});
  EXPECT_THROW(qualify(stale_schema), ProviderManifestCodecError);
}

TEST(ProviderManifestCodec,
     EnforcesSupportPresenceAndRegisteredKindConditionals) {
  EXPECT_THROW(qualify(manifest_identity_value(
                   {"execute"},
                   CanonicalValue::Array{contract_declaration(
                       provider_contract_ref(), std::nullopt, std::nullopt)})),
               ProviderManifestCodecError);

  EXPECT_THROW(
      qualify(manifest_identity_value(
          {"execute"}, CanonicalValue::Array{contract_declaration(
                           provider_contract_ref(),
                           command_support("registered"), std::nullopt)})),
      ProviderManifestCodecError);
  EXPECT_THROW(qualify(manifest_identity_value(
                   {"execute"},
                   CanonicalValue::Array{contract_declaration(
                       provider_contract_ref(),
                       command_support("safe_output_command", {},
                                       std::string("unexpected_registered_id")),
                       std::nullopt)})),
               ProviderManifestCodecError);

  const auto registered = qualify(manifest_identity_value(
      {"execute"},
      CanonicalValue::Array{contract_declaration(
          provider_contract_ref(),
          command_support("registered", {},
                          std::string("vendor_controlled_stop")),
          evidence_support("registered", {},
                           std::string("vendor_rest_evidence")))}));
  EXPECT_EQ(registered.value()
                .provider_contracts.front()
                .stop_command_support->registered_command_kind_id,
            core::RegisteredStopCommandKindId("vendor_controlled_stop"));
}

TEST(ProviderManifestCodec, BoundsTheComputedManifestWideEndpointRoleUnion) {
  std::vector<std::string> roles;
  roles.reserve(core::kMaxProviderManifestEndpointRoles);
  for (std::size_t index = 0U; index < core::kMaxProviderManifestEndpointRoles;
       ++index) {
    roles.push_back("role_" + std::to_string(index));
  }
  EXPECT_NO_THROW(
      qualify(manifest_identity_value(roles, CanonicalValue::Array{})));

  EXPECT_THROW(
      qualify(manifest_identity_value(
          std::move(roles),
          CanonicalValue::Array{contract_declaration(
              provider_contract_ref(),
              command_support("motion_controlled_stop", {"overflow_role"}),
              std::nullopt)})),
      ProviderManifestCodecError);
}

TEST(ProviderManifestCodec, RejectsLegacyFieldsAndIntrinsicStopKinds) {
  auto legacy = manifest_identity_value();
  auto root = legacy.as_object();
  root.emplace_back("runtime_dependencies", CanonicalValue::Array{});
  const auto legacy_document =
      document_from_identity(CanonicalValue(std::move(root)));
  const core::ContentAddressedStoreKey legacy_key{"sha256/legacy-provider"};
  EXPECT_THROW(resolve_and_qualify_provider_manifest(
                   reference_for("xarm_trajectory_provider", legacy_document,
                                 legacy_key),
                   legacy_key, legacy_document),
               ClosedSchemaError);

  auto intrinsic = manifest_identity_value(
      {"execute"},
      CanonicalValue::Array{contract_declaration(
          provider_contract_ref(),
          command_support("controller_goal_cancel", {}), std::nullopt)});
  const auto intrinsic_document = document_from_identity(std::move(intrinsic));
  const core::ContentAddressedStoreKey intrinsic_key{
      "sha256/intrinsic-provider"};
  EXPECT_THROW(resolve_and_qualify_provider_manifest(
                   reference_for("xarm_trajectory_provider", intrinsic_document,
                                 intrinsic_key),
                   intrinsic_key, intrinsic_document),
               ClosedSchemaError);
}

TEST(ProviderManifestCodec, RejectsEveryOuterReferentMismatch) {
  const auto document = document_from_identity(manifest_identity_value());
  const core::ContentAddressedStoreKey store_key{
      "sha256/provider-manifest-referent"};
  const auto reference =
      reference_for("xarm_trajectory_provider", document, store_key);

  EXPECT_THROW(
      resolve_and_qualify_provider_manifest(
          reference, core::ContentAddressedStoreKey("sha256/other"), document),
      ProviderManifestCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(
      resolve_and_qualify_provider_manifest(wrong_digest, store_key, document),
      ProviderManifestCodecError);

  auto wrong_id = reference;
  wrong_id.record_id = core::ProviderManifestId("different_manifest");
  EXPECT_THROW(
      resolve_and_qualify_provider_manifest(wrong_id, store_key, document),
      ProviderManifestCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision = core::ProviderManifestRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_provider_manifest(wrong_revision, store_key,
                                                     document),
               ProviderManifestCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id = core::SchemaId("manyforge.component_type.v1");
  EXPECT_THROW(resolve_and_qualify_provider_manifest(wrong_schema_id, store_key,
                                                     document),
               ProviderManifestCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision = core::SchemaRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_provider_manifest(wrong_schema_revision,
                                                     store_key, document),
               ProviderManifestCodecError);

  auto stale_semantic_document = document;
  const auto role_offset = stale_semantic_document.find("\"execute\"");
  ASSERT_NE(role_offset, std::string::npos);
  stale_semantic_document.replace(role_offset,
                                  std::string_view("\"execute\"").size(),
                                  "\"execute_changed\"");
  EXPECT_THROW(resolve_and_qualify_provider_manifest(
                   reference_for("xarm_trajectory_provider",
                                 stale_semantic_document, store_key),
                   store_key, stale_semantic_document),
               ProviderManifestCodecError);
}

} // namespace
} // namespace manyforge::deployment
