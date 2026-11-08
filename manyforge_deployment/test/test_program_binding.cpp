#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/initial_scene_projection.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "lite6_retained_consumer_fixture.hpp"
#include "io_retained_fixture.hpp"
#include "program_binding_internal.hpp"
#include "scoped_procfd_root.hpp"

namespace {

using manyforge::core::CapabilityDefinitionRevision;
using manyforge::core::CapabilityId;
using manyforge::core::CollisionBodyRoleId;
using manyforge::core::ComponentRoleId;
using manyforge::core::ContentDigest;
using manyforge::core::FrameRoleId;
using manyforge::core::ProgramBindingDTO;
using manyforge::core::ProgramBindingId;
using manyforge::core::ProgramBindingProvenanceDTO;
using manyforge::core::ProgramCapabilityBindingDTO;
using manyforge::core::ProgramComponentBindingDTO;
using manyforge::core::ProgramControlBindingRefDTO;
using manyforge::core::ProgramFrameBindingDTO;
using manyforge::core::ProgramId;
using manyforge::core::ProgramModelRoleBindingDTO;
using manyforge::core::ProgramResourceRoleBindingDTO;
using manyforge::core::ProgramRevision;
using manyforge::core::ProgramTargetBindingDTO;
using manyforge::core::ResourceId;
using manyforge::core::ResourceRoleId;
using manyforge::core::TargetRoleId;
using manyforge::deployment::CompositionComponentModelFrame;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::FilesystemProgramBindingRepository;
using manyforge::deployment::FilesystemVerifiedProgramBindingSource;
using manyforge::deployment::FilesystemVerifiedCatalogArtifactSource;
using manyforge::deployment::FilesystemVerifiedRetainedBundleSource;
using manyforge::deployment::FilesystemVerifiedRetainedModelMapSource;
using manyforge::deployment::ProgramBindingError;
using manyforge::deployment::RetainedModelMapQuery;
using manyforge::deployment::VerifiedRetainedModelMap;
using manyforge::deployment::canonical_json;
using manyforge::deployment::detail::ProgramBindingConstructionAuthority;
using manyforge::deployment::reload_verified_program_binding;
using manyforge::deployment::sha256_digest;
using manyforge::deployment::test::materialize_lite6_retained_consumer_fixture;
using manyforge::deployment::test::
    materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

class ScopedTrustedRoot final {
 public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-program-binding-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

struct BindingContext final {
  std::shared_ptr<const manyforge::deployment::RetainedBundle> bundle;
  VerifiedRetainedModelMap model_map;
  std::filesystem::path catalog_root;

