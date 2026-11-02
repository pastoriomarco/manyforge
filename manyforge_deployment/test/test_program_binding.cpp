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
using manyforge::core::ProgramComponentRoleBindingDTO;
using manyforge::core::ProgramControlBindingRefDTO;
using manyforge::core::ProgramFrameRoleBindingDTO;
using manyforge::core::ProgramId;
using manyforge::core::ProgramModelRoleBindingDTO;
using manyforge::core::ProgramResourceRoleBindingDTO;
using manyforge::core::ProgramRevision;
using manyforge::core::ProgramTargetRoleBindingDTO;
using manyforge::core::ResourceId;
using manyforge::core::ResourceRoleId;
using manyforge::core::TargetRoleId;
using manyforge::deployment::CompositionComponentModelFrame;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::FilesystemProgramBindingRepository;
using manyforge::deployment::FilesystemVerifiedProgramBindingSource;
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
};

[[nodiscard]] BindingContext make_context(
    const std::filesystem::path& root) {
  const auto materialized =
      materialize_lite6_retained_consumer_fixture(root);
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
  return {std::move(bundle), std::move(*model_map)};
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
  return {std::move(bundle), std::move(*model_map)};
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

  return ProgramBindingDTO{
      std::string(manyforge::core::kProgramBindingApiVersion),
      ProgramBindingId(std::string(kZeroDigest)),
      ProgramId("i6_test_program"),
      ProgramRevision(sha256_digest("i6 test program revision")),
      context.bundle->retained_bundle_id(),
      graph.graph_revision,
      bundle.catalogs.node_environment,
      {ProgramComponentRoleBindingDTO{
          ComponentRoleId("robot"),
          component.controllable_component_id,
          component.component_type_id,
          component.component_type_revision,
          {ProgramModelRoleBindingDTO{
              component.model_role_bindings.front().model_role_id,
              model.model_instance_id,
              model.model_revision,
          }},
      }},
      {ProgramTargetRoleBindingDTO{
          TargetRoleId("arm"),
          ComponentRoleId("robot"),
          component.controllable_component_id,
          control.command_target_id,
          {ProgramFrameRoleBindingDTO{
              FrameRoleId("tcp"), frame->model_instance_id,
              frame->model_revision, frame->compiled_frame_name,
          }},
          {ProgramResourceRoleBindingDTO{
              ResourceRoleId("joints"), std::move(resources),
              std::move(compiled_names),
          }},
          {manyforge::core::ProgramCollisionBodyRoleBindingDTO{
              CollisionBodyRoleId("arm_collision"),
              target->collision_bodies.front().model_instance_id,
              model.model_revision,
              target->collision_bodies.front().compiled_link_names,
          }},
          {ProgramCapabilityBindingDTO{
              CapabilityId(control.capability_id),
              CapabilityDefinitionRevision(
                  control.capability_definition_revision),
              ProgramControlBindingRefDTO{
                  control.control_binding_id,
                  control.control_binding_revision,
              },
              std::nullopt,
              std::nullopt,
          }},
      }},
      ProgramBindingProvenanceDTO{
          bundle.provenance.compiler_contract_digest,
          ContentDigest(sha256_digest("i6 exact role assignment")),
          bundle.provenance.source_records,
          bundle.provenance.source_records,
      },
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
  const auto &component = binding.component_role_bindings.front();
  const auto model_role =
      component.model_role_bindings.front().model_role_id.value();
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
                                 {"component_role", CanonicalValue("robot")},
                                 {"model_role", CanonicalValue(model_role)},
                                 {"frame_role", CanonicalValue("tcp")}})},
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
            component.controllable_component_id);
  EXPECT_EQ(frame->model_instance_id,
            component.model_role_bindings.front().model_instance_id);
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
                                                       {"component_role", CanonicalValue("robot")},
                                                       {"model_role", CanonicalValue("missing")},
                                                       {"frame_role", CanonicalValue("tcp")}})},
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
      sealed->canonical_bytes(), *context.bundle, context.model_map);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->program_binding_id(), sealed->program_binding_id());
  EXPECT_EQ(reloaded->value(), sealed->value());
}

