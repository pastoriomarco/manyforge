#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/structural_model_preparation.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kModelAssetSchemaRevision =
    "sha256:397af1d5fbe5d2e83d442565c9096637a26595d63b5d8e03012e8f2d082fa0b6";
constexpr std::string_view kIdentityPrefix =
    "manyforge.content-identity.v1";

static_assert(
    !std::is_aggregate_v<BuildQualifiedStructuralModelPreparation>);
static_assert(
    !std::is_default_constructible_v<
        BuildQualifiedStructuralModelPreparation>);
static_assert(
    std::is_copy_constructible_v<
        BuildQualifiedStructuralModelPreparation>);
static_assert(
    !std::is_constructible_v<
        BuildQualifiedStructuralModelPreparation,
        core::RetainedGeneratorRecordDTO, SourceVerifiedModelAsset,
        std::string>);

[[nodiscard]] std::string read_file(
    const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    ADD_FAILURE() << "unable to read fixture " << path;
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

[[nodiscard]] CanonicalValue source_artifact(
    std::string logical_uri, std::string media_type,
    std::string_view bytes) {
  return CanonicalValue(CanonicalValue::Object{
      {"logical_uri", CanonicalValue(std::move(logical_uri))},
      {"media_type", CanonicalValue(std::move(media_type))},
      {"byte_size", CanonicalValue(static_cast<double>(bytes.size()))},
      {"content_digest", CanonicalValue(sha256_digest(bytes))},
  });
}

[[nodiscard]] CanonicalValue model_asset_identity(
    std::string_view urdf, std::string_view srdf,
    std::string_view mesh) {
  CanonicalValue::Array artifacts{
      source_artifact(
          "asset://qualified_model/meshes/tool.stl", "model/stl",
          mesh),
      source_artifact(
          "asset://qualified_model/model.srdf", "application/xml",
          srdf),
      source_artifact(
          "asset://qualified_model/model.urdf", "application/xml",
          urdf),
  };
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kModelAssetApiVersion))},
      {"model_asset_id", CanonicalValue("qualified_model")},
      {"instance_cardinality",
       CanonicalValue("multiple_per_cell")},
      {"expanded_source",
       CanonicalValue(CanonicalValue::Object{
           {"urdf_logical_uri",
            CanonicalValue(
                "asset://qualified_model/model.urdf")},
           {"srdf_logical_uri",
            CanonicalValue(
                "asset://qualified_model/model.srdf")},
           {"artifacts", CanonicalValue(std::move(artifacts))},
       })},
      {"root_frame_role_id", CanonicalValue("base")},
      {"frame_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("base")},
               {"source_link_name", CanonicalValue("base")},
           }),
       })},
      {"model_group_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"model_group_role_id", CanonicalValue("arm")},
               {"source_group_name", CanonicalValue("arm")},
           }),
       })},
      {"resource_role_bindings",
       CanonicalValue(CanonicalValue::Array{})},
      {"collision_body_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"collision_body_role_id", CanonicalValue("body")},
               {"source_link_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("base")})},
           }),
       })},
  });
}

[[nodiscard]] std::string content_identity(
    std::string_view domain, std::string_view api_version,
    const CanonicalValue& payload) {
  std::string preimage(kIdentityPrefix);
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(api_version);
  preimage.push_back('\0');
  preimage.append(canonical_json(payload));
  return sha256_digest(preimage);
}

[[nodiscard]] std::string model_asset_document(
    std::string_view urdf, std::string_view srdf,
    std::string_view mesh) {
  const auto identity = model_asset_identity(urdf, srdf, mesh);
  auto complete = identity.as_object();
  complete.emplace_back(
      "model_revision",
      CanonicalValue(content_identity(
          "catalog-record-revision/model_asset",
          core::kModelAssetApiVersion, identity)));
  return canonical_json(CanonicalValue(std::move(complete)));
}

