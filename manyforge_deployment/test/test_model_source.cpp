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
static_assert(!std::is_aggregate_v<ModelSourceIr>);
static_assert(!std::is_default_constructible_v<ModelSourceIr>);
static_assert(std::is_aggregate_v<UrdfDocumentIr>);
static_assert(std::is_copy_constructible_v<UrdfDocumentIr>);
static_assert(std::is_aggregate_v<SrdfDocumentIr>);
static_assert(std::is_copy_constructible_v<SrdfDocumentIr>);

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

[[nodiscard]] SourceVerifiedModelAsset canonical_round_trip(
    Fixture fixture, const ModelSourceIr& source_ir) {
  blob_for(fixture, fixture.urdf_uri).bytes =
      emit_canonical_urdf(source_ir);
  const auto srdf = emit_canonical_srdf(source_ir);
  if (fixture.srdf_uri.has_value()) {
    EXPECT_TRUE(srdf.has_value());
    if (srdf.has_value()) {
      blob_for(fixture, *fixture.srdf_uri).bytes = *srdf;
    }
  } else {
    EXPECT_FALSE(srdf.has_value());
  }
  return verify(fixture);
}

template <typename Values>
[[nodiscard]] const auto& named(const Values& values,
                                std::string_view name) {
  const auto found =
      std::find_if(values.begin(), values.end(), [name](const auto& value) {
        return value.name == name;
      });
  EXPECT_NE(found, values.end());
  return *found;
}

void replace_once(std::string& value, std::string_view before,
                  std::string_view after) {
  const auto position = value.find(before);
  ASSERT_NE(position, std::string::npos);
  if (position != std::string::npos) {
    value.replace(position, before.size(), after);
  }
}

[[nodiscard]] std::string srdf_for_exact_expansion_budget(
    std::size_t target_rows) {
  // Every non-empty one-joint expansion with P provenance occurrences costs
  // P provenance rows plus one unique expanded-joint row. A Fibonacci-shaped
  // subgroup DAG creates large, deterministic overlap from a tiny source.
  std::vector<std::size_t> provenance_counts{1U, 1U};
  std::vector<std::string> groups{
      "<group name=\"budget_0\"><joint name=\"joint1\"/></group>",
      "<group name=\"budget_1\"><group name=\"budget_0\"/></group>"};
  std::size_t used_rows = 4U;
  while (true) {
    const auto next_provenance =
        provenance_counts[provenance_counts.size() - 1U] +
        provenance_counts[provenance_counts.size() - 2U];
    const auto next_cost = next_provenance + 1U;
    if (next_cost >= target_rows - used_rows) {
      break;
    }
    const auto index = provenance_counts.size();
    groups.push_back(
        "<group name=\"budget_" + std::to_string(index) +
        "\"><group name=\"budget_" + std::to_string(index - 1U) +
        "\"/><group name=\"budget_" + std::to_string(index - 2U) +
        "\"/></group>");
    provenance_counts.push_back(next_provenance);
    used_rows += next_cost;
  }

  if (target_rows <= used_rows + 1U) {
    ADD_FAILURE() << "target expansion budget is not representable";
    return {};
  }
  std::size_t required_provenance = target_rows - used_rows - 1U;
  std::string final_group{"<group name=\"manipulator\">"};
  for (std::size_t index = provenance_counts.size(); index-- > 0U;) {
    if (provenance_counts[index] <= required_provenance) {
      final_group += "<group name=\"budget_" + std::to_string(index) +
                     "\"/>";
      required_provenance -= provenance_counts[index];
    }
  }
  EXPECT_EQ(required_provenance, 0U);
  final_group += "</group>";

  std::string srdf{"<robot name=\"test_robot\">"};
  for (const auto& group : groups) {
    srdf += group;
  }
  srdf += final_group;
  srdf +=
      "<group_state name=\"home\" group=\"manipulator\">"
      "<joint name=\"joint1\" value=\"0\"/></group_state>"
      "<disable_collisions link1=\"base_link\" link2=\"tool0\" "
      "reason=\"Adjacent\"/></robot>";
  return srdf;
}