  [[nodiscard]] FilesystemVerifiedCatalogArtifactSource catalog_source() const {
    return FilesystemVerifiedCatalogArtifactSource(catalog_root);
  }
};

[[nodiscard]] BindingContext make_context(
    const std::filesystem::path& root, bool tcp_contact = false) {
  const auto materialized = tcp_contact
      ? manyforge::deployment::test::materialize_lite6_retained_consumer_fixture_from(
            root, manyforge::deployment::test::make_lite6_ros2_control_shared_consumer_retention_fixture(
                      {}, 1U, false, true))
      : materialize_lite6_retained_consumer_fixture(root);
  const FilesystemVerifiedRetainedBundleSource bundle_source(root);
  auto bundle = bundle_source.get_verified(
      materialized.bundle->retained_bundle_id());
  if (bundle == nullptr ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    throw std::logic_error("test retained bundle has no sole model map");
  }
  const auto& product =
      bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource model_map_source(root);
  auto model_map = model_map_source.get_verified(
      RetainedModelMapQuery{bundle->retained_bundle_id(), product.product_id});
  if (!model_map.has_value()) {
    throw std::logic_error("test could not reload retained model map");
  }
  return {std::move(bundle), std::move(*model_map), root};
}

[[nodiscard]] BindingContext make_other_cell_context(
    const std::filesystem::path& root) {
  // Keep the alternate context materializable through the same qualified
  // runtime family as make_context; its other cell still changes identity.
  const auto materialized =
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          root, "program_binding_other_cell");
  const FilesystemVerifiedRetainedBundleSource bundle_source(root);
  auto bundle = bundle_source.get_verified(
      materialized.bundle->retained_bundle_id());
  if (bundle == nullptr ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    throw std::logic_error(
        "test other-cell retained bundle has no sole model map");
  }
  const auto& product =
      bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource model_map_source(root);
  auto model_map = model_map_source.get_verified(
      RetainedModelMapQuery{bundle->retained_bundle_id(), product.product_id});
  if (!model_map.has_value()) {
    throw std::logic_error(
        "test could not reload other-cell retained model map");
  }
  return {std::move(bundle), std::move(*model_map), root};
}

[[nodiscard]] ProgramBindingDTO make_binding_value(
    const BindingContext& context) {
  const auto& bundle = context.bundle->value();
  const auto& graph = bundle.cell_graph;
  if (graph.controllable_components.size() != 1U ||
      graph.model_instances.size() != 1U ||
      graph.control_bindings.size() != 1U) {
    throw std::logic_error("test requires the sole Lite6 binding fixture");
  }
  const auto& component = graph.controllable_components.front();
  const auto& model = graph.model_instances.front();
  const auto& control = graph.control_bindings.front();
  const auto* target = context.model_map.find_target(
      component.controllable_component_id, control.command_target_id);
  const auto* coordinate_map = context.model_map.find_binding(
      component.controllable_component_id, control.command_target_id);
  if (target == nullptr || coordinate_map == nullptr ||
      target->collision_bodies.size() != 1U) {
    throw std::logic_error("test model map has an unexpected target shape");
  }
  const auto tcp_resolution = std::find_if(
      target->frames.begin(), target->frames.end(), [](const auto& candidate) {
        return candidate.frame_role_id == FrameRoleId("tcp");
      });
  if (tcp_resolution == target->frames.end()) {
    throw std::logic_error("test model map has no TCP frame");
  }
  const auto* frame = std::get_if<CompositionComponentModelFrame>(
      &tcp_resolution->frame);
  if (frame == nullptr) {
    throw std::logic_error("test target frame is not component-owned");
  }

  std::vector<ResourceId> resources;
  std::vector<std::string> compiled_names;
  for (const auto& row : coordinate_map->resources) {
    if (row.resource_role_id != ResourceRoleId("joints") ||
        row.coordinates.empty()) {
      throw std::logic_error("test coordinate map is malformed");
    }
    resources.push_back(row.resource_id);
    compiled_names.push_back(row.coordinates.front().compiled_joint_name);
  }

  const auto frame_ref = manyforge::core::ProgramFrameReferenceDTO{component.controllable_component_id, FrameRoleId("tcp")};
  const auto body_ref = manyforge::core::ProgramCollisionBodyReferenceDTO{component.controllable_component_id, CollisionBodyRoleId("arm_collision")};
  const auto target_ref = manyforge::core::ProgramTargetReferenceDTO{component.controllable_component_id, control.command_target_id};
  // A motion binding carries the exact TCP inventory derived from the same
  // retained map and qualified catalogs used by contextual verification.
  const auto direct = manyforge::deployment::compile_direct_control_motion_program_binding(
      component.controllable_component_id, control.command_target_id,
      *context.bundle, context.model_map, context.catalog_source());
  if (!direct)
    throw std::logic_error("test motion target did not compile");
  const auto compiled_target = std::find_if(
      direct->value().target_bindings.begin(), direct->value().target_bindings.end(),
      [&](const auto& row) { return row.target == target_ref; });
  if (compiled_target == direct->value().target_bindings.end() || !compiled_target->robot_tcps)
    throw std::logic_error("test motion target has no exact compiled TCP inventory");
  const auto robot_tcps = compiled_target->robot_tcps;
  std::optional<manyforge::core::TcpEntryReferenceDTO> default_tcp;
  if (const auto* value = context.model_map.kinematic_index().default_tcp_of(component.controllable_component_id)) {
    const auto* default_target = context.model_map.find_target(component.controllable_component_id, value->command_target_id);
    const auto* default_row = default_target ? manyforge::deployment::find_target_frame_role(*default_target, value->frame_role_id) : nullptr;
    const auto* default_frame = default_row ? std::get_if<CompositionComponentModelFrame>(&default_row->frame) : nullptr;
    if (!default_frame) throw std::logic_error("test default TCP does not resolve");
    default_tcp = manyforge::core::TcpEntryReferenceDTO{{component.controllable_component_id, value->command_target_id},
        {default_frame->controllable_component_id, default_frame->frame_role_id}};
  }
  return ProgramBindingDTO{
      std::string(manyforge::core::kProgramBindingApiVersion),
      ProgramBindingId(std::string(kZeroDigest)),
      ProgramId("binding_test_program"),
      ProgramRevision(sha256_digest("i6 test program revision")),
      context.bundle->retained_bundle_id(), graph.graph_revision,
      bundle.catalogs.node_environment,
      {ProgramComponentBindingDTO{component.controllable_component_id,
          component.component_type_id, component.component_type_revision,
          {ProgramModelRoleBindingDTO{component.model_role_bindings.front().model_role_id,
                                      model.model_instance_id, model.model_revision}}}},
      {ProgramTargetBindingDTO{target_ref, {frame_ref}, {body_ref}, {},
          {ProgramResourceRoleBindingDTO{ResourceRoleId("joints"), std::move(resources), std::move(compiled_names)}},
          {ProgramCapabilityBindingDTO{CapabilityId(control.capability_id),
              CapabilityDefinitionRevision(control.capability_definition_revision),
              ProgramControlBindingRefDTO{control.control_binding_id, control.control_binding_revision},
              std::nullopt, std::nullopt}}, std::nullopt, robot_tcps, default_tcp,
          {{frame_ref, {}}}}},
      {ProgramFrameBindingDTO{frame_ref, frame->model_instance_id,
                              frame->model_revision, frame->compiled_frame_name}},
      {},
      {manyforge::core::ProgramCollisionBodyBindingDTO{body_ref,
          target->collision_bodies.front().model_instance_id, model.model_revision,
          target->collision_bodies.front().compiled_link_names}},
      ProgramBindingProvenanceDTO{bundle.provenance.compiler_contract_digest,
          bundle.provenance.source_records, bundle.provenance.source_records},
  };
}

[[nodiscard]] ProgramBindingDTO make_policy_binding_value(
    const BindingContext& context) {
  auto value = make_binding_value(context);
  value.node_policies.push_back(manyforge::core::ProgramNodePolicyDTO{
      manyforge::core::ProgramNodeInstanceId("root"),
      manyforge::core::ProgramNodeRecoveryPolicyDTO{true, 1U},
      manyforge::core::ProgramOperatorStopPolicy::kHold});
  return value;
}

[[nodiscard]] std::string replace_once(std::string value,
                                       std::string_view from,
                                       std::string_view to) {
  const auto position = value.find(from);
  if (position == std::string::npos) {
    throw std::logic_error("test could not find raw binding mutation");
  }
  value.replace(position, from.size(), to);
  return value;
}

[[nodiscard]] std::string policy_row(const std::string& bytes) {
  constexpr std::string_view prefix = "\"node_policies\":[";
  constexpr std::string_view suffix = "],\"program_binding_id\"";
  const auto begin = bytes.find(prefix);
  const auto end = bytes.find(suffix, begin + prefix.size());
  if (begin == std::string::npos || end == std::string::npos) {
    throw std::logic_error("test binding omitted node-policy table");
  }
  return bytes.substr(begin + prefix.size(), end - begin - prefix.size());
}

[[nodiscard]] std::string replace_policy_table(
    const std::string& bytes, const std::string& table) {
  constexpr std::string_view prefix = "\"node_policies\":[";
  constexpr std::string_view suffix = "],\"program_binding_id\"";
  const auto begin = bytes.find(prefix);
  const auto end = bytes.find(suffix, begin + prefix.size());
  if (begin == std::string::npos || end == std::string::npos) {
    throw std::logic_error("test binding omitted node-policy table");
  }
  std::string result = bytes;
  result.replace(begin + prefix.size(), end - begin - prefix.size(), table);
  return result;
}

TEST(ProgramBindingTest,
     InitialSceneProjectionKeepsCellAndExactCandidateComponentFrames) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  const auto binding = make_binding_value(context);
  const auto &component = binding.component_bindings.front();
  const CanonicalValue scene(CanonicalValue::Object{
      {"scene",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"scene_resource_id", CanonicalValue("fixture")},
               {"shape", CanonicalValue(CanonicalValue::Object{
                             {"type", CanonicalValue("box")},
                             {"box_dimensions_m",
                              CanonicalValue(CanonicalValue::Array{
                                  CanonicalValue(0.1), CanonicalValue(0.2),
                                  CanonicalValue(0.3)})}})},
               {"pose", CanonicalValue(CanonicalValue::Object{
                            {"frame_ref",
                             CanonicalValue(CanonicalValue::Object{
                                 {"kind", CanonicalValue("component_frame")},
                                 {"frame", CanonicalValue(CanonicalValue::Object{
                                     {"component_id", CanonicalValue(component.component_id.value())},
                                     {"frame_id", CanonicalValue("tcp")}})}})},
                            {"position_m",
                             CanonicalValue(CanonicalValue::Array{
                                 CanonicalValue(0.0), CanonicalValue(0.0),
                                 CanonicalValue(0.0)})},
                            {"orientation",
                             CanonicalValue(CanonicalValue::Object{
                                 {"rpy_deg",
                                  CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(0.0),
                                      CanonicalValue(0.0),
                                      CanonicalValue(90.0)})}})}})},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"scene_resource_id", CanonicalValue("ground")},
               {"shape", CanonicalValue(CanonicalValue::Object{
                             {"type", CanonicalValue("sphere")},
                             {"sphere_radius_m", CanonicalValue(0.5)}})},
               {"pose", CanonicalValue(CanonicalValue::Object{
                            {"frame_ref", CanonicalValue(
                                              CanonicalValue::Object{{"kind",
                                                                      CanonicalValue("cell_frame")}})},
                            {"position_m",
                             CanonicalValue(CanonicalValue::Array{
                                 CanonicalValue(1.0), CanonicalValue(2.0),
                                 CanonicalValue(3.0)})},
                            {"orientation",
                             CanonicalValue(CanonicalValue::Object{
                                 {"xyzw",
                                  CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(0.0),
                                      CanonicalValue(0.0),
                                      CanonicalValue(0.0),
                                      CanonicalValue(1.0)})}})}})},
           }),
       })},
  });
  const auto objects =
      manyforge::deployment::detail::project_bound_initial_scene_value(
          scene, binding, context.bundle->value().cell_graph.cell_id);
  ASSERT_EQ(objects.size(), 2U);
  const auto *frame = std::get_if<manyforge::core::ComponentSceneFrameRefDTO>(
      &objects.front().frame_ref);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->controllable_component_id,
            component.component_id);
  EXPECT_EQ(frame->model_instance_id,
            component.model_bindings.front().model_instance_id);
  EXPECT_NEAR(objects.front().pose.orientation.z, 0.70710678118, 1e-9);
  EXPECT_TRUE(std::holds_alternative<manyforge::core::CellSceneFrameRefDTO>(
      objects.back().frame_ref));
  EXPECT_EQ(objects.back().pose.position_m.x_m, 1.0);
}