[[nodiscard]] SourceVerifiedModelAsset verified_source() {
  const std::filesystem::path source_root{
      MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT};
  const std::filesystem::path output_root{
      MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT};
  const std::string urdf = read_file(output_root / "model.urdf");
  const std::string srdf = read_file(output_root / "model.srdf");
  const std::string mesh =
      read_file(source_root / "meshes" / "tool.stl");
  const std::string document =
      model_asset_document(urdf, srdf, mesh);
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto& revision = std::get<std::string>(
      parsed.root()
          .find_object_member("model_revision")
          ->storage());
  const std::string document_digest = sha256_digest(document);
  const core::ContentAddressedStoreKey document_key(
      "sha256/" + document_digest.substr(7U));
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId("qualified_model"),
      core::ModelRevision(revision),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      core::SchemaRevision(std::string(kModelAssetSchemaRevision)),
      core::ContentDigest(document_digest), document_key};
  const auto qualified = resolve_and_qualify_model_asset(
      reference, document_key, document);

  auto candidate = [](std::string uri, std::string bytes) {
    const std::string digest = sha256_digest(bytes);
    return ResolvedModelSourceArtifactCandidate{
        std::move(uri),
        core::ContentAddressedStoreKey(
            "sha256/" + digest.substr(7U)),
        std::move(bytes)};
  };
  return verify_model_source_closure(
      qualified,
      {
          candidate("asset://qualified_model/meshes/tool.stl",
                    mesh),
          candidate("asset://qualified_model/model.srdf", srdf),
          candidate("asset://qualified_model/model.urdf", urdf),
      });
}

[[nodiscard]] std::string resign_receipt(std::string receipt) {
  const auto parsed = parse_strict_json_model_yaml(receipt);
  CanonicalValue::Object payload;
  std::string old_digest;
  for (const auto& [name, value] : parsed.root().as_object()) {
    if (name == "configuration_digest") {
      old_digest = std::get<std::string>(value.storage());
    } else {
      payload.emplace_back(name, value);
    }
  }
  const std::string digest = content_identity(
      kStructuralModelPreparationConfigurationIdentityDomain,
      kStructuralModelPreparationApiVersion,
      CanonicalValue(std::move(payload)));
  const std::string needle =
      "\"configuration_digest\":\"" + old_digest + "\"";
  const std::string replacement =
      "\"configuration_digest\":\"" + digest + "\"";
  const auto offset = receipt.find(needle);
  EXPECT_NE(offset, std::string::npos);
  receipt.replace(offset, needle.size(), replacement);
  return receipt;
}

struct Fixture final {
  std::string original_urdf;
  std::string original_srdf;
  std::string receipt;
  SourceVerifiedModelAsset verified;

  Fixture()
      : original_urdf(read_file(
            std::filesystem::path{
                MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT} /
            "source.urdf")),
        original_srdf(read_file(
            std::filesystem::path{
                MANYFORGE_STRUCTURAL_PREPARATION_TEST_SOURCE_ROOT} /
            "source.srdf")),
        receipt(read_file(
            std::filesystem::path{
                MANYFORGE_STRUCTURAL_PREPARATION_TEST_OUTPUT_ROOT} /
            "preparation-manifest.json")),
        verified(verified_source()) {}
};

TEST(StructuralModelPreparation,
     SealsExactBuildReceiptAgainstSourceVerifiedClosure) {
  const Fixture fixture;
  const auto qualified =
      verify_build_qualified_structural_model_preparation(
          fixture.receipt, fixture.original_urdf,
          fixture.original_srdf, fixture.verified);

  EXPECT_EQ(qualified.generator_record().generator_id.value(),
            kStructuralModelPreparationGeneratorId);
  EXPECT_EQ(qualified.generator_record().generator_version.value(),
            kStructuralModelPreparationGeneratorVersion);
  EXPECT_EQ(qualified.canonical_receipt_bytes(), fixture.receipt);
  EXPECT_EQ(qualified.verified_model_asset().value().model_asset_id,
            core::ModelAssetId("qualified_model"));
  EXPECT_TRUE(is_sha256_digest(
      qualified.generator_record().build_digest.value()));
  EXPECT_TRUE(is_sha256_digest(
      qualified.generator_record().configuration_digest.value()));
}

