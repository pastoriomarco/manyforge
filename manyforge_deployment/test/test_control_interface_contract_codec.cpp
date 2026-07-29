#include <gtest/gtest.h>

#include <cstddef>
#include <manyforge_core/types/control_interface_contract.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

using Claim = core::ResourceInterfaceClaimDTO;

constexpr std::string_view kControlInterfaceSchemaRevision =
    "sha256:4abdf2c402d8e6e818bc26df59ce1c9cdc911d1e807144bdf525d2f69d0a449d";

static_assert(!std::is_aggregate_v<QualifiedControlInterfaceContract>);
static_assert(!std::is_default_constructible_v<QualifiedControlInterfaceContract>);

[[nodiscard]] CanonicalValue claim_value(const Claim& claim) {
  return CanonicalValue(CanonicalValue::Object{
      {"resource_role_id", CanonicalValue(claim.resource_role_id.value())},
      {"interface_name", CanonicalValue(claim.interface_name)},
  });
}

[[nodiscard]] std::string contract_document(std::string_view contract_id,
                                            const std::vector<Claim>& command_interfaces,
                                            const std::vector<Claim>& state_interfaces,
                                            const std::vector<core::ResourceRoleId>& ordering) {
  CanonicalValue::Array command_values;
  command_values.reserve(command_interfaces.size());
  for (const auto& claim : command_interfaces) {
    command_values.push_back(claim_value(claim));
  }
  CanonicalValue::Array state_values;
  state_values.reserve(state_interfaces.size());
  for (const auto& claim : state_interfaces) {
    state_values.push_back(claim_value(claim));
  }
  CanonicalValue::Array ordering_values;
  ordering_values.reserve(ordering.size());
  for (const auto& role : ordering) {
    ordering_values.emplace_back(role.value());
  }

  const CanonicalValue identity_value(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(core::kControlInterfaceContractApiVersion))},
      {"control_interface_contract_id", CanonicalValue(std::string(contract_id))},
      {"command_interfaces", CanonicalValue(std::move(command_values))},
      {"state_interfaces", CanonicalValue(std::move(state_values))},
      {"ordering", CanonicalValue(std::move(ordering_values))},
  });
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/control_interface_contract");
  preimage.push_back('\0');
  preimage.append(core::kControlInterfaceContractApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity_value));
  const auto revision = sha256_digest(preimage);

  auto root = identity_value.as_object();
  root.emplace_back("control_interface_revision", CanonicalValue(revision));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string revision_from_document(std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision = parsed.root().find_object_member("control_interface_revision");
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] core::ControlInterfaceContractCatalogRecordRefDTO reference_for(
    std::string_view contract_id, std::string_view document,
    const core::ContentAddressedStoreKey& store_key) {
  return {
      core::ControlInterfaceContractId(std::string(contract_id)),
      core::ControlInterfaceRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kControlInterfaceContractApiVersion)),
      core::SchemaRevision(std::string(kControlInterfaceSchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      store_key,
  };
}

void expect_semantic_rejection(const std::vector<Claim>& command_interfaces,
                               const std::vector<Claim>& state_interfaces,
                               const std::vector<core::ResourceRoleId>& ordering) {
  const auto document = contract_document("contract.semantic.negative", command_interfaces,
                                          state_interfaces, ordering);
  const core::ContentAddressedStoreKey store_key{"sha256/control-interface-semantic-negative"};
  const auto reference = reference_for("contract.semantic.negative", document, store_key);
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(reference, store_key, document),
               ControlInterfaceContractCodecError);
}

