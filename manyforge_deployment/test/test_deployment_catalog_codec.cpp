#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_core/types/catalog.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment::detail {
namespace {

struct AuthoredReference {
  std::string_view kind;
  std::string_view id;
  std::string_view family;
  char revision_digit;
  char schema_digit;
  char content_digit;
  char key_digit;
};

// Authored independently from kCatalogRecordTypeRegistry so this fixture can
// catch a mistaken or incomplete implementation mapping.
constexpr std::array<AuthoredReference, 10> kAllReferences{{
    {"model_asset", "model.alpha", "manyforge.model_asset.v1", '1', '2',
     '3', '4'},
    {"runtime_profile", "runtime.alpha", "manyforge.runtime_profile.v1",
     '2', '3', '4', '5'},
    {"component_type", "component.alpha", "manyforge.component_type.v1",
     '3', '4', '5', '6'},
    {"control_interface_contract", "contract.alpha",
     "manyforge.control_interface_contract.v1", '4', '5', '6', '7'},
    {"controller_topology", "topology.alpha",
     "manyforge.controller_topology.v1", '5', '6', '7', '8'},
    {"provider_manifest", "provider.alpha", "manyforge.provider_manifest.v1",
     '6', '7', '8', '9'},
    {"command_schema", "command.alpha", "manyforge.command_schema.v1", '7',
     '8', '9', 'a'},
    {"capability_definition", "capability.alpha",
     "manyforge.capability_definition.v1", '8', '9', 'a', 'b'},
    {"contact_policy", "contact.alpha", "manyforge.contact_policy.v1", '9',
     'a', 'b', 'c'},
    {"stop_evidence_policy", "stop.alpha",
     "manyforge.stop_evidence_policy.v2", 'a', 'b', 'c', 'd'},
}};

[[nodiscard]] std::string digest(char digit) {
  return "sha256:" + std::string(64U, digit);
}

[[nodiscard]] std::string store_key(char digit) {
  return "sha256/" + std::string(64U, digit);
}

[[nodiscard]] CanonicalValue reference_value(
    const AuthoredReference& reference) {
  return CanonicalValue(CanonicalValue::Object{
      {"record_kind", CanonicalValue(std::string(reference.kind))},
      {"record_id", CanonicalValue(std::string(reference.id))},
      {"record_revision", CanonicalValue(digest(reference.revision_digit))},
      {"schema_id", CanonicalValue(std::string(reference.family))},
      {"schema_revision", CanonicalValue(digest(reference.schema_digit))},
      {"content_digest", CanonicalValue(digest(reference.content_digit))},
      {"content_addressed_store_key",
       CanonicalValue(store_key(reference.key_digit))},
  });
}

[[nodiscard]] CanonicalValue catalogs_value(
    const std::vector<std::size_t>& authored_order) {
  CanonicalValue::Array records;
  records.reserve(authored_order.size());
  for (const std::size_t index : authored_order) {
    records.push_back(reference_value(kAllReferences[index]));
  }
  return CanonicalValue(
      CanonicalValue::Object{{"records", CanonicalValue(std::move(records))}});
}

template <typename Record>
[[nodiscard]] core::CatalogRecordRefDTO typed_reference_as(
    const AuthoredReference& reference) {
  return Record{
      typename Record::IdType(std::string(reference.id)),
      typename Record::RevisionType(digest(reference.revision_digit)),
      core::SchemaId(std::string(reference.family)),
      core::SchemaRevision(digest(reference.schema_digit)),
      core::ContentDigest(digest(reference.content_digit)),
      core::ContentAddressedStoreKey(store_key(reference.key_digit)),
  };
}

[[nodiscard]] core::CatalogRecordRefDTO independently_typed_reference(
    std::size_t index) {
  const auto& reference = kAllReferences.at(index);
  switch (index) {
    case 0U:
      return typed_reference_as<core::ModelAssetCatalogRecordRefDTO>(
          reference);
    case 1U:
      return typed_reference_as<core::RuntimeProfileCatalogRecordRefDTO>(
          reference);
    case 2U:
      return typed_reference_as<core::ComponentTypeCatalogRecordRefDTO>(
          reference);
    case 3U:
      return typed_reference_as<
          core::ControlInterfaceContractCatalogRecordRefDTO>(reference);
    case 4U:
      return typed_reference_as<core::ControllerTopologyCatalogRecordRefDTO>(
          reference);
    case 5U:
      return typed_reference_as<core::ProviderManifestCatalogRecordRefDTO>(
          reference);
    case 6U:
      return typed_reference_as<core::CommandSchemaCatalogRecordRefDTO>(
          reference);
    case 7U:
      return typed_reference_as<
          core::CapabilityDefinitionCatalogRecordRefDTO>(reference);
    case 8U:
      return typed_reference_as<core::ContactPolicyCatalogRecordRefDTO>(
          reference);
    case 9U:
      return typed_reference_as<
          core::StopEvidencePolicyCatalogRecordRefDTO>(reference);
    default:
      throw std::out_of_range("unknown authored catalog reference index");
  }
}

[[nodiscard]] const CanonicalValue::Array& encoded_records(
    const CanonicalValue& catalogs) {
  const CanonicalValue* records = catalogs.find_object_member("records");
  EXPECT_NE(records, nullptr);
  return std::get<CanonicalValue::Array>(records->storage());
}

[[nodiscard]] CanonicalValue replace_field(const CanonicalValue& record,
                                           std::string_view field,
                                           CanonicalValue replacement) {
  CanonicalValue::Object object = record.as_object();
  const auto iterator =
      std::find_if(object.begin(), object.end(), [field](const auto& member) {
        return member.first == field;
      });
  EXPECT_NE(iterator, object.end());
  iterator->second = std::move(replacement);
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue one_record_catalog(CanonicalValue record) {
  return CanonicalValue(CanonicalValue::Object{
      {"records",
       CanonicalValue(CanonicalValue::Array{std::move(record)})},
  });
}

TEST(DeploymentCatalogCodec, DecodesAllTenStrongVariantsAndRetainsFields) {
  const auto decoded =
      decode_deployment_catalogs(catalogs_value({7, 4, 1, 9, 6, 3, 0, 8,
                                                 5, 2}));
  ASSERT_EQ(decoded.size(), kAllReferences.size());
  EXPECT_TRUE(std::holds_alternative<
              core::CapabilityDefinitionCatalogRecordRefDTO>(decoded[0]));
  EXPECT_TRUE(std::holds_alternative<core::CommandSchemaCatalogRecordRefDTO>(
      decoded[1]));
  EXPECT_TRUE(std::holds_alternative<core::ComponentTypeCatalogRecordRefDTO>(
      decoded[2]));
  EXPECT_TRUE(std::holds_alternative<core::ContactPolicyCatalogRecordRefDTO>(
      decoded[3]));
  EXPECT_TRUE(
      std::holds_alternative<
          core::ControlInterfaceContractCatalogRecordRefDTO>(decoded[4]));
  EXPECT_TRUE(
      std::holds_alternative<core::ControllerTopologyCatalogRecordRefDTO>(
          decoded[5]));
  EXPECT_TRUE(std::holds_alternative<core::ModelAssetCatalogRecordRefDTO>(
      decoded[6]));
  EXPECT_TRUE(std::holds_alternative<core::ProviderManifestCatalogRecordRefDTO>(
      decoded[7]));
  EXPECT_TRUE(std::holds_alternative<core::RuntimeProfileCatalogRecordRefDTO>(
      decoded[8]));
  EXPECT_TRUE(
      std::holds_alternative<core::StopEvidencePolicyCatalogRecordRefDTO>(
          decoded[9]));

  for (const auto& actual : decoded) {
    const std::string_view kind =
        core::to_string(core::catalog_record_kind(actual));
    const auto authored =
        std::find_if(kAllReferences.begin(), kAllReferences.end(),
                     [kind](const AuthoredReference& candidate) {
                       return candidate.kind == kind;
                     });
    ASSERT_NE(authored, kAllReferences.end());
    EXPECT_EQ(core::catalog_record_id_value(actual), authored->id);
    EXPECT_EQ(core::catalog_record_revision_value(actual),
              digest(authored->revision_digit));
    std::visit(
        [&authored](const auto& typed) {
          EXPECT_EQ(typed.schema_id.value(), authored->family);
          EXPECT_EQ(typed.schema_revision.value(),
                    digest(authored->schema_digit));
          EXPECT_EQ(typed.content_digest.value(),
                    digest(authored->content_digit));
          EXPECT_EQ(typed.content_addressed_store_key.value(),
                    store_key(authored->key_digit));
        },
        actual);
  }
}

TEST(DeploymentCatalogCodec, EncodingIsCanonicalAndShuffleIndependent) {
  const auto forward =
      decode_deployment_catalogs(catalogs_value({0, 1, 2, 3, 4, 5, 6, 7, 8,
                                                 9}));
  const auto reverse =
      decode_deployment_catalogs(catalogs_value({9, 8, 7, 6, 5, 4, 3, 2, 1,
                                                 0}));
  const CanonicalValue forward_encoded = encode_deployment_catalogs(forward);
  const CanonicalValue reverse_encoded = encode_deployment_catalogs(reverse);
  EXPECT_EQ(forward_encoded, reverse_encoded);
  EXPECT_EQ(canonical_json(forward_encoded), canonical_json(reverse_encoded));
  EXPECT_EQ(decode_deployment_catalogs(forward_encoded), forward);

  const auto& records = encoded_records(forward_encoded);
  ASSERT_EQ(records.size(), kAllReferences.size());
  EXPECT_EQ(std::get<std::string>(
                records.front()
                    .find_object_member("record_kind")
                    ->storage()),
            "capability_definition");
  EXPECT_EQ(std::get<std::string>(
                records.back()
                    .find_object_member("record_kind")
                    ->storage()),
            "stop_evidence_policy");
}

TEST(DeploymentCatalogCodec, EncoderMatchesIndependentTenVariantOracle) {
  std::vector<core::CatalogRecordRefDTO> typed;
  typed.reserve(kAllReferences.size());
  for (std::size_t index = 0U; index < kAllReferences.size(); ++index) {
    typed.push_back(independently_typed_reference(index));
  }

  const CanonicalValue encoded = encode_deployment_catalogs(typed);
  const CanonicalValue expected = catalogs_value(
      {7U, 6U, 2U, 8U, 3U, 4U, 0U, 5U, 1U, 9U});
  EXPECT_EQ(encoded, expected);
  EXPECT_EQ(canonical_json(encoded), canonical_json(expected));
}

TEST(DeploymentCatalogCodec, RejectsNonExactCatalogAndRecordShapes) {
  EXPECT_THROW(decode_deployment_catalogs(CanonicalValue("not-an-object")),
               DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(
          CanonicalValue(CanonicalValue::Object{
              {"records", CanonicalValue(CanonicalValue::Array{})},
              {"extra", CanonicalValue("forbidden")},
          })),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(
          CanonicalValue(CanonicalValue::Object{
              {"records", CanonicalValue("not-an-array")},
          })),
      DeploymentCatalogCodecError);

  CanonicalValue::Object missing = reference_value(kAllReferences[0]).as_object();
  missing.erase(std::find_if(missing.begin(), missing.end(),
                             [](const auto& member) {
                               return member.first == "schema_revision";
                             }));
  EXPECT_THROW(
      decode_deployment_catalogs(
          one_record_catalog(CanonicalValue(std::move(missing)))),
      DeploymentCatalogCodecError);

  CanonicalValue::Object extra = reference_value(kAllReferences[0]).as_object();
  extra.emplace_back("extra", CanonicalValue("forbidden"));
  EXPECT_THROW(
      decode_deployment_catalogs(
          one_record_catalog(CanonicalValue(std::move(extra)))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          reference_value(kAllReferences[0]), "record_id",
          CanonicalValue(true)))),
      DeploymentCatalogCodecError);
}