TEST(StructuralModelPreparation,
     RejectsWrongOriginalBytesAndNoncanonicalReceipt) {
  const Fixture fixture;
  EXPECT_THROW(
      verify_build_qualified_structural_model_preparation(
          fixture.receipt, fixture.original_urdf + " ",
          fixture.original_srdf, fixture.verified),
      StructuralModelPreparationError);
  EXPECT_THROW(
      verify_build_qualified_structural_model_preparation(
          fixture.receipt + "\n", fixture.original_urdf,
          fixture.original_srdf, fixture.verified),
      StructuralModelPreparationError);
}

TEST(StructuralModelPreparation,
     RecomputedReceiptCannotReclassifyVerifiedResource) {
  const Fixture fixture;
  std::string tampered = fixture.receipt;
  const std::string original = "\"kind\":\"mesh\"";
  const auto offset = tampered.find(original);
  ASSERT_NE(offset, std::string::npos);
  tampered.replace(offset, original.size(), "\"kind\":\"texture\"");
  tampered = resign_receipt(std::move(tampered));

  EXPECT_THROW(
      verify_build_qualified_structural_model_preparation(
          tampered, fixture.original_urdf, fixture.original_srdf,
          fixture.verified),
      StructuralModelPreparationError);
}

TEST(StructuralModelPreparation,
     RecomputedReceiptCannotChangeVerifiedResourceOccurrenceCount) {
  const Fixture fixture;
  std::string tampered = fixture.receipt;
  const std::string original = "\"occurrence_count\":2";
  const auto offset = tampered.find(original);
  ASSERT_NE(offset, std::string::npos);
  tampered.replace(offset, original.size(), "\"occurrence_count\":1");
  tampered = resign_receipt(std::move(tampered));

  EXPECT_THROW(
      verify_build_qualified_structural_model_preparation(
          tampered, fixture.original_urdf, fixture.original_srdf,
          fixture.verified),
      StructuralModelPreparationError);
}

TEST(StructuralModelPreparation,
     RecomputedReceiptCannotOmitOriginalSrdfOfVerifiedSource) {
  const Fixture fixture;
  std::string tampered = fixture.receipt;
  const std::string original =
      "\"srdf\":{\"byte_size\":" +
      std::to_string(fixture.original_srdf.size()) +
      ",\"content_digest\":\"" +
      sha256_digest(fixture.original_srdf) + "\"}";
  const auto offset = tampered.find(original);
  ASSERT_NE(offset, std::string::npos);
  tampered.replace(offset, original.size(), "\"srdf\":null");
  tampered = resign_receipt(std::move(tampered));

  EXPECT_THROW(
      verify_build_qualified_structural_model_preparation(
          tampered, fixture.original_urdf, std::nullopt,
          fixture.verified),
      StructuralModelPreparationError);
}

TEST(StructuralModelPreparation,
     RecomputedReceiptCannotSubstituteToolBuild) {
  const Fixture fixture;
  std::string tampered = fixture.receipt;
  const std::string field = "\"build_digest\":\"sha256:";
  const auto offset = tampered.find(field);
  ASSERT_NE(offset, std::string::npos);
  const auto digit = offset + field.size();
  tampered[digit] = tampered[digit] == '0' ? '1' : '0';
  tampered = resign_receipt(std::move(tampered));

  EXPECT_THROW(
      verify_build_qualified_structural_model_preparation(
          tampered, fixture.original_urdf, fixture.original_srdf,
          fixture.verified),
      StructuralModelPreparationError);
}

}  // namespace
}  // namespace manyforge::deployment
