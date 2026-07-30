#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kModelAssetSchemaRevision =
    "sha256:0820a2fbe627f2ccc5b76ff8d5348137d796893d3bd67707f7171fb420ec671c";

static_assert(!std::is_aggregate_v<SourceVerifiedModelAsset>);
static_assert(!std::is_default_constructible_v<SourceVerifiedModelAsset>);
static_assert(!std::is_aggregate_v<ModelSourceRetrievalPlan>);
static_assert(!std::is_default_constructible_v<ModelSourceRetrievalPlan>);

struct SourceBlob final {
  std::string logical_uri;
  std::string media_type;
  std::string bytes;
  std::optional<std::uint64_t> declared_byte_size;

  SourceBlob(std::string uri, std::string type, std::string content,
             std::optional<std::uint64_t> declared_size = std::nullopt)
      : logical_uri(std::move(uri)),
        media_type(std::move(type)),
        bytes(std::move(content)),
        declared_byte_size(declared_size) {}
};

struct Fixture final {
  std::string asset_id{"test_robot"};
  std::string urdf_uri{"asset://test_robot/urdf/model.urdf"};
  std::optional<std::string> srdf_uri{
      "asset://test_robot/srdf/model.srdf"};
  std::vector<SourceBlob> blobs;

  Fixture()
      : blobs{
            {urdf_uri, "application/xml",
             "<robot name=\"test_robot\">"
             "<material name=\"Steel\"><color rgba=\"0.5 0.5 0.5 1\"/>"
             "</material>"
             "<link name=\"base_link\"><visual><geometry>"
             "<mesh filename=\"asset://test_robot/meshes/base.stl\"/>"
             "</geometry><material name=\"Steel\"/></visual></link>"
             "<link name=\"tool0\"/>"
             "<joint name=\"joint1\" type=\"revolute\">"
             "<parent link=\"base_link\"/><child link=\"tool0\"/>"
             "<limit lower=\"-3\" upper=\"3\" effort=\"10\" "
             "velocity=\"1\"/>"
             "</joint></robot>"},
            {*srdf_uri, "application/xml",
             "<robot name=\"test_robot\">"
             "<group name=\"manipulator\">"
             "<chain base_link=\"base_link\" tip_link=\"tool0\"/>"
             "<joint name=\"joint1\"/>"
             "</group>"
             "<group_state name=\"home\" group=\"manipulator\">"
             "<joint name=\"joint1\" value=\"0\"/>"
             "</group_state>"
             "<disable_collisions link1=\"base_link\" link2=\"tool0\" "
             "reason=\"Adjacent\"/>"
             "</robot>"},
            {"asset://test_robot/meshes/base.stl", "model/stl",
             "solid bounded-test-mesh\nendsolid bounded-test-mesh\n"},
        } {}
};

[[nodiscard]] SourceBlob& blob_for(Fixture& fixture, std::string_view uri) {
  const auto found =
      std::find_if(fixture.blobs.begin(), fixture.blobs.end(),
                   [uri](const SourceBlob& blob) {
                     return blob.logical_uri == uri;
                   });
  EXPECT_NE(found, fixture.blobs.end());
  return *found;
}

