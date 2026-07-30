#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "proof_root_transfer_test.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kModelAssetSchemaRevision =
    "sha256:0820a2fbe627f2ccc5b76ff8d5348137d796893d3bd67707f7171fb420ec671c";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kOneDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

static_assert(!std::is_aggregate_v<QualifiedModelAsset>);
static_assert(!std::is_default_constructible_v<QualifiedModelAsset>);

struct ArtifactSpec final {
  std::string logical_uri;
  std::string media_type;
  std::uint64_t byte_size;
  std::string digest{std::string(kZeroDigest)};
};

struct FrameSpec final {
  std::string role;
  std::string source_link;
};

struct GroupSpec final {
  std::string role;
  std::string source_group;
};

struct ResourceSpec final {
  std::string role;
  std::vector<std::string> source_joints;
};

struct CollisionSpec final {
  std::string role;
  std::vector<std::string> source_links;
};

struct AssetSpec final {
  std::string asset_id{"lite6_aggregate"};
  std::string cardinality{"multiple_per_cell"};
  std::string urdf_uri{"asset://lite6_aggregate/urdf/model.urdf"};
  std::optional<std::string> srdf_uri{
      "asset://lite6_aggregate/srdf/model.srdf"};
  std::vector<ArtifactSpec> artifacts{
      {"asset://lite6_aggregate/urdf/model.urdf", "application/xml", 1024U},
      {"asset://lite6_aggregate/srdf/model.srdf", "application/xml", 512U},
      {"asset://lite6_aggregate/meshes/base.stl", "model/stl", 2048U,
       std::string(kOneDigest)},
  };
  std::string root_frame_role{"base"};
  std::vector<FrameSpec> frames{{"tcp", "tool0"}, {"base", "base_link"}};
  std::vector<GroupSpec> groups{{"gripper", "gripper"},
                                {"arm", "manipulator"}};
  std::vector<ResourceSpec> resources{
      {"gripper_joints", {"finger_joint"}},
      {"arm_joints", {"joint1", "joint2"}},
  };
  std::vector<CollisionSpec> collision_bodies{
      {"arm_collision", {"tool0", "base_link"}},
  };
};

[[nodiscard]] CanonicalValue string_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue asset_identity_value(AssetSpec spec) {
  std::sort(spec.artifacts.begin(), spec.artifacts.end(),
            [](const ArtifactSpec& left, const ArtifactSpec& right) {
              return left.logical_uri < right.logical_uri;
            });
  std::sort(spec.frames.begin(), spec.frames.end(),
            [](const FrameSpec& left, const FrameSpec& right) {
              return left.role < right.role;
            });
  std::sort(spec.groups.begin(), spec.groups.end(),
            [](const GroupSpec& left, const GroupSpec& right) {
              return left.role < right.role;
            });
  std::sort(spec.resources.begin(), spec.resources.end(),
            [](const ResourceSpec& left, const ResourceSpec& right) {
              return left.role < right.role;
            });
  std::sort(spec.collision_bodies.begin(), spec.collision_bodies.end(),
            [](const CollisionSpec& left, const CollisionSpec& right) {
              return left.role < right.role;
            });

  CanonicalValue::Array artifacts;
  artifacts.reserve(spec.artifacts.size());
  for (const auto& artifact : spec.artifacts) {
    artifacts.emplace_back(CanonicalValue::Object{
        {"logical_uri", CanonicalValue(artifact.logical_uri)},
        {"media_type", CanonicalValue(artifact.media_type)},
        {"byte_size", CanonicalValue(static_cast<double>(artifact.byte_size))},
        {"content_digest", CanonicalValue(artifact.digest)},
    });
  }

  CanonicalValue::Object expanded_source{
      {"urdf_logical_uri", CanonicalValue(spec.urdf_uri)},
      {"artifacts", CanonicalValue(std::move(artifacts))},
  };
  if (spec.srdf_uri.has_value()) {
    expanded_source.emplace_back("srdf_logical_uri",
                                 CanonicalValue(*spec.srdf_uri));
  }

  CanonicalValue::Array frames;
  frames.reserve(spec.frames.size());
  for (const auto& binding : spec.frames) {
    frames.emplace_back(CanonicalValue::Object{
        {"frame_role_id", CanonicalValue(binding.role)},
        {"source_link_name", CanonicalValue(binding.source_link)},
    });
  }

  CanonicalValue::Array groups;
  groups.reserve(spec.groups.size());
  for (const auto& binding : spec.groups) {
    groups.emplace_back(CanonicalValue::Object{
        {"model_group_role_id", CanonicalValue(binding.role)},
        {"source_group_name", CanonicalValue(binding.source_group)},
    });
  }

  CanonicalValue::Array resources;
  resources.reserve(spec.resources.size());
  for (const auto& binding : spec.resources) {
    resources.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue(binding.role)},
        {"source_joint_names", string_array(binding.source_joints)},
    });
  }

  CanonicalValue::Array collision_bodies;
  collision_bodies.reserve(spec.collision_bodies.size());
  for (auto binding : spec.collision_bodies) {
    std::sort(binding.source_links.begin(), binding.source_links.end());
    collision_bodies.emplace_back(CanonicalValue::Object{
        {"collision_body_role_id", CanonicalValue(binding.role)},
        {"source_link_names", string_array(binding.source_links)},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kModelAssetApiVersion))},
      {"model_asset_id", CanonicalValue(spec.asset_id)},
      {"instance_cardinality", CanonicalValue(spec.cardinality)},
      {"expanded_source", CanonicalValue(std::move(expanded_source))},
      {"root_frame_role_id", CanonicalValue(spec.root_frame_role)},
      {"frame_role_bindings", CanonicalValue(std::move(frames))},
      {"model_group_role_bindings", CanonicalValue(std::move(groups))},
      {"resource_role_bindings", CanonicalValue(std::move(resources))},
      {"collision_body_role_bindings",
       CanonicalValue(std::move(collision_bodies))},
  });
}