TEST(ProgramBindingTest, InitialSceneProjectionRejectsMissingComponentFrame) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  const auto binding = make_binding_value(context);
  const CanonicalValue scene(CanonicalValue::Object{
      {"scene", CanonicalValue(CanonicalValue::Array{
                    CanonicalValue(CanonicalValue::Object{
                        {"scene_resource_id", CanonicalValue("object")},
                        {"shape", CanonicalValue(CanonicalValue::Object{
                                      {"type", CanonicalValue("sphere")},
                                      {"sphere_radius_m", CanonicalValue(0.1)}})},
                        {"pose", CanonicalValue(CanonicalValue::Object{
                                     {"frame_ref", CanonicalValue(CanonicalValue::Object{
                                                       {"kind", CanonicalValue("component_frame")},
                                                       {"frame", CanonicalValue(CanonicalValue::Object{
                                                           {"component_id", CanonicalValue("missing")},
                                                           {"frame_id", CanonicalValue("tcp")}})}})},
                                     {"position_m", CanonicalValue(CanonicalValue::Array{
                                                        CanonicalValue(0.0), CanonicalValue(0.0),
                                                        CanonicalValue(0.0)})},
                                     {"orientation", CanonicalValue(CanonicalValue::Object{
                                                         {"xyzw", CanonicalValue(CanonicalValue::Array{
                                                                      CanonicalValue(0.0), CanonicalValue(0.0),
                                                                      CanonicalValue(0.0), CanonicalValue(1.0)})}})}})},
                    })})},
  });
  EXPECT_THROW(static_cast<void>(
                   manyforge::deployment::detail::project_bound_initial_scene_value(
                       scene, binding, context.bundle->value().cell_graph.cell_id)),
               ProgramBindingError);
}

TEST(ProgramBindingTest, InitialSceneProjectionParsesOptionalMetadataMass) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  const auto binding = make_binding_value(context);
  const auto scene_with = [](std::optional<CanonicalValue> metadata) {
    CanonicalValue::Object row{
        {"scene_resource_id", CanonicalValue("object")},
        {"shape", CanonicalValue(CanonicalValue::Object{
                      {"type", CanonicalValue("sphere")},
                      {"sphere_radius_m", CanonicalValue(0.1)}})},
        {"pose", CanonicalValue(CanonicalValue::Object{
                     {"frame_ref", CanonicalValue(CanonicalValue::Object{
                                       {"kind", CanonicalValue("cell_frame")}})},
                     {"position_m", CanonicalValue(CanonicalValue::Array{
                                        CanonicalValue(0.0), CanonicalValue(0.0),
                                        CanonicalValue(0.0)})},
                     {"orientation", CanonicalValue(CanonicalValue::Object{
                                         {"xyzw", CanonicalValue(CanonicalValue::Array{
                                                      CanonicalValue(0.0), CanonicalValue(0.0),
                                                      CanonicalValue(0.0), CanonicalValue(1.0)})}})}})},
    };
    if (metadata) {
      row.emplace_back("metadata", std::move(*metadata));
    }
    return CanonicalValue(CanonicalValue::Object{
        {"scene", CanonicalValue(CanonicalValue::Array{
                      CanonicalValue(std::move(row))})}});
  };
  const auto project = [&](const CanonicalValue &scene) {
    return manyforge::deployment::detail::project_bound_initial_scene_value(
        scene, binding, context.bundle->value().cell_graph.cell_id);
  };
  const auto mass = [](CanonicalValue value) {
    return CanonicalValue(
        CanonicalValue::Object{{"mass_kg", std::move(value)}});
  };

  // Absent metadata or an absent key is massless.
  EXPECT_DOUBLE_EQ(project(scene_with(std::nullopt)).front().mass_kg, 0.0);
  EXPECT_DOUBLE_EQ(
      project(scene_with(CanonicalValue(CanonicalValue::Object{})))
          .front()
          .mass_kg,
      0.0);
  EXPECT_DOUBLE_EQ(project(scene_with(mass(CanonicalValue(2.5)))).front().mass_kg,
                   2.5);
  EXPECT_DOUBLE_EQ(project(scene_with(mass(CanonicalValue(0.0)))).front().mass_kg,
                   0.0);

  for (auto invalid : {CanonicalValue(-1.0), CanonicalValue(1000.5),
                       CanonicalValue("2.5"), CanonicalValue(nullptr)}) {
    EXPECT_THROW(static_cast<void>(project(scene_with(mass(std::move(invalid))))),
                 ProgramBindingError);
  }
}

TEST(ProgramBindingTest,
     SealedEnvelopeReloadsOnlyAgainstItsExactBundleAndModelMap) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));

  ASSERT_NE(sealed, nullptr);
  EXPECT_NE(sealed->program_binding_id().value(), kZeroDigest);
  EXPECT_TRUE(manyforge::deployment::verify_program_binding_identity(*sealed));
  EXPECT_EQ(sealed->canonical_bytes(), canonical_json(sealed->normalized_root()));

  const auto reloaded = reload_verified_program_binding(
      sealed->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->program_binding_id(), sealed->program_binding_id());
  EXPECT_EQ(reloaded->value(), sealed->value());
}

TEST(ProgramBindingTest, RobotTcpPresenceSurvivesCanonicalSealAndReload) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  auto &target = value.target_bindings.front();
  // The exact compiled TCP inventory of this motion target. A motion target
  // must carry it, so presence is the exact inventory, never an empty list.
  const auto exact_tcps = target.robot_tcps;
  ASSERT_TRUE(exact_tcps && !exact_tcps->empty());
  target.robot_tcps.reset();
  const auto absent = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(absent->canonical_bytes().find("\"robot_tcps\""), std::string::npos);
  // Structural round-trip only: this fixture is an arm, not a non-robot.
  const auto absent_reloaded =
      ProgramBindingConstructionAuthority::reload_intrinsic(absent->canonical_bytes());
  EXPECT_FALSE(absent_reloaded->value().target_bindings.front().robot_tcps);

  target.robot_tcps = exact_tcps;
  const auto present = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(present->canonical_bytes().find("\"robot_tcps\":["), std::string::npos);
  EXPECT_EQ(present->canonical_bytes().find("\"robot_tcps\":[]"), std::string::npos);
  EXPECT_NE(present->program_binding_id(), absent->program_binding_id());
  const auto present_reloaded = reload_verified_program_binding(
      present->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  ASSERT_TRUE(present_reloaded->value().target_bindings.front().robot_tcps);
  EXPECT_EQ(*present_reloaded->value().target_bindings.front().robot_tcps, *exact_tcps);
  EXPECT_EQ(present_reloaded->value(), present->value());
}