[[nodiscard]] CanonicalValue asset_identity_value(const Fixture& fixture) {
  auto blobs = fixture.blobs;
  std::sort(blobs.begin(), blobs.end(),
            [](const SourceBlob& left, const SourceBlob& right) {
              return left.logical_uri < right.logical_uri;
            });
  CanonicalValue::Array artifacts;
  for (const auto& blob : blobs) {
    artifacts.emplace_back(CanonicalValue::Object{
        {"logical_uri", CanonicalValue(blob.logical_uri)},
        {"media_type", CanonicalValue(blob.media_type)},
        {"byte_size",
         CanonicalValue(static_cast<double>(
             blob.declared_byte_size.value_or(blob.bytes.size())))},
        {"content_digest", CanonicalValue(sha256_digest(blob.bytes))},
    });
  }
  CanonicalValue::Object expanded{
      {"urdf_logical_uri", CanonicalValue(fixture.urdf_uri)},
      {"artifacts", CanonicalValue(std::move(artifacts))},
  };
  if (fixture.srdf_uri.has_value()) {
    expanded.emplace_back("srdf_logical_uri",
                          CanonicalValue(*fixture.srdf_uri));
  }
  CanonicalValue::Array groups;
  if (fixture.srdf_uri.has_value()) {
    groups.emplace_back(CanonicalValue::Object{
        {"model_group_role_id", CanonicalValue("arm")},
        {"source_group_name", CanonicalValue("manipulator")},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kModelAssetApiVersion))},
      {"model_asset_id", CanonicalValue(fixture.asset_id)},
      {"instance_cardinality", CanonicalValue("multiple_per_cell")},
      {"expanded_source", CanonicalValue(std::move(expanded))},
      {"root_frame_role_id", CanonicalValue("base")},
      {"frame_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("base")},
               {"source_link_name", CanonicalValue("base_link")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"frame_role_id", CanonicalValue("tcp")},
               {"source_link_name", CanonicalValue("tool0")},
           }),
       })},
      {"model_group_role_bindings", CanonicalValue(std::move(groups))},
      {"resource_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"resource_role_id", CanonicalValue("arm_joints")},
               {"source_joint_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("joint1")})},
           }),
       })},
      {"collision_body_role_bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"collision_body_role_id",
                CanonicalValue("robot_collision")},
               {"source_link_names",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("base_link"),
                    CanonicalValue("tool0")})},
           }),
       })},
  });
}