TEST(DeploymentCatalogCodec, RejectsUnknownKindAndWrongSchemaFamily) {
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          reference_value(kAllReferences[0]), "record_kind",
          CanonicalValue("unknown_kind")))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          reference_value(kAllReferences[0]), "schema_id",
          CanonicalValue("manyforge.runtime_profile.v1")))),
      DeploymentCatalogCodecError);
}

TEST(DeploymentCatalogCodec, RejectsInvalidIdDigestRevisionAndStoreKeySyntax) {
  const CanonicalValue valid = reference_value(kAllReferences[0]);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(
          replace_field(valid, "record_id", CanonicalValue("_invalid")))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          valid, "record_revision", CanonicalValue("sha256:ABC")))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          valid, "schema_revision",
          CanonicalValue("sha256:" + std::string(64U, 'G'))))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          valid, "content_digest",
          CanonicalValue("sha256:" + std::string(63U, 'a'))))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          valid, "content_addressed_store_key",
          CanonicalValue("")))),
      DeploymentCatalogCodecError);
  EXPECT_THROW(
      decode_deployment_catalogs(one_record_catalog(replace_field(
          valid, "content_addressed_store_key",
          CanonicalValue(std::string(kMaxArtifactScalarBytes + 1U, 'k'))))),
      DeploymentCatalogCodecError);
}