TEST(ProgramBindingTest, MotionEndpointReleaseBindingRoundTripsExactEnvelope) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  manyforge::core::ContactReleaseEnvelopeDTO release;
  release.max_start_penetration_m = 0.002;
  value.contact_grasp_bindings.push_back({
      manyforge::core::ContactPolicyId("endpoint_release"),
      manyforge::core::ContactPolicyRevision(std::string(kZeroDigest)),
      manyforge::core::RuntimeObjectId("selected_object"), {}, release, true});
  const auto sealed = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(sealed->canonical_bytes().find("\"mode\":\"contact_release\""),
            std::string::npos);
  EXPECT_EQ(sealed->canonical_bytes().find("terminal_window_start_fraction"),
            std::string::npos);
  const auto loaded = reload_verified_program_binding(
      sealed->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  EXPECT_EQ(loaded->value(), sealed->value());
  EXPECT_EQ(std::get<manyforge::core::ContactReleaseEnvelopeDTO>(
                loaded->value().contact_grasp_bindings.front().envelope), release);
}

TEST(ProgramBindingTest,
     MotionEndpointContactBindingRoundTripsWithoutExplicitBodies) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  value.contact_grasp_bindings.push_back(
      manyforge::core::ProgramContactGraspBindingDTO{
          manyforge::core::ContactPolicyId("endpoint_contact"),
          manyforge::core::ContactPolicyRevision(std::string(kZeroDigest)),
          manyforge::core::RuntimeObjectId("selected_object"),
          {},
          manyforge::core::ContactGraspEnvelopeDTO{0.002, 0.8},
          true});
  const auto sealed = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(
      sealed->canonical_bytes().find("\"bodies_from_motion_endpoint\":true"),
      std::string::npos);
  const auto loaded = reload_verified_program_binding(
      sealed->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  EXPECT_EQ(loaded->value(), sealed->value());
  value.contact_grasp_bindings.front().bodies_from_motion_endpoint = false;
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
  const auto &body = value.collision_body_bindings.front();
  std::vector<std::string> ids;
  for (std::size_t i = 0U; i < body.compiled_collision_body_names.size(); ++i) {
    ids.push_back(manyforge::core::make_component_collision_body_id(
        body.reference.component_id, body.model_instance_id,
        body.reference.collision_body_id, i));
  }
  value.contact_grasp_bindings.front().allowed_body_bindings.push_back(
      {body.reference, std::nullopt, body.model_instance_id, body.model_revision,
       std::move(ids)});
  const auto legacy = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(legacy->canonical_bytes().find("bodies_from_motion_endpoint"),
            std::string::npos);
  EXPECT_EQ(reload_verified_program_binding(legacy->canonical_bytes(),
                                            *context.bundle, context.model_map, context.catalog_source())
                ->value(),
            legacy->value());
  value.contact_grasp_bindings.front().bodies_from_motion_endpoint = true;
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
}

TEST(ProgramBindingTest,
     EndpointContactBodyAssociationsAreOptionalAndFrameScoped) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
  auto value = make_binding_value(context);
  const auto legacy = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(legacy->canonical_bytes().find("\"contact_body_role_ids\":[]"),
            std::string::npos);

  value.target_bindings.front().frame_associations.front().contact_body_role_ids = {
      CollisionBodyRoleId("arm_collision")};
  const auto first_endpoint = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(first_endpoint->canonical_bytes().find(
                "\"contact_body_role_ids\":[\"arm_collision\"]"),
            std::string::npos);
  const auto reopened = reload_verified_program_binding(
      first_endpoint->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  EXPECT_EQ(reopened->value(), first_endpoint->value());

  auto second_frame = value.frame_bindings.front();
  second_frame.reference.frame_id = manyforge::core::FrameRoleId("sibling_tcp");
  second_frame.compiled_frame_name += "_sibling";
  const auto second_ref = second_frame.reference;
  value.frame_bindings.push_back(std::move(second_frame));
  value.target_bindings.front().frame_references.push_back(second_ref);
  value.target_bindings.front().frame_associations.push_back(
      {second_ref, {CollisionBodyRoleId("sibling_collision")}});
  auto second_body = value.collision_body_bindings.front();
  second_body.reference.collision_body_id = CollisionBodyRoleId("sibling_collision");
  second_body.compiled_collision_body_names = {"sibling_left", "sibling_right"};
  value.collision_body_bindings.push_back(std::move(second_body));
  const auto two_endpoints = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(two_endpoints->canonical_bytes().find("\"frame_id\":\"tcp\""),
            std::string::npos);
  EXPECT_NE(two_endpoints->canonical_bytes().find(
                "\"frame_id\":\"sibling_tcp\""),
            std::string::npos);
  EXPECT_NE(two_endpoints->canonical_bytes().find(
                "\"contact_body_role_ids\":[\"sibling_collision\"]"),
            std::string::npos);

  value.collision_body_bindings.back().model_revision =
      manyforge::core::ModelRevision(std::string(kZeroDigest));
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
}

TEST(ProgramBindingTest, NodePoliciesAreRequiredSparseAndIdentitySensitive) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  ProgramBindingDTO value = make_binding_value(context);
  value.node_policies.push_back(manyforge::core::ProgramNodePolicyDTO{
      manyforge::core::ProgramNodeInstanceId("root"),
      manyforge::core::ProgramNodeRecoveryPolicyDTO{true, 1U},
      manyforge::core::ProgramOperatorStopPolicy::kHold});
  const auto sealed = ProgramBindingConstructionAuthority::seal(value);

  ASSERT_NE(sealed, nullptr);
  ASSERT_EQ(sealed->value().node_policies.size(), 1U);
  EXPECT_EQ(sealed->value().node_policies.front().instance_id.value(), "root");
  const auto reloaded = reload_verified_program_binding(
      sealed->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), sealed->value());

  value.node_policies.front().operator_stop_policy =
      manyforge::core::ProgramOperatorStopPolicy::kContinue;
  const auto changed = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(changed->program_binding_id(), sealed->program_binding_id());
}