TEST(ProgramBindingTest, RobotTcpPresenceSurvivesCanonicalSealAndReload) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  auto &target = value.target_role_bindings.front();
  target.robot_tcps.reset();
  const auto absent = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(absent->canonical_bytes().find("\"robot_tcps\""), std::string::npos);
  // Structural round-trip only: this fixture is an arm, not a non-robot.
  const auto absent_reloaded =
      ProgramBindingConstructionAuthority::reload_intrinsic(absent->canonical_bytes());
  EXPECT_FALSE(absent_reloaded->value().target_role_bindings.front().robot_tcps);

  target.robot_tcps.emplace();
  const auto empty = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(empty->canonical_bytes().find("\"robot_tcps\":[]"), std::string::npos);
  EXPECT_NE(empty->program_binding_id(), absent->program_binding_id());
  const auto empty_reloaded = reload_verified_program_binding(
      empty->canonical_bytes(), *context.bundle, context.model_map);
  ASSERT_TRUE(empty_reloaded->value().target_role_bindings.front().robot_tcps);
  EXPECT_TRUE(empty_reloaded->value().target_role_bindings.front().robot_tcps->empty());
  EXPECT_EQ(empty_reloaded->value(), empty->value());
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
      sealed->canonical_bytes(), *context.bundle, context.model_map);
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
      sealed->canonical_bytes(), *context.bundle, context.model_map);
  EXPECT_EQ(loaded->value(), sealed->value());
  value.contact_grasp_bindings.front().bodies_from_motion_endpoint = false;
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
  const auto &target = value.target_role_bindings.front();
  const auto &body = target.collision_body_role_bindings.front();
  std::vector<std::string> ids;
  for (std::size_t i = 0U; i < body.compiled_collision_body_names.size(); ++i) {
    ids.push_back(manyforge::core::make_component_collision_body_id(
        target.controllable_component_id, body.model_instance_id,
        body.collision_body_role_id, i));
  }
  value.contact_grasp_bindings.front().allowed_body_bindings.push_back(
      {target.component_role_id, target.target_role_id,
       target.controllable_component_id, target.command_target_id, std::nullopt,
       body.collision_body_role_id, body.model_instance_id, body.model_revision,
       std::move(ids)});
  const auto legacy = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(legacy->canonical_bytes().find("bodies_from_motion_endpoint"),
            std::string::npos);
  EXPECT_EQ(reload_verified_program_binding(legacy->canonical_bytes(),
                                            *context.bundle, context.model_map)
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
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  const auto legacy = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(legacy->canonical_bytes().find("contact_body_role_ids"),
            std::string::npos);

  auto &target = value.target_role_bindings.front();
  target.frame_role_bindings.front().contact_body_role_ids = {
      CollisionBodyRoleId("arm_collision")};
  const auto first_endpoint = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(first_endpoint->canonical_bytes().find(
                "\"contact_body_role_ids\":[\"arm_collision\"]"),
            std::string::npos);
  const auto reopened = reload_verified_program_binding(
      first_endpoint->canonical_bytes(), *context.bundle, context.model_map);
  EXPECT_EQ(reopened->value(), first_endpoint->value());

  auto second_frame = target.frame_role_bindings.front();
  second_frame.frame_role_id = manyforge::core::FrameRoleId("sibling_tcp");
  second_frame.compiled_frame_name += "_sibling";
  second_frame.contact_body_role_ids = {
      CollisionBodyRoleId("sibling_collision")};
  target.frame_role_bindings.push_back(std::move(second_frame));
  auto second_body = target.collision_body_role_bindings.front();
  second_body.collision_body_role_id = CollisionBodyRoleId("sibling_collision");
  second_body.compiled_collision_body_names = {"sibling_left", "sibling_right"};
  target.collision_body_role_bindings.push_back(std::move(second_body));
  const auto two_endpoints = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(two_endpoints->canonical_bytes().find("\"frame_role_id\":\"tcp\""),
            std::string::npos);
  EXPECT_NE(two_endpoints->canonical_bytes().find(
                "\"frame_role_id\":\"sibling_tcp\""),
            std::string::npos);
  EXPECT_NE(two_endpoints->canonical_bytes().find(
                "\"contact_body_role_ids\":[\"sibling_collision\"]"),
            std::string::npos);

  target.collision_body_role_bindings.back().model_revision =
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
      sealed->canonical_bytes(), *context.bundle, context.model_map);
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
          missing_node_policies, *context.bundle, context.model_map)),
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
            bytes, *context.bundle, context.model_map)),
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
      own_groups->canonical_bytes(), *context.bundle, context.model_map);
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
            *context.bundle, context.model_map)),
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
  stale.target_role_bindings.front()
      .frame_role_bindings.front()
      .compiled_frame_name += "_substituted";
  const auto sealed = ProgramBindingConstructionAuthority::seal(std::move(stale));

  EXPECT_THROW(
      static_cast<void>(reload_verified_program_binding(
          sealed->canonical_bytes(), *context.bundle, context.model_map)),
      ProgramBindingError);
}