TEST(ControlInterfaceContractCodec, QualifiesArbitraryAuthoredContractAndPreservesEveryOrder) {
  const std::vector<Claim> command{
      {core::ResourceRoleId("arm"), "position"},
      {core::ResourceRoleId("arm"), "effort"},
      {core::ResourceRoleId("tool"), "position"},
  };
  const std::vector<Claim> state{
      {core::ResourceRoleId("arm"), "position"},
      {core::ResourceRoleId("tool"), "position"},
      {core::ResourceRoleId("tool"), "velocity"},
  };
  const std::vector<core::ResourceRoleId> ordering{core::ResourceRoleId("arm"),
                                                   core::ResourceRoleId("tool")};
  const auto document =
      contract_document("application_specific_arm_and_tool", command, state, ordering);
  const core::ContentAddressedStoreKey store_key{"sha256/control-interface-authored"};
  const auto reference = reference_for("application_specific_arm_and_tool", document, store_key);

  const auto qualified =
      resolve_and_qualify_control_interface_contract(reference, store_key, document);
  EXPECT_EQ(qualified.value().control_interface_contract_id,
            core::ControlInterfaceContractId("application_specific_arm_and_tool"));
  EXPECT_EQ(qualified.value().control_interface_revision, reference.record_revision);
  EXPECT_EQ(qualified.value().command_interfaces, command);
  EXPECT_EQ(qualified.value().state_interfaces, state);
  EXPECT_EQ(qualified.value().ordering, ordering);
  EXPECT_EQ(canonical_json(qualified.normalized_root()), document);
}

TEST(ControlInterfaceContractCodec, AcceptsEitherSingleDirectionAndSamePairAcrossDirections) {
  const std::vector<core::ResourceRoleId> ordering{core::ResourceRoleId("io")};
  const std::vector<Claim> claims{{core::ResourceRoleId("io"), "value"}};
  const core::ContentAddressedStoreKey store_key{"sha256/control-interface-direction"};

  for (const auto& [command, state] :
       std::vector<std::pair<std::vector<Claim>, std::vector<Claim>>>{
           {claims, {}}, {{}, claims}, {claims, claims}}) {
    const auto document = contract_document("direction_contract", command, state, ordering);
    const auto reference = reference_for("direction_contract", document, store_key);
    EXPECT_NO_THROW(resolve_and_qualify_control_interface_contract(reference, store_key, document));
  }
}

TEST(ControlInterfaceContractCodec, RejectsEveryCrossFieldOrderingDefect) {
  const Claim arm_position{core::ResourceRoleId("arm"), "position"};
  const Claim arm_velocity{core::ResourceRoleId("arm"), "velocity"};
  const Claim tool_position{core::ResourceRoleId("tool"), "position"};

  expect_semantic_rejection({}, {}, {});
  expect_semantic_rejection({arm_position, arm_position}, {}, {core::ResourceRoleId("arm")});
  expect_semantic_rejection({arm_position, tool_position, arm_velocity}, {},
                            {core::ResourceRoleId("arm"), core::ResourceRoleId("tool")});
  expect_semantic_rejection({tool_position, arm_position}, {},
                            {core::ResourceRoleId("arm"), core::ResourceRoleId("tool")});
  expect_semantic_rejection({arm_position}, {},
                            {core::ResourceRoleId("arm"), core::ResourceRoleId("arm")});
  expect_semantic_rejection({arm_position, tool_position}, {}, {core::ResourceRoleId("arm")});
  expect_semantic_rejection({arm_position}, {},
                            {core::ResourceRoleId("arm"), core::ResourceRoleId("tool")});
}