[[nodiscard]] std::string model_asset_document(const Fixture& fixture) {
  const auto identity = asset_identity_value(fixture);
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/model_asset");
  preimage.push_back('\0');
  preimage.append(core::kModelAssetApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("model_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string revision_from_document(
    std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision = parsed.root().find_object_member("model_revision");
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] QualifiedModelAsset qualify(const Fixture& fixture) {
  const auto document = model_asset_document(fixture);
  const core::ContentAddressedStoreKey key("sha256/model-source-fixture");
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId(fixture.asset_id),
      core::ModelRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      core::SchemaRevision(std::string(kModelAssetSchemaRevision)),
      core::ContentDigest(sha256_digest(document)), key};
  return resolve_and_qualify_model_asset(reference, key, document);
}

[[nodiscard]] std::vector<ResolvedModelSourceArtifactCandidate> candidates(
    const Fixture& fixture) {
  std::vector<ResolvedModelSourceArtifactCandidate> result;
  for (const auto& blob : fixture.blobs) {
    const auto digest = sha256_digest(blob.bytes);
    result.push_back(ResolvedModelSourceArtifactCandidate{
        blob.logical_uri,
        core::ContentAddressedStoreKey("sha256/" + digest.substr(7U)),
        blob.bytes});
  }
  return result;
}

[[nodiscard]] SourceVerifiedModelAsset verify(const Fixture& fixture) {
  return verify_model_source_closure(qualify(fixture),
                                     candidates(fixture));
}

TEST(ModelSource, PreflightsOneCanonicalExactRetrievalPlan) {
  const Fixture fixture;
  const auto qualified = qualify(fixture);
  const auto plan = preflight_model_source_declaration(qualified);

  ASSERT_EQ(plan.artifacts().size(), fixture.blobs.size());
  EXPECT_TRUE(std::is_sorted(
      plan.artifacts().begin(), plan.artifacts().end(),
      [](const auto& left, const auto& right) {
        return left.logical_uri < right.logical_uri;
      }));
  std::uint64_t expected_total = 0U;
  for (const auto& blob : fixture.blobs) {
    expected_total += blob.bytes.size();
    const auto row = std::find_if(
        plan.artifacts().begin(), plan.artifacts().end(),
        [&blob](const auto& candidate) {
          return candidate.logical_uri == blob.logical_uri;
        });
    ASSERT_NE(row, plan.artifacts().end());
    const auto digest = sha256_digest(blob.bytes);
    EXPECT_EQ(row->content_digest.value(), digest);
    EXPECT_EQ(row->expected_store_key.value(),
              "sha256/" + digest.substr(7U));
  }
  EXPECT_EQ(plan.total_bytes(), expected_total);

  const auto verified =
      verify_model_source_closure(plan, candidates(fixture));
  EXPECT_EQ(verified.value(), qualified.value());
}

TEST(ModelSource, PreflightEnforcesExactPerBlobAndClosureByteBounds) {
  Fixture exact_xml;
  blob_for(exact_xml, exact_xml.urdf_uri).declared_byte_size =
      kMaxModelSourceXmlEntrypointBytes;
  EXPECT_NO_THROW(
      (void)preflight_model_source_declaration(qualify(exact_xml)));
  blob_for(exact_xml, exact_xml.urdf_uri).declared_byte_size =
      kMaxModelSourceXmlEntrypointBytes + 1U;
  EXPECT_THROW(preflight_model_source_declaration(qualify(exact_xml)),
               ModelSourceError);

  Fixture exact_opaque;
  blob_for(exact_opaque, "asset://test_robot/meshes/base.stl")
      .declared_byte_size = kMaxModelSourceOpaqueArtifactBytes;
  EXPECT_NO_THROW(
      (void)preflight_model_source_declaration(qualify(exact_opaque)));
  blob_for(exact_opaque, "asset://test_robot/meshes/base.stl")
      .declared_byte_size = kMaxModelSourceOpaqueArtifactBytes + 1U;
  EXPECT_THROW(preflight_model_source_declaration(qualify(exact_opaque)),
               ModelSourceError);

  Fixture exact_closure;
  blob_for(exact_closure, "asset://test_robot/meshes/base.stl")
      .declared_byte_size = kMaxModelSourceOpaqueArtifactBytes;
  const auto entrypoint_bytes =
      blob_for(exact_closure, exact_closure.urdf_uri).bytes.size() +
      blob_for(exact_closure, *exact_closure.srdf_uri).bytes.size();
  const auto second_opaque_bytes =
      kMaxModelSourceClosureBytes -
      kMaxModelSourceOpaqueArtifactBytes - entrypoint_bytes;
  exact_closure.blobs.push_back(
      {"asset://test_robot/meshes/second.stl", "model/stl", "unused",
       second_opaque_bytes});
  const auto exact_plan =
      preflight_model_source_declaration(qualify(exact_closure));
  EXPECT_EQ(exact_plan.total_bytes(), kMaxModelSourceClosureBytes);
  exact_closure.blobs.back().declared_byte_size =
      second_opaque_bytes + 1U;
  EXPECT_THROW(preflight_model_source_declaration(qualify(exact_closure)),
               ModelSourceError);
}

TEST(ModelSource, VerifiesExactClosureAndRetainsCanonicalTypedIndex) {
  const Fixture fixture;
  auto resolved = candidates(fixture);
  std::reverse(resolved.begin(), resolved.end());
  const auto verified =
      verify_model_source_closure(qualify(fixture), std::move(resolved));

  EXPECT_EQ(verified.value().model_asset_id,
            core::ModelAssetId("test_robot"));
  ASSERT_EQ(verified.verified_artifacts().size(), 3U);
  EXPECT_EQ(verified.verified_artifacts().front().logical_uri,
            "asset://test_robot/meshes/base.stl");
  EXPECT_EQ(verified.parsed_ir().robot_name, "test_robot");
  EXPECT_EQ(verified.parsed_ir().root_link_name, "base_link");
  EXPECT_EQ(verified.parsed_ir().material_names,
            (std::vector<std::string>{"Steel"}));
  EXPECT_EQ(verified.parsed_ir().link_names,
            (std::vector<std::string>{"base_link", "tool0"}));
  ASSERT_EQ(verified.parsed_ir().joints.size(), 1U);
  EXPECT_EQ(verified.parsed_ir().joints.front().name, "joint1");
  ASSERT_EQ(verified.parsed_ir().groups.size(), 1U);
  EXPECT_EQ(verified.parsed_ir().groups.front().name, "manipulator");
  EXPECT_EQ(verified.parsed_ir().referenced_artifact_logical_uris,
            (std::vector<std::string>{
                "asset://test_robot/meshes/base.stl"}));
}

TEST(ModelSource, SupportsStrictUrdfOnlyFixedModel) {
  Fixture fixture;
  fixture.srdf_uri = std::nullopt;
  fixture.blobs.erase(
      std::remove_if(fixture.blobs.begin(), fixture.blobs.end(),
                     [](const SourceBlob& blob) {
                       return blob.logical_uri.ends_with(".srdf");
                     }),
      fixture.blobs.end());
  const auto verified = verify(fixture);
  EXPECT_TRUE(verified.parsed_ir().groups.empty());
}

TEST(ModelSource, RejectsMissingExtraAndDuplicateCandidateRows) {
  const Fixture fixture;
  auto missing = candidates(fixture);
  missing.pop_back();
  EXPECT_THROW(verify_model_source_closure(qualify(fixture),
                                           std::move(missing)),
               ModelSourceError);

  auto duplicate = candidates(fixture);
  duplicate.back() = duplicate.front();
  EXPECT_THROW(verify_model_source_closure(qualify(fixture),
                                           std::move(duplicate)),
               ModelSourceError);

  auto extra = candidates(fixture);
  extra.push_back(extra.front());
  EXPECT_THROW(verify_model_source_closure(qualify(fixture), std::move(extra)),
               ModelSourceError);
}

TEST(ModelSource, RejectsWrongStoreKeySizeAndDigest) {
  const Fixture fixture;
  auto wrong_key = candidates(fixture);
  wrong_key.front().resolved_store_key =
      core::ContentAddressedStoreKey("sha256/not-the-declared-digest");
  EXPECT_THROW(verify_model_source_closure(qualify(fixture),
                                           std::move(wrong_key)),
               ModelSourceError);

  auto wrong_size = candidates(fixture);
  wrong_size.front().stored_bytes.push_back('x');
  EXPECT_THROW(verify_model_source_closure(qualify(fixture),
                                           std::move(wrong_size)),
               ModelSourceError);

  auto wrong_digest = candidates(fixture);
  wrong_digest.front().stored_bytes.front() ^= 1;
  EXPECT_THROW(verify_model_source_closure(qualify(fixture),
                                           std::move(wrong_digest)),
               ModelSourceError);
}

TEST(ModelSource, RejectsUnusedOrMissingClosureDependencies) {
  Fixture unused;
  unused.blobs.push_back(
      {"asset://test_robot/meshes/unused.stl", "model/stl", "unused"});
  EXPECT_THROW(verify(unused), ModelSourceError);

  Fixture missing_reference;
  auto& urdf =
      blob_for(missing_reference, missing_reference.urdf_uri);
  const auto offset =
      urdf.bytes.find("asset://test_robot/meshes/base.stl");
  ASSERT_NE(offset, std::string::npos);
  urdf.bytes.replace(offset,
                     std::string("asset://test_robot/meshes/base.stl").size(),
                     "asset://test_robot/meshes/gone.stl");
  EXPECT_THROW(verify(missing_reference), ModelSourceError);
}

TEST(ModelSource, RejectsLiveUrisForbiddenElementsAndXmlEntities) {
  Fixture live_uri;
  auto& live_urdf = blob_for(live_uri, live_uri.urdf_uri);
  const auto asset_offset = live_urdf.bytes.find("asset://");
  ASSERT_NE(asset_offset, std::string::npos);
  live_urdf.bytes.replace(asset_offset, 8U, "package://");
  EXPECT_THROW(verify(live_uri), ModelSourceError);

  Fixture runtime_construct;
  auto& runtime_urdf =
      blob_for(runtime_construct, runtime_construct.urdf_uri);
  const auto robot_end = runtime_urdf.bytes.rfind("</robot>");
  ASSERT_NE(robot_end, std::string::npos);
  runtime_urdf.bytes.insert(
      robot_end, "<ros2_control name=\"runtime\" type=\"system\"/>");
  EXPECT_THROW(verify(runtime_construct), ModelSourceError);

  Fixture entity;
  auto& entity_urdf = blob_for(entity, entity.urdf_uri);
  entity_urdf.bytes =
      "<!DOCTYPE robot [<!ENTITY x \"test_robot\">]>"
      "<robot name=\"&x;\"><link name=\"base_link\"/></robot>";
  entity.blobs.erase(
      std::remove_if(entity.blobs.begin(), entity.blobs.end(),
                     [&entity](const SourceBlob& blob) {
                       return blob.logical_uri != entity.urdf_uri;
                     }),
      entity.blobs.end());
  entity.srdf_uri = std::nullopt;
  EXPECT_THROW(verify(entity), ModelSourceError);
}

TEST(ModelSource, EnforcesXmlDepthAndAttributeCountBeforeBuildingTheDom) {
  Fixture depth;
  auto& depth_urdf = blob_for(depth, depth.urdf_uri);
  std::string nested{"<robot name=\"test_robot\">"};
  for (std::size_t index = 0U;
       index < kMaxModelSourceXmlDepth; ++index) {
    nested += "<x>";
  }
  for (std::size_t index = 0U;
       index < kMaxModelSourceXmlDepth; ++index) {
    nested += "</x>";
  }
  nested += "</robot>";
  depth_urdf.bytes = std::move(nested);
  depth.blobs.erase(
      std::remove_if(depth.blobs.begin(), depth.blobs.end(),
                     [&depth](const SourceBlob& blob) {
                       return blob.logical_uri != depth.urdf_uri;
                     }),
      depth.blobs.end());
  depth.srdf_uri = std::nullopt;
  try {
    (void)verify(depth);
    FAIL() << "XML depth bound was not enforced";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find("nesting bound"),
              std::string::npos);
  }

  Fixture attributes;
  auto& attribute_urdf = blob_for(attributes, attributes.urdf_uri);
  std::string many_attributes{"<robot name=\"test_robot\""};
  many_attributes.reserve(12U * kMaxModelSourceXmlAttributeCount);
  for (std::size_t index = 0U;
       index < kMaxModelSourceXmlAttributeCount; ++index) {
    many_attributes += " a" + std::to_string(index) + "=\"\"";
  }
  many_attributes += "><link name=\"base_link\"/></robot>";
  ASSERT_LT(many_attributes.size(),
            kMaxModelSourceXmlEntrypointBytes);
  attribute_urdf.bytes = std::move(many_attributes);
  attributes.blobs.erase(
      std::remove_if(attributes.blobs.begin(), attributes.blobs.end(),
                     [&attributes](const SourceBlob& blob) {
                       return blob.logical_uri != attributes.urdf_uri;
                     }),
      attributes.blobs.end());
  attributes.srdf_uri = std::nullopt;
  try {
    (void)verify(attributes);
    FAIL() << "XML attribute-count bound was not enforced";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find("attribute-count bound"),
              std::string::npos);
  }
}