TEST(ProgramBindingTest, ReloadRejectsCanonicalBytesWithAStaleContentIdentity) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  const auto sealed =
      ProgramBindingConstructionAuthority::seal(make_binding_value(context));
  std::string tampered(sealed->canonical_bytes());
  const std::string original = "\"program_id\":\"i6_test_program\"";
  const std::string replacement = "\"program_id\":\"i6_fake_program\"";
  const std::size_t position = tampered.find(original);
  ASSERT_NE(position, std::string::npos);
  tampered.replace(position, original.size(), replacement);

  EXPECT_THROW(
      static_cast<void>(reload_verified_program_binding(
          tampered, *context.bundle, context.model_map)),
      ProgramBindingError);
}

TEST(ProgramBindingTest, ConstructionCanonicalizesRoleTablesAndRejectsDuplicates) {
  ScopedTrustedRoot root;
  const BindingContext context = make_context(root.path());
  ProgramBindingDTO duplicate = make_binding_value(context);
  duplicate.component_role_bindings.push_back(
      duplicate.component_role_bindings.front());
  EXPECT_THROW(
      static_cast<void>(
          ProgramBindingConstructionAuthority::seal(std::move(duplicate))),
      ProgramBindingError);
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
        sealed, *context.bundle, context.model_map));
    EXPECT_FALSE(repository.put_if_absent(
        sealed, *context.bundle, context.model_map));
    EXPECT_EQ(repository.size(), 1U);
    EXPECT_EQ(repository.retained_bytes(), sealed->canonical_bytes().size());
  }

  FilesystemProgramBindingRepository reopened(root.path());
  const auto read = reopened.get_verified(
      sealed->program_binding_id(), *context.bundle, context.model_map);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->program_binding_id(), sealed->program_binding_id());
  EXPECT_EQ(read->value(), sealed->value());
  EXPECT_EQ(read->canonical_bytes(), sealed->canonical_bytes());
  EXPECT_EQ(reopened.get_verified(
                ProgramBindingId(std::string(kZeroDigest)),
                *context.bundle, context.model_map),
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
        sealed, *context.bundle, context.model_map));
  }

  const auto entry_count_before = static_cast<std::size_t>(std::distance(
      std::filesystem::recursive_directory_iterator(root.path()),
      std::filesystem::recursive_directory_iterator()));
  manyforge::deployment::test::ScopedProcfdRoot procfd(root.path());
  const FilesystemVerifiedProgramBindingSource source(procfd.path());
  manyforge::deployment::test::ScopedPathReplacement replacement(
      root.path());
  const auto read = source.get_verified(
      sealed->program_binding_id(), *context.bundle, context.model_map);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->canonical_bytes(), sealed->canonical_bytes());
  EXPECT_EQ(source.get_verified(
                ProgramBindingId(std::string(kZeroDigest)),
                *context.bundle, context.model_map),
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
                context.model_map),
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
          sealed->program_binding_id(), *context.bundle, context.model_map)),
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
      sealed, *context.bundle, context.model_map));

  ScopedTrustedRoot other_root;
  const BindingContext other = make_other_cell_context(other_root.path());
  ASSERT_NE(other.bundle->retained_bundle_id(),
            context.bundle->retained_bundle_id());
  EXPECT_THROW(
      static_cast<void>(repository.get_verified(
          sealed->program_binding_id(), *other.bundle, other.model_map)),
      ProgramBindingError);
}

TEST(ProgramBindingTest,
     SignalBindingsAreSparseClosedAndCannotAliasCoordinates) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  auto value = make_binding_value(context);
  const auto ordinary = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_EQ(ordinary->canonical_bytes().find("signal_role_bindings"),
            std::string::npos);
  auto &target = value.target_role_bindings.front();
  target.signal_role_bindings.push_back(
      {ResourceRoleId("input_ready"), "ready",
       manyforge::core::IoSignalValueType::kBoolean,
       manyforge::core::IoSignalDirection::kInput,
       manyforge::core::IoTransitionCapability::kSampledOnly,
       ContentDigest(std::string(kZeroDigest))});
  const auto extended = ProgramBindingConstructionAuthority::seal(value);
  EXPECT_NE(extended->program_binding_id(), ordinary->program_binding_id());
  EXPECT_NE(extended->canonical_bytes().find("signal_role_bindings"),
            std::string::npos);
  // This retained fixture declares no I/O descriptor: a resealed invented row
  // cannot acquire a qualified binding merely by passing the closed codec.
  EXPECT_THROW(
      static_cast<void>(reload_verified_program_binding(
          extended->canonical_bytes(), *context.bundle, context.model_map)),
      ProgramBindingError);
  target.signal_role_bindings.front().resource_role_id =
      target.resource_role_bindings.front().resource_role_id;
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
  target.signal_role_bindings.clear();
  EXPECT_EQ(ProgramBindingConstructionAuthority::seal(value)->canonical_bytes(),
            ordinary->canonical_bytes());
}