[[nodiscard]] std::string model_asset_document(const AssetSpec& spec) {
  const auto identity_value = asset_identity_value(spec);
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/model_asset");
  preimage.push_back('\0');
  preimage.append(core::kModelAssetApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity_value));
  auto root = identity_value.as_object();
  root.emplace_back("model_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string revision_from_document(
    std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision =
      parsed.root().find_object_member("model_revision");
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] core::ModelAssetCatalogRecordRefDTO reference_for(
    std::string_view asset_id, std::string_view document,
    const core::ContentAddressedStoreKey& store_key) {
  return {
      core::ModelAssetId(std::string(asset_id)),
      core::ModelRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      core::SchemaRevision(std::string(kModelAssetSchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      store_key,
  };
}

[[nodiscard]] QualifiedModelAsset qualify(
    const AssetSpec& spec,
    const core::ContentAddressedStoreKey& store_key =
        core::ContentAddressedStoreKey("sha256/model-asset")) {
  const auto document = model_asset_document(spec);
  return resolve_and_qualify_model_asset(
      reference_for(spec.asset_id, document, store_key), store_key, document);
}

void expect_semantic_rejection(const AssetSpec& spec) {
  const auto document = model_asset_document(spec);
  const core::ContentAddressedStoreKey store_key{
      "sha256/model-asset-semantic-negative"};
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference_for(spec.asset_id, document, store_key),
                   store_key, document),
               ModelAssetCodecError);
}

[[nodiscard]] std::string replace_once(std::string source,
                                       std::string_view needle,
                                       std::string_view replacement) {
  const auto offset = source.find(needle);
  EXPECT_NE(offset, std::string::npos) << needle;
  if (offset != std::string::npos) {
    source.replace(offset, needle.size(), replacement);
  }
  return source;
}

void expect_closed_schema_rejection(std::string_view asset_id,
                                    std::string_view valid_document,
                                    std::string mutated_document) {
  const core::ContentAddressedStoreKey store_key{
      "sha256/model-asset-shape-negative"};
  auto reference = reference_for(asset_id, valid_document, store_key);
  reference.content_digest =
      core::ContentDigest(sha256_digest(mutated_document));
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference, store_key, mutated_document),
               ClosedSchemaError);
}