TEST(ProgramBindingTest, NodePolicyWireAndCrossFieldNegativesFailClosed) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());

  ProgramBindingDTO default_row = make_binding_value(context);
  default_row.node_policies.push_back(manyforge::core::ProgramNodePolicyDTO{
      manyforge::core::ProgramNodeInstanceId("root"),
      {},
      manyforge::core::ProgramOperatorStopPolicy::kInherit});
  EXPECT_THROW(
      static_cast<void>(
          ProgramBindingConstructionAuthority::seal(std::move(default_row))),
      ProgramBindingError);

  // Zero is Unlimited: an intervention-allowing row with a zero budget is a
  // real non-default policy, never a default to omit.
  ProgramBindingDTO unlimited = make_binding_value(context);
  unlimited.node_policies.push_back(manyforge::core::ProgramNodePolicyDTO{
      manyforge::core::ProgramNodeInstanceId("root"),
      manyforge::core::ProgramNodeRecoveryPolicyDTO{true, 0U},
      manyforge::core::ProgramOperatorStopPolicy::kInherit});
  const auto sealed_unlimited =
      ProgramBindingConstructionAuthority::seal(std::move(unlimited));
  ASSERT_NE(sealed_unlimited, nullptr);
  EXPECT_NE(sealed_unlimited->canonical_bytes().find(
                "\"recovery_policy\":{\"allow_intervention\":true,"
                "\"max_retries\":0}"),
            std::string::npos);

  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));
  std::string missing_node_policies(sealed->canonical_bytes());
  const std::string field = "\"node_policies\":[],";
  const std::size_t position = missing_node_policies.find(field);
  ASSERT_NE(position, std::string::npos);
  missing_node_policies.erase(position, field.size());
  EXPECT_THROW(
      static_cast<void>(reload_verified_program_binding(
          missing_node_policies, *context.bundle, context.model_map, context.catalog_source())),
      ProgramBindingError);
}

TEST(ProgramBindingTest, RawNodePolicyReloadRejectsEveryBoundedNegativeFamily) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed = ProgramBindingConstructionAuthority::seal(
      make_policy_binding_value(context));
  const std::string valid(sealed->canonical_bytes());
  const std::string row = policy_row(valid);
  const auto expect_rejected = [&](const std::string& bytes) {
    EXPECT_THROW(
        static_cast<void>(reload_verified_program_binding(
            bytes, *context.bundle, context.model_map, context.catalog_source())),
        ProgramBindingError);
  };

  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"max_retries\":1}}",
                          "\"max_retries\":1},\"zzz\":true}")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"operator_stop_policy\":\"hold\",", "")));
  expect_rejected(replace_policy_table(
      valid,
      replace_once(row,
                   "\"recovery_policy\":{\"allow_intervention\":true,"
                   "\"max_retries\":1}",
                   "\"recovery_policy\":null")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, ",\"max_retries\":1", "")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"allow_intervention\":true",
                          "\"allow_intervention\":1")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"max_retries\":1", "\"max_retries\":true")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"max_retries\":1", "\"max_retries\":256")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"max_retries\":1", "\"max_retries\":-1")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"max_retries\":1", "\"max_retries\":1.5")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"instance_id\":\"root\"",
                          "\"instance_id\":\"Root\"")));
  expect_rejected(replace_policy_table(
      valid,
      replace_once(row, "\"operator_stop_policy\":\"hold\"",
                   "\"operator_stop_policy\":\"pause\"")));
  expect_rejected(replace_policy_table(
      valid,
      replace_once(row, "\"operator_stop_policy\":\"hold\"",
                   "\"operator_stop_policy\":false")));

  // The removed policy forms are refused, never reinterpreted.
  expect_rejected(replace_policy_table(
      valid,
      replace_once(row, "\"instance_id\":\"root\",",
                   "\"instance_id\":\"root\",\"modification_policy\":{"
                   "\"intervention_delegate\":\"forbidden\","
                   "\"persistent_edit\":\"forbidden\","
                   "\"runtime_override\":\"forbidden\"},")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"allow_intervention\":true,",
                          "\"allow_intervention\":true,"
                          "\"allowed_recovery_actions\":[\"intervention\"],"
                          "\"allowed_skills\":[],")));
  expect_rejected(replace_policy_table(
      valid, replace_once(row, "\"max_retries\":1}",
                          "\"max_retries\":1,"
                          "\"persistence_ceiling\":\"session_only\"}")));

  const std::string duplicate_table = row + "," + row;
  expect_rejected(replace_policy_table(valid, duplicate_table));
  const std::string lower_row = replace_once(
      row, "\"instance_id\":\"root\"", "\"instance_id\":\"a\"");
  expect_rejected(replace_policy_table(valid, row + "," + lower_row));
}

TEST(ProgramBindingTest, InterventionStopScopeIsClosedAndSurvivesReload) {
  using Scope = manyforge::core::ProgramInterventionStopScope;
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_policy_binding_value(context);
  ASSERT_EQ(value.node_policies.size(), 1U);
  const auto default_scope = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(default_scope->canonical_bytes().find("intervention_stop_scope"),
            std::string::npos);
  value.node_policies.front().recovery_policy.intervention_stop_scope =
      Scope::kOwnGroups;
  const auto own_groups = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(own_groups->program_binding_id(),
            default_scope->program_binding_id());
  const auto reopened = reload_verified_program_binding(
      own_groups->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source());
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->value()
                .node_policies.front()
                .recovery_policy.intervention_stop_scope,
            Scope::kOwnGroups);
  const std::string valid(own_groups->canonical_bytes());
  for (const auto *invalid : {"\"own_group\"", "false", "null"}) {
    EXPECT_THROW(
        static_cast<void>(reload_verified_program_binding(
            replace_once(valid, "\"intervention_stop_scope\":\"own_groups\"",
                         "\"intervention_stop_scope\":" + std::string(invalid)),
            *context.bundle, context.model_map, context.catalog_source())),
        ProgramBindingError);
  }
  value.node_policies.front().recovery_policy.intervention_stop_scope =
      static_cast<Scope>(255U);
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
}

TEST(ProgramBindingTest, ContextRejectsAResealedGeneratedFrameSubstitution) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  ProgramBindingDTO stale = make_binding_value(context);
  stale.frame_bindings.front()
      .compiled_frame_name += "_substituted";
  const auto sealed = ProgramBindingConstructionAuthority::seal(std::move(stale));

  EXPECT_THROW(
      static_cast<void>(reload_verified_program_binding(
          sealed->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())),
      ProgramBindingError);
}

TEST(ProgramBindingTest, ReloadRejectsCanonicalBytesWithAStaleContentIdentity) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));
  std::string tampered(sealed->canonical_bytes());
  const std::string original = "\"program_id\":\"binding_test_program\"";
  const std::string replacement = "\"program_id\":\"binding_fake_program\"";
  const std::size_t position = tampered.find(original);
  ASSERT_NE(position, std::string::npos);
  tampered.replace(position, original.size(), replacement);

  EXPECT_THROW(
      static_cast<void>(reload_verified_program_binding(
          tampered, *context.bundle, context.model_map, context.catalog_source())),
      ProgramBindingError);
}

TEST(ProgramBindingTest, ConstructionCanonicalizesRoleTablesAndRejectsDuplicates) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  ProgramBindingDTO duplicate = make_binding_value(context);
  duplicate.component_bindings.push_back(
      duplicate.component_bindings.front());
  EXPECT_THROW(
      static_cast<void>(
          ProgramBindingConstructionAuthority::seal(std::move(duplicate))),
      ProgramBindingError);
}