TEST(ProgramBindingTest,
     SignalBindingRejectsUnknownEnumsDuplicateRolesAndBadDigests) {
  ScopedTrustedRoot root;
  auto value = make_binding_value(make_context(root.path()));
  auto &signals = value.target_role_bindings.front().signal_role_bindings;
  signals.push_back({ResourceRoleId("input_ready"), "ready",
                     manyforge::core::IoSignalValueType::kBoolean,
                     manyforge::core::IoSignalDirection::kInput,
                     manyforge::core::IoTransitionCapability::kSampledOnly,
                     ContentDigest(std::string(kZeroDigest))});
  const auto valid = signals.front();
  signals.front().direction =
      static_cast<manyforge::core::IoSignalDirection>(255);
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
  signals.front() = valid;
  signals.front().transition_capability =
      static_cast<manyforge::core::IoTransitionCapability>(255);
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
  signals.front() = valid;
  signals.front().signal_descriptor_digest = ContentDigest("not_a_digest");
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
  signals.front() = valid;
  signals.push_back(valid);
  EXPECT_THROW(
      static_cast<void>(ProgramBindingConstructionAuthority::seal(value)),
      ProgramBindingError);
}



// TCP-D: the owner guard (`CellKinematicIndex::carries`) accepts the component
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
      {edge("lite6_robot", "flange", "lite_gripper"),
       edge("lite_gripper", "tool_flange", "sensor")},
  };
  const auto mounted = [&](std::string component, std::string owner) {
    return manyforge::deployment::CellKinematicIndex(graph).carries(
        core::ControllableComponentId(component),
        core::ControllableComponentId(owner));
  };
  EXPECT_TRUE(mounted("lite6_robot", "lite6_robot"));
  EXPECT_TRUE(mounted("lite6_robot", "lite_gripper"));
  EXPECT_TRUE(mounted("lite6_robot", "sensor"));        // tool chain
  EXPECT_TRUE(mounted("lite_gripper", "sensor"));
  EXPECT_FALSE(mounted("lite_gripper", "lite6_robot"));  // never upwards
  EXPECT_FALSE(mounted("lite6_robot", "station_io"));    // unmounted
  graph.component_attachments.clear();
  EXPECT_FALSE(mounted("lite6_robot", "lite_gripper"));
}

// B5/B1 with BFIX: on a shared flange without a primary mark, the tool is the
// child carrying a TCP (a camera that is no tool needs no mark), and a robot
// whose flange tool has no default TCP has no End-effector TCP (C-P2,
// B-run-1).
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
  const core::ControllableComponentId robot("lite6_robot");
  // A camera (no TCP) first on the flange, the gripper second: the gripper.
  const CellKinematicIndex shared(graph(
      {component("lite6_robot", std::nullopt), component("camera", std::nullopt),
       component("lite_gripper", cup)},
      {edge("lite6_robot", "camera", false), edge("lite6_robot", "lite_gripper", false)}));
  ASSERT_NE(shared.tool_on(robot, "flange"), nullptr);
  EXPECT_EQ(shared.tool_on(robot, "flange")->value(), "lite_gripper");
  const core::CommandTargetId arm("arm");
  ASSERT_TRUE(shared.end_effector_entry(robot, arm).has_value());
  EXPECT_EQ(shared.end_effector_entry(robot, arm)->component_id.value(), "lite_gripper");
  // A primary mark still wins.
  const CellKinematicIndex marked(graph(
      {component("lite6_robot", std::nullopt), component("camera", std::nullopt),
       component("lite_gripper", cup)},
      {edge("lite6_robot", "camera", true), edge("lite6_robot", "lite_gripper", false)}));
  EXPECT_EQ(marked.tool_on(robot, "flange")->value(), "camera");
  // C-P2: a bracket without a TCP leaves the robot without an End-effector TCP.
  const CellKinematicIndex bracket(graph(
      {component("lite6_robot", std::nullopt), component("lite_bracket", std::nullopt)},
      {edge("lite6_robot", "lite_bracket", false)}));
  EXPECT_EQ(bracket.tool_on(robot, "flange")->value(), "lite_bracket");
  EXPECT_FALSE(bracket.end_effector_entry(robot, arm).has_value());
}

} // namespace