TEST(ModelAssetCodec,
     QualifiesExpandedAggregateAndPreservesOnlySemanticOrdering) {
  const auto qualified = qualify(AssetSpec{});
  const auto& asset = qualified.value();
  EXPECT_EQ(
      qualified.reference(),
      reference_for(AssetSpec{}.asset_id, model_asset_document(AssetSpec{}),
                    core::ContentAddressedStoreKey("sha256/model-asset")));

  EXPECT_EQ(asset.model_asset_id, core::ModelAssetId("lite6_aggregate"));
  EXPECT_EQ(asset.instance_cardinality,
            core::ModelAssetInstanceCardinality::kMultiplePerCell);
  ASSERT_TRUE(asset.expanded_source.srdf_logical_uri.has_value());
  EXPECT_EQ(*asset.expanded_source.srdf_logical_uri,
            "asset://lite6_aggregate/srdf/model.srdf");
  ASSERT_EQ(asset.expanded_source.artifacts.size(), 3U);
  EXPECT_EQ(asset.expanded_source.artifacts[0].logical_uri,
            "asset://lite6_aggregate/meshes/base.stl");
  ASSERT_EQ(asset.resource_role_bindings.size(), 2U);
  EXPECT_EQ(asset.resource_role_bindings[0].source_joint_names,
            (std::vector<std::string>{"joint1", "joint2"}));
  ASSERT_EQ(asset.collision_body_role_bindings.size(), 1U);
  EXPECT_EQ(asset.collision_body_role_bindings[0].source_link_names,
            (std::vector<std::string>{"base_link", "tool0"}));
  EXPECT_EQ(canonical_json(qualified.normalized_root()),
            model_asset_document(AssetSpec{}));
}

TEST(ModelAssetCodec, RvalueTransfersPreserveQualifiedAssetSourceAndResult) {
  const auto qualified = qualify(AssetSpec{});
  test::expect_source_preserving_rvalue_transfers(
      qualified, [](const QualifiedModelAsset& value) {
        return std::tuple{value.reference(), value.value(),
                          value.normalized_root()};
      });
}

TEST(ModelAssetCodec, QualifiesFixedPneumaticAndPassiveUrdfOnlyAssets) {
  AssetSpec fixed;
  fixed.asset_id = "fixed_pneumatic_tool";
  fixed.cardinality = "one_per_cell";
  fixed.urdf_uri =
      "asset://fixed_pneumatic_tool/urdf/fixed_tool.urdf";
  fixed.srdf_uri = std::nullopt;
  fixed.artifacts = {
      {fixed.urdf_uri, "application/xml", 256U},
  };
  fixed.frames = {{"mount", "mount_link"}, {"tcp", "tcp_link"}};
  fixed.root_frame_role = "mount";
  fixed.groups.clear();
  fixed.resources.clear();
  fixed.collision_bodies = {{"tool_collision", {"tool_body"}}};

  const auto qualified = qualify(fixed);
  EXPECT_EQ(qualified.value().instance_cardinality,
            core::ModelAssetInstanceCardinality::kOnePerCell);
  EXPECT_FALSE(
      qualified.value().expanded_source.srdf_logical_uri.has_value());
  EXPECT_TRUE(qualified.value().model_group_role_bindings.empty());
  EXPECT_TRUE(qualified.value().resource_role_bindings.empty());
}

TEST(ModelAssetCodec, EnforcesTheSoleClosureLocalUriGrammar) {
  const std::vector<std::string> invalid_uris{
      "package://lite6_aggregate/urdf/model.urdf",
      "model://lite6_aggregate/urdf/model.urdf",
      "asset://other/urdf/model.urdf",
      "asset://lite6_aggregate",
      "asset://lite6_aggregate/",
      "asset://lite6_aggregate//model.urdf",
      "asset://lite6_aggregate/./model.urdf",
      "asset://lite6_aggregate/../model.urdf",
      "asset://lite6_aggregate/urdf/%6dodel.urdf",
      "asset://lite6_aggregate/urdf\\model.urdf",
      "asset://lite6_aggregate/urdf/model.urdf?x=1",
      "asset://lite6_aggregate/urdf/model.urdf#fragment",
      "asset://lite6_aggregate/urdf/model file.urdf",
      "asset://lite6_aggregate/urdf/\xC3\xA9.urdf",
      "asset://lite6_aggregate/" + std::string(129U, 'a'),
  };
  for (const auto& uri : invalid_uris) {
    AssetSpec spec;
    spec.urdf_uri = uri;
    spec.artifacts[0].logical_uri = uri;
    expect_semantic_rejection(spec);
  }

  AssetSpec safe_segments;
  safe_segments.urdf_uri =
      "asset://lite6_aggregate/URDF_1/model-name.v1.urdf";
  safe_segments.artifacts[0].logical_uri = safe_segments.urdf_uri;
  EXPECT_NO_THROW(qualify(safe_segments));
}

TEST(ModelAssetCodec, EnforcesExactNormalizedMediaTypeGrammar) {
  const std::vector<std::string> invalid{
      "Application/xml", "application/xml; charset=utf-8", "application/*",
      "/xml",            "application/",                  "application//xml",
      "application/x ml", "application/x@y",
  };
  for (const auto& media_type : invalid) {
    AssetSpec spec;
    spec.artifacts[0].media_type = media_type;
    expect_semantic_rejection(spec);
  }

  AssetSpec supporting;
  supporting.artifacts[2].media_type = "model/gltf+json";
  EXPECT_NO_THROW(qualify(supporting));
}