TEST(ModelSource, RejectsAmbiguousOrUnknownUrdfMaterials) {
  Fixture duplicate_material;
  auto& duplicate_urdf =
      blob_for(duplicate_material, duplicate_material.urdf_uri);
  const auto material_end = duplicate_urdf.bytes.find("</material>");
  ASSERT_NE(material_end, std::string::npos);
  duplicate_urdf.bytes.insert(
      material_end + std::string_view("</material>").size(),
      "<material name=\"Steel\"><color rgba=\"1 0 0 1\"/></material>");
  EXPECT_THROW(verify(duplicate_material), ModelSourceError);

  Fixture unknown_reference;
  auto& unknown_urdf =
      blob_for(unknown_reference, unknown_reference.urdf_uri);
  const auto material_reference =
      unknown_urdf.bytes.find("<material name=\"Steel\"/>");
  ASSERT_NE(material_reference, std::string::npos);
  unknown_urdf.bytes.replace(
      material_reference, std::string_view("<material name=\"Steel\"/>").size(),
      "<material name=\"Missing\"/>");
  EXPECT_THROW(verify(unknown_reference), ModelSourceError);

  Fixture anonymous_empty;
  auto& anonymous_urdf =
      blob_for(anonymous_empty, anonymous_empty.urdf_uri);
  const auto named_reference =
      anonymous_urdf.bytes.find("<material name=\"Steel\"/>");
  ASSERT_NE(named_reference, std::string::npos);
  anonymous_urdf.bytes.replace(
      named_reference, std::string_view("<material name=\"Steel\"/>").size(),
      "<material/>");
  EXPECT_THROW(verify(anonymous_empty), ModelSourceError);
}