TEST(ProgramBindingTest, DirectReferencesAreBoundToExactOwnerAndTarget) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  const auto original = make_binding_value(context);
  const auto sealed = ProgramBindingConstructionAuthority::seal(original);
  EXPECT_NE(sealed->canonical_bytes().find("manyforge.program_binding.v3"),
            std::string::npos);
  EXPECT_EQ(sealed->canonical_bytes().find("role_assignment_digest"),
            std::string::npos);
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::reload_intrinsic(
                   replace_once(std::string(sealed->canonical_bytes()),
                                "\"frame_bindings\":[{",
                                "\"frame_bindings\":[{\"contact_body_role_ids\":[],"))),
               ProgramBindingError);
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::reload_intrinsic(
                   replace_once(std::string(sealed->canonical_bytes()),
                                "manyforge.program_binding.v3",
                                "manyforge.program_binding.v2"))),
               ProgramBindingError);
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::reload_intrinsic(
                   replace_once(std::string(sealed->canonical_bytes()),
                                "\"signal_bindings\":[]",
                                "\"signal_bindings\":[],\"role_assignment_digest\":null"))),
               ProgramBindingError);

  auto duplicate = original;
  duplicate.target_bindings.front().frame_references.push_back(
      duplicate.target_bindings.front().frame_references.front());
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(duplicate)),
               ProgramBindingError);

  auto wrong_owner = original;
  wrong_owner.target_bindings.front().frame_references.front().component_id =
      manyforge::core::ControllableComponentId("other_component");
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(wrong_owner)),
               ProgramBindingError);

  auto substituted = original;
  substituted.frame_bindings.front().reference.component_id =
      manyforge::core::ControllableComponentId("other_component");
  substituted.target_bindings.front().frame_references.front() =
      substituted.frame_bindings.front().reference;
  substituted.target_bindings.front().frame_associations.front().frame =
      substituted.frame_bindings.front().reference;
  const auto resealed = ProgramBindingConstructionAuthority::seal(substituted);
  EXPECT_THROW(static_cast<void>(reload_verified_program_binding(
                   resealed->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())),
               ProgramBindingError);
}

TEST(ProgramBindingTest, ContextRejectsMissingOrForgedDefaultTcp) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  const auto original = make_binding_value(context);
  ASSERT_TRUE(original.target_bindings.front().default_tcp.has_value());
  const auto valid = ProgramBindingConstructionAuthority::seal(original);
  EXPECT_NO_THROW(static_cast<void>(reload_verified_program_binding(valid->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())));

  auto missing = original;
  missing.target_bindings.front().default_tcp.reset();
  const auto sealed_missing = ProgramBindingConstructionAuthority::seal(std::move(missing));
  EXPECT_THROW(static_cast<void>(reload_verified_program_binding(sealed_missing->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())), ProgramBindingError);

  auto forged = original;
  forged.target_bindings.front().default_tcp->frame.frame_id = FrameRoleId("forged_tcp");
  const auto sealed_forged = ProgramBindingConstructionAuthority::seal(std::move(forged));
  EXPECT_THROW(static_cast<void>(reload_verified_program_binding(sealed_forged->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())), ProgramBindingError);
}

TEST(ProgramBindingTest, SharedFrameKeepsDistinctTargetContactSets) {
  ScopedTrustedRoot root;
  auto value = make_binding_value(make_context(root.path()));
  auto& first = value.target_bindings.front();
  first.frame_associations.front().contact_body_role_ids = {
      CollisionBodyRoleId("arm_collision")};
  auto second = first;
  second.target.command_target_id = manyforge::core::CommandTargetId("other_target");
  second.frame_associations.front().contact_body_role_ids.clear();
  value.target_bindings.push_back(second);
  const auto sealed = ProgramBindingConstructionAuthority::seal(value);
  const auto loaded = ProgramBindingConstructionAuthority::reload_intrinsic(
      sealed->canonical_bytes());
  ASSERT_EQ(loaded->value().target_bindings.size(), 2U);
  const auto original_target = value.target_bindings.front().target;
  const auto found = std::find_if(loaded->value().target_bindings.begin(),
                                  loaded->value().target_bindings.end(),
                                  [&](const auto& target) { return target.target == original_target; });
  ASSERT_NE(found, loaded->value().target_bindings.end());
  const auto other = std::find_if(loaded->value().target_bindings.begin(),
                                  loaded->value().target_bindings.end(),
                                  [&](const auto& target) { return target.target == second.target; });
  ASSERT_NE(other, loaded->value().target_bindings.end());
  const auto& a = found->frame_associations.front();
  const auto& b = other->frame_associations.front();
  EXPECT_EQ(a.frame, b.frame);
  EXPECT_EQ(a.contact_body_role_ids,
            std::vector<CollisionBodyRoleId>{CollisionBodyRoleId("arm_collision")});
  EXPECT_TRUE(b.contact_body_role_ids.empty());

  auto missing = value;
  missing.target_bindings.front().frame_associations.clear();
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(missing)),
               ProgramBindingError);
  auto duplicate = value;
  duplicate.target_bindings.front().frame_associations.push_back(
      duplicate.target_bindings.front().frame_associations.front());
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(duplicate)),
               ProgramBindingError);
}

TEST(ProgramBindingTest, SharedTcpFrameKeepsInventoryContactSets) {
  ScopedTrustedRoot root;
  auto value = make_binding_value(make_context(root.path()));
  auto& target = value.target_bindings.front();
  const auto frame = value.frame_bindings.front();
  auto other_target = target.target;
  other_target.command_target_id = manyforge::core::CommandTargetId("other_target");
  target.robot_tcps = std::vector<manyforge::core::ProgramRobotTcpBindingDTO>{
      {{target.target, frame.reference}, frame, {CollisionBodyRoleId("arm_collision")}, false, false},
      {{other_target, frame.reference}, frame, {}, false, false}};
  const auto sealed = ProgramBindingConstructionAuthority::seal(value);
  const auto loaded = ProgramBindingConstructionAuthority::reload_intrinsic(sealed->canonical_bytes());
  ASSERT_TRUE(loaded->value().target_bindings.front().robot_tcps);
  const auto& rows = *loaded->value().target_bindings.front().robot_tcps;
  ASSERT_EQ(rows.size(), 2U);
  const auto first = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
    return row.entry.target == target.target;
  });
  const auto second = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
    return row.entry.target == other_target;
  });
  ASSERT_NE(first, rows.end());
  ASSERT_NE(second, rows.end());
  EXPECT_EQ(first->contact_body_role_ids,
            std::vector<CollisionBodyRoleId>{CollisionBodyRoleId("arm_collision")});
  EXPECT_TRUE(second->contact_body_role_ids.empty());
}

TEST(ProgramBindingTest, ContextRequiresExactTcpOwnerTarget) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  auto& target = value.target_bindings.front();
  ASSERT_TRUE(target.robot_tcps);
  ASSERT_FALSE(target.robot_tcps->empty());
  const auto valid = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NO_THROW(static_cast<void>(reload_verified_program_binding(
      valid->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())));

  target.robot_tcps->front().entry.target.command_target_id =
      manyforge::core::CommandTargetId("unrelated_target");
  const auto forged = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_THROW(static_cast<void>(reload_verified_program_binding(
      forged->canonical_bytes(), *context.bundle, context.model_map, context.catalog_source())), ProgramBindingError);
}