TEST(DeploymentCatalogCodec, RejectsExactDuplicatesAndConflictingMetadata) {
  const CanonicalValue row = reference_value(kAllReferences[0]);
  EXPECT_THROW(
      decode_deployment_catalogs(CanonicalValue(CanonicalValue::Object{
          {"records", CanonicalValue(CanonicalValue::Array{row, row})},
      })),
      DeploymentCatalogCodecError);

  CanonicalValue conflict =
      replace_field(row, "content_digest", CanonicalValue(digest('f')));
  conflict = replace_field(conflict, "content_addressed_store_key",
                           CanonicalValue(store_key('f')));
  EXPECT_THROW(
      decode_deployment_catalogs(CanonicalValue(CanonicalValue::Object{
          {"records",
           CanonicalValue(CanonicalValue::Array{row, conflict})},
      })),
      DeploymentCatalogCodecError);
}

TEST(DeploymentCatalogCodec, EncoderRejectsInvalidTypedReferences) {
  core::ModelAssetCatalogRecordRefDTO invalid{
      core::ModelAssetId("model.alpha"),
      core::ModelRevision(digest('1')),
      core::SchemaId("manyforge.runtime_profile.v1"),
      core::SchemaRevision(digest('2')),
      core::ContentDigest(digest('3')),
      core::ContentAddressedStoreKey(store_key('4')),
  };
  const core::CatalogRecordRefDTO erased = invalid;
  EXPECT_THROW(encode_deployment_catalogs(
                   std::span<const core::CatalogRecordRefDTO>(&erased, 1U)),
               DeploymentCatalogCodecError);
}