TEST(ModelSource, RejectsDuplicateSrdfSemanticKeys) {
  const auto expect_duplicate_rejected =
      [](std::string_view extra_srdf) {
        Fixture fixture;
        auto& srdf = blob_for(fixture, *fixture.srdf_uri);
        const auto robot_end = srdf.bytes.rfind("</robot>");
        EXPECT_NE(robot_end, std::string::npos);
        if (robot_end == std::string::npos) {
          return;
        }
        srdf.bytes.insert(robot_end, extra_srdf);
        EXPECT_THROW(verify(fixture), ModelSourceError);
      };

  expect_duplicate_rejected(
      "<group name=\"duplicates\"><link name=\"base_link\"/>"
      "<link name=\"base_link\"/></group>");
  expect_duplicate_rejected(
      "<group name=\"duplicates\"><joint name=\"joint1\"/>"
      "<joint name=\"joint1\"/></group>");
  expect_duplicate_rejected(
      "<group name=\"duplicates\">"
      "<chain base_link=\"base_link\" tip_link=\"tool0\"/>"
      "<chain base_link=\"base_link\" tip_link=\"tool0\"/></group>");
  expect_duplicate_rejected(
      "<group name=\"duplicates\"><group name=\"manipulator\"/>"
      "<group name=\"manipulator\"/></group>");
  expect_duplicate_rejected(
      "<group_state name=\"home\" group=\"manipulator\">"
      "<joint name=\"joint1\" value=\"1\"/></group_state>");
  expect_duplicate_rejected(
      "<end_effector name=\"tool\" parent_link=\"tool0\" "
      "group=\"manipulator\"/>"
      "<end_effector name=\"tool\" parent_link=\"tool0\" "
      "group=\"manipulator\"/>");
  expect_duplicate_rejected(
      "<disable_collisions link1=\"tool0\" link2=\"base_link\" "
      "reason=\"reversed duplicate\"/>");
}