TEST(ProgramBindingTest,
     DurableRepositoryPublishesAndReopensOnlyInExactContext) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));

  {
    FilesystemProgramBindingRepository repository(root.path());
    EXPECT_TRUE(repository.put_if_absent(
        sealed, *context.bundle, context.model_map, context.catalog_source()));
    EXPECT_FALSE(repository.put_if_absent(
        sealed, *context.bundle, context.model_map, context.catalog_source()));
    EXPECT_EQ(repository.size(), 1U);
    EXPECT_EQ(repository.retained_bytes(), sealed->canonical_bytes().size());
  }

  FilesystemProgramBindingRepository reopened(root.path());
  const auto read = reopened.get_verified(
      sealed->program_binding_id(), *context.bundle, context.model_map, context.catalog_source());
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->program_binding_id(), sealed->program_binding_id());
  EXPECT_EQ(read->value(), sealed->value());
  EXPECT_EQ(read->canonical_bytes(), sealed->canonical_bytes());
  EXPECT_EQ(reopened.get_verified(
                ProgramBindingId(std::string(kZeroDigest)),
                *context.bundle, context.model_map, context.catalog_source()),
            nullptr);
}

TEST(ProgramBindingTest,
     VerifiedSourceReadsThroughProcfdAndLeavesMissingNamespaceAbsent) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));
  {
    FilesystemProgramBindingRepository writer(root.path());
    ASSERT_TRUE(writer.put_if_absent(
        sealed, *context.bundle, context.model_map, context.catalog_source()));
  }

  const auto entry_count_before = static_cast<std::size_t>(std::distance(
      std::filesystem::recursive_directory_iterator(root.path()),
      std::filesystem::recursive_directory_iterator()));
  manyforge::deployment::test::ScopedProcfdRoot procfd(root.path());
  const FilesystemVerifiedCatalogArtifactSource procfd_catalog_source(
      procfd.path());
  const FilesystemVerifiedProgramBindingSource source(procfd.path());
  manyforge::deployment::test::ScopedPathReplacement replacement(
      root.path());
  const auto read = source.get_verified(
      sealed->program_binding_id(), *context.bundle, context.model_map,
      procfd_catalog_source);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->canonical_bytes(), sealed->canonical_bytes());
  EXPECT_EQ(source.get_verified(
                ProgramBindingId(std::string(kZeroDigest)),
                *context.bundle, context.model_map, procfd_catalog_source),
            nullptr);
  EXPECT_EQ(static_cast<std::size_t>(std::distance(
      std::filesystem::recursive_directory_iterator(root.path()),
                std::filesystem::recursive_directory_iterator())),
            0U);
  EXPECT_EQ(static_cast<std::size_t>(std::distance(
                std::filesystem::recursive_directory_iterator(
                    replacement.backing()),
                std::filesystem::recursive_directory_iterator())),
            entry_count_before);

  ScopedTrustedRoot empty_root;
  manyforge::deployment::test::ScopedProcfdRoot empty_procfd(
      empty_root.path());
  const FilesystemVerifiedProgramBindingSource empty_source(
      empty_procfd.path());
  EXPECT_EQ(empty_source.get_verified(
                sealed->program_binding_id(), *context.bundle,
                context.model_map, context.catalog_source()),
            nullptr);
  EXPECT_FALSE(
      std::filesystem::exists(empty_root.path() / "program-bindings"));

  ScopedTrustedRoot unsafe_root;
  const auto outside = unsafe_root.path().parent_path() /
                       (unsafe_root.path().filename().string() + "-outside");
  std::filesystem::create_directory(outside);
  std::filesystem::create_directory(
      unsafe_root.path() / "program-bindings");
  std::filesystem::create_directory_symlink(
      outside, unsafe_root.path() / "program-bindings" / "sha256");
  manyforge::deployment::test::ScopedProcfdRoot unsafe_procfd(
      unsafe_root.path());
  const FilesystemVerifiedProgramBindingSource unsafe_source(
      unsafe_procfd.path());
  EXPECT_THROW(
      static_cast<void>(unsafe_source.get_verified(
          sealed->program_binding_id(), *context.bundle, context.model_map, context.catalog_source())),
      manyforge::deployment::RetainedBundleError);
  EXPECT_TRUE(std::filesystem::is_empty(outside));
  std::filesystem::remove_all(outside);
}

TEST(ProgramBindingTest,
     DurableRepositoryRejectsLookupThroughAnotherBundleContext) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));
  FilesystemProgramBindingRepository repository(root.path());
  ASSERT_TRUE(repository.put_if_absent(
      sealed, *context.bundle, context.model_map, context.catalog_source()));

  ScopedTrustedRoot other_root;
  const BindingContext other = make_other_cell_context(other_root.path());
  ASSERT_NE(other.bundle->retained_bundle_id(),
            context.bundle->retained_bundle_id());
  EXPECT_THROW(
      static_cast<void>(repository.get_verified(
          sealed->program_binding_id(), *other.bundle, other.model_map, other.catalog_source())),
      ProgramBindingError);
}

TEST(ProgramBindingTest, DirectSignalBindingsAreClosedAndContextVerified) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  const auto ordinary = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(ordinary->canonical_bytes().find("\"signal_bindings\":[]"), std::string::npos);
  const auto target = value.target_bindings.front().target;
  const manyforge::core::ProgramSignalReferenceDTO ref{target.component_id, "ready"};
  value.target_bindings.front().signal_references.push_back(ref);
  value.signal_bindings.push_back({ref, target, manyforge::core::RuntimeUnitId("io"),
      manyforge::core::IoSignalValueType::kBoolean,
      manyforge::core::IoSignalDirection::kInput,
      manyforge::core::IoTransitionCapability::kSampledOnly,
      ContentDigest(std::string(kZeroDigest))});
  const auto invented = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(invented->program_binding_id(), ordinary->program_binding_id());
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::reload_intrinsic(
                   replace_once(std::string(invented->canonical_bytes()),
                                "\"direction\":\"input\"",
                                "\"direction\":\"unknown\""))),
               ProgramBindingError);
  EXPECT_THROW(static_cast<void>(reload_verified_program_binding(invented->canonical_bytes(),
      *context.bundle, context.model_map, context.catalog_source())), ProgramBindingError);
  value.signal_bindings.push_back(value.signal_bindings.front());
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(value)), ProgramBindingError);
}

TEST(ProgramBindingTest, DirectSignalBindingRejectsInvalidEnumsAndDigest) {
  ScopedTrustedRoot root;
  auto value = make_binding_value(make_context(root.path()));
  const auto target = value.target_bindings.front().target;
  const manyforge::core::ProgramSignalReferenceDTO ref{target.component_id, "ready"};
  value.target_bindings.front().signal_references.push_back(ref);
  value.signal_bindings.push_back({ref, target, manyforge::core::RuntimeUnitId("io"),
      manyforge::core::IoSignalValueType::kBoolean,
      manyforge::core::IoSignalDirection::kInput,
      manyforge::core::IoTransitionCapability::kSampledOnly,
      ContentDigest(std::string(kZeroDigest))});
  const auto valid = value.signal_bindings.front();
  value.signal_bindings.front().direction = static_cast<manyforge::core::IoSignalDirection>(255);
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(value)), ProgramBindingError);
  value.signal_bindings.front() = valid;
  value.signal_bindings.front().transition_capability = static_cast<manyforge::core::IoTransitionCapability>(255);
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(value)), ProgramBindingError);
  value.signal_bindings.front() = valid;
  value.signal_bindings.front().signal_descriptor_digest = ContentDigest("not_a_digest");
  EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(value)), ProgramBindingError);
}