TEST(ControlInterfaceContractCodec, AcceptsAndRejectsExactArrayBoundaries) {
  std::vector<Claim> exact_claims;
  std::vector<core::ResourceRoleId> exact_ordering;
  exact_claims.reserve(core::kMaxControlInterfaceContractItems);
  exact_ordering.reserve(core::kMaxControlInterfaceContractItems);
  for (std::size_t index = 0; index < core::kMaxControlInterfaceContractItems; ++index) {
    const std::string role = std::string("r") + std::to_string(index);
    exact_claims.push_back({core::ResourceRoleId(role), "position"});
    exact_ordering.emplace_back(role);
  }
  const core::ContentAddressedStoreKey store_key{"sha256/control-interface-boundary"};
  const auto exact_document = contract_document("exact_boundary", exact_claims, {}, exact_ordering);
  const auto exact_reference = reference_for("exact_boundary", exact_document, store_key);
  const auto exact =
      resolve_and_qualify_control_interface_contract(exact_reference, store_key, exact_document);
  EXPECT_EQ(exact.value().command_interfaces.size(), core::kMaxControlInterfaceContractItems);
  EXPECT_EQ(exact.value().ordering.size(), core::kMaxControlInterfaceContractItems);

  std::vector<Claim> too_many_claims(core::kMaxControlInterfaceContractItems + 1U,
                                     Claim{core::ResourceRoleId("role"), "position"});
  for (std::size_t index = 0; index < too_many_claims.size(); ++index) {
    too_many_claims[index].interface_name = std::string("i") + std::to_string(index);
  }
  const auto too_many_command =
      contract_document("too_many_command", too_many_claims, {}, {core::ResourceRoleId("role")});
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(
                   reference_for("too_many_command", too_many_command, store_key), store_key,
                   too_many_command),
               ClosedSchemaError);

  const auto too_many_state =
      contract_document("too_many_state", {}, too_many_claims, {core::ResourceRoleId("role")});
  EXPECT_THROW(
      resolve_and_qualify_control_interface_contract(
          reference_for("too_many_state", too_many_state, store_key), store_key, too_many_state),
      ClosedSchemaError);

  auto too_many_ordering = exact_ordering;
  too_many_ordering.emplace_back("overflow");
  const auto too_many_order =
      contract_document("too_many_order", {exact_claims.front()}, {}, too_many_ordering);
  EXPECT_THROW(
      resolve_and_qualify_control_interface_contract(
          reference_for("too_many_order", too_many_order, store_key), store_key, too_many_order),
      ClosedSchemaError);
}

TEST(ControlInterfaceContractCodec, RejectsInvalidTokensOpenShapeAndStaleSemanticIdentity) {
  const std::vector<Claim> valid{{core::ResourceRoleId("arm"), "position"}};
  const std::vector<core::ResourceRoleId> ordering{core::ResourceRoleId("arm")};
  const core::ContentAddressedStoreKey store_key{"sha256/control-interface-shape-negative"};

  const auto invalid_token =
      contract_document("invalid_token", {{core::ResourceRoleId("arm"), "bad/name"}}, {}, ordering);
  EXPECT_THROW(
      resolve_and_qualify_control_interface_contract(
          reference_for("invalid_token", invalid_token, store_key), store_key, invalid_token),
      ClosedSchemaError);

  const auto valid_document = contract_document("open_shape", valid, {}, ordering);
  auto open_shape = valid_document;
  const auto closing = open_shape.rfind('}');
  ASSERT_NE(closing, std::string::npos);
  open_shape.insert(closing, ",\"unexpected\":true");
  auto open_reference = reference_for("open_shape", valid_document, store_key);
  open_reference.content_digest = core::ContentDigest(sha256_digest(open_shape));
  EXPECT_THROW(
      resolve_and_qualify_control_interface_contract(open_reference, store_key, open_shape),
      ClosedSchemaError);

  auto stale = valid_document;
  const auto digest = stale.find("sha256:");
  ASSERT_NE(digest, std::string::npos);
  stale[digest + 7U] = stale[digest + 7U] == '0' ? '1' : '0';
  auto stale_reference = reference_for("open_shape", valid_document, store_key);
  stale_reference.content_digest = core::ContentDigest(sha256_digest(stale));
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(stale_reference, store_key, stale),
               ControlInterfaceContractCodecError);
}