TEST(ModelSource, ValidatesManyDeepSrdfChainsWithBoundedTraversal) {
  constexpr std::size_t kLinkCount = 12000U;
  Fixture fixture;
  fixture.blobs.erase(
      std::remove_if(fixture.blobs.begin(), fixture.blobs.end(),
                     [](const SourceBlob& blob) {
                       return blob.media_type == "model/stl";
                     }),
      fixture.blobs.end());
  auto& urdf = blob_for(fixture, fixture.urdf_uri);
  std::string deep_urdf{"<robot name=\"test_robot\">"
                        "<link name=\"base_link\"/>"};
  deep_urdf.reserve(kLinkCount * 150U);
  std::string parent{"base_link"};
  for (std::size_t index = 1U; index < kLinkCount; ++index) {
    const auto child =
        index + 1U == kLinkCount ? std::string("tool0")
                                : "link" + std::to_string(index);
    const auto joint = "joint" + std::to_string(index);
    deep_urdf += "<link name=\"" + child + "\"/>";
    deep_urdf += "<joint name=\"" + joint +
                 "\" type=\"fixed\"><parent link=\"" + parent +
                 "\"/><child link=\"" + child + "\"/></joint>";
    parent = child;
  }
  deep_urdf += "</robot>";
  urdf.bytes = std::move(deep_urdf);

  auto& srdf = blob_for(fixture, *fixture.srdf_uri);
  std::string deep_srdf{
      "<robot name=\"test_robot\"><group name=\"manipulator\">"};
  deep_srdf.reserve(kLinkCount * 90U);
  for (std::size_t index = 1U; index < kLinkCount; ++index) {
    const auto tip =
        index + 1U == kLinkCount ? std::string("tool0")
                                : "link" + std::to_string(index);
    deep_srdf +=
        "<chain base_link=\"base_link\" tip_link=\"" + tip + "\"/>";
  }
  deep_srdf += "</group></robot>";
  srdf.bytes = std::move(deep_srdf);

  const auto verified = verify(fixture);
  ASSERT_EQ(verified.parsed_ir().groups.size(), 1U);
  EXPECT_EQ(verified.parsed_ir().groups.front().chains.size(),
            kLinkCount - 1U);
}