TEST(ModelAssetCodec, RequiresDistinctExactXmlEntrypointRows) {
  AssetSpec missing_urdf;
  missing_urdf.urdf_uri =
      "asset://lite6_aggregate/urdf/missing.urdf";
  expect_semantic_rejection(missing_urdf);

  AssetSpec wrong_urdf_media;
  wrong_urdf_media.artifacts[0].media_type = "text/xml";
  expect_semantic_rejection(wrong_urdf_media);

  AssetSpec missing_srdf;
  missing_srdf.srdf_uri =
      "asset://lite6_aggregate/srdf/missing.srdf";
  expect_semantic_rejection(missing_srdf);

  AssetSpec same_entry;
  same_entry.srdf_uri = same_entry.urdf_uri;
  expect_semantic_rejection(same_entry);
}

TEST(ModelAssetCodec, EnforcesSafeAggregateDeclaredByteSize) {
  AssetSpec zero;
  zero.artifacts[0].byte_size = 0U;
  const auto zero_document = model_asset_document(zero);
  const core::ContentAddressedStoreKey zero_store_key{
      "sha256/zero-source-size"};
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference_for(zero.asset_id, zero_document,
                                 zero_store_key),
                   zero_store_key, zero_document),
               ClosedSchemaError);

  AssetSpec exact;
  exact.srdf_uri = std::nullopt;
  exact.groups.clear();
  exact.artifacts = {
      {exact.urdf_uri, "application/xml",
       core::kMaxModelAssetByteSize},
  };
  EXPECT_NO_THROW(qualify(exact));

  AssetSpec overflow = exact;
  overflow.artifacts.push_back(
      {"asset://lite6_aggregate/meshes/one.stl", "model/stl", 1U});
  expect_semantic_rejection(overflow);
}

TEST(ModelAssetCodec, EnforcesRootAndSrdfLocalInvariants) {
  AssetSpec missing_root;
  missing_root.root_frame_role = "missing";
  expect_semantic_rejection(missing_root);

  AssetSpec no_srdf_with_group;
  no_srdf_with_group.srdf_uri = std::nullopt;
  expect_semantic_rejection(no_srdf_with_group);

  AssetSpec no_frames;
  no_frames.frames.clear();
  const auto no_frames_document = model_asset_document(no_frames);
  const core::ContentAddressedStoreKey store_key{"sha256/no-frames"};
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference_for(no_frames.asset_id, no_frames_document,
                                 store_key),
                   store_key, no_frames_document),
               ClosedSchemaError);
}

TEST(ModelAssetCodec,
     RejectsOrderedDuplicatesButAllowsIntentionalCrossRoleOverlap) {
  AssetSpec duplicate_joint;
  duplicate_joint.resources[1].source_joints = {"joint1", "joint1"};
  expect_semantic_rejection(duplicate_joint);

  AssetSpec duplicate_collision;
  duplicate_collision.collision_bodies[0].source_links = {"tool0", "tool0"};
  const auto document = model_asset_document(duplicate_collision);
  const core::ContentAddressedStoreKey store_key{
      "sha256/duplicate-collision"};
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference_for(duplicate_collision.asset_id, document,
                                 store_key),
                   store_key, document),
               ClosedSchemaError);

  AssetSpec overlapping;
  overlapping.resources.push_back({"secondary_arm", {"joint1"}});
  overlapping.frames.push_back({"collision_alias", "base_link"});
  overlapping.collision_bodies.push_back(
      {"secondary_collision", {"base_link"}});
  EXPECT_NO_THROW(qualify(overlapping));
}

TEST(ModelAssetCodec, RejectsWhitespaceControlAndFormatSourceSymbols) {
  const std::vector<std::string> invalid{
      "bad name",
      "bad\nname",
      std::string("bad") + "\xE2\x80\x8B" + "name",
  };
  for (const auto& symbol : invalid) {
    AssetSpec spec;
    spec.frames[0].source_link = symbol;
    expect_semantic_rejection(spec);
  }

  AssetSpec exact;
  exact.frames[0].source_link = std::string(
      core::kMaxModelAssetSourceSymbolCodePoints, 'a');
  EXPECT_NO_THROW(qualify(exact));

  AssetSpec too_long = exact;
  too_long.frames[0].source_link.push_back('a');
  const auto document = model_asset_document(too_long);
  const core::ContentAddressedStoreKey store_key{
      "sha256/source-symbol-overflow"};
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference_for(too_long.asset_id, document, store_key),
                   store_key, document),
               ClosedSchemaError);
}