[[nodiscard]] Fixture complete_ir_fixture() {
  Fixture fixture;
  blob_for(fixture, fixture.urdf_uri).bytes =
      "<robot name=\"test_robot\">"
      "<material name=\"Steel\"><color rgba=\"0.125 0.25 0.5 1\"/>"
      "<texture filename=\"asset://test_robot/textures/steel.png\"/>"
      "</material>"
      "<link name=\"planar_link\"/>"
      "<link name=\"base_link\">"
      "<inertial><origin xyz=\"0.1 0.2 0.3\" rpy=\"0.4 0.5 0.6\"/>"
      "<mass value=\"2.5\"/><inertia ixx=\"1\" ixy=\"0.1\" ixz=\"0.2\" "
      "iyy=\"2\" iyz=\"0.3\" izz=\"3\"/></inertial>"
      "<visual name=\"mesh_visual\"><origin xyz=\"-0 0.2 0\" "
      "rpy=\"0 0 0.5\"/><geometry><mesh "
      "filename=\"asset://test_robot/meshes/base.stl\" "
      "scale=\"1 2 3\"/></geometry><material name=\"Steel\"/></visual>"
      "<visual name=\"box_visual\"><geometry><box size=\"1 2 3\"/>"
      "</geometry><material name=\"Inline\"><color rgba=\"1 0 0 0.5\"/>"
      "<texture filename=\"asset://test_robot/textures/steel.png\"/>"
      "</material></visual>"
      "<collision name=\"sphere_collision\"><geometry>"
      "<sphere radius=\"0.75\"/></geometry></collision>"
      "<collision name=\"cylinder_collision\"><origin xyz=\"0 0 1\"/>"
      "<geometry><cylinder radius=\"0.25\" length=\"1.5\"/>"
      "</geometry></collision></link>"
      "<link name=\"tool0\"/><link name=\"mimic_link\"/>"
      "<link name=\"linear_link\"/><link name=\"fixed_link\"/>"
      "<link name=\"floating_link\"/>"
      "<joint name=\"joint6\" type=\"planar\"><parent link=\"floating_link\"/>"
      "<child link=\"planar_link\"/><axis xyz=\"0 0 1\"/></joint>"
      "<joint name=\"joint1\" type=\"revolute\"><origin xyz=\"0 0 0.1\" "
      "rpy=\"0 0.2 0\"/><parent link=\"base_link\"/>"
      "<child link=\"tool0\"/><axis xyz=\"0 0 1\"/>"
      "<calibration rising=\"0.1\" falling=\"-0.2\" "
      "reference_position=\"0.3\"/><dynamics damping=\"0.4\" "
      "friction=\"0.5\"/><limit lower=\"-2\" upper=\"2\" "
      "effort=\"10\" velocity=\"1.5\"/>"
      "<safety_controller soft_lower_limit=\"-1.5\" "
      "soft_upper_limit=\"1.5\" k_position=\"2\" k_velocity=\"3\"/>"
      "</joint>"
      "<joint name=\"joint2\" type=\"continuous\"><parent link=\"tool0\"/>"
      "<child link=\"mimic_link\"/><axis xyz=\"0 0 1\"/>"
      "<limit effort=\"8\" velocity=\"2\"/>"
      "<mimic joint=\"joint1\" multiplier=\"-2\" offset=\"0.125\"/>"
      "</joint>"
      "<joint name=\"joint3\" type=\"prismatic\">"
      "<parent link=\"mimic_link\"/><child link=\"linear_link\"/>"
      "<axis xyz=\"0 1 0\"/><limit lower=\"0\" upper=\"0.5\" "
      "effort=\"5\" velocity=\"0.2\"/></joint>"
      "<joint name=\"joint4\" type=\"fixed\"><parent link=\"linear_link\"/>"
      "<child link=\"fixed_link\"/></joint>"
      "<joint name=\"joint5\" type=\"floating\"><parent link=\"fixed_link\"/>"
      "<child link=\"floating_link\"/></joint></robot>";
  blob_for(fixture, *fixture.srdf_uri).bytes =
      "<robot name=\"test_robot\">"
      "<group name=\"tool_group\"><link name=\"planar_link\"/></group>"
      "<group name=\"manipulator\"><chain base_link=\"base_link\" "
      "tip_link=\"tool0\"/></group>"
      "<group name=\"angular_pair\"><joint name=\"joint2\"/>"
      "<joint name=\"joint1\"/></group>"
      "<group name=\"whole\"><chain base_link=\"base_link\" "
      "tip_link=\"planar_link\"/><group name=\"angular_pair\"/></group>"
      "<group_state name=\"ready\" group=\"angular_pair\">"
      "<joint name=\"joint2\" value=\"0.25\"/>"
      "<joint name=\"joint1\" value=\"-0\"/></group_state>"
      "<group_state name=\"home\" group=\"manipulator\">"
      "<joint name=\"joint1\" value=\"0\"/></group_state>"
      "<end_effector name=\"tool\" parent_link=\"planar_link\" "
      "group=\"tool_group\" parent_group=\"whole\"/>"
      "<passive_joint name=\"joint4\"/>"
      "<disable_collisions link1=\"tool0\" link2=\"base_link\" "
      "reason=\"quote &quot; &amp; &lt; > ' &#9;&#10;&#13; café\"/>"
      "</robot>";
  fixture.blobs.push_back(
      {"asset://test_robot/textures/steel.png", "image/png", "png"});
  return fixture;
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

TEST(ModelSource, VerifiesExactClosureAndRetainsCompleteTypedSourceIr) {
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
  EXPECT_EQ(verified.source_ir().urdf().robot_name, "test_robot");
  EXPECT_EQ(verified.source_ir().semantic_index().root_link_name,
            "base_link");
  ASSERT_EQ(verified.source_ir().urdf().materials.size(), 1U);
  EXPECT_EQ(verified.source_ir().urdf().materials.front().name, "Steel");
  ASSERT_EQ(verified.source_ir().urdf().links.size(), 2U);
  EXPECT_EQ(verified.source_ir().urdf().links.front().name, "base_link");
  ASSERT_EQ(verified.source_ir().urdf().joints.size(), 1U);
  EXPECT_EQ(verified.source_ir().urdf().joints.front().name, "joint1");
  ASSERT_TRUE(verified.source_ir().srdf().has_value());
  ASSERT_EQ(verified.source_ir().srdf()->groups.size(), 1U);
  EXPECT_EQ(verified.source_ir().srdf()->groups.front().name,
            "manipulator");
  EXPECT_EQ(
      verified.source_ir().artifact_references(),
      (std::vector<SourceArtifactReferenceIr>{
          {"asset://test_robot/meshes/base.stl",
           SourceArtifactReferenceKind::kMesh}}));
}

TEST(ModelSource, RetainsEverySupportedRecordAndCanonicalRoundTrips) {
  const auto fixture = complete_ir_fixture();
  const auto verified = verify(fixture);
  const auto& source = verified.source_ir();

  ASSERT_EQ(source.urdf().materials.size(), 1U);
  const auto& steel = source.urdf().materials.front();
  ASSERT_TRUE(steel.name.has_value());
  EXPECT_EQ(*steel.name, "Steel");
  ASSERT_TRUE(steel.color.has_value());
  EXPECT_EQ(steel.color->rgba,
            (ModelSourceVector4{0.125, 0.25, 0.5, 1.0}));
  ASSERT_TRUE(steel.texture.has_value());
  EXPECT_EQ(steel.texture->logical_uri,
            "asset://test_robot/textures/steel.png");

  const auto& base = named(source.urdf().links, "base_link");
  ASSERT_TRUE(base.inertial.has_value());
  EXPECT_EQ(base.inertial->origin.xyz_m,
            (ModelSourceVector3{0.1, 0.2, 0.3}));
  EXPECT_DOUBLE_EQ(base.inertial->mass_kg, 2.5);
  EXPECT_DOUBLE_EQ(base.inertial->inertia.iyz, 0.3);
  ASSERT_EQ(base.visuals.size(), 2U);
  const auto& mesh_visual = named(base.visuals, "mesh_visual");
  const auto& mesh = std::get<UrdfMeshGeometryIr>(mesh_visual.geometry);
  ASSERT_TRUE(mesh.authored_scale.has_value());
  EXPECT_EQ(mesh.scale, (ModelSourceVector3{1.0, 2.0, 3.0}));
  ASSERT_TRUE(mesh_visual.material.has_value());
  ASSERT_TRUE(mesh_visual.material->resolved_reference.has_value());
  EXPECT_EQ(mesh_visual.material->resolved_reference->definition_name,
            "Steel");
  EXPECT_EQ(mesh_visual.material->resolved_reference->color, steel.color);
  EXPECT_EQ(mesh_visual.material->resolved_reference->texture,
            steel.texture);
  const auto& box_visual = named(base.visuals, "box_visual");
  EXPECT_EQ(std::get<UrdfBoxGeometryIr>(box_visual.geometry).size_m,
            (ModelSourceVector3{1.0, 2.0, 3.0}));
  ASSERT_EQ(base.collisions.size(), 2U);
  EXPECT_DOUBLE_EQ(
      std::get<UrdfCylinderGeometryIr>(
          named(base.collisions, "cylinder_collision").geometry)
          .length_m,
      1.5);
  EXPECT_DOUBLE_EQ(
      std::get<UrdfSphereGeometryIr>(
          named(base.collisions, "sphere_collision").geometry)
          .radius_m,
      0.75);

  const auto& joint1 = named(source.urdf().joints, "joint1");
  EXPECT_EQ(joint1.type, UrdfJointType::kRevolute);
  EXPECT_EQ(joint1.coordinate_kind, UrdfCoordinateKind::kAngular);
  ASSERT_TRUE(joint1.axis.has_value());
  EXPECT_TRUE(joint1.axis->element_authored);
  EXPECT_EQ(joint1.axis->xyz, (ModelSourceVector3{0.0, 0.0, 1.0}));
  ASSERT_TRUE(joint1.calibration.has_value());
  ASSERT_TRUE(
      joint1.calibration->authored_reference_position.has_value());
  EXPECT_DOUBLE_EQ(joint1.calibration->reference_position, 0.3);
  ASSERT_TRUE(joint1.dynamics.has_value());
  EXPECT_DOUBLE_EQ(joint1.dynamics->friction, 0.5);
  ASSERT_TRUE(joint1.limit.has_value());
  EXPECT_DOUBLE_EQ(joint1.limit->upper, 2.0);
  ASSERT_TRUE(joint1.safety_controller.has_value());
  EXPECT_DOUBLE_EQ(joint1.safety_controller->k_velocity, 3.0);
  const auto& joint2 = named(source.urdf().joints, "joint2");
  ASSERT_TRUE(joint2.mimic.has_value());
  EXPECT_EQ(joint2.mimic->joint_name, "joint1");
  EXPECT_DOUBLE_EQ(joint2.mimic->multiplier, -2.0);
  EXPECT_DOUBLE_EQ(joint2.mimic->offset, 0.125);
  EXPECT_EQ(named(source.urdf().joints, "joint3").coordinate_kind,
            UrdfCoordinateKind::kLinear);
  EXPECT_EQ(named(source.urdf().joints, "joint4").coordinate_kind,
            UrdfCoordinateKind::kNone);
  EXPECT_EQ(named(source.urdf().joints, "joint5").coordinate_kind,
            UrdfCoordinateKind::kFloating);
  const auto& planar = named(source.urdf().joints, "joint6");
  EXPECT_EQ(planar.coordinate_kind, UrdfCoordinateKind::kPlanar);
  ASSERT_TRUE(planar.axis.has_value());
  EXPECT_TRUE(planar.axis->element_authored);
  EXPECT_EQ(planar.axis->xyz, (ModelSourceVector3{0.0, 0.0, 1.0}));

  auto default_planar_fixture = fixture;
  replace_once(
      blob_for(default_planar_fixture,
               default_planar_fixture.urdf_uri)
          .bytes,
      "<child link=\"planar_link\"/><axis xyz=\"0 0 1\"/></joint>",
      "<child link=\"planar_link\"/></joint>");
  const auto default_planar = verify(default_planar_fixture);
  const auto& default_planar_joint =
      named(default_planar.source_ir().urdf().joints, "joint6");
  ASSERT_TRUE(default_planar_joint.axis.has_value());
  EXPECT_FALSE(default_planar_joint.axis->element_authored);
  EXPECT_EQ(default_planar_joint.axis->xyz,
            (ModelSourceVector3{1.0, 0.0, 0.0}));

  ASSERT_TRUE(source.srdf().has_value());
  const auto& srdf = *source.srdf();
  ASSERT_EQ(srdf.groups.size(), 4U);
  const auto& whole = named(srdf.groups, "whole");
  ASSERT_EQ(whole.members.size(), 2U);
  EXPECT_TRUE(
      std::holds_alternative<SrdfChainGroupMemberIr>(whole.members[0]));
  EXPECT_TRUE(
      std::holds_alternative<SrdfSubgroupMemberIr>(whole.members[1]));
  ASSERT_EQ(srdf.group_states.size(), 2U);
  EXPECT_EQ(srdf.group_states.front().group_name, "angular_pair");
  EXPECT_EQ(srdf.group_states.front().joints.front().joint_name,
            "joint1");
  ASSERT_EQ(srdf.end_effectors.size(), 1U);
  EXPECT_EQ(srdf.end_effectors.front().parent_group_name, "whole");
  EXPECT_EQ(srdf.passive_joint_names,
            (std::vector<std::string>{"joint4"}));
  ASSERT_EQ(srdf.disabled_collisions.size(), 1U);
  EXPECT_EQ(srdf.disabled_collisions.front().first_link_name,
            "base_link");
  EXPECT_EQ(srdf.disabled_collisions.front().second_link_name, "tool0");
  ASSERT_TRUE(srdf.disabled_collisions.front().reason.has_value());
  EXPECT_EQ(*srdf.disabled_collisions.front().reason,
            "quote \" & < > ' \t\n\r café");

  const auto& index = source.semantic_index();
  EXPECT_EQ(index.root_link_name, "base_link");
  ASSERT_FALSE(index.group_expansion_provenance.empty());
  const auto expanded_whole = std::find_if(
      index.expanded_groups.begin(), index.expanded_groups.end(),
      [](const ExpandedSrdfGroupIr& group) {
        return group.group_name == "whole";
      });
  ASSERT_NE(expanded_whole, index.expanded_groups.end());
  EXPECT_EQ(expanded_whole->links.front().name, "base_link");
  EXPECT_GE(expanded_whole->joints.front().provenance_indices.size(),
            2U);
  EXPECT_EQ(
      source.artifact_references(),
      (std::vector<SourceArtifactReferenceIr>{
          {"asset://test_robot/meshes/base.stl",
           SourceArtifactReferenceKind::kMesh},
          {"asset://test_robot/textures/steel.png",
           SourceArtifactReferenceKind::kTexture}}));

  const auto urdf_xml = emit_canonical_urdf(source);
  const auto srdf_xml = emit_canonical_srdf(source);
  ASSERT_TRUE(srdf_xml.has_value());
  EXPECT_EQ(urdf_xml.find("xyz=\"-0 "), std::string::npos);
  EXPECT_NE(srdf_xml->find(
                "reason=\"quote &quot; &amp; &lt; > ' "
                "&#9;&#10;&#13; café\""),
            std::string::npos);
  const auto round_trip = canonical_round_trip(fixture, source);
  EXPECT_EQ(round_trip.source_ir(), source);
  // Canonical output intentionally erases authored-default provenance while
  // retaining the same complete semantic projection.
  EXPECT_EQ(emit_canonical_urdf(round_trip.source_ir()), urdf_xml);
  EXPECT_EQ(emit_canonical_srdf(round_trip.source_ir()), srdf_xml);
}

TEST(ModelSource, TreatsAuthoredIdentityDefaultsAsNonSemanticProvenance) {
  Fixture omitted;
  auto& omitted_urdf = blob_for(omitted, omitted.urdf_uri);
  omitted_urdf.bytes =
      "<robot name=\"test_robot\">"
      "<link name=\"base_link\"><visual><geometry><mesh "
      "filename=\"asset://test_robot/meshes/base.stl\"/>"
      "</geometry></visual></link><link name=\"tool0\"/>"
      "<link name=\"mimic_link\"/>"
      "<joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"tool0\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"10\" velocity=\"1\"/>"
      "</joint><joint name=\"joint2\" type=\"revolute\">"
      "<parent link=\"tool0\"/><child link=\"mimic_link\"/>"
      "<limit effort=\"10\" velocity=\"1\"/>"
      "<mimic joint=\"joint1\"/></joint></robot>";
  Fixture explicit_defaults = omitted;
  auto& explicit_urdf =
      blob_for(explicit_defaults, explicit_defaults.urdf_uri);
  replace_once(explicit_urdf.bytes, "<visual>",
               "<visual><origin xyz=\"-0 0 0\" rpy=\"0 -0 0\"/>");
  replace_once(explicit_urdf.bytes,
               "meshes/base.stl\"/>",
               "meshes/base.stl\" scale=\"1 1 1\"/>");
  replace_once(explicit_urdf.bytes,
               "<parent link=\"base_link\"/>",
               "<origin xyz=\"0 0 -0\" rpy=\"-0 0 0\"/>"
               "<parent link=\"base_link\"/>");
  replace_once(explicit_urdf.bytes,
               "<limit lower=\"-3\"",
               "<axis xyz=\"1 0 0\"/><limit lower=\"-3\"");
  replace_once(explicit_urdf.bytes,
               "<mimic joint=\"joint1\"/>",
               "<axis xyz=\"1 0 0\"/><mimic joint=\"joint1\" "
               "multiplier=\"1\" offset=\"-0\"/>");

  const auto omitted_source = verify(omitted);
  const auto explicit_source = verify(explicit_defaults);
  EXPECT_NE(omitted_source.source_ir().urdf(),
            explicit_source.source_ir().urdf());
  EXPECT_EQ(omitted_source.source_ir().semantic_index(),
            explicit_source.source_ir().semantic_index());
  EXPECT_EQ(omitted_source.source_ir(), explicit_source.source_ir());
  EXPECT_EQ(emit_canonical_urdf(omitted_source.source_ir()),
            emit_canonical_urdf(explicit_source.source_ir()));
}

TEST(ModelSource, MaterializesJointElementDefaultsWithoutLosingAuthorship) {
  Fixture left;
  auto& left_urdf = blob_for(left, left.urdf_uri);
  replace_once(
      left_urdf.bytes, "<limit lower=\"-3\"",
      "<calibration reference_position=\"0\"/>"
      "<dynamics damping=\"0\"/>"
      "<safety_controller k_velocity=\"1\"/>"
      "<limit lower=\"-3\"");
  replace_once(
      left_urdf.bytes,
      "<limit lower=\"-3\" upper=\"3\" effort=\"10\" velocity=\"1\"/>",
      "<limit effort=\"10\" velocity=\"1\"/>");

  Fixture right = left;
  auto& right_urdf = blob_for(right, right.urdf_uri);
  replace_once(
      right_urdf.bytes,
      "<calibration reference_position=\"0\"/>"
      "<dynamics damping=\"0\"/>"
      "<safety_controller k_velocity=\"1\"/>",
      "<calibration/>"
      "<dynamics friction=\"-0\"/>"
      "<safety_controller soft_lower_limit=\"-0\" "
      "soft_upper_limit=\"0\" k_position=\"0\" k_velocity=\"1\"/>");
  replace_once(right_urdf.bytes,
               "<limit effort=\"10\" velocity=\"1\"/>",
               "<limit lower=\"0\" upper=\"-0\" "
               "effort=\"10\" velocity=\"1\"/>");

  const auto left_source = verify(left);
  const auto right_source = verify(right);
  const auto& left_joint =
      named(left_source.source_ir().urdf().joints, "joint1");
  const auto& right_joint =
      named(right_source.source_ir().urdf().joints, "joint1");

  ASSERT_TRUE(left_joint.calibration.has_value());
  ASSERT_TRUE(
      left_joint.calibration->authored_reference_position.has_value());
  EXPECT_DOUBLE_EQ(left_joint.calibration->reference_position, 0.0);
  ASSERT_TRUE(right_joint.calibration.has_value());
  EXPECT_FALSE(
      right_joint.calibration->authored_reference_position.has_value());
  EXPECT_DOUBLE_EQ(right_joint.calibration->reference_position, 0.0);

  ASSERT_TRUE(left_joint.dynamics.has_value());
  ASSERT_TRUE(left_joint.dynamics->authored_damping.has_value());
  EXPECT_FALSE(left_joint.dynamics->authored_friction.has_value());
  EXPECT_DOUBLE_EQ(left_joint.dynamics->damping, 0.0);
  EXPECT_DOUBLE_EQ(left_joint.dynamics->friction, 0.0);
  ASSERT_TRUE(right_joint.dynamics.has_value());
  EXPECT_FALSE(right_joint.dynamics->authored_damping.has_value());
  ASSERT_TRUE(right_joint.dynamics->authored_friction.has_value());
  EXPECT_DOUBLE_EQ(right_joint.dynamics->damping, 0.0);
  EXPECT_DOUBLE_EQ(right_joint.dynamics->friction, 0.0);

  ASSERT_TRUE(right_joint.safety_controller.has_value());
  EXPECT_DOUBLE_EQ(right_joint.safety_controller->soft_lower_limit, 0.0);
  EXPECT_DOUBLE_EQ(right_joint.safety_controller->soft_upper_limit, 0.0);
  EXPECT_DOUBLE_EQ(right_joint.safety_controller->k_position, 0.0);
  EXPECT_DOUBLE_EQ(right_joint.safety_controller->k_velocity, 1.0);
  ASSERT_TRUE(left_joint.limit.has_value());
  EXPECT_FALSE(left_joint.limit->authored_lower.has_value());
  EXPECT_FALSE(left_joint.limit->authored_upper.has_value());
  EXPECT_DOUBLE_EQ(left_joint.limit->lower, 0.0);
  EXPECT_DOUBLE_EQ(left_joint.limit->upper, 0.0);
  ASSERT_TRUE(right_joint.limit.has_value());
  ASSERT_TRUE(right_joint.limit->authored_lower.has_value());
  ASSERT_TRUE(right_joint.limit->authored_upper.has_value());
  EXPECT_DOUBLE_EQ(right_joint.limit->lower, 0.0);
  EXPECT_DOUBLE_EQ(right_joint.limit->upper, 0.0);
  EXPECT_NE(left_source.source_ir().urdf(),
            right_source.source_ir().urdf());
  EXPECT_EQ(left_source.source_ir(), right_source.source_ir());
  EXPECT_EQ(emit_canonical_urdf(left_source.source_ir()),
            emit_canonical_urdf(right_source.source_ir()));
}

TEST(ModelSource, CanonicalizesAllNonSemanticCollectionsInMemory) {
  Fixture ordered;
  ordered.blobs.erase(
      std::remove_if(ordered.blobs.begin(), ordered.blobs.end(),
                     [](const SourceBlob& blob) {
                       return blob.media_type == "model/stl";
                     }),
      ordered.blobs.end());
  blob_for(ordered, ordered.urdf_uri).bytes =
      "<robot name=\"test_robot\">"
      "<material name=\"A\"><color rgba=\"1 0 0 1\"/></material>"
      "<material name=\"B\"><color rgba=\"0 1 0 1\"/></material>"
      "<link name=\"base_link\"><visual name=\"a\"><geometry>"
      "<box size=\"1 1 1\"/></geometry><material name=\"A\"/></visual>"
      "<visual name=\"b\"><geometry><sphere radius=\"1\"/></geometry>"
      "<material name=\"B\"/></visual>"
      "<collision name=\"a\"><geometry><box size=\"1 1 1\"/>"
      "</geometry></collision><collision name=\"b\"><geometry>"
      "<sphere radius=\"1\"/></geometry></collision></link>"
      "<link name=\"tool0\"/><link name=\"aux\"/>"
      "<joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"tool0\"/>"
      "<limit effort=\"10\" velocity=\"1\"/></joint>"
      "<joint name=\"joint2\" type=\"revolute\">"
      "<parent link=\"tool0\"/><child link=\"aux\"/>"
      "<limit effort=\"10\" velocity=\"1\"/></joint></robot>";
  blob_for(ordered, *ordered.srdf_uri).bytes =
      "<robot name=\"test_robot\">"
      "<group name=\"manipulator\"><chain base_link=\"base_link\" "
      "tip_link=\"tool0\"/></group>"
      "<group name=\"zgroup\"><joint name=\"joint2\"/></group>"
      "<group_state name=\"home\" group=\"manipulator\">"
      "<joint name=\"joint1\" value=\"0\"/></group_state>"
      "<group_state name=\"ready\" group=\"zgroup\">"
      "<joint name=\"joint2\" value=\"1\"/></group_state>"
      "<end_effector name=\"tool\" parent_link=\"tool0\" group=\"zgroup\" "
      "parent_group=\"manipulator\"/>"
      "<passive_joint name=\"joint2\"/>"
      "<disable_collisions link1=\"base_link\" link2=\"tool0\"/>"
      "<disable_collisions link1=\"tool0\" link2=\"aux\" reason=\"x\"/>"
      "</robot>";

  Fixture shuffled = ordered;
  blob_for(shuffled, shuffled.urdf_uri).bytes =
      "<robot name=\"test_robot\">"
      "<joint name=\"joint2\" type=\"revolute\"><parent link=\"tool0\"/>"
      "<child link=\"aux\"/><limit effort=\"10\" velocity=\"1\"/>"
      "</joint><link name=\"aux\"/>"
      "<material name=\"B\"><color rgba=\"0 1 0 1\"/></material>"
      "<link name=\"tool0\"/><link name=\"base_link\">"
      "<collision name=\"b\"><geometry><sphere radius=\"1\"/>"
      "</geometry></collision><collision name=\"a\"><geometry>"
      "<box size=\"1 1 1\"/></geometry></collision>"
      "<visual name=\"b\"><geometry><sphere radius=\"1\"/></geometry>"
      "<material name=\"B\"/></visual><visual name=\"a\"><geometry>"
      "<box size=\"1 1 1\"/></geometry><material name=\"A\"/></visual>"
      "</link><material name=\"A\"><color rgba=\"1 0 0 1\"/></material>"
      "<joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"tool0\"/>"
      "<limit effort=\"10\" velocity=\"1\"/></joint>"
      "</robot>";
  blob_for(shuffled, *shuffled.srdf_uri).bytes =
      "<robot name=\"test_robot\">"
      "<disable_collisions link1=\"aux\" link2=\"tool0\" reason=\"x\"/>"
      "<passive_joint name=\"joint2\"/>"
      "<end_effector name=\"tool\" parent_link=\"tool0\" group=\"zgroup\" "
      "parent_group=\"manipulator\"/>"
      "<group_state name=\"ready\" group=\"zgroup\">"
      "<joint name=\"joint2\" value=\"1\"/></group_state>"
      "<group name=\"zgroup\"><joint name=\"joint2\"/></group>"
      "<disable_collisions link1=\"tool0\" link2=\"base_link\"/>"
      "<group_state name=\"home\" group=\"manipulator\">"
      "<joint name=\"joint1\" value=\"0\"/></group_state>"
      "<group name=\"manipulator\"><chain base_link=\"base_link\" "
      "tip_link=\"tool0\"/></group></robot>";

  const auto canonical = verify(ordered);
  const auto reordered = verify(shuffled);
  EXPECT_EQ(canonical.source_ir().urdf(), reordered.source_ir().urdf());
  EXPECT_EQ(canonical.source_ir().srdf(), reordered.source_ir().srdf());
  EXPECT_EQ(canonical.source_ir().semantic_index(),
            reordered.source_ir().semantic_index());
  EXPECT_EQ(canonical.source_ir().artifact_references(),
            reordered.source_ir().artifact_references());
  EXPECT_EQ(canonical.source_ir(), reordered.source_ir());
  EXPECT_EQ(emit_canonical_urdf(canonical.source_ir()),
            emit_canonical_urdf(reordered.source_ir()));
  EXPECT_EQ(emit_canonical_srdf(canonical.source_ir()),
            emit_canonical_srdf(reordered.source_ir()));
}

TEST(ModelSource, ResolvesMaterialReferencesOnceAndRetainsTheirIdentity) {
  Fixture referenced;
  const auto referenced_source = verify(referenced);
  const auto& referenced_visual =
      referenced_source.source_ir().urdf().links.front().visuals.front();
  ASSERT_TRUE(referenced_visual.material.has_value());
  ASSERT_TRUE(referenced_visual.material->resolved_reference.has_value());
  EXPECT_EQ(
      referenced_visual.material->resolved_reference->definition_name,
      "Steel");
  ASSERT_TRUE(referenced_visual.material->resolved_reference->color
                  .has_value());
  EXPECT_DOUBLE_EQ(
      referenced_visual.material->resolved_reference->color->rgba[0],
      0.5);

  Fixture changed = referenced;
  replace_once(blob_for(changed, changed.urdf_uri).bytes,
               "0.5 0.5 0.5 1", "0.25 0.5 0.5 1");
  const auto changed_source = verify(changed);
  const auto& changed_visual =
      changed_source.source_ir().urdf().links.front().visuals.front();
  EXPECT_DOUBLE_EQ(
      changed_visual.material->resolved_reference->color->rgba[0],
      0.25);
  EXPECT_NE(referenced_source.source_ir(), changed_source.source_ir());

  Fixture anonymous_definition = referenced;
  replace_once(blob_for(anonymous_definition,
                        anonymous_definition.urdf_uri).bytes,
               "<material name=\"Steel\"/>",
               "<material><color rgba=\"0.5 0.5 0.5 1\"/></material>");
  EXPECT_THROW(verify(anonymous_definition), ModelSourceError);
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
  EXPECT_FALSE(verified.source_ir().srdf().has_value());
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

TEST(ModelSource, FailsAtomicallyWithoutExposingOrMutatingATrustedRoot) {
  const Fixture fixture;
  auto input = candidates(fixture);
  const auto original_uri = input.front().logical_uri;
  const auto original_key = input.front().resolved_store_key;
  const auto original_bytes = input.front().stored_bytes;
  input.front().stored_bytes.front() ^= 1;
  const auto corrupted_bytes = input.front().stored_bytes;

  EXPECT_THROW(
      verify_model_source_closure(qualify(fixture), input),
      ModelSourceError);
  EXPECT_EQ(input.front().logical_uri, original_uri);
  EXPECT_EQ(input.front().resolved_store_key, original_key);
  EXPECT_EQ(input.front().stored_bytes, corrupted_bytes);
  EXPECT_NE(input.front().stored_bytes, original_bytes);

  // A failed attempt creates no public partial stage and cannot poison a
  // later exact verification of the same declaration.
  EXPECT_NO_THROW((void)verify(fixture));
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

  Fixture malformed;
  auto& malformed_urdf = blob_for(malformed, malformed.urdf_uri);
  malformed_urdf.bytes =
      "<robot name=\"test_robot\"><link name=\"base_link\">"
      "<visual><geometry><box size=\"1 1 1\"/></geometry>"
      "</link></visual></robot>";
  malformed.blobs.erase(
      std::remove_if(malformed.blobs.begin(), malformed.blobs.end(),
                     [&malformed](const SourceBlob& blob) {
                       return blob.logical_uri != malformed.urdf_uri;
                     }),
      malformed.blobs.end());
  malformed.srdf_uri = std::nullopt;
  EXPECT_THROW(verify(malformed), ModelSourceError);
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

TEST(ModelSource, RejectsWhenCanonicalXmlWouldExceedTheEntrypointBound) {
  Fixture fixture;
  fixture.srdf_uri = std::nullopt;
  fixture.blobs.erase(
      std::remove_if(fixture.blobs.begin(), fixture.blobs.end(),
                     [&fixture](const SourceBlob& blob) {
                       return blob.logical_uri != fixture.urdf_uri;
                     }),
      fixture.blobs.end());

  std::string urdf{"<robot name=\"test_robot\"><link name=\"base_link\">"};
  constexpr std::string_view closing{
      "</link><link name=\"tool0\"/>"
      "<joint name=\"joint1\" type=\"revolute\">"
      "<parent link=\"base_link\"/><child link=\"tool0\"/>"
      "<limit lower=\"-3\" upper=\"3\" effort=\"10\" velocity=\"1\"/>"
      "</joint></robot>"};
  std::size_t visual_count = 0U;
  while (true) {
    const auto prefix =
        "visual_" + std::to_string(visual_count) + "_";
    ASSERT_LT(prefix.size(),
              core::kMaxModelAssetSourceSymbolCodePoints);
    const auto name =
        prefix +
        std::string(core::kMaxModelAssetSourceSymbolCodePoints -
                        prefix.size(),
                    'a');
    const auto visual =
        "<visual name=\"" + name +
        "\"><geometry><box size=\"1e20 1e20 1e20\"/>"
        "</geometry></visual>";
    if (urdf.size() + visual.size() + closing.size() >
        kMaxModelSourceXmlEntrypointBytes) {
      break;
    }
    urdf += visual;
    ++visual_count;
  }
  urdf += closing;
  ASSERT_GT(visual_count, 10000U);
  ASSERT_LE(urdf.size(), kMaxModelSourceXmlEntrypointBytes);
  ASSERT_LT(kMaxModelSourceXmlEntrypointBytes - urdf.size(),
            visual_count);
  blob_for(fixture, fixture.urdf_uri).bytes = std::move(urdf);

  try {
    (void)verify(fixture);
    FAIL() << "canonical XML growth escaped the entrypoint byte bound";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find(
                  "canonical URDF exceeds"),
              std::string::npos)
        << error.what();
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

  Fixture empty_top_level;
  auto& empty_top_level_urdf =
      blob_for(empty_top_level, empty_top_level.urdf_uri);
  replace_once(empty_top_level_urdf.bytes,
               "<material name=\"Steel\"><color rgba=\"0.5 0.5 0.5 1\"/>"
               "</material>",
               "<material name=\"Steel\"/>");
  EXPECT_THROW(verify(empty_top_level), ModelSourceError);
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
  std::string deep_srdf{"<robot name=\"test_robot\">"};
  deep_srdf.reserve(kLinkCount * 150U);
  parent = "base_link";
  for (std::size_t index = 1U; index < kLinkCount; ++index) {
    const auto tip =
        index + 1U == kLinkCount ? std::string("tool0")
                                : "link" + std::to_string(index);
    const auto group =
        index == 1U ? std::string("manipulator")
                    : "chain_group_" + std::to_string(index);
    deep_srdf += "<group name=\"" + group +
                 "\"><chain base_link=\"" + parent +
                 "\" tip_link=\"" + tip + "\"/></group>";
    parent = tip;
  }
  deep_srdf += "</robot>";
  srdf.bytes = std::move(deep_srdf);

  const auto verified = verify(fixture);
  ASSERT_TRUE(verified.source_ir().srdf().has_value());
  ASSERT_EQ(verified.source_ir().srdf()->groups.size(),
            kLinkCount - 1U);
  ASSERT_EQ(verified.source_ir().semantic_index().expanded_groups.size(),
            kLinkCount - 1U);
  for (const auto& group :
       verified.source_ir().semantic_index().expanded_groups) {
    EXPECT_EQ(group.links.size(), 2U);
    EXPECT_EQ(group.joints.size(), 1U);
  }
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

TEST(ModelSource, RejectsInvalidRetainedUrdfAndSrdfSemantics) {
  const auto expect_urdf_rejected =
      [](std::string_view before, std::string_view after,
         std::string_view message_fragment) {
        Fixture fixture;
        replace_once(blob_for(fixture, fixture.urdf_uri).bytes,
                     before, after);
        try {
          (void)verify(fixture);
          ADD_FAILURE() << "invalid model source was accepted";
        } catch (const ModelSourceError& error) {
          EXPECT_NE(std::string(error.what()).find(message_fragment),
                    std::string::npos);
        }
      };

  expect_urdf_rejected(
      "</visual>",
      "</visual><collision><geometry><sphere radius=\"1\"/></geometry>"
      "<material name=\"Steel\"/></collision>",
      "collision material");
  expect_urdf_rejected(
      "<limit lower=\"-3\"",
      "<axis xyz=\"0 0 -0\"/><limit lower=\"-3\"",
      "axis must not be zero");
  expect_urdf_rejected(
      "<mesh filename=\"asset://test_robot/meshes/base.stl\"/>",
      "<box size=\"1 0 1\"/>", "box dimensions");
  expect_urdf_rejected(
      "<mesh filename=\"asset://test_robot/meshes/base.stl\"/>",
      "<cylinder radius=\"0\" length=\"1\"/>",
      "cylinder dimensions");
  expect_urdf_rejected(
      "<mesh filename=\"asset://test_robot/meshes/base.stl\"/>",
      "<sphere radius=\"-1\"/>", "sphere radius");
  expect_urdf_rejected(
      "<mesh filename=\"asset://test_robot/meshes/base.stl\"/>",
      "<mesh filename=\"asset://test_robot/meshes/base.stl\" "
      "scale=\"1 0 1\"/>",
      "mesh scale");
  expect_urdf_rejected(
      "lower=\"-3\" upper=\"3\"",
      "lower=\"4\" upper=\"3\"", "lower limit exceeds");
  expect_urdf_rejected(
      "<limit lower=\"-3\"",
      "<dynamics damping=\"-1\"/><limit lower=\"-3\"",
      "dynamics values");
  expect_urdf_rejected(
      "<limit lower=\"-3\"",
      "<safety_controller soft_lower_limit=\"2\" "
      "soft_upper_limit=\"1\" k_velocity=\"1\"/>"
      "<limit lower=\"-3\"",
      "safety-controller lower limit");

  Fixture cross_coordinate_mimic;
  auto& mimic_urdf =
      blob_for(cross_coordinate_mimic, cross_coordinate_mimic.urdf_uri);
  const auto mimic_end = mimic_urdf.bytes.rfind("</robot>");
  ASSERT_NE(mimic_end, std::string::npos);
  mimic_urdf.bytes.insert(
      mimic_end,
      "<link name=\"linear\"/><joint name=\"linear_joint\" "
      "type=\"prismatic\"><parent link=\"tool0\"/>"
      "<child link=\"linear\"/><limit lower=\"0\" upper=\"1\" "
      "effort=\"1\" velocity=\"1\"/>"
      "<mimic joint=\"joint1\"/></joint>");
  try {
    (void)verify(cross_coordinate_mimic);
    FAIL() << "cross-coordinate mimic relation was accepted";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find(
                  "incompatible coordinate kinds"),
              std::string::npos);
  }

  for (const std::string_view joint_type :
       {"planar", "floating", "fixed"}) {
    Fixture multi_coordinate_state;
    auto& urdf =
        blob_for(multi_coordinate_state, multi_coordinate_state.urdf_uri);
    const auto urdf_end = urdf.bytes.rfind("</robot>");
    ASSERT_NE(urdf_end, std::string::npos);
    urdf.bytes.insert(
        urdf_end,
        "<link name=\"multi\"/><joint name=\"multi_joint\" type=\"" +
            std::string(joint_type) +
            "\"><parent link=\"tool0\"/><child link=\"multi\"/></joint>");
    auto& srdf =
        blob_for(multi_coordinate_state,
                 *multi_coordinate_state.srdf_uri);
    const auto srdf_end = srdf.bytes.rfind("</robot>");
    ASSERT_NE(srdf_end, std::string::npos);
    srdf.bytes.insert(
        srdf_end,
        "<group name=\"multi_group\"><joint name=\"multi_joint\"/>"
        "</group><group_state name=\"invalid\" group=\"multi_group\">"
        "<joint name=\"multi_joint\" value=\"0\"/></group_state>");
    try {
      (void)verify(multi_coordinate_state);
      FAIL() << "multi-coordinate SRDF group state was accepted";
    } catch (const ModelSourceError& error) {
      EXPECT_NE(std::string(error.what()).find(
                    "cannot assign a non-scalar"),
                std::string::npos);
    }
  }
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
  ASSERT_TRUE(verified.source_ir().srdf().has_value());
  EXPECT_EQ(verified.source_ir().srdf()->groups.size(), 5U);
  const auto& index = verified.source_ir().semantic_index();
  const auto diamond = std::find_if(
      index.expanded_groups.begin(), index.expanded_groups.end(),
      [](const ExpandedSrdfGroupIr& group) {
        return group.group_name == "diamond";
      });
  ASSERT_NE(diamond, index.expanded_groups.end());
  ASSERT_EQ(diamond->joints.size(), 1U);
  EXPECT_EQ(diamond->joints.front().name, "joint1");
  ASSERT_EQ(diamond->joints.front().provenance_indices.size(), 2U);
  for (const auto provenance_index :
       diamond->joints.front().provenance_indices) {
    ASSERT_LT(provenance_index,
              index.group_expansion_provenance.size());
    const auto& diamond_node =
        index.group_expansion_provenance[provenance_index];
    EXPECT_EQ(diamond_node.expansion_group_name, "diamond");
    EXPECT_EQ(diamond_node.kind,
              SrdfExpansionProvenanceKind::kSubgroup);
    ASSERT_TRUE(diamond_node.nested_provenance_index.has_value());
    const auto& side_node = index.group_expansion_provenance.at(
        *diamond_node.nested_provenance_index);
    EXPECT_TRUE(side_node.expansion_group_name == "left" ||
                side_node.expansion_group_name == "right");
    ASSERT_TRUE(side_node.nested_provenance_index.has_value());
    const auto& leaf_node = index.group_expansion_provenance.at(
        *side_node.nested_provenance_index);
    EXPECT_EQ(leaf_node.expansion_group_name, "leaf");
    EXPECT_EQ(leaf_node.kind,
              SrdfExpansionProvenanceKind::kDirectJoint);
    EXPECT_EQ(leaf_node.source_symbol_name, "joint1");
  }
}

TEST(ModelSource, EnforcesExactAggregateExpandedSemanticRowBound) {
  Fixture exact;
  blob_for(exact, *exact.srdf_uri).bytes =
      srdf_for_exact_expansion_budget(
          kMaxModelSourceExpandedSrdfSemanticRows);
  const auto accepted = verify(exact);
  const auto& index = accepted.source_ir().semantic_index();
  std::size_t retained_rows =
      index.group_expansion_provenance.size();
  std::size_t provenance_index_rows = 0U;
  for (const auto& group : index.expanded_groups) {
    retained_rows += group.links.size() + group.joints.size();
    for (const auto& member : group.links) {
      provenance_index_rows += member.provenance_indices.size();
    }
    for (const auto& member : group.joints) {
      provenance_index_rows += member.provenance_indices.size();
    }
  }
  EXPECT_EQ(retained_rows,
            kMaxModelSourceExpandedSrdfSemanticRows);
  // Each retained provenance occurrence has exactly one member index. The
  // aggregate budget counts that occurrence once, not twice.
  EXPECT_EQ(provenance_index_rows,
            index.group_expansion_provenance.size());

  Fixture over;
  blob_for(over, *over.srdf_uri).bytes =
      srdf_for_exact_expansion_budget(
          kMaxModelSourceExpandedSrdfSemanticRows + 1U);
  try {
    (void)verify(over);
    FAIL() << "expanded SRDF semantic row bound was not enforced";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find("aggregate row bound"),
              std::string::npos);
  }
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
  ASSERT_TRUE(verified.source_ir().srdf().has_value());
  EXPECT_EQ(verified.source_ir().srdf()->groups.size(),
            kGroupCount + 1U);
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
      "<joint name=\"joint2\" type=\"revolute\">"
      "<parent link=\"tool0\"/><child link=\"tool1\"/>"
      "<limit effort=\"10\" velocity=\"1\"/>"
      "<mimic joint=\"joint1\"/></joint>");
  try {
    (void)verify(mimic);
    FAIL() << "mimic cycle was not rejected";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find(
                  "mimic relation contains a cycle"),
              std::string::npos)
        << error.what();
  }

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
  EXPECT_EQ(verified.source_ir().artifact_references(),
            (std::vector<SourceArtifactReferenceIr>{
                {long_uri, SourceArtifactReferenceKind::kMesh}}));
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

  Fixture collada;
  auto& collada_urdf = blob_for(collada, collada.urdf_uri);
  replace_once(collada_urdf.bytes,
               "asset://test_robot/meshes/base.stl",
               "asset://test_robot/meshes/base.dae");
  auto& collada_blob =
      blob_for(collada, "asset://test_robot/meshes/base.stl");
  collada_blob.logical_uri =
      "asset://test_robot/meshes/base.dae";
  collada_blob.media_type = "model/vnd.collada+xml";
  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\"/>";

  const auto collada_plan =
      preflight_model_source_declaration(qualify(collada));
  const auto collada_row = std::find_if(
      collada_plan.artifacts().begin(),
      collada_plan.artifacts().end(),
      [](const auto& artifact) {
        return artifact.logical_uri ==
               "asset://test_robot/meshes/base.dae";
      });
  ASSERT_NE(collada_row, collada_plan.artifacts().end());
  EXPECT_EQ(collada_row->artifact_kind,
            ModelSourceArtifactKind::kColladaMesh);
  EXPECT_NO_THROW((void)verify_model_source_closure(
      collada_plan, candidates(collada)));

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\">"
      "<library_images><image id=\"texture\"><init_from>"
      "file:///tmp/undeclared.png"
      "</init_from></image></library_images></COLLADA>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\">"
      "<library_geometries><instance_geometry "
      "url=\"other.dae#mesh\"/></library_geometries></COLLADA>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\">"
      "<instance_material symbol=\"material\" "
      "target=\"other.dae#material\"/></COLLADA>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\">"
      "<skeleton>other.dae#root</skeleton></COLLADA>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\">"
      "<render camera_node=\"other.dae#camera\"/></COLLADA>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" xmlns:xml=\"http://www.w3.org/XML/1998/"
      "namespace\" xml:base=\"https://example.invalid/\" "
      "version=\"1.4.1\"/>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\"><library_geometries></COLLADA>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2008/03/"
      "COLLADASchema\" version=\"1.5.0\"/>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.0\"/>";
  EXPECT_THROW(
      verify_model_source_closure(
          preflight_model_source_declaration(qualify(collada)),
          candidates(collada)),
      ModelSourceError);

  collada_blob.media_type = "application/xml";
  EXPECT_THROW(
      preflight_model_source_declaration(qualify(collada)),
      ModelSourceError);
}