TEST(ModelSource, RejectsInvalidUrdfTopologyAndBindingSymbols) {
  Fixture duplicate_link;
  auto& duplicate_urdf =
      blob_for(duplicate_link, duplicate_link.urdf_uri);
  const auto robot_end = duplicate_urdf.bytes.rfind("</robot>");
  ASSERT_NE(robot_end, std::string::npos);
  duplicate_urdf.bytes.insert(robot_end, "<link name=\"base_link\"/>");
  EXPECT_THROW(verify(duplicate_link), ModelSourceError);

  Fixture cycle;
  auto& cycle_urdf = blob_for(cycle, cycle.urdf_uri);
  const auto cycle_end = cycle_urdf.bytes.rfind("</robot>");
  ASSERT_NE(cycle_end, std::string::npos);
  cycle_urdf.bytes.insert(
      cycle_end,
      "<joint name=\"joint2\" type=\"fixed\">"
      "<parent link=\"tool0\"/><child link=\"base_link\"/></joint>");
  EXPECT_THROW(verify(cycle), ModelSourceError);
}

TEST(ModelSource, RejectsInvalidSrdfGroupsAndPlacementVirtualJoints) {
  Fixture missing_group;
  auto& missing_srdf =
      blob_for(missing_group, *missing_group.srdf_uri);
  const auto group_name = missing_srdf.bytes.find("manipulator");
  ASSERT_NE(group_name, std::string::npos);
  missing_srdf.bytes.replace(group_name, std::string("manipulator").size(),
                             "different");
  EXPECT_THROW(verify(missing_group), ModelSourceError);

  Fixture virtual_joint;
  auto& virtual_srdf =
      blob_for(virtual_joint, *virtual_joint.srdf_uri);
  const auto srdf_end = virtual_srdf.bytes.rfind("</robot>");
  ASSERT_NE(srdf_end, std::string::npos);
  virtual_srdf.bytes.insert(
      srdf_end,
      "<virtual_joint name=\"world\" type=\"fixed\" "
      "parent_frame=\"world\" child_link=\"base_link\"/>");
  EXPECT_THROW(verify(virtual_joint), ModelSourceError);
}

TEST(ModelSource, AcceptsSharedSrdfSubgroupsWithoutCallingThemACycle) {
  Fixture fixture;
  auto& srdf = blob_for(fixture, *fixture.srdf_uri);
  const auto end = srdf.bytes.rfind("</robot>");
  ASSERT_NE(end, std::string::npos);
  srdf.bytes.insert(
      end,
      "<group name=\"leaf\"><joint name=\"joint1\"/></group>"
      "<group name=\"left\"><group name=\"leaf\"/></group>"
      "<group name=\"right\"><group name=\"leaf\"/></group>"
      "<group name=\"diamond\"><group name=\"left\"/>"
      "<group name=\"right\"/></group>");

  const auto verified = verify(fixture);
  EXPECT_EQ(verified.parsed_ir().groups.size(), 5U);
}

TEST(ModelSource, HandlesADeepFlatSubgroupChainWithoutCallStackRecursion) {
  Fixture fixture;
  auto& srdf = blob_for(fixture, *fixture.srdf_uri);
  const auto end = srdf.bytes.rfind("</robot>");
  ASSERT_NE(end, std::string::npos);
  std::string chain;
  chain.reserve(500000U);
  constexpr std::size_t kGroupCount = 10000U;
  for (std::size_t index = 0U; index < kGroupCount; ++index) {
    chain.append("<group name=\"g");
    chain.append(std::to_string(index));
    chain.append("\">");
    if (index + 1U < kGroupCount) {
      chain.append("<group name=\"g");
      chain.append(std::to_string(index + 1U));
      chain.append("\"/>");
    }
    chain.append("</group>");
  }
  srdf.bytes.insert(end, chain);

  const auto verified = verify(fixture);
  EXPECT_EQ(verified.parsed_ir().groups.size(), kGroupCount + 1U);
}