TEST(ProgramBindingTest, DirectSignalNamesUseBoundedUtf8BytesWithoutNul) {
  ScopedTrustedRoot root;
  auto value = make_binding_value(make_context(root.path()));
  const auto target = value.target_bindings.front().target;
  const auto check = [&](const std::string &name) {
    auto candidate = value;
    const manyforge::core::ProgramSignalReferenceDTO ref{target.component_id, name};
    candidate.target_bindings.front().signal_references.push_back(ref);
    candidate.signal_bindings.push_back({ref, target, manyforge::core::RuntimeUnitId("io"),
        manyforge::core::IoSignalValueType::kBoolean,
        manyforge::core::IoSignalDirection::kInput,
        manyforge::core::IoTransitionCapability::kSampledOnly,
        ContentDigest(std::string(kZeroDigest))});
    return candidate;
  };
  for (const auto &name : {std::string("sensor / ready?"), std::string(510U, 'x') + "/?",
                           std::string(510U, 'x') + "\xc3\xa9"}) {
    const auto sealed = ProgramBindingConstructionAuthority::seal(check(name));
    const auto loaded = ProgramBindingConstructionAuthority::reload_intrinsic(sealed->canonical_bytes());
    ASSERT_EQ(loaded->value().signal_bindings.size(), 1U);
    EXPECT_EQ(loaded->value().signal_bindings.front().reference.signal_name, name);
  }
  for (const auto &name : {std::string{}, std::string(513U, 'x'),
                           std::string(511U, 'x') + "\xc3\xa9", std::string("a\0b", 3U)})
    EXPECT_THROW(static_cast<void>(ProgramBindingConstructionAuthority::seal(check(name))),
                 ProgramBindingError);
}



// The owner guard (`CellKinematicIndex::carries`) accepts the component
// itself and every component mounted on it, directly or through a tool chain;
// nothing else.
TEST(ProgramBindingMountedOwnerTest, SelfOrMountedDescendantFollowsAttachments) {
  namespace core = manyforge::core;
  const auto edge = [](std::string parent, std::string parent_interface,
                       std::string child) {
    return core::ComponentAttachmentEdgeDTO{
        core::ComponentAttachmentId(child),
        core::ComponentAttachmentRevision(
            "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
        {core::ControllableComponentId(parent),
         core::AttachmentInterfaceId(parent_interface)},
        {core::ControllableComponentId(child), core::AttachmentInterfaceId("mount")},
        {}};
  };
  core::CellGraphDTO graph{
      "manyforge.cell_graph.v1",
      core::GraphRevision(
          "sha256:0000000000000000000000000000000000000000000000000000000000000000"),
      core::CellId("tool-split-cell"),
      {}, {}, {}, {}, {},
      {edge("lite6_arm", "flange", "lite_gripper"),
       edge("lite_gripper", "tool_flange", "sensor")},
  };
  const auto mounted = [&](std::string component, std::string owner) {
    return manyforge::deployment::CellKinematicIndex(graph).carries(
        core::ControllableComponentId(component),
        core::ControllableComponentId(owner));
  };
  EXPECT_TRUE(mounted("lite6_arm", "lite6_arm"));
  EXPECT_TRUE(mounted("lite6_arm", "lite_gripper"));
  EXPECT_TRUE(mounted("lite6_arm", "sensor"));        // tool chain
  EXPECT_TRUE(mounted("lite_gripper", "sensor"));
  EXPECT_FALSE(mounted("lite_gripper", "lite6_arm"));  // never upwards
  EXPECT_FALSE(mounted("lite6_arm", "station_io"));    // unmounted
  graph.component_attachments.clear();
  EXPECT_FALSE(mounted("lite6_arm", "lite_gripper"));
}

// On a shared flange without a primary mark, the tool is the
// child carrying a TCP (a camera that is no tool needs no mark), and a robot
// whose flange tool has no default TCP has no End-effector TCP.
TEST(ProgramBindingMountedOwnerTest, FlangeToolAndMissingEndEffectorTcp) {
  namespace core = manyforge::core;
  const std::string zero =
      "sha256:0000000000000000000000000000000000000000000000000000000000000000";
  const auto component = [&](std::string id,
                             std::optional<core::ComponentDefaultTcpDTO> tcp) {
    return core::ControllableComponentDTO{
        core::ControllableComponentId(id), core::ComponentTypeId(id + "_type"),
        core::ComponentTypeRevision(zero), {}, {}, std::nullopt, std::move(tcp)};
  };
  const auto edge = [&](std::string parent, std::string child, bool primary) {
    return core::ComponentAttachmentEdgeDTO{
        core::ComponentAttachmentId(child), core::ComponentAttachmentRevision(zero),
        {core::ControllableComponentId(parent), core::AttachmentInterfaceId("flange")},
        {core::ControllableComponentId(child), core::AttachmentInterfaceId("mount")},
        {}, primary};
  };
  const core::ComponentDefaultTcpDTO cup{core::CommandTargetId("gripper"),
                                         core::FrameRoleId("cup_center")};
  const auto graph = [&](std::vector<core::ControllableComponentDTO> components,
                         std::vector<core::ComponentAttachmentEdgeDTO> edges) {
    return core::CellGraphDTO{"manyforge.cell_graph.v1", core::GraphRevision(zero),
                              core::CellId("flange-tool-cell"), {}, {},
                              std::move(components), {}, {}, std::move(edges)};
  };
  using manyforge::deployment::CellKinematicIndex;
  const core::ControllableComponentId robot("lite6_arm");
  // A camera (no TCP) first on the flange, the gripper second: the gripper.
  const CellKinematicIndex shared(graph(
      {component("lite6_arm", std::nullopt), component("camera", std::nullopt),
       component("lite_gripper", cup)},
      {edge("lite6_arm", "camera", false), edge("lite6_arm", "lite_gripper", false)}));
  ASSERT_NE(shared.tool_on(robot, "flange"), nullptr);
  EXPECT_EQ(shared.tool_on(robot, "flange")->value(), "lite_gripper");
  const core::CommandTargetId arm("arm");
  ASSERT_TRUE(shared.end_effector_entry(robot, arm).has_value());
  EXPECT_EQ(shared.end_effector_entry(robot, arm)->component_id.value(), "lite_gripper");
  // A primary mark still wins.
  const CellKinematicIndex marked(graph(
      {component("lite6_arm", std::nullopt), component("camera", std::nullopt),
       component("lite_gripper", cup)},
      {edge("lite6_arm", "camera", true), edge("lite6_arm", "lite_gripper", false)}));
  EXPECT_EQ(marked.tool_on(robot, "flange")->value(), "camera");
  // A bracket without a TCP leaves the robot without an End-effector TCP.
  const CellKinematicIndex bracket(graph(
      {component("lite6_arm", std::nullopt), component("lite_bracket", std::nullopt)},
      {edge("lite6_arm", "lite_bracket", false)}));
  EXPECT_EQ(bracket.tool_on(robot, "flange")->value(), "lite_bracket");
  EXPECT_FALSE(bracket.end_effector_entry(robot, arm).has_value());
}

} // namespace