TEST(ModelSource, GivesColladaAnIndependentAggregateXmlBudget) {
  Fixture fixture;
  auto& urdf = blob_for(fixture, fixture.urdf_uri);
  replace_once(urdf.bytes, "asset://test_robot/meshes/base.stl",
               "asset://test_robot/meshes/base.dae");
  auto& collada =
      blob_for(fixture, "asset://test_robot/meshes/base.stl");
  collada.logical_uri = "asset://test_robot/meshes/base.dae";
  collada.media_type = "model/vnd.collada+xml";

  std::string bytes =
      "<COLLADA xmlns=\"http://www.collada.org/2005/11/"
      "COLLADASchema\" version=\"1.4.1\"><library_visual_scenes>"
      "<visual_scene>";
  constexpr std::size_t kWrapperElementCount = 3U;
  for (std::size_t index = kWrapperElementCount;
       index < kMaxModelSourceColladaXmlElementCount; ++index) {
    (void)index;
    bytes.append("<node/>");
  }
  bytes.append(
      "</visual_scene></library_visual_scenes></COLLADA>");
  collada.bytes = std::move(bytes);

  // This reaches the Collada ceiling, then still parses the URDF/SRDF under
  // their independent aggregate budget.
  EXPECT_NO_THROW((void)verify(fixture));

  const auto close = collada.bytes.find("</visual_scene>");
  ASSERT_NE(close, std::string::npos);
  collada.bytes.insert(close, "<node/>");
  try {
    (void)verify(fixture);
    FAIL() << "Collada first-over-bound document was accepted";
  } catch (const ModelSourceError& error) {
    EXPECT_NE(std::string(error.what()).find("element bound"),
              std::string::npos)
        << error.what();
  }
}

TEST(ModelSource, EnforcesOneCombinedUrdfSrdfElementBudget) {
  Fixture fixture;
  auto& urdf = blob_for(fixture, fixture.urdf_uri);
  const auto first_link_end = urdf.bytes.find("</link>");
  ASSERT_NE(first_link_end, std::string::npos);
  std::string urdf_padding;
  urdf_padding.reserve(4000000U);
  // Three individually valid elements per occurrence keep one connected
  // URDF while consuming the same near-half aggregate XML budget.
  for (std::size_t index = 0U; index < 41663U; ++index) {
    (void)index;
    urdf_padding.append(
        "<visual><geometry><sphere radius=\"1\"/></geometry></visual>");
  }
  urdf.bytes.insert(first_link_end, urdf_padding);

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
              std::string::npos)
        << error.what();
  }
}

}  // namespace
}  // namespace manyforge::deployment