TEST(ModelSource, RejectsMimicAndSubgroupCyclesWithoutRecursiveWalks) {
  Fixture mimic;
  auto& urdf = blob_for(mimic, mimic.urdf_uri);
  const auto first_joint_end = urdf.bytes.find("</joint>");
  ASSERT_NE(first_joint_end, std::string::npos);
  urdf.bytes.insert(first_joint_end, "<mimic joint=\"joint2\"/>");
  const auto robot_end = urdf.bytes.rfind("</robot>");
  ASSERT_NE(robot_end, std::string::npos);
  urdf.bytes.insert(
      robot_end,
      "<link name=\"tool1\"/>"
      "<joint name=\"joint2\" type=\"fixed\">"
      "<parent link=\"tool0\"/><child link=\"tool1\"/>"
      "<mimic joint=\"joint1\"/></joint>");
  EXPECT_THROW(verify(mimic), ModelSourceError);

  Fixture subgroup;
  auto& srdf = blob_for(subgroup, *subgroup.srdf_uri);
  const auto srdf_end = srdf.bytes.rfind("</robot>");
  ASSERT_NE(srdf_end, std::string::npos);
  srdf.bytes.insert(
      srdf_end,
      "<group name=\"a\"><group name=\"b\"/></group>"
      "<group name=\"b\"><group name=\"a\"/></group>");
  EXPECT_THROW(verify(subgroup), ModelSourceError);
}

TEST(ModelSource, AcceptsLogicalUrisLongerThanSourceSymbols) {
  Fixture fixture;
  std::string long_uri{"asset://test_robot/meshes/"};
  for (std::size_t index = 0U; index < 520U; ++index) {
    long_uri.append("a/");
  }
  long_uri.append("base.stl");
  ASSERT_GT(long_uri.size(),
            core::kMaxModelAssetSourceSymbolCodePoints);
  ASSERT_LT(long_uri.size(), core::kMaxModelAssetLogicalUriBytes);

  auto& urdf = blob_for(fixture, fixture.urdf_uri);
  const std::string old_uri{"asset://test_robot/meshes/base.stl"};
  const auto offset = urdf.bytes.find(old_uri);
  ASSERT_NE(offset, std::string::npos);
  urdf.bytes.replace(offset, old_uri.size(), long_uri);
  blob_for(fixture, old_uri).logical_uri = long_uri;

  const auto verified = verify(fixture);
  EXPECT_EQ(verified.parsed_ir().referenced_artifact_logical_uris,
            (std::vector<std::string>{long_uri}));
}

TEST(ModelSource, EnforcesReferenceKindSpecificMediaTypes) {
  Fixture fixture;
  blob_for(fixture, "asset://test_robot/meshes/base.stl").media_type =
      "image/png";
  const auto plan = preflight_model_source_declaration(qualify(fixture));
  const auto row = std::find_if(
      plan.artifacts().begin(), plan.artifacts().end(),
      [](const auto& artifact) {
        return artifact.logical_uri ==
               "asset://test_robot/meshes/base.stl";
      });
  ASSERT_NE(row, plan.artifacts().end());
  EXPECT_EQ(row->artifact_kind, ModelSourceArtifactKind::kPngTexture);
  EXPECT_THROW(verify_model_source_closure(plan, candidates(fixture)),
               ModelSourceError);
}

TEST(ModelSource, EnforcesOneCombinedUrdfSrdfElementBudget) {
  Fixture fixture;
  auto& urdf = blob_for(fixture, fixture.urdf_uri);
  const auto urdf_end = urdf.bytes.rfind("</robot>");
  ASSERT_NE(urdf_end, std::string::npos);
  std::string urdf_padding;
  urdf_padding.reserve(4000000U);
  for (std::size_t index = 0U; index < 124990U; ++index) {
    urdf_padding.append("<material name=\"m");
    urdf_padding.append(std::to_string(index));
    urdf_padding.append("\"/>");
  }
  urdf.bytes.insert(urdf_end, urdf_padding);

  auto& srdf = blob_for(fixture, *fixture.srdf_uri);
  const auto srdf_end = srdf.bytes.rfind("</robot>");
  ASSERT_NE(srdf_end, std::string::npos);
  std::string srdf_padding;
  srdf_padding.reserve(4000000U);
  for (std::size_t index = 0U; index < 125000U; ++index) {
    srdf_padding.append("<group name=\"g");
    srdf_padding.append(std::to_string(index));
    srdf_padding.append("\"/>");
  }
  srdf.bytes.insert(srdf_end, srdf_padding);

  try {
    (void)verify(fixture);
    FAIL() << "combined URDF/SRDF element budget was not enforced";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find("element bound"),
              std::string::npos);
  }
}

}  // namespace
}  // namespace manyforge::deployment