TEST(ControlInterfaceContractCodec, PresentationIsIrrelevantButClaimAndRoleOrderAreSemantic) {
  const std::vector<Claim> claims{{core::ResourceRoleId("arm"), "position"},
                                  {core::ResourceRoleId("arm"), "velocity"}};
  const std::vector<core::ResourceRoleId> ordering{core::ResourceRoleId("arm")};
  const auto canonical = contract_document("presentation", claims, {}, ordering);
  const auto revision = revision_from_document(canonical);
  const std::string yaml =
      "ordering: [arm]\n"
      "state_interfaces: []\n"
      "command_interfaces:\n"
      "  - interface_name: position\n"
      "    resource_role_id: arm\n"
      "  - resource_role_id: arm\n"
      "    interface_name: velocity\n"
      "control_interface_revision: " +
      revision +
      "\ncontrol_interface_contract_id: presentation\n"
      "api_version: manyforge.control_interface_contract.v1\n";
  const core::ContentAddressedStoreKey canonical_key{"sha256/control-interface-canonical"};
  const core::ContentAddressedStoreKey yaml_key{"sha256/control-interface-yaml"};
  const auto canonical_qualified = resolve_and_qualify_control_interface_contract(
      reference_for("presentation", canonical, canonical_key), canonical_key, canonical);
  const auto yaml_qualified = resolve_and_qualify_control_interface_contract(
      reference_for("presentation", yaml, yaml_key), yaml_key, yaml);
  EXPECT_EQ(canonical_qualified.value(), yaml_qualified.value());
  EXPECT_EQ(canonical_qualified.normalized_root(), yaml_qualified.normalized_root());
  EXPECT_NE(sha256_digest(canonical), sha256_digest(yaml));

  const std::vector<Claim> reversed_claims(claims.rbegin(), claims.rend());
  const auto reversed = contract_document("presentation", reversed_claims, {}, ordering);
  EXPECT_NE(revision_from_document(canonical), revision_from_document(reversed));

  const std::vector<Claim> role_major_claims{
      {core::ResourceRoleId("arm"), "position"},
      {core::ResourceRoleId("tool"), "position"},
  };
  const auto arm_then_tool =
      contract_document("role_order", role_major_claims, {},
                        {core::ResourceRoleId("arm"), core::ResourceRoleId("tool")});
  const std::vector<Claim> reversed_role_groups(role_major_claims.rbegin(),
                                                role_major_claims.rend());
  const auto tool_then_arm =
      contract_document("role_order", reversed_role_groups, {},
                        {core::ResourceRoleId("tool"), core::ResourceRoleId("arm")});
  EXPECT_NE(revision_from_document(arm_then_tool), revision_from_document(tool_then_arm));
}

TEST(ControlInterfaceContractCodec,
     TypedCatalogReferenceQualifiesAllSevenFieldsAndActualRetrievalKey) {
  const std::vector<Claim> claims{{core::ResourceRoleId("arm"), "position"}};
  const auto document =
      contract_document("reference_boundary", claims, {}, {core::ResourceRoleId("arm")});
  const core::ContentAddressedStoreKey store_key{"sha256/control-interface-reference"};
  auto reference = reference_for("reference_boundary", document, store_key);
  EXPECT_NO_THROW(resolve_and_qualify_control_interface_contract(reference, store_key, document));

  auto wrong_id = reference;
  wrong_id.record_id = core::ControlInterfaceContractId("different_contract");
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(wrong_id, store_key, document),
               ControlInterfaceContractCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision = core::ControlInterfaceRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(wrong_revision, store_key, document),
               ControlInterfaceContractCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id = core::SchemaId("manyforge.capability_definition.v1");
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(wrong_schema_id, store_key, document),
               ControlInterfaceContractCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision = core::SchemaRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(
      resolve_and_qualify_control_interface_contract(wrong_schema_revision, store_key, document),
      ControlInterfaceContractCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(wrong_digest, store_key, document),
               ControlInterfaceContractCodecError);

  EXPECT_THROW(resolve_and_qualify_control_interface_contract(
                   reference, core::ContentAddressedStoreKey("sha256/other"), document),
               ControlInterfaceContractCodecError);
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(
                   reference, core::ContentAddressedStoreKey(""), document),
               ControlInterfaceContractCodecError);

  const std::string oversized(kMaxArtifactEncodedBytes + 1U, ' ');
  EXPECT_THROW(resolve_and_qualify_control_interface_contract(reference, store_key, oversized),
               ControlInterfaceContractCodecError);
}

}  // namespace
}  // namespace manyforge::deployment