TEST(ModelAssetCodec, RejectsOpenShapeNullAndStaleSemanticIdentity) {
  const AssetSpec spec;
  const auto valid = model_asset_document(spec);

  const auto extra = replace_once(
      valid, "\"root_frame_role_id\":\"base\"",
      "\"root_frame_role_id\":\"base\",\"unexpected\":true");
  expect_closed_schema_rejection(spec.asset_id, valid, extra);

  const auto legacy_instance_parameter_schema = replace_once(
      valid, "\"root_frame_role_id\":\"base\"",
      "\"root_frame_role_id\":\"base\","
      "\"instance_parameters_schema_ref\":{}");
  expect_closed_schema_rejection(spec.asset_id, valid,
                                 legacy_instance_parameter_schema);

  const auto legacy_source_parameters = replace_once(
      valid, "\"root_frame_role_id\":\"base\"",
      "\"root_frame_role_id\":\"base\",\"source_parameters\":{}");
  expect_closed_schema_rejection(spec.asset_id, valid,
                                 legacy_source_parameters);

  const auto explicit_null = replace_once(
      valid, "\"srdf_logical_uri\":\"asset://lite6_aggregate/srdf/model.srdf\"",
      "\"srdf_logical_uri\":null");
  expect_closed_schema_rejection(spec.asset_id, valid, explicit_null);

  const auto nested_extra = replace_once(
      valid, "\"media_type\":\"model/stl\"",
      "\"media_type\":\"model/stl\",\"store_key\":\"sha256/forbidden\"");
  expect_closed_schema_rejection(spec.asset_id, valid, nested_extra);

  auto stale = valid;
  const auto revision = stale.find("\"model_revision\":\"sha256:");
  ASSERT_NE(revision, std::string::npos);
  const auto hex = revision + std::string_view("\"model_revision\":\"sha256:").size();
  stale[hex] = stale[hex] == '0' ? '1' : '0';
  const core::ContentAddressedStoreKey store_key{"sha256/stale-model"};
  auto stale_reference = reference_for(spec.asset_id, valid, store_key);
  stale_reference.content_digest =
      core::ContentDigest(sha256_digest(stale));
  EXPECT_THROW(resolve_and_qualify_model_asset(
                   stale_reference, store_key, stale),
               ModelAssetCodecError);
}

TEST(ModelAssetCodec, EnforcesTheCompleteTypedReferentBoundary) {
  const AssetSpec spec;
  const auto document = model_asset_document(spec);
  const core::ContentAddressedStoreKey store_key{"sha256/model-referent"};
  const auto reference = reference_for(spec.asset_id, document, store_key);

  const auto qualified =
      resolve_and_qualify_model_asset(reference, store_key, document);
  EXPECT_EQ(qualified.reference(), reference);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_model_asset(wrong_digest, store_key,
                                              document),
               ModelAssetCodecError);

  auto wrong_schema = reference;
  wrong_schema.schema_revision =
      core::SchemaRevision(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_model_asset(wrong_schema, store_key,
                                              document),
               ModelAssetCodecError);

  auto wrong_id = reference;
  wrong_id.record_id = core::ModelAssetId("other_asset");
  EXPECT_THROW(resolve_and_qualify_model_asset(wrong_id, store_key, document),
               ModelAssetCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision =
      core::ModelRevision(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_model_asset(wrong_revision, store_key,
                                              document),
               ModelAssetCodecError);

  EXPECT_THROW(resolve_and_qualify_model_asset(
                   reference,
                   core::ContentAddressedStoreKey("sha256/wrong-key"),
                   document),
               ModelAssetCodecError);
}

TEST(ModelAssetCodec,
     QualificationDoesNotClaimRetrievalXmlSymbolsClosureOrRewrite) {
  AssetSpec declaration_only;
  declaration_only.artifacts[0].digest = std::string(kOneDigest);
  declaration_only.frames[0].source_link =
      "not_proven_to_exist_in_urdf";
  declaration_only.groups[0].source_group =
      "not_proven_to_exist_in_srdf";
  declaration_only.artifacts.push_back(
      {"asset://lite6_aggregate/meshes/not_proven_reachable.stl",
       "model/stl", 1U, std::string(kZeroDigest)});

  EXPECT_NO_THROW(qualify(declaration_only));
}

}  // namespace
}  // namespace manyforge::deployment