TEST(DeploymentCatalogCodec, AcceptsEmptyAndDistinctRevisionTables) {
  EXPECT_TRUE(decode_deployment_catalogs(catalogs_value({})).empty());

  CanonicalValue second_revision = replace_field(
      reference_value(kAllReferences[0]), "record_revision",
      CanonicalValue(digest('f')));
  const auto decoded = decode_deployment_catalogs(
      CanonicalValue(CanonicalValue::Object{
          {"records",
           CanonicalValue(CanonicalValue::Array{
               reference_value(kAllReferences[0]),
               std::move(second_revision)})},
      }));
  ASSERT_EQ(decoded.size(), 2U);
  EXPECT_NE(core::catalog_record_revision_value(decoded[0]),
            core::catalog_record_revision_value(decoded[1]));
}

TEST(DeploymentCatalogCodec, EnforcesAggregateBoundsBeforeDecodeOrEncode) {
  CanonicalValue::Array oversized_nodes(
      kMaxArtifactNodeCount, CanonicalValue(nullptr));
  EXPECT_THROW(
      decode_deployment_catalogs(
          CanonicalValue(CanonicalValue::Object{
              {"records", CanonicalValue(std::move(oversized_nodes))},
          })),
      std::invalid_argument);

  const auto one =
      decode_deployment_catalogs(catalogs_value({0})).front();
  constexpr std::size_t kNodesPerCatalogRecord = 15U;
  constexpr std::size_t kContainerNodes = 3U;
  constexpr std::size_t kMaximumCatalogRecords =
      (kMaxArtifactNodeCount - kContainerNodes) /
      kNodesPerCatalogRecord;
  CanonicalValue::Array impossible_record_count(
      kMaximumCatalogRecords + 1U, CanonicalValue(nullptr));
  try {
    (void)decode_deployment_catalogs(
        CanonicalValue(CanonicalValue::Object{
            {"records",
             CanonicalValue(std::move(impossible_record_count))},
        }));
    FAIL() << "expected DeploymentCatalogCodecError";
  } catch (const DeploymentCatalogCodecError& error) {
    EXPECT_NE(std::string_view(error.what()).find(
                  "catalog record count exceeds the fixed v1 output budget"),
              std::string_view::npos)
        << error.what();
  }

  const std::vector<core::CatalogRecordRefDTO> oversized_records(
      kMaximumCatalogRecords + 1U, one);
  EXPECT_THROW(encode_deployment_catalogs(oversized_records),
               DeploymentCatalogCodecError);
}

TEST(DeploymentCatalogCodec,
     RejectsAggregateScalarBudgetBeforeCopyingTheCallerTable) {
  const std::string maximum_store_key(kMaxArtifactScalarBytes, 'k');
  std::vector<core::CatalogRecordRefDTO> records;
  records.reserve(17U);
  for (std::size_t index = 0U; index < 17U; ++index) {
    records.emplace_back(core::ModelAssetCatalogRecordRefDTO{
        core::ModelAssetId("model.scalar_" + std::to_string(index)),
        core::ModelRevision(digest('1')),
        core::SchemaId("manyforge.model_asset.v1"),
        core::SchemaRevision(digest('2')),
        core::ContentDigest(digest('3')),
        core::ContentAddressedStoreKey(maximum_store_key),
    });
  }

  EXPECT_THROW(encode_deployment_catalogs(records),
               DeploymentCatalogCodecError);
}

}  // namespace
}  // namespace manyforge::deployment::detail
