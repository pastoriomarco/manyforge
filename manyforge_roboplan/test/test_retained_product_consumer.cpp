#include <gtest/gtest.h>

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <map>
#include <memory>
#include <optional>
#include <roboplan/core/geometry_wrappers.hpp>
#include <roboplan/core/scene.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "lite6_retained_consumer_fixture.hpp"
#include "mf2_joint_name_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::roboplan_backend {
namespace {

using collision::ManyForgeCollisionEngine;
constexpr std::string_view kCanonicalCellFrame =
    "mf1_cell_e228ede37638a8d03080644d418a75c681930a4b5b3f6b0bbde2603974dab681";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProductArtifact>);
static_assert(
    !std::is_aggregate_v<deployment::VerifiedRetainedProductArtifact>);
static_assert(
    !std::is_default_constructible_v<deployment::VerifiedRetainedModelMap>);
static_assert(!std::is_aggregate_v<deployment::VerifiedRetainedModelMap>);
static_assert(!std::is_default_constructible_v<
              deployment::VerifiedRetainedProjectionInput>);
static_assert(
    !std::is_aggregate_v<deployment::VerifiedRetainedProjectionInput>);

[[nodiscard]] core::LogicalSceneSnapshotRoot
make_projection_runtime_shadow_snapshot(const core::CellId &cell_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  auto snapshot = std::make_shared<core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          core::SceneRevision{
              core::logical_scene_identity::MintSceneId({
                  cell_id,
                  core::BootstrapId("i3_projection_runtime_shadow"),
                  core::BootstrapGeneration(1U),
              }),
              1U, core::ContentDigest(std::string(kZeroDigest))},
          cell_id,
          0U,
          0U,
          0U,
          std::move(policy),
          {},
          {},
      });
  snapshot->revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(*snapshot);
  if (!snapshot->locally_valid()) {
    throw std::logic_error(
        "test-only projection runtime-shadow snapshot is invalid");
  }
  return core::LogicalSceneSnapshotRoot(std::move(snapshot));
}

[[nodiscard]] core::LogicalSceneSnapshotDTO
make_collision_scene_snapshot(const core::CellId &cell_id,
                              const std::uint64_t scene_revision_id,
                              const core::SceneResourceId &scene_resource_id,
                              const double x_m, const double extent_m) {
  auto base = make_projection_runtime_shadow_snapshot(cell_id);
  core::LogicalSceneSnapshotDTO snapshot = *base.shared();
  snapshot.revision.revision_id = scene_revision_id;
  snapshot.transaction_sequence = scene_revision_id - 1U;
  snapshot.topology_epoch = scene_revision_id - 1U;
  snapshot.objects.push_back(core::SceneObjectSnapshotDTO{
      scene_resource_id,
      {core::BoxShapeDTO{{extent_m, extent_m, extent_m}}},
      core::CellFixedSceneObjectPlacementDTO{
          cell_id,
          {{x_m, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
      },
      std::nullopt,
  });
  snapshot.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
  if (!snapshot.locally_valid()) {
    throw std::logic_error("test collision logical-scene snapshot is invalid");
  }
  return snapshot;
}

class ScopedTemporaryRoot final {
public:
  ScopedTemporaryRoot() {
    std::filesystem::path pattern = std::filesystem::temp_directory_path() /
                                    "manyforge-lite6-retained-consumer-XXXXXX";
    std::string owned_pattern = pattern.string();
    char *created = ::mkdtemp(owned_pattern.data());
    if (created == nullptr) {
      throw std::runtime_error(
          "cannot create retained Lite6 consumer test root");
    }
    path_ = created;
  }

  ~ScopedTemporaryRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedTemporaryRoot(const ScopedTemporaryRoot &) = delete;
  ScopedTemporaryRoot &operator=(const ScopedTemporaryRoot &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class ScopedCurrentDirectory final {
public:
  explicit ScopedCurrentDirectory(const std::filesystem::path &replacement)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(replacement);
  }

  ~ScopedCurrentDirectory() {
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
  }

  ScopedCurrentDirectory(const ScopedCurrentDirectory &) = delete;
  ScopedCurrentDirectory &operator=(const ScopedCurrentDirectory &) = delete;

private:
  std::filesystem::path previous_;
};

[[nodiscard]] deployment::VerifiedRetainedPlanningModel
load_lite6_planning_model(const std::filesystem::path &package_root,
                          const deployment::RetainedPlanningModelQuery &query) {
  const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
      package_root);
  auto model = source.get_verified(query);
  if (!model.has_value()) {
    throw std::logic_error(
        "durable Lite6 planning model was absent on exact point read");
  }
  return std::move(*model);
}

[[nodiscard]] deployment::VerifiedRetainedModelMap
load_lite6_model_map(const std::filesystem::path &package_root,
                     const deployment::RetainedModelMapQuery &query) {
  const deployment::FilesystemVerifiedRetainedModelMapSource source(
      package_root);
  auto model_map = source.get_verified(query);
  if (!model_map.has_value()) {
    throw std::logic_error(
        "durable Lite6 model map was absent on exact point read");
  }
  return std::move(*model_map);
}

template <typename Range, typename Predicate>
[[nodiscard]] const typename Range::value_type &
require_fixture_one(const Range &values, Predicate predicate,
                    std::string_view description) {
  const auto found = std::find_if(values.begin(), values.end(), predicate);
  if (found == values.end() ||
      std::find_if(std::next(found), values.end(), predicate) != values.end()) {
    throw std::logic_error("retained fixture has no sole " +
                           std::string(description));
  }
  return *found;
}

[[nodiscard]] core::RuntimeParticipantBinding make_fixture_runtime_participant(
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const core::ControllableComponentId &component_id,
    const core::CommandTargetId &target_id,
    std::optional<core::StateAuthorityClass> state_authority_override =
        std::nullopt,
    std::optional<core::ProgramCommandSchemaRefDTO> command_schema =
        std::nullopt,
    std::optional<core::RuntimeEpoch> provider_runtime_epoch = std::nullopt) {
  const auto &graph = bundle.value().cell_graph;
  const auto &component = require_fixture_one(
      graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id;
      },
      "controllable component");
  const auto &control_binding = require_fixture_one(
      graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id &&
               candidate.command_target_id == target_id;
      },
      "component target control binding");
  const auto &control_domain = require_fixture_one(
      graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id == control_binding.control_domain_id;
      },
      "control domain");
  const auto &desired = require_fixture_one(
      bundle.value().desired_control_configurations,
      [&](const auto &candidate) {
        return candidate.control_domain_id ==
                   control_domain.control_domain_id &&
               candidate.control_domain_revision ==
                   control_domain.control_domain_revision;
      },
      "desired control configuration");
  const auto &desired_route = require_fixture_one(
      desired.binding_routes,
      [&](const auto &candidate) {
        return candidate.control_binding_id ==
                   control_binding.control_binding_id &&
               candidate.control_binding_revision ==
                   control_binding.control_binding_revision;
      },
      "desired control route");
  const auto *target = model_map.find_target(component_id, target_id);
  const auto *coordinate_map = model_map.find_binding(
      component_id, target_id, control_binding.control_binding_id);
  if (target == nullptr || coordinate_map == nullptr) {
    throw std::logic_error(
        "retained fixture target has no exact model coordinate map");
  }

  const auto model_instance =
      [&](const core::ModelInstanceId &id) -> const core::ModelInstanceDTO & {
    return require_fixture_one(
        graph.model_instances,
        [&](const auto &candidate) {
          return candidate.model_instance_id == id;
        },
        "model instance");
  };

  std::vector<core::ProgramModelRoleBindingDTO> model_bindings;
  for (const auto &binding : component.model_role_bindings) {
    const auto &model = model_instance(binding.model_instance_id);
    model_bindings.push_back({binding.model_role_id, binding.model_instance_id,
                              model.model_revision});
  }

  std::vector<core::RuntimeModelGroupBindingDTO> model_groups;
  for (const auto &group : target->model_groups) {
    model_groups.push_back({group.model_group_role_id, group.model_instance_id,
                            group.compiled_group_name,
                            group.expanded_compiled_joint_names});
  }

  std::vector<core::ProgramFrameRoleBindingDTO> frame_bindings;
  for (const auto &resolution : target->frames) {
    std::visit(
        [&](const auto &frame) {
          using Frame = std::decay_t<decltype(frame)>;
          if constexpr (std::is_same_v<
                            Frame,
                            deployment::CompositionComponentModelFrame>) {
            if (frame.controllable_component_id != component_id ||
                frame.frame_role_id != resolution.frame_role_id) {
              throw std::logic_error(
                  "retained fixture target frame ownership differs");
            }
            frame_bindings.push_back(
                {resolution.frame_role_id, frame.model_instance_id,
                 frame.model_revision, frame.compiled_frame_name});
          }
        },
        resolution.frame);
  }

  std::vector<core::RuntimeResourceBindingDTO> resources;
  for (const auto &binding : control_binding.resource_bindings) {
    const auto &row = require_fixture_one(
        coordinate_map->resources,
        [&](const auto &candidate) {
          return candidate.resource_id == binding.resource_id;
        },
        "resource coordinate row");
    if (row.resource_role_id != binding.resource_role_id ||
        row.coordinates.empty() || row.coordinates.front().derived_mimic) {
      throw std::logic_error(
          "retained fixture direct resource coordinate differs");
    }
    const auto &coordinate = row.coordinates.front();
    std::optional<core::CoordinateKind> kind;
    if (coordinate.coordinate_kind ==
            deployment::UrdfCoordinateKind::kAngular &&
        coordinate.unit == deployment::CompositionCoordinateUnit::kRadians) {
      kind = core::CoordinateKind::kAngular;
    } else if (coordinate.coordinate_kind ==
                   deployment::UrdfCoordinateKind::kLinear &&
               coordinate.unit ==
                   deployment::CompositionCoordinateUnit::kMeters) {
      kind = core::CoordinateKind::kLinear;
    }
    if (!kind.has_value()) {
      throw std::logic_error(
          "retained fixture resource has no supported canonical unit");
    }
    resources.push_back({binding.resource_role_id, binding.resource_id,
                         coordinate.source_joint_name,
                         coordinate.compiled_joint_name, *kind});
  }

  std::vector<core::ProgramCollisionBodyRoleBindingDTO> collision_bodies;
  for (const auto &body : target->collision_bodies) {
    collision_bodies.push_back(
        {body.collision_body_role_id, body.model_instance_id,
         model_instance(body.model_instance_id).model_revision,
         body.compiled_link_names});
  }

  std::set<core::RuntimeUnitId> runtime_unit_ids(
      component.runtime_unit_ids.begin(), component.runtime_unit_ids.end());
  runtime_unit_ids.insert(control_domain.runtime_unit_ids.begin(),
                          control_domain.runtime_unit_ids.end());
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  for (const auto &runtime_unit_id : runtime_unit_ids) {
    runtime_epochs.push_back(
        {runtime_unit_id,
         core::RuntimeEpoch("epoch." + runtime_unit_id.value())});
  }

  std::optional<core::RuntimeScaleRouteQualificationDTO>
      runtime_scale_qualification;
  const auto qualification = std::find_if(
      control_binding.runtime_scale_route_qualifications.begin(),
      control_binding.runtime_scale_route_qualifications.end(),
      [&](const auto &candidate) {
        return candidate.control_route_id == desired_route.control_route_id;
      });
  if (qualification !=
      control_binding.runtime_scale_route_qualifications.end()) {
    runtime_scale_qualification = *qualification;
  }

  std::vector<core::RuntimeCapabilityProviderPathDTO> supporting_providers;
  if (control_binding.capability_id ==
      core::CapabilityId("execution.trajectory.execute")) {
    const auto &planner = require_fixture_one(
        bundle.value().capability_provider_bindings,
        [](const auto &candidate) {
          return candidate.capability_id ==
                 core::CapabilityId("planning.motion.joint_goal");
        },
        "joint-planner capability provider");
    supporting_providers.push_back(
        {planner.capability_id, planner.capability_definition_revision,
         planner.capability_provider_binding_id,
         planner.capability_provider_binding_revision,
         planner.provider_instance_id, planner.provider_manifest_id,
         planner.provider_manifest_revision,
         core::RuntimeEpoch("epoch." + planner.provider_instance_id.value())});
  }

  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("i6.aggregate.state.fixture"),
      core::ProgramRevision(std::string(kZeroDigest)),
      core::ProgramBindingId("i6.aggregate.state.binding"),
      bundle.retained_bundle_id(),
      graph.graph_revision,
      core::ComponentRoleId("robot"),
      core::TargetRoleId(target_id.value()),
      component.controllable_component_id,
      component.component_type_id,
      component.component_type_revision,
      target_id,
      control_binding.capability_id,
      control_binding.capability_definition_revision,
      std::move(command_schema),
      std::move(model_bindings),
      std::move(model_groups),
      std::move(frame_bindings),
      std::move(resources),
      std::move(collision_bodies),
      core::RuntimeControlPathDTO{
          control_domain.control_domain_id,
          control_domain.control_domain_revision,
          control_binding.control_binding_id,
          control_binding.control_binding_revision,
          control_binding.control_interface_contract_id,
          control_binding.control_interface_revision,
          control_binding.controller_topology_id,
          control_binding.topology_revision,
          desired_route.control_route_id,
          desired.activation_set_id,
          core::ActiveControlConfigurationId("active.native"),
          core::ActiveControlConfigurationGeneration(1U),
          control_binding.provider_instance_id,
          control_binding.provider_manifest_id,
          control_binding.provider_manifest_revision,
          provider_runtime_epoch.value_or(core::RuntimeEpoch(
              "epoch." + control_binding.provider_instance_id.value())),
          control_binding.state_authority_id,
          state_authority_override.value_or(
              control_binding.state_authority_class),
          control_binding.stop_evidence_policy_id,
          control_binding.stop_evidence_policy_revision,
          control_binding.containment_granularity,
          std::move(runtime_scale_qualification),
      },
      std::nullopt,
      std::move(supporting_providers),
      std::move(runtime_epochs),
  };
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error("retained fixture runtime participant failed: " +
                           sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::ResolvedExecutionBindingDTO make_fixture_execution_binding(
    const core::RuntimeParticipantBinding &runtime,
    const core::FrameRoleId &base_role = core::FrameRoleId("base"),
    const core::FrameRoleId &tcp_role = core::FrameRoleId("tcp")) {
  const auto &value = runtime.value();
  if (!value.control_path.has_value() || value.model_groups.size() != 1U ||
      value.supporting_capability_provider_paths.size() != 1U) {
    throw std::logic_error(
        "fixture runtime participant has no sole motion/control path");
  }
  const auto &control = *value.control_path;
  const auto &planner = value.supporting_capability_provider_paths.front();
  const auto &group = value.model_groups.front();
  const auto base = require_fixture_one(
      value.frame_bindings,
      [&](const auto &candidate) {
        return candidate.frame_role_id == base_role;
      },
      "base frame binding");
  const auto tcp = require_fixture_one(
      value.frame_bindings,
      [&](const auto &candidate) {
        return candidate.frame_role_id == tcp_role;
      },
      "tcp frame binding");
  const auto model_revision = [&](const core::ModelInstanceId &model_id) {
    return require_fixture_one(
               value.model_bindings,
               [&](const auto &candidate) {
                 return candidate.model_instance_id == model_id;
               },
               "execution model binding")
        .model_revision;
  };
  std::vector<core::PlannerModelBindingDTO> models;
  for (const auto &model : value.model_bindings) {
    models.push_back({model.model_instance_id, model.model_revision});
  }
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  std::vector<core::PlannerRuntimeUnitBindingDTO> runtime_units;
  for (const auto &runtime_unit : value.runtime_unit_epochs) {
    runtime_units.push_back(
        {runtime_unit.runtime_unit_id, runtime_unit.runtime_epoch});
  }
  return core::ResolvedExecutionBindingDTO{
      core::OperationId("operation:reference-resolution"),
      core::OperationParticipantId("participant:reference-resolution"),
      core::ControllerGoalId("goal:reference-resolution"),
      core::ControllerGoalGeneration(1U),
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      core::ProgramRunId("run:reference-resolution"),
      value.controllable_component_id,
      value.component_type_id,
      value.component_type_revision,
      value.command_target_id,
      value.capability_id,
      value.capability_definition_revision,
      core::PlannerCapabilityBindingDTO{
          planner.capability_id, planner.capability_definition_revision,
          planner.capability_provider_binding_id,
          planner.capability_provider_binding_revision,
          planner.provider_instance_id, planner.provider_manifest_id,
          planner.provider_manifest_revision, planner.provider_runtime_epoch},
      std::move(models),
      core::PlannerModelGroupBindingDTO{
          group.model_group_role_id, group.model_instance_id,
          model_revision(group.model_instance_id), group.compiled_group_name,
          std::move(compiled_joint_names)},
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      control.control_interface_contract_id,
      control.control_interface_revision,
      control.controller_topology_id,
      control.topology_revision,
      control.control_route_id,
      control.activation_set_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation,
      control.provider_instance_id,
      control.provider_manifest_id,
      control.provider_manifest_revision,
      control.provider_runtime_epoch,
      std::move(runtime_units),
      std::move(resources),
      core::ResolvedComponentModelFrameDTO{
          value.controllable_component_id, base.model_instance_id,
          base.frame_role_id, base.compiled_frame_name, base.model_revision},
      core::ResolvedComponentModelFrameDTO{
          value.controllable_component_id, tcp.model_instance_id,
          tcp.frame_role_id, tcp.compiled_frame_name, tcp.model_revision},
      {},
      {},
      control.runtime_scale_qualification,
      value.retained_bundle_id,
      value.graph_revision,
      value.target_role_id,
      value.frame_bindings,
      value.collision_body_bindings};
}

[[nodiscard]] core::ObservedStateSnapshotDTO
make_fixture_state(const core::ObservedStateQueryContextDTO &context,
                   const std::vector<double> &positions_si,
                   std::uint64_t sequence, std::int64_t timestamp_ns) {
  if (positions_si.size() != context.coordinate_kinds.size()) {
    throw std::logic_error("fixture state dimension differs from context");
  }
  std::vector<core::CoordinatePositionDTO> positions;
  positions.reserve(positions_si.size());
  for (std::size_t index = 0U; index < positions_si.size(); ++index) {
    if (context.coordinate_kinds[index] == core::CoordinateKind::kAngular) {
      positions.emplace_back(core::AngularPositionDTO{positions_si[index]});
    } else {
      positions.emplace_back(core::LinearPositionDTO{positions_si[index]});
    }
  }
  core::ObservedStateSourceKind source_kind =
      core::ObservedStateSourceKind::kJointState;
  if (context.state_authority_class == core::StateAuthorityClass::kCommanded) {
    source_kind = core::ObservedStateSourceKind::kCommandedState;
  } else if (context.state_authority_class ==
             core::StateAuthorityClass::kPredicted) {
    source_kind = core::ObservedStateSourceKind::kPredictedState;
  }
  return {
      context.controllable_component_id,
      context.command_target_id,
      context.control_domain_id,
      context.control_domain_revision,
      context.control_binding_id,
      context.control_binding_revision,
      context.provider_instance_id,
      context.provider_manifest_id,
      context.provider_manifest_revision,
      context.controller_topology_id,
      context.topology_revision,
      context.control_route_id,
      context.active_control_configuration_id,
      context.active_control_configuration_generation,
      context.resources,
      sequence,
      context.joint_names,
      std::move(positions),
      std::nullopt,
      "fixture.state." + context.command_target_id.value(),
      context.runtime_epoch,
      source_kind,
      timestamp_ns,
      timestamp_ns,
      context.state_authority_id,
      context.state_authority_class,
      1U,
      context.freshness_budget_ns,
      true,
  };
}

[[nodiscard]] RoboPlanCandidateBindingDTO
make_candidate_binding(const core::RuntimeParticipantBinding &runtime) {
  const auto &value = runtime.value();
  if (value.model_groups.size() != 1U) {
    throw std::logic_error("fixture motion target has no sole model group");
  }
  const auto &group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &candidate) {
        return candidate.model_instance_id == group.model_instance_id;
      });
  if (model == value.model_bindings.end()) {
    throw std::logic_error("fixture motion group has no model revision");
  }
  std::vector<core::ResourceId> resources;
  std::vector<std::string> joint_names;
  resources.reserve(value.resources.size());
  joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    joint_names.push_back(resource.compiled_resource_name);
  }
  std::vector<core::ResolvedFrameDTO> frames;
  frames.reserve(value.frame_bindings.size());
  for (const auto &frame : value.frame_bindings) {
    frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id,
        frame.frame_role_id, frame.compiled_frame_name, frame.model_revision});
  }
  return RoboPlanCandidateBindingDTO{
      value.controllable_component_id, value.command_target_id,
      group.model_instance_id,         model->model_revision,
      group.compiled_group_name,       std::move(resources),
      std::move(joint_names),          std::move(frames)};
}

[[nodiscard]] core::OperationIntentDTO
make_motion_operation(const core::RuntimeParticipantBinding &binding,
                      const core::RequestOwnerEpoch &owner_epoch,
                      const core::SceneRevision &scene_revision) {
  const auto &value = binding.value();
  std::vector<std::string> source_joint_names;
  source_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    if (resource.coordinate_kind != core::CoordinateKind::kAngular ||
        !resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      throw std::logic_error(
          "fixture motion target has no source angular joint");
    }
    source_joint_names.push_back(*resource.source_joint_name);
  }
  std::vector<double> goal_positions(value.resources.size(), 0.0);
  if (!goal_positions.empty()) {
    goal_positions.front() = 0.05;
  }
  return core::OperationIntentDTO{
      core::PublicRequestContextDTO{core::RequestId("request.native.motion.1"),
                                    owner_epoch, "retained_product_consumer",
                                    std::nullopt},
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      core::ProgramRunId("run.native.motion.1"),
      value.graph_revision,
      value.retained_bundle_id,
      scene_revision,
      {core::OperationParticipantIntentDTO{
          value.component_role_id, value.target_role_id, value.capability_id,
          std::nullopt,
          core::MotionParticipantIntentDTO{
              core::MoveId("move.native.retained-projection"),
              core::FrameRoleId("base"),
              core::FrameRoleId("tcp"),
              core::TolerancesDTO{1.0e-4, 5.0e-3, 1.0e-2},
              core::SpeedProfileDTO{0.5, 0.5, std::nullopt},
              core::TimeoutsDTO{3.0, 5.0},
              core::CacheMode::kDefault,
              core::PlanningProfileDTO::kFirstFeasible,
              core::MotionTargetModeDTO::kAbsolute,
              std::nullopt,
              std::nullopt,
              core::JointGoalDTO{std::move(source_joint_names),
                                 std::move(goal_positions)},
              std::nullopt,
              {},
              {}}}},
      core::OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] core::OperationIntentDTO
make_gripper_operation(const core::RuntimeParticipantBinding &binding,
                       const core::RequestOwnerEpoch &owner_epoch) {
  const auto &value = binding.value();
  return core::OperationIntentDTO{
      core::PublicRequestContextDTO{core::RequestId("request.native.gripper.1"),
                                    owner_epoch, "retained_product_consumer",
                                    std::nullopt},
      value.program_id,
      value.program_revision,
      value.program_binding_id,
      core::ProgramRunId("run.native.gripper.1"),
      value.graph_revision,
      value.retained_bundle_id,
      std::nullopt,
      {core::OperationParticipantIntentDTO{
          value.component_role_id, value.target_role_id, value.capability_id,
          std::nullopt,
          core::CapabilityCommandIntentDTO{
              core::CommandSchemaId(
                  std::string(core::kGripperPositionCommandSchemaId)),
              core::CommandSchemaRevision(
                  std::string(core::kGripperPositionCommandSchemaRevision)),
              core::GripperPositionCommandValueDTO{
                  core::LinearPositionDTO{0.004}, std::nullopt},
              1.0,
              {core::CommandDurationOptionDTO{
                  core::CommandOptionId("command_duration_s"), 0.0}}}}},
      core::OperationCoordinationPolicy::kIndependent};
}

void expect_exact_planning_reopen(
    const deployment::VerifiedRetainedPlanningModel &first,
    const deployment::VerifiedRetainedPlanningModel &second) {
  EXPECT_EQ(first.query(), second.query());
  EXPECT_EQ(first.urdf_bytes(), second.urdf_bytes());
  EXPECT_EQ(first.srdf_bytes(), second.srdf_bytes());
  ASSERT_EQ(first.resources().size(), second.resources().size());
  for (std::size_t index = 0U; index < first.resources().size(); ++index) {
    const auto &left = first.resources()[index];
    const auto &right = second.resources()[index];
    EXPECT_EQ(left.logical_uri(), right.logical_uri());
    EXPECT_EQ(left.media_type(), right.media_type());
    EXPECT_EQ(left.byte_size(), right.byte_size());
    EXPECT_EQ(left.content_digest(), right.content_digest());
    EXPECT_EQ(left.content_addressed_store_key(),
              right.content_addressed_store_key());
    EXPECT_EQ(left.bytes(), right.bytes());
  }
}

struct Lite6SceneFacts final {
  std::size_t joint_count;
  std::size_t configuration_size;
  std::size_t velocity_size;
  std::size_t frame_count;
  std::size_t geometry_count;
  std::size_t collision_pair_count;
  std::vector<std::string> independent_joint_names;
  std::vector<std::string> mimic_expanded_joint_names;

  friend bool operator==(const Lite6SceneFacts &,
                         const Lite6SceneFacts &) = default;
};

[[nodiscard]] Lite6SceneFacts
require_meaningful_lite6_scene(const roboplan::Scene &scene) {
  const auto &model = scene.getModel();
  const auto independent = scene.getJointNames();
  const auto with_mimics = scene.getJointNamesWithMimics();
  const auto &collision_model = scene.getCollisionModel();

  EXPECT_GT(model.njoints, 1);
  EXPECT_GE(model.nq, 7);
  EXPECT_GE(model.nv, 7);
  EXPECT_GE(model.nframes, 10);
  EXPECT_GE(independent.size(), 7U);
  EXPECT_GT(with_mimics.size(), independent.size());
  EXPECT_GE(collision_model.geometryObjects.size(), 10U);
  EXPECT_FALSE(collision_model.collisionPairs.empty());
  EXPECT_EQ(scene.getCurrentJointPositions().size(), model.nq);
  EXPECT_NO_THROW(
      scene.computeCollisionDistances(scene.getCurrentJointPositions()));

  return {
      static_cast<std::size_t>(model.njoints),
      static_cast<std::size_t>(model.nq),
      static_cast<std::size_t>(model.nv),
      static_cast<std::size_t>(model.nframes),
      collision_model.geometryObjects.size(),
      collision_model.collisionPairs.size(),
      independent,
      with_mimics,
  };
}

[[nodiscard]] std::filesystem::path
staged_resource_path(const RetainedPlanningModelStage &stage,
                     std::string_view logical_uri) {
  constexpr std::string_view kAssetPrefix = "asset://";
  if (!logical_uri.starts_with(kAssetPrefix)) {
    throw std::logic_error("verified planning resource is not an asset URI");
  }
  return stage.staging_root() /
         std::string(logical_uri.substr(kAssetPrefix.size()));
}

[[nodiscard]] std::string
compiled_symbol(const deployment::VerifiedRetainedModelMap &model_map,
                deployment::OccurrenceSymbolKind kind,
                std::string_view source_name,
                std::string_view scope_name = {}) {
  const auto found =
      std::find_if(model_map.symbols().begin(), model_map.symbols().end(),
                   [&](const deployment::OccurrenceSymbolRewrite &rewrite) {
                     return rewrite.source.model_instance_id.value() ==
                                "lite6_fixture_instance" &&
                            rewrite.source.kind == kind &&
                            rewrite.source.source_name == source_name &&
                            rewrite.source.scope_name == scope_name;
                   });
  if (found == model_map.symbols().end()) {
    throw std::logic_error(
        "retained Lite6 model map is missing source symbol " +
        std::string(source_name));
  }
  return found->compiled_name;
}

void expect_total_retained_model_map_semantics(
    const deployment::VerifiedRetainedModelMap &model_map) {
  using SourceKey = std::tuple<std::string, deployment::OccurrenceSymbolKind,
                               std::string, std::string>;
  std::set<SourceKey> source_keys;
  std::set<std::string, std::less<>> compiled_names;
  for (const auto &rewrite : model_map.symbols()) {
    EXPECT_TRUE(source_keys
                    .emplace(rewrite.source.model_instance_id.value(),
                             rewrite.source.kind, rewrite.source.scope_name,
                             rewrite.source.source_name)
                    .second);
    EXPECT_TRUE(compiled_names.emplace(rewrite.compiled_name).second);
  }
  ASSERT_EQ(source_keys.size(), model_map.symbols().size());
  ASSERT_EQ(compiled_names.size(), model_map.symbols().size());

  for (const auto &reference : model_map.references()) {
    const auto target =
        std::find_if(model_map.symbols().begin(), model_map.symbols().end(),
                     [&](const deployment::OccurrenceSymbolRewrite &candidate) {
                       return candidate.source == reference.source_target;
                     });
    ASSERT_NE(target, model_map.symbols().end());
    EXPECT_EQ(target->compiled_name, reference.compiled_target_name);
  }

  std::set<std::pair<std::string, std::size_t>> coordinate_keys;
  for (const auto &coordinate : model_map.coordinates()) {
    EXPECT_TRUE(coordinate_keys
                    .emplace(coordinate.model_instance_id.value(),
                             coordinate.source_joint_index)
                    .second);
    EXPECT_EQ(coordinate.compiled_joint_name,
              compiled_symbol(model_map,
                              deployment::OccurrenceSymbolKind::kJoint,
                              coordinate.source_joint_name));
  }
  EXPECT_EQ(coordinate_keys.size(), model_map.coordinates().size());
}

void expect_exact_retained_planning_resources(
    const deployment::VerifiedRetainedPlanningModel &retained) {
  std::set<std::string, std::less<>> retained_uris;
  for (const auto &resource : retained.resources()) {
    EXPECT_TRUE(
        retained_uris.emplace(std::string(resource.logical_uri())).second);
    EXPECT_EQ(resource.byte_size(), resource.bytes().size());
    EXPECT_EQ(resource.content_digest().value(),
              deployment::sha256_digest(resource.bytes()));
    EXPECT_EQ(resource.content_addressed_store_key().value(),
              "sha256/" + resource.content_digest().value().substr(
                              std::string_view("sha256:").size()));
  }
}

[[nodiscard]] double mimic_expanded_position(const roboplan::Scene &scene,
                                             std::string_view joint_name) {
  const auto names = scene.getJointNamesWithMimics();
  const auto values = scene.getCurrentJointPositionsWithMimics();
  const auto found = std::find(names.begin(), names.end(), joint_name);
  if (found == names.end()) {
    throw std::logic_error("RoboPlan mimic-expanded state is missing " +
                           std::string(joint_name));
  }
  return values[static_cast<Eigen::Index>(std::distance(names.begin(), found))];
}

void expect_exact_retained_and_roboplan_mimic_semantics(
    const deployment::VerifiedRetainedModelMap &model_map,
    roboplan::Scene &scene) {
  for (std::size_t index = 0U; index < model_map.coordinates().size();
       ++index) {
    const auto &coordinate = model_map.coordinates()[index];
    EXPECT_EQ(coordinate.model_instance_id.value(), "lite6_fixture_instance");
    EXPECT_EQ(coordinate.source_joint_index, index);
    EXPECT_EQ(coordinate.compiled_joint_name,
              compiled_symbol(model_map,
                              deployment::OccurrenceSymbolKind::kJoint,
                              coordinate.source_joint_name));
    if (coordinate.mimic_source_compiled_joint_name.has_value()) {
      const auto master = std::find_if(
          model_map.coordinates().begin(), model_map.coordinates().end(),
          [&](const deployment::CompositionModelCoordinate &candidate) {
            return candidate.model_instance_id ==
                       coordinate.model_instance_id &&
                   candidate.compiled_joint_name ==
                       *coordinate.mimic_source_compiled_joint_name;
          });
      ASSERT_NE(master, model_map.coordinates().end());
      EXPECT_NE(master->source_joint_name, coordinate.source_joint_name);
    }
  }

  const std::string master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                      "right_finger_joint");
  const std::string follower = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kJoint, "left_finger_joint");
  const std::string gripper_group = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kSrdfGroup, "lite_gripper");
  EXPECT_LT(master, follower);
  const auto follower_coordinate = std::find_if(
      model_map.coordinates().begin(), model_map.coordinates().end(),
      [&follower](const deployment::CompositionModelCoordinate &candidate) {
        return candidate.compiled_joint_name == follower;
      });
  ASSERT_NE(follower_coordinate, model_map.coordinates().end());
  ASSERT_TRUE(
      follower_coordinate->mimic_source_compiled_joint_name.has_value());
  EXPECT_EQ(*follower_coordinate->mimic_source_compiled_joint_name, master);
  EXPECT_DOUBLE_EQ(follower_coordinate->mimic_multiplier, 1.0);
  EXPECT_DOUBLE_EQ(follower_coordinate->mimic_offset, 0.0);

  const auto group = scene.getJointGroupInfo(gripper_group);
  ASSERT_TRUE(group.has_value()) << group.error();
  EXPECT_EQ(group->joint_names, (std::vector<std::string>{follower, master}));
  EXPECT_EQ(static_cast<std::size_t>(group->q_indices.size()), 1U);
  const auto independent = scene.getJointNames();
  const auto expanded = scene.getJointNamesWithMimics();
  std::vector<std::string> expected_independent;
  for (int index = 1; index <= 6; ++index) {
    expected_independent.push_back(
        compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                        "joint" + std::to_string(index)));
  }
  expected_independent.push_back(master);
  auto expected_expanded = expected_independent;
  expected_expanded.push_back(follower);
  EXPECT_EQ(scene.getModel().nq, 7);
  EXPECT_EQ(scene.getModel().nv, 7);
  EXPECT_EQ(independent.size(), 7U);
  EXPECT_EQ(expanded.size(), 8U);
  EXPECT_EQ(independent, expected_independent);
  EXPECT_EQ(expanded, expected_expanded);
  EXPECT_EQ(std::count(independent.begin(), independent.end(), master), 1);
  EXPECT_EQ(std::count(independent.begin(), independent.end(), follower), 0);
  EXPECT_EQ(std::count(expanded.begin(), expanded.end(), master), 1);
  EXPECT_EQ(std::count(expanded.begin(), expanded.end(), follower), 1);

  auto positions = scene.getCurrentJointPositions();
  const auto master_id = scene.getModel().getJointId(master);
  ASSERT_LT(master_id, scene.getModel().idx_qs.size());
  positions[scene.getModel().idx_qs[master_id]] = 0.004;
  scene.setJointPositions(positions);
  EXPECT_NEAR(mimic_expanded_position(scene, master), 0.004, 1e-12);
  EXPECT_NEAR(mimic_expanded_position(scene, follower), 0.004, 1e-12);
}

[[nodiscard]] deployment::UrdfJointIr
synthetic_joint(std::string name, std::string parent_link,
                std::string child_link, deployment::UrdfJointType type,
                std::optional<std::string> mimic_master = std::nullopt) {
  deployment::UrdfJointIr joint{};
  joint.name = std::move(name);
  joint.parent_link_name = std::move(parent_link);
  joint.child_link_name = std::move(child_link);
  joint.type = type;
  joint.coordinate_kind = type == deployment::UrdfJointType::kFixed
                              ? deployment::UrdfCoordinateKind::kNone
                              : deployment::UrdfCoordinateKind::kLinear;
  if (mimic_master.has_value()) {
    joint.mimic = deployment::UrdfMimicIr{
        .joint_name = std::move(*mimic_master),
        .authored_multiplier = 1.0,
        .authored_offset = 0.0,
        .multiplier = 1.0,
        .offset = 0.0,
    };
  }
  return joint;
}

[[nodiscard]] std::string synthetic_mf2_name(std::size_t rank,
                                             std::string_view source_name) {
  return deployment::detail::make_mf2_generated_joint_name(
      rank, {
                {"fixture", "pinocchio_cross_branch"},
                {"source", source_name},
            });
}

[[nodiscard]] deployment::RetainedProductArtifactQuery artifact_query(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    std::string_view role) {
  const auto &product = admission.retained_bundle()
                            .value()
                            .compiled_products.planning_collision_model;
  return {admission.retained_bundle_id(), product.product_id,
          product.product_kind, core::ArtifactRoleId(std::string(role))};
}

[[nodiscard]] deployment::VerifiedRetainedProductArtifact require_artifact(
    const deployment::IVerifiedRetainedProductArtifactSource &source,
    const deployment::RetainedProductArtifactQuery &query) {
  std::optional<deployment::VerifiedRetainedProductArtifact> artifact =
      source.get_verified(query);
  if (!artifact.has_value()) {
    throw std::runtime_error(
        "canonical-empty retained product artifact is absent");
  }
  return std::move(*artifact);
}

void expect_canonical_empty_scene(const roboplan::Scene &scene) {
  ASSERT_EQ(scene.getModel().njoints, 1);
  ASSERT_EQ(scene.getModel().names.size(), 1U);
  EXPECT_EQ(scene.getModel().names.front(), "universe");
  ASSERT_EQ(scene.getModel().nframes, 2);
  const auto cell_frame_id =
      scene.getModel().getFrameId(std::string(kCanonicalCellFrame));
  ASSERT_EQ(cell_frame_id, 1U);
  const auto &cell_frame = scene.getModel().frames[cell_frame_id];
  EXPECT_EQ(cell_frame.parentJoint, 0U);
  EXPECT_TRUE(cell_frame.placement.toHomogeneousMatrix().isApprox(
      Eigen::Matrix4d::Identity()));
  EXPECT_EQ(scene.getModel().nq, 0);
  EXPECT_EQ(scene.getModel().nv, 0);
  EXPECT_TRUE(scene.getJointNames().empty());
  EXPECT_TRUE(scene.getJointNamesWithMimics().empty());
  EXPECT_EQ(scene.getCurrentJointPositions().size(), 0);
  EXPECT_TRUE(scene.getCollisionModel().geometryObjects.empty());
  EXPECT_TRUE(scene.getCollisionModel().collisionPairs.empty());

  const Eigen::VectorXd empty_configuration(0);
  EXPECT_FALSE(scene.hasCollisions(empty_configuration));
  EXPECT_NO_THROW(scene.computeCollisionDistances(empty_configuration));
  EXPECT_TRUE(scene.getCollisionData().distanceResults.empty());
}

TEST(RetainedProductRoboPlanConsumer,
     Mf2CrossBranchTraversalImportsDeeperMasterBeforeFollower) {
  deployment::UrdfDocumentIr source;
  source.robot_name = "mf2_cross_branch";
  source.joints = {
      synthetic_joint("a_follower_branch", "root", "follower_branch",
                      deployment::UrdfJointType::kFixed),
      synthetic_joint("follower", "follower_branch", "follower_tip",
                      deployment::UrdfJointType::kPrismatic, "master"),
      synthetic_joint("z_master_branch", "root", "master_branch",
                      deployment::UrdfJointType::kFixed),
      synthetic_joint("master", "master_branch", "master_tip",
                      deployment::UrdfJointType::kPrismatic),
  };
  const auto rank_rows =
      deployment::detail::dependency_ordered_joint_local_ranks(source);
  const std::map<std::string, std::size_t, std::less<>> ranks(rank_rows.begin(),
                                                              rank_rows.end());
  ASSERT_EQ(ranks.at("z_master_branch"), 0U);
  ASSERT_EQ(ranks.at("a_follower_branch"), 1U);
  const std::string master_branch =
      synthetic_mf2_name(ranks.at("z_master_branch"), "z_master_branch");
  const std::string follower_branch =
      synthetic_mf2_name(ranks.at("a_follower_branch"), "a_follower_branch");
  const std::string master = synthetic_mf2_name(ranks.at("master"), "master");
  const std::string follower =
      synthetic_mf2_name(ranks.at("follower"), "follower");
  ASSERT_LT(master_branch, follower_branch);

  const std::string urdf =
      "<robot name=\"mf2_cross_branch\">"
      "<link name=\"root\"/>"
      "<link name=\"follower_branch\"/>"
      "<joint name=\"" +
      follower_branch +
      "\" type=\"fixed\"><parent link=\"root\"/><child "
      "link=\"follower_branch\"/></joint>"
      "<link name=\"follower_tip\"/>"
      "<joint name=\"" +
      follower +
      "\" type=\"prismatic\"><parent link=\"follower_branch\"/><child "
      "link=\"follower_tip\"/><axis xyz=\"1 0 0\"/><limit lower=\"-1\" "
      "upper=\"1\" effort=\"1\" velocity=\"1\"/><mimic joint=\"" +
      master +
      "\" multiplier=\"1\" offset=\"0\"/></joint>"
      "<link name=\"master_branch\"/>"
      "<joint name=\"" +
      master_branch +
      "\" type=\"fixed\"><parent link=\"root\"/><child "
      "link=\"master_branch\"/></joint>"
      "<link name=\"master_tip\"/>"
      "<joint name=\"" +
      master +
      "\" type=\"prismatic\"><parent link=\"master_branch\"/><child "
      "link=\"master_tip\"/><axis xyz=\"1 0 0\"/><limit lower=\"-1\" "
      "upper=\"1\" effort=\"1\" velocity=\"1\"/></joint>"
      "</robot>";
  const std::string srdf =
      "<robot name=\"mf2_cross_branch\"><group name=\"motion\">"
      "<joint name=\"" +
      follower + "\"/><joint name=\"" + master + "\"/></group></robot>";

  roboplan::Scene scene("mf2_cross_branch", urdf, srdf);
  EXPECT_EQ(scene.getModel().nq, 1);
  EXPECT_EQ(scene.getModel().nv, 1);
  EXPECT_EQ(scene.getJointNames(), (std::vector<std::string>{master}));
  EXPECT_EQ(scene.getJointNamesWithMimics(),
            (std::vector<std::string>{master, follower}));
  const auto group = scene.getJointGroupInfo("motion");
  ASSERT_TRUE(group.has_value()) << group.error();
  EXPECT_EQ(group->joint_names, (std::vector<std::string>{follower, master}));
  EXPECT_EQ(static_cast<std::size_t>(group->q_indices.size()), 1U);
  auto positions = scene.getCurrentJointPositions();
  ASSERT_EQ(positions.size(), 1);
  positions[0] = 0.25;
  scene.setJointPositions(positions);
  EXPECT_NEAR(mimic_expanded_position(scene, master), 0.25, 1e-12);
  EXPECT_NEAR(mimic_expanded_position(scene, follower), 0.25, 1e-12);
}

TEST(RetainedProductRoboPlanConsumer,
     CanonicalEmptyExactBytesYieldDeterministicPlanningCollisionScene) {
  const std::filesystem::path package_root(
      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT);
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource
      admission_source(package_root);
  const auto admission = admission_source.load_verified();
  const deployment::FilesystemVerifiedRetainedProductArtifactSource
      product_source(package_root);

  const auto urdf_query = artifact_query(admission, "urdf");
  const auto srdf_query = artifact_query(admission, "srdf");
  const auto urdf = require_artifact(product_source, urdf_query);
  const auto srdf = require_artifact(product_source, srdf_query);
  EXPECT_EQ(urdf.query(), urdf_query);
  EXPECT_EQ(srdf.query(), srdf_query);
  EXPECT_EQ(urdf.product_contract_id(), srdf.product_contract_id());
  EXPECT_EQ(urdf.product_contract_revision(), srdf.product_contract_revision());
  EXPECT_EQ(urdf.artifact_ref().media_type, "application/xml");
  EXPECT_EQ(srdf.artifact_ref().media_type, "application/xml");

  const roboplan::Scene first_scene(
      admission.cell_id().value(), std::string(urdf.bytes()),
      std::string(srdf.bytes()), std::vector<std::filesystem::path>{});
  expect_canonical_empty_scene(first_scene);

  const deployment::FilesystemCanonicalEmptyRetainedBundleSource
      reopened_admission_source(package_root);
  const auto reopened_admission = reopened_admission_source.load_verified();
  const deployment::FilesystemVerifiedRetainedProductArtifactSource
      reopened_product_source(package_root);
  const auto reopened_urdf = require_artifact(
      reopened_product_source, artifact_query(reopened_admission, "urdf"));
  const auto reopened_srdf = require_artifact(
      reopened_product_source, artifact_query(reopened_admission, "srdf"));

  EXPECT_EQ(reopened_admission.retained_bundle_id(),
            admission.retained_bundle_id());
  EXPECT_EQ(reopened_urdf.query(), urdf.query());
  EXPECT_EQ(reopened_srdf.query(), srdf.query());
  EXPECT_EQ(reopened_urdf.product_contract_id(), urdf.product_contract_id());
  EXPECT_EQ(reopened_srdf.product_contract_id(), srdf.product_contract_id());
  EXPECT_EQ(reopened_urdf.product_contract_revision(),
            urdf.product_contract_revision());
  EXPECT_EQ(reopened_srdf.product_contract_revision(),
            srdf.product_contract_revision());
  EXPECT_EQ(reopened_urdf.artifact_ref(), urdf.artifact_ref());
  EXPECT_EQ(reopened_srdf.artifact_ref(), srdf.artifact_ref());
  EXPECT_EQ(reopened_urdf.bytes(), urdf.bytes());
  EXPECT_EQ(reopened_srdf.bytes(), srdf.bytes());

  const roboplan::Scene reopened_scene(
      reopened_admission.cell_id().value(), std::string(reopened_urdf.bytes()),
      std::string(reopened_srdf.bytes()), std::vector<std::filesystem::path>{});
  expect_canonical_empty_scene(reopened_scene);
  EXPECT_EQ(reopened_scene.getModel().nframes, first_scene.getModel().nframes);
  EXPECT_EQ(reopened_scene.getCollisionModel().geometryObjects.size(),
            first_scene.getCollisionModel().geometryObjects.size());
}

TEST(RetainedProductRoboPlanConsumer,
     DurableLite6ReopensStagesAndBuildsDeterministicNonEmptyScene) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized =
      deployment::test::materialize_lite6_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  ASSERT_EQ(materialized.bundle->value().provenance.generator_records.size(),
            1U);
  EXPECT_EQ(materialized.bundle->value().provenance.generator_records.front(),
            materialized.input.preparation.generator_record());
  const auto &planning =
      materialized.bundle->value().compiled_products.planning_collision_model;
  const deployment::RetainedPlanningModelQuery query{
      materialized.bundle->retained_bundle_id(), planning.product_id};
  ASSERT_EQ(
      materialized.bundle->value().compiled_products.symbol_role_maps.size(),
      1U);
  const auto &symbol_product =
      materialized.bundle->value().compiled_products.symbol_role_maps.front();
  const deployment::RetainedModelMapQuery model_map_query{
      materialized.bundle->retained_bundle_id(), symbol_product.product_id};

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      projection_source(package_root);
  const auto first_projection = deployment::verify_retained_projection_input(
      *materialized.bundle,
      make_projection_runtime_shadow_snapshot(
          materialized.bundle->value().cell_graph.cell_id),
      projection_source);
  const auto &first = first_projection.planning_model();
  EXPECT_EQ(first_projection.projection_revision(),
            (core::ProjectionRevisionDTO{
                materialized.bundle->retained_bundle_id(),
                materialized.bundle->value().cell_graph.graph_revision,
                first_projection.logical_scene().shared()->revision}));
  const auto model_map = load_lite6_model_map(package_root, model_map_query);
  expect_total_retained_model_map_semantics(model_map);
  ASSERT_NO_THROW(model_map.require_same_bundle_as(first));
  ASSERT_EQ(model_map.query().retained_bundle_id,
            first.query().retained_bundle_id);
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto *arm_binding =
      model_map.find_binding(component_id, command_target_id);
  const auto *arm_target =
      model_map.find_target(component_id, command_target_id);
  ASSERT_NE(arm_binding, nullptr);
  ASSERT_NE(arm_target, nullptr);
  ASSERT_FALSE(arm_target->model_groups.empty());
  ASSERT_EQ(arm_binding->resources.size(), 6U);
  ASSERT_FALSE(first.resources().empty());
  expect_exact_retained_planning_resources(first);
  const std::filesystem::path generated_fixture_root(
      MANYFORGE_LITE6_RETAINED_CONSUMER_FIXTURE_ROOT);
  EXPECT_EQ(first.urdf_bytes(),
            deployment::test::read_lite6_fixture_file(generated_fixture_root /
                                                      "retained-model.urdf"));
  EXPECT_EQ(first.srdf_bytes(),
            deployment::test::read_lite6_fixture_file(generated_fixture_root /
                                                      "retained-model.srdf"));

  // This unrelated valid CAS blob must never become an implicit model
  // resource merely because it is present under the package root.
  const deployment::StoredRawArtifact ambient_blob = [&]() {
    deployment::FilesystemRawArtifactStore raw_store(package_root);
    return raw_store.put_if_absent("unreferenced ambient retained bytes");
  }();
  for (const auto &resource : first.resources()) {
    EXPECT_NE(resource.content_digest(), ambient_blob.content_digest);
    EXPECT_NE(resource.bytes(), "unreferenced ambient retained bytes");
  }

  const auto reopened_projection = deployment::verify_retained_projection_input(
      *materialized.bundle,
      make_projection_runtime_shadow_snapshot(
          materialized.bundle->value().cell_graph.cell_id),
      projection_source);
  const auto &reopened = reopened_projection.planning_model();
  EXPECT_EQ(reopened_projection.projection_revision(),
            first_projection.projection_revision());
  EXPECT_EQ(reopened_projection.logical_scene().shared()->identity(),
            first_projection.logical_scene().shared()->identity());
  EXPECT_TRUE(
      core::IsCanonicalEmptyCollisionPolicy(reopened_projection.logical_scene()
                                                .shared()
                                                ->configured_collision_policy));
  EXPECT_TRUE(reopened_projection.logical_scene().shared()->objects.empty());
  EXPECT_TRUE(reopened_projection.logical_scene()
                  .shared()
                  ->carried_object_attachments.empty());
  expect_exact_planning_reopen(first, reopened);

  std::filesystem::path first_stage_root;
  std::string first_loader_urdf;
  Lite6SceneFacts first_scene_facts{};
  {
    auto stage =
        RetainedPlanningModelStage::create_verified(first, staging_parent);
    first_stage_root = stage.staging_root();
    first_loader_urdf = stage.loader_urdf_bytes();
    ASSERT_EQ(stage.package_paths().size(), 1U);
    EXPECT_EQ(stage.package_paths().front(), stage.staging_root());
    EXPECT_EQ(stage.staging_root().parent_path(), staging_parent);
    EXPECT_EQ(stage.loader_urdf_bytes().find("asset://"),
              std::string_view::npos);
    EXPECT_EQ(stage.loader_urdf_bytes().find("file://"),
              std::string_view::npos);
    EXPECT_EQ(stage.loader_urdf_bytes().find("model://"),
              std::string_view::npos);
    EXPECT_EQ(
        stage.loader_urdf_bytes().find("package://manyforge_example_models/"),
        std::string_view::npos);
    for (const auto &resource : first.resources()) {
      EXPECT_TRUE(std::filesystem::is_regular_file(
          staged_resource_path(stage, resource.logical_uri())));
    }

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    try {
      roboplan::Scene scene(
          "retained_lite6", std::string(stage.loader_urdf_bytes()),
          std::string(stage.srdf_bytes()), stage.package_paths());
      first_scene_facts = require_meaningful_lite6_scene(scene);
      expect_exact_retained_and_roboplan_mimic_semantics(model_map, scene);

      const std::string master =
          compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                          "right_finger_joint");
      auto positions = scene.getCurrentJointPositions();
      const auto master_id = scene.getModel().getJointId(master);
      ASSERT_LT(master_id, scene.getModel().idx_qs.size());
      positions[scene.getModel().idx_qs[master_id]] = 0.004;
      scene.setJointPositions(positions);
      EXPECT_NEAR(mimic_expanded_position(scene, master), 0.004, 1e-12);
    } catch (const std::exception &error) {
      ADD_FAILURE() << error.what() << "\n" << first.urdf_bytes();
      return;
    }
  }
  EXPECT_FALSE(std::filesystem::exists(first_stage_root));

  std::filesystem::path reopened_stage_root;
  Lite6SceneFacts reopened_scene_facts{};
  {
    auto stage =
        RetainedPlanningModelStage::create_verified(reopened, staging_parent);
    reopened_stage_root = stage.staging_root();
    EXPECT_NE(reopened_stage_root, first_stage_root);
    EXPECT_EQ(stage.loader_urdf_bytes(), first_loader_urdf);

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    const roboplan::Scene scene(
        "retained_lite6", std::string(stage.loader_urdf_bytes()),
        std::string(stage.srdf_bytes()), stage.package_paths());
    reopened_scene_facts = require_meaningful_lite6_scene(scene);
  }
  EXPECT_EQ(reopened_scene_facts, first_scene_facts);
  EXPECT_FALSE(std::filesystem::exists(reopened_stage_root));

  // Removing a selected staged file must make a fresh real Scene load fail.
  // If Pinocchio/RoboPlan searched an ambient ROS/package location, this
  // could incorrectly keep succeeding.
  std::filesystem::path incomplete_stage_root;
  {
    auto stage =
        RetainedPlanningModelStage::create_verified(reopened, staging_parent);
    incomplete_stage_root = stage.staging_root();
    const auto selected = std::find_if(
        reopened.resources().begin(), reopened.resources().end(),
        [&stage](const deployment::VerifiedRetainedPlanningResource &resource) {
          const std::string package_uri =
              "package://" + std::string(resource.logical_uri().substr(
                                 std::string_view("asset://").size()));
          return stage.loader_urdf_bytes().find(package_uri) !=
                 std::string_view::npos;
        });
    ASSERT_NE(selected, reopened.resources().end());
    const std::filesystem::path selected_path =
        staged_resource_path(stage, selected->logical_uri());
    const std::filesystem::path hidden_path =
        selected_path.string() + ".missing";
    ASSERT_NO_THROW(std::filesystem::rename(selected_path, hidden_path));

    ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
    EXPECT_ANY_THROW(static_cast<void>(roboplan::Scene(
        "retained_lite6_missing_resource",
        std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
        stage.package_paths())));
  }
  EXPECT_FALSE(std::filesystem::exists(incomplete_stage_root));
  EXPECT_TRUE(std::filesystem::is_empty(staging_parent));
}

TEST(RetainedProductCrossConsumer,
     AggregateArmAndGripperStateProjectsExactUnitsMimicsAndFk) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized = deployment::test::
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const deployment::RetainedPlanningModelQuery planning_query{
      materialized.bundle->retained_bundle_id(),
      products.planning_collision_model.product_id};
  const deployment::RetainedModelMapQuery map_query{
      materialized.bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id};
  const auto retained = load_lite6_planning_model(package_root, planning_query);
  const auto model_map = load_lite6_model_map(package_root, map_query);

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id);
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  auto scene = std::make_shared<roboplan::Scene>(
      "retained_lite6_state_projection", std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths());
  auto scene_mutex = std::make_shared<std::mutex>();
  auto projector = RetainedKinematicStateProjector::create_verified(
      scene, scene_mutex, *materialized.bundle, model_map, retained,
      std::vector<core::RuntimeParticipantBinding>{arm_binding,
                                                   gripper_binding},
      1'000'000'000);

  const auto contexts = projector.query_contexts();
  ASSERT_EQ(contexts.size(), 2U);
  const auto arm_context =
      std::find_if(contexts.begin(), contexts.end(), [&](const auto &context) {
        return context.command_target_id == arm_target_id;
      });
  const auto gripper_context =
      std::find_if(contexts.begin(), contexts.end(), [&](const auto &context) {
        return context.command_target_id == gripper_target_id;
      });
  ASSERT_NE(arm_context, contexts.end());
  ASSERT_NE(gripper_context, contexts.end());
  ASSERT_EQ(arm_context->coordinate_kinds.size(), 6U);
  EXPECT_TRUE(std::all_of(
      arm_context->coordinate_kinds.begin(),
      arm_context->coordinate_kinds.end(),
      [](const auto kind) { return kind == core::CoordinateKind::kAngular; }));
  ASSERT_EQ(gripper_context->coordinate_kinds.size(), 1U);
  EXPECT_EQ(gripper_context->coordinate_kinds.front(),
            core::CoordinateKind::kLinear);

  const auto *arm_target = model_map.find_target(component_id, arm_target_id);
  ASSERT_NE(arm_target, nullptr);
  const auto arm_tcp = std::find_if(
      arm_target->frames.begin(), arm_target->frames.end(),
      [](const auto &frame) { return frame.frame_role_id.value() == "tcp"; });
  ASSERT_NE(arm_tcp, arm_target->frames.end());
  std::string arm_tcp_name;
  std::visit(
      [&](const auto &frame) {
        if constexpr (requires { frame.compiled_frame_name; }) {
          arm_tcp_name = frame.compiled_frame_name;
        }
      },
      arm_tcp->frame);
  ASSERT_FALSE(arm_tcp_name.empty());
  const Eigen::Matrix4d zero_tcp =
      scene->forwardKinematics(scene->getCurrentJointPositions(), arm_tcp_name,
                               model_map.cell_frame().compiled_frame_name);

  const std::vector<double> arm_positions{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  const auto arm_state =
      make_fixture_state(*arm_context, arm_positions, 1U, 100);
  const auto arm_result = projector.apply(arm_state, 100);
  ASSERT_TRUE(arm_result.applied())
      << (arm_result.error.has_value() ? arm_result.error->message : "");
  const Eigen::Matrix4d moved_tcp =
      scene->forwardKinematics(scene->getCurrentJointPositions(), arm_tcp_name,
                               model_map.cell_frame().compiled_frame_name);
  EXPECT_FALSE(moved_tcp.isApprox(zero_tcp, 1e-10));

  const auto gripper_state =
      make_fixture_state(*gripper_context, {0.004}, 1U, 110);
  const auto gripper_result = projector.apply(gripper_state, 110);
  ASSERT_TRUE(gripper_result.applied())
      << (gripper_result.error.has_value() ? gripper_result.error->message
                                           : "");
  const Eigen::Matrix4d after_gripper_tcp =
      scene->forwardKinematics(scene->getCurrentJointPositions(), arm_tcp_name,
                               model_map.cell_frame().compiled_frame_name);
  EXPECT_TRUE(after_gripper_tcp.isApprox(moved_tcp, 1e-12));

  const auto arm_read = projector.current_state(
      arm_context->control_binding_id, arm_context->control_binding_revision);
  ASSERT_TRUE(arm_read.ok()) << arm_read.error().message;
  EXPECT_EQ(arm_read.value().positions.size(), arm_positions.size());
  for (std::size_t index = 0U; index < arm_positions.size(); ++index) {
    const auto *angular = std::get_if<core::AngularPositionDTO>(
        &arm_read.value().positions[index]);
    ASSERT_NE(angular, nullptr);
    EXPECT_NEAR(angular->value_rad, arm_positions[index], 1e-12);
  }
  const auto gripper_read =
      projector.current_state(gripper_context->control_binding_id,
                              gripper_context->control_binding_revision);
  ASSERT_TRUE(gripper_read.ok()) << gripper_read.error().message;
  ASSERT_EQ(gripper_read.value().positions.size(), 1U);
  const auto *linear = std::get_if<core::LinearPositionDTO>(
      &gripper_read.value().positions.front());
  ASSERT_NE(linear, nullptr);
  EXPECT_NEAR(linear->value_m, 0.004, 1e-12);

  const auto coordinates = projector.model_coordinate_state();
  ASSERT_TRUE(coordinates.ok()) << coordinates.error().message;
  const std::string gripper_master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                      "right_finger_joint");
  const std::string gripper_follower = compiled_symbol(
      model_map, deployment::OccurrenceSymbolKind::kJoint, "left_finger_joint");
  for (const auto &name : {gripper_master, gripper_follower}) {
    const auto found =
        std::find_if(coordinates.value().begin(), coordinates.value().end(),
                     [&](const auto &coordinate) {
                       return coordinate.compiled_joint_name == name;
                     });
    ASSERT_NE(found, coordinates.value().end());
    const auto *position =
        std::get_if<core::LinearPositionDTO>(&found->position);
    ASSERT_NE(position, nullptr);
    EXPECT_NEAR(position->value_m, 0.004, 1e-12);
  }

  auto foreign = gripper_state;
  foreign.control_binding_revision = arm_context->control_binding_revision;
  const auto rejected = projector.apply(foreign, 120);
  EXPECT_EQ(rejected.status, RetainedStateProjectionStatus::kRejected);
  const auto unchanged =
      projector.current_state(gripper_context->control_binding_id,
                              gripper_context->control_binding_revision);
  ASSERT_TRUE(unchanged.ok()) << unchanged.error().message;
  EXPECT_EQ(unchanged.value(), gripper_read.value());
}

TEST(
    RetainedProductCrossConsumer,
    SceneFrameResolverReadsDirectOrExternalOwnerPublicationNotMutablePlannerState) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  const auto materialized = deployment::test::
      materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.symbol_role_maps.front().product_id});

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto &component = require_fixture_one(
      materialized.bundle->value().cell_graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id;
      },
      "scene-frame controllable component");
  const auto &arm_graph_binding = require_fixture_one(
      materialized.bundle->value().cell_graph.control_bindings,
      [&](const auto &candidate) {
        return candidate.controllable_component_id == component_id &&
               candidate.command_target_id == arm_target_id;
      },
      "scene-frame arm control binding");
  const auto &control_domain = require_fixture_one(
      materialized.bundle->value().cell_graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id ==
               arm_graph_binding.control_domain_id;
      },
      "scene-frame control domain");
  std::set<core::RuntimeUnitId> local_runtime_units(
      component.runtime_unit_ids.begin(), component.runtime_unit_ids.end());
  local_runtime_units.insert(control_domain.runtime_unit_ids.begin(),
                             control_domain.runtime_unit_ids.end());
  ASSERT_EQ(local_runtime_units.size(), 1U);
  const auto local_runtime_unit_id = *local_runtime_units.begin();
  const core::RuntimeEpoch local_runtime_epoch(
      "scene-frame-direct-owner-epoch");
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id,
      core::StateAuthorityClass::kPredicted, std::nullopt, local_runtime_epoch);
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id,
      core::StateAuthorityClass::kPredicted, std::nullopt, local_runtime_epoch);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  auto scene = std::make_shared<roboplan::Scene>(
      "retained_lite6_direct_frame_state",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths());
  auto scene_mutex = std::make_shared<std::mutex>();
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *materialized.bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{arm_binding,
                                                       gripper_binding},
          0));
  auto arm_channel = std::make_shared<RetainedObservedStateChannel>(
      arm_binding, state_projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kPredictedState,
          "test/direct-owner/arm", 0, 1U});
  auto gripper_channel = std::make_shared<RetainedObservedStateChannel>(
      gripper_binding, state_projector,
      RetainedObservedStateChannelConfig{
          core::ObservedStateSourceKind::kPredictedState,
          "test/direct-owner/gripper", 0, 1U});
  ASSERT_TRUE(arm_channel->initialize_from_projection(10).applied());
  ASSERT_TRUE(gripper_channel->initialize_from_projection(10).applied());

  const auto *arm_target = model_map.find_target(component_id, arm_target_id);
  ASSERT_NE(arm_target, nullptr);
  ASSERT_FALSE(arm_target->model_groups.empty());
  auto collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_direct_frame_state", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(),
      arm_target->model_groups.front().compiled_group_name);

  EXPECT_THROW(
      static_cast<void>(RetainedSceneFrameResolver::create_verified(
          collision_model, *materialized.bundle, model_map, retained,
          local_runtime_unit_id, local_runtime_epoch,
          RetainedSceneFrameLocalStateMode::kRequireDirectProviders, {}, 10)),
      std::invalid_argument);

  const std::shared_ptr<const core::IObservedStateProvider> arm_provider(
      arm_channel, arm_channel->addressed_publication());
  const std::shared_ptr<const core::IObservedStateProvider> gripper_provider(
      gripper_channel, gripper_channel->addressed_publication());
  const std::vector<RetainedSceneFrameStateSourceInput> provided_state_sources{
      {arm_channel->query_context(),
       core::ObservedStateSourceKind::kPredictedState,
       arm_provider,
       {}},
      {gripper_channel->query_context(),
       core::ObservedStateSourceKind::kPredictedState,
       gripper_provider,
       {}}};
  auto resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, *materialized.bundle, model_map, retained,
      local_runtime_unit_id, local_runtime_epoch,
      RetainedSceneFrameLocalStateMode::kRequireDirectProviders,
      provided_state_sources, 10);
  ASSERT_NE(resolver, nullptr);

  auto external_resolver = RetainedSceneFrameResolver::create_verified(
      collision_model, *materialized.bundle, model_map, retained,
      core::RuntimeUnitId("scene-frame-external-consumer"),
      core::RuntimeEpoch("scene-frame-external-receiver-epoch"),
      RetainedSceneFrameLocalStateMode::kRequireDirectProviders,
      provided_state_sources, 10);
  ASSERT_NE(external_resolver, nullptr);

  const auto logical_scene = make_projection_runtime_shadow_snapshot(
      materialized.bundle->value().cell_graph.cell_id);
  const core::ProjectionLeaseBindingDTO projection_binding{
      {materialized.bundle->retained_bundle_id(),
       materialized.bundle->value().cell_graph.graph_revision,
       logical_scene.shared()->revision},
      core::ProjectionLeaseId("direct-owner-frame-lease")};
  const core::SceneFrameResolutionQueryDTO query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("tcp")},
      projection_binding};
  const auto neutral = resolver->Resolve(query, logical_scene.shared(), 10);
  ASSERT_TRUE(neutral.ok()) << neutral.error().message;
  const auto external_neutral =
      external_resolver->Resolve(query, logical_scene.shared(), 10);
  ASSERT_TRUE(external_neutral.ok()) << external_neutral.error().message;
  EXPECT_EQ(external_neutral.value(), neutral.value());

  const Eigen::VectorXd planner_state_before =
      scene->getCurrentJointPositions();
  const std::vector<double> moved_positions{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  const std::vector<double> zero_velocities(moved_positions.size(), 0.0);
  const auto published =
      arm_channel->publish_state(moved_positions, zero_velocities, 20, 20);
  ASSERT_TRUE(published.applied())
      << (published.error.has_value() ? published.error->message : "");
  EXPECT_TRUE(scene->getCurrentJointPositions().isApprox(planner_state_before));

  const auto moved = resolver->Resolve(query, logical_scene.shared(), 20);
  ASSERT_TRUE(moved.ok()) << moved.error().message;
  const auto external_moved =
      external_resolver->Resolve(query, logical_scene.shared(), 20);
  ASSERT_TRUE(external_moved.ok()) << external_moved.error().message;
  EXPECT_EQ(external_moved.value(), moved.value());
  EXPECT_FALSE(moved.value().frame_pose_in_cell ==
               neutral.value().frame_pose_in_cell);
  EXPECT_NE(moved.value().resolution_evidence_digest,
            neutral.value().resolution_evidence_digest);
  ASSERT_EQ(moved.value().kinematic_state_path_view.model_states.size(), 1U);
  EXPECT_FALSE(moved.value().kinematic_state_path_view.resource_states.empty());

  const auto exact_binding = make_fixture_execution_binding(arm_binding);
  auto start_publication = arm_channel->addressed_publication()->publication(
      exact_binding.control_binding_id, exact_binding.control_binding_revision);
  ASSERT_TRUE(start_publication.ok()) << start_publication.error().message;
  const auto &start_snapshot = start_publication.value().snapshot;
  core::ControlledStateDTO controlled_start{
      exact_binding.controllable_component_id,
      exact_binding.command_target_id,
      exact_binding.control_domain_id,
      exact_binding.control_domain_revision,
      exact_binding.control_binding_id,
      exact_binding.control_binding_revision,
      exact_binding.controller_topology_id,
      exact_binding.topology_revision,
      exact_binding.control_route_id,
      exact_binding.active_control_configuration_id,
      exact_binding.active_control_configuration_generation,
      start_snapshot.resources,
      start_snapshot.joint_names,
      start_snapshot.positions,
      start_snapshot.velocities};
  auto reference_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 2U,
      core::SceneResourceId("reference-object"), 0.4, 0.05);
  auto reference_scene_ptr =
      std::make_shared<const core::LogicalSceneSnapshotDTO>(reference_scene);
  const core::ProjectionLeaseBindingDTO reference_projection{
      {materialized.bundle->retained_bundle_id(),
       materialized.bundle->value().cell_graph.graph_revision,
       reference_scene.revision},
      core::ProjectionLeaseId("program-reference-resolution-lease")};
  const core::ReferenceDTO tcp_reference(core::ReferenceDTO::Type::kFrameRole,
                                         "tcp");
  ASSERT_FALSE(exact_binding.collision_body_role_bindings.empty());
  const auto &body_role = exact_binding.collision_body_role_bindings.front();
  ASSERT_GT(body_role.compiled_collision_body_names.size(), 1U);
  const core::ReferenceDTO body_reference(
      core::ReferenceDTO::Type::kComponentBodyRole,
      body_role.collision_body_role_id.value());
  const core::ReferenceDTO scene_reference(
      core::ReferenceDTO::Type::kSceneResource, "reference-object");
  const core::ReferenceDTO runtime_reference(
      core::ReferenceDTO::Type::kRuntimeObject, "selected-object");
  const core::Pose3DTO inline_pose{{0.1, -0.2, 0.3}, {0.0, 0.0, 0.0, 1.0}};
  const core::ReferenceDTO inline_reference(inline_pose);
  core::ProgramFrameResolutionCaptureDTO capture{
      exact_binding,
      controlled_start,
      start_publication.value(),
      reference_projection,
      reference_scene_ptr,
      {{core::RuntimeObjectId("selected-object"),
        core::SceneResourceId("reference-object")}},
      {tcp_reference, body_reference, scene_reference, runtime_reference,
       inline_reference},
      20};
  auto captured = resolver->capture_program_frame_resolution_context(capture);
  ASSERT_TRUE(captured.ok()) << captured.error().message;
  ASSERT_TRUE(captured.value().valid());

  const auto resolved_tcp =
      resolver->resolve_reference_pose(tcp_reference, captured.value());
  const auto resolved_body =
      resolver->resolve_reference_pose(body_reference, captured.value());
  const auto resolved_scene =
      resolver->resolve_reference_pose(scene_reference, captured.value());
  const auto resolved_runtime =
      resolver->resolve_reference_pose(runtime_reference, captured.value());
  const auto resolved_inline =
      resolver->resolve_reference_pose(inline_reference, captured.value());
  ASSERT_TRUE(resolved_tcp.ok()) << resolved_tcp.error().message;
  ASSERT_FALSE(resolved_body.ok());
  EXPECT_EQ(resolved_body.error().details.at("reason"),
            "program_component_body_role_spatially_ambiguous");
  ASSERT_TRUE(resolved_scene.ok()) << resolved_scene.error().message;
  ASSERT_TRUE(resolved_runtime.ok()) << resolved_runtime.error().message;
  ASSERT_TRUE(resolved_inline.ok()) << resolved_inline.error().message;
  EXPECT_EQ(resolved_runtime.value(), resolved_scene.value());
  EXPECT_EQ(resolved_inline.value(), inline_pose);

  const core::SceneFrameResolutionQueryDTO base_query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("base")},
      reference_projection};
  const core::SceneFrameResolutionQueryDTO tcp_query{
      core::TargetCarrierSceneFrameRefDTO{component_id, arm_target_id,
                                          core::FrameRoleId("tcp")},
      reference_projection};
  const auto base_pose = resolver->Resolve(base_query, reference_scene_ptr, 20);
  const auto tcp_pose = resolver->Resolve(tcp_query, reference_scene_ptr, 20);
  ASSERT_TRUE(base_pose.ok()) << base_pose.error().message;
  ASSERT_TRUE(tcp_pose.ok()) << tcp_pose.error().message;
  const auto expected_tcp = core::multiply_poses(
      core::inverse_pose(base_pose.value().frame_pose_in_cell),
      tcp_pose.value().frame_pose_in_cell);
  const auto expected_scene = core::multiply_poses(
      core::inverse_pose(base_pose.value().frame_pose_in_cell),
      std::get<core::CellFixedSceneObjectPlacementDTO>(
          reference_scene.objects.front().placement)
          .pose_in_cell);
  const auto expect_pose_near = [](const core::Pose3DTO &actual,
                                   const core::Pose3DTO &expected) {
    EXPECT_NEAR(actual.position_m.x_m, expected.position_m.x_m, 1e-12);
    EXPECT_NEAR(actual.position_m.y_m, expected.position_m.y_m, 1e-12);
    EXPECT_NEAR(actual.position_m.z_m, expected.position_m.z_m, 1e-12);
    EXPECT_NEAR(actual.orientation.x, expected.orientation.x, 1e-12);
    EXPECT_NEAR(actual.orientation.y, expected.orientation.y, 1e-12);
    EXPECT_NEAR(actual.orientation.z, expected.orientation.z, 1e-12);
    EXPECT_NEAR(actual.orientation.w, expected.orientation.w, 1e-12);
  };
  expect_pose_near(resolved_tcp.value(), expected_tcp);
  expect_pose_near(resolved_scene.value(), expected_scene);

  const std::vector<double> later_positions{0.1, -0.6, 0.5, 0.2, 1.2, -0.1};
  ASSERT_TRUE(
      arm_channel->publish_state(later_positions, zero_velocities, 30, 30)
          .applied());
  const auto frozen_tcp =
      resolver->resolve_reference_pose(tcp_reference, captured.value());
  ASSERT_TRUE(frozen_tcp.ok()) << frozen_tcp.error().message;
  EXPECT_EQ(frozen_tcp.value(), resolved_tcp.value());

  auto newer_publication = arm_channel->addressed_publication()->publication(
      exact_binding.control_binding_id, exact_binding.control_binding_revision);
  ASSERT_TRUE(newer_publication.ok()) << newer_publication.error().message;
  auto conflicting_capture = capture;
  conflicting_capture.observed_start_publication = newer_publication.value();
  conflicting_capture.captured_at_ns = 30;
  const auto conflict = resolver->capture_program_frame_resolution_context(
      std::move(conflicting_capture));
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"),
            "program_reference_start_state_mismatch");
}

TEST(RetainedProductCrossConsumer,
     PredictedLite6AggregateComposesAndExecutesAddressedGripperCommand) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized = deployment::test::
      materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, deployment::RetainedPlanningModelQuery{
                        materialized.bundle->retained_bundle_id(),
                        products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, deployment::RetainedModelMapQuery{
                        materialized.bundle->retained_bundle_id(),
                        products.symbol_role_maps.front().product_id});

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId arm_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto &graph = materialized.bundle->value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  const core::RuntimeEpoch local_runtime_epoch(
      "epoch." + graph.runtime_units.front().runtime_unit_id.value());
  const auto arm_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, arm_target_id,
      core::StateAuthorityClass::kPredicted, std::nullopt, local_runtime_epoch);
  const core::ProgramCommandSchemaRefDTO command_schema{
      core::CommandSchemaId(std::string(core::kGripperPositionCommandSchemaId)),
      core::CommandSchemaRevision(
          std::string(core::kGripperPositionCommandSchemaRevision))};
  const auto gripper_binding = make_fixture_runtime_participant(
      *materialized.bundle, model_map, component_id, gripper_target_id,
      core::StateAuthorityClass::kPredicted, command_schema,
      local_runtime_epoch);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  auto scene = std::make_shared<roboplan::Scene>(
      "retained_lite6_native_composition",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths());
  auto scene_mutex = std::make_shared<std::mutex>();
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *materialized.bundle, model_map, retained,
          std::vector<core::RuntimeParticipantBinding>{arm_binding,
                                                       gripper_binding},
          0));

  const auto &arm_group = arm_binding.value().model_groups.front();
  auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_native_composition", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(), arm_group.compiled_group_name);
  auto collision_projector = RetainedCollisionWorldProjector::create_verified(
      shared_collision_model, *materialized.bundle, model_map, retained);
  const auto initial_scene = make_projection_runtime_shadow_snapshot(
      materialized.bundle->value().cell_graph.cell_id);
  ASSERT_FALSE(arm_binding.value().runtime_unit_epochs.empty());
  const auto local_runtime = arm_binding.value().runtime_unit_epochs.front();
  const core::ProjectionNativeRuntimeBindingDTO native_binding{
      core::CapabilityProviderBindingId("lite6.runtime.projection"),
      core::CapabilityProviderBindingRevision("lite6-runtime-projection-v1"),
      local_runtime.runtime_unit_id, local_runtime.runtime_epoch};
  std::int64_t projection_clock =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      std::move(collision_projector), products.planning_collision_model,
      native_binding, *initial_scene.shared(),
      RetainedPlannerSceneProjectionConfig{"retained-lite6-native-runtime", 32U,
                                           32U},
      [&projection_clock] { return projection_clock++; });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  auto projection = std::move(projection_result.value());
  const auto inventory = projection->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(inventory.has_value());

  const auto candidate_binding = make_candidate_binding(arm_binding);
  ProcessContainedCandidateClientConfig candidate_config;
  candidate_config.worker_executable = MANYFORGE_ROBOPLAN_CANDIDATE_WORKER_PATH;
  candidate_config.require_retained_projection = true;
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding,
      std::make_unique<ProcessContainedRoboPlanCandidateClient>(
          RoboPlanWorkerModelDTO{"retained_lite6_native_worker",
                                 stage.loader_urdf_path(),
                                 stage.srdf_path(),
                                 stage.package_paths(),
                                 std::nullopt,
                                 {},
                                 {}},
          candidate_binding, RoboPlanCandidateComputerConfig{.rng_seed = 7U},
          std::move(candidate_config)));
  const core::RequestOwnerEpoch owner_epoch("i6.native.runtime.epoch.1");
  RetainedNativeRuntimeConfig runtime_config{
      owner_epoch,
      materialized.bundle->value().cell_graph.cell_id,
      temporary.path() / std::string(core::kOperationJournalFilename),
      1024U,
      core::CacheConfig{},
      core::ExecutionKernelConfig{},
      RoboPlanExecutionBackendConfig{},
      SimpleSchedulerConfig{},
      10'000'000};
  auto runtime = create_retained_native_runtime(
      RetainedNativeRuntimeInput{
          arm_binding, gripper_binding, scene, scene_mutex, state_projector,
          projection, shared_collision_model, std::move(planner),
          [&](const auto &channels) {
            std::vector<RetainedSceneFrameStateSourceInput> state_sources;
            state_sources.reserve(channels.size());
            for (const auto &channel : channels) {
              state_sources.push_back(RetainedSceneFrameStateSourceInput{
                  channel->query_context(),
                  core::ObservedStateSourceKind::kPredictedState,
                  std::shared_ptr<const core::IObservedStateProvider>(
                      channel, channel->addressed_publication()),
                  {}});
            }
            return RetainedSceneFrameResolver::create_verified(
                shared_collision_model, *materialized.bundle, model_map,
                retained, local_runtime.runtime_unit_id,
                local_runtime.runtime_epoch,
                RetainedSceneFrameLocalStateMode::kRequireDirectProviders,
                std::move(state_sources), 0);
          }},
      std::move(runtime_config));

  ASSERT_NE(runtime, nullptr);
  ASSERT_NE(runtime->addressed_kernel, nullptr);
  ASSERT_NE(runtime->participant_engine, nullptr);
  ASSERT_NE(runtime->motion_state_channel, nullptr);
  ASSERT_NE(runtime->gripper_state_channel, nullptr);
  EXPECT_EQ(runtime->collision_publisher.get(), projection.get());
  EXPECT_TRUE(std::all_of(
      runtime->motion_state_channel->query_context().coordinate_kinds.begin(),
      runtime->motion_state_channel->query_context().coordinate_kinds.end(),
      [](const auto kind) { return kind == core::CoordinateKind::kAngular; }));
  ASSERT_EQ(
      runtime->gripper_state_channel->query_context().coordinate_kinds.size(),
      1U);
  EXPECT_EQ(
      runtime->gripper_state_channel->query_context().coordinate_kinds.front(),
      core::CoordinateKind::kLinear);

  const auto &motion_value = runtime->motion_binding.value();
  const auto joint_names = runtime->kinematics->group_joint_names(
      motion_value.command_target_id.value());
  ASSERT_TRUE(joint_names.ok()) << joint_names.error().message;
  std::vector<std::string> expected_joint_names;
  for (const auto &resource : motion_value.resources) {
    expected_joint_names.push_back(resource.compiled_resource_name);
  }
  EXPECT_EQ(joint_names.value(), expected_joint_names);
  const auto limits = runtime->kinematics->joint_limits(
      motion_value.controllable_component_id.value(),
      motion_value.command_target_id.value());
  ASSERT_TRUE(limits.ok()) << limits.error().message;
  EXPECT_EQ(limits.value().joint_names, joint_names.value());
  const auto base_frame = require_fixture_one(
      motion_value.frame_bindings,
      [](const auto &frame) { return frame.frame_role_id.value() == "base"; },
      "motion base frame");
  const auto tcp_frame = require_fixture_one(
      motion_value.frame_bindings,
      [](const auto &frame) { return frame.frame_role_id.value() == "tcp"; },
      "motion tcp frame");
  const core::RobotStateDTO zero_arm_state{
      joint_names.value(),
      std::vector<double>(joint_names.value().size(), 0.0),
      {}};
  const auto tcp_pose = runtime->kinematics->forward_kinematics(
      core::FkRequestDTO{zero_arm_state, tcp_frame.compiled_frame_name,
                         base_frame.compiled_frame_name});
  ASSERT_TRUE(tcp_pose.ok()) << tcp_pose.error().message;
  std::vector<core::ResourceId> motion_resources;
  for (const auto &resource : motion_value.resources) {
    motion_resources.push_back(resource.resource_id);
  }
  const core::TrajectoryDTO stationary_trajectory{
      std::move(motion_resources),
      joint_names.value(),
      {core::TrajectoryWaypointDTO{
           0.0, std::vector<double>(joint_names.value().size(), 0.0), {}, {}},
       core::TrajectoryWaypointDTO{
           1.0, std::vector<double>(joint_names.value().size(), 0.0), {}, {}}}};
  core::CollisionCancellationToken cancellation;
  const auto validation = runtime->trajectory_validator->validate_non_collision(
      stationary_trajectory, core::ValidationOptionsDTO{0.0, {}, {}, false},
      cancellation);
  ASSERT_TRUE(validation.valid)
      << (validation.error.has_value() ? validation.error->message : "");
  const auto expanded =
      runtime->collision_trajectory_projector->expand_for_collision(
          stationary_trajectory, zero_arm_state);
  ASSERT_TRUE(expanded.ok()) << expanded.error().message;
  ASSERT_EQ(expanded.value().waypoints.size(), 2U);
  EXPECT_EQ(expanded.value().waypoints.front().positions_rad.size(),
            expanded.value().joint_names.size());

  const auto motion_accepted =
      runtime->addressed_kernel->execute(make_motion_operation(
          arm_binding, owner_epoch,
          inventory->projection_binding.projection_revision.scene_revision));
  ASSERT_TRUE(motion_accepted.ok()) << motion_accepted.error().message;
  core::OperationStateKind motion_terminal =
      core::OperationStateKind::kExecuting;
  const auto motion_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::optional<core::OperationStateDTO> motion_state;
  while (std::chrono::steady_clock::now() < motion_deadline) {
    const auto reconciled =
        runtime->addressed_kernel->reconcile_subordinate_state();
    ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
    const auto state = runtime->addressed_kernel->get_operation_state(
        motion_accepted.value().operation_id);
    ASSERT_TRUE(state.ok()) << state.error().message;
    motion_state = state.value();
    motion_terminal = motion_state->state;
    if (motion_terminal == core::OperationStateKind::kCompleted ||
        motion_terminal == core::OperationStateKind::kFailed ||
        motion_terminal == core::OperationStateKind::kInterrupted ||
        motion_terminal == core::OperationStateKind::kInterventionRequired) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(motion_terminal, core::OperationStateKind::kCompleted)
      << (!motion_state.has_value() || motion_state->participants.empty() ||
                  !motion_state->participants.front().terminal_error.has_value()
              ? "motion did not complete"
              : motion_state->participants.front().terminal_error->message);
  ASSERT_TRUE(motion_state.has_value());
  const auto motion_metrics = runtime->participant_engine->get_metrics();
  EXPECT_EQ(motion_metrics.plans_succeeded, 1U);
  EXPECT_EQ(motion_metrics.validations_passed, 1U);
  EXPECT_EQ(motion_metrics.executions_completed, 1U);
  EXPECT_EQ(runtime->cache->size(), 1U);
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats().retained_count,
            0U);
  const auto moved_state =
      runtime->motion_state_channel->current_controlled_state();
  ASSERT_TRUE(moved_state.ok()) << moved_state.error().message;
  ASSERT_FALSE(moved_state.value().positions.empty());
  const auto *first_joint = std::get_if<core::AngularPositionDTO>(
      &moved_state.value().positions.front());
  ASSERT_NE(first_joint, nullptr);
  EXPECT_NEAR(first_joint->value_rad, 0.05, 1.0e-6);

  const auto motion_acknowledged =
      runtime->addressed_kernel->acknowledge_completed(
          core::OperationControlRequestDTO{
              core::PublicRequestContextDTO{
                  core::RequestId("request.native.motion.ack.1"), owner_epoch,
                  "retained_product_consumer", std::nullopt},
              motion_accepted.value().operation_id,
              motion_state->operation_revision,
              "release completed motion resources"});
  ASSERT_TRUE(motion_acknowledged.ok()) << motion_acknowledged.error().message;

  const auto accepted = runtime->addressed_kernel->execute(
      make_gripper_operation(gripper_binding, owner_epoch));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  core::OperationStateKind terminal_state =
      core::OperationStateKind::kExecuting;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto reconciled =
        runtime->addressed_kernel->reconcile_subordinate_state();
    ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
    const auto state = runtime->addressed_kernel->get_operation_state(
        accepted.value().operation_id);
    ASSERT_TRUE(state.ok()) << state.error().message;
    terminal_state = state.value().state;
    if (terminal_state == core::OperationStateKind::kCompleted) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(terminal_state, core::OperationStateKind::kCompleted);
  const auto gripper_state =
      runtime->gripper_state_channel->current_controlled_state();
  ASSERT_TRUE(gripper_state.ok()) << gripper_state.error().message;
  ASSERT_EQ(gripper_state.value().positions.size(), 1U);
  const auto *gripper_position = std::get_if<core::LinearPositionDTO>(
      &gripper_state.value().positions.front());
  ASSERT_NE(gripper_position, nullptr);
  EXPECT_NEAR(gripper_position->value_m, 0.004, 1e-12);
}

TEST(RetainedProductCrossConsumer,
     DurableLite6ModelPreservesAuthoritativeCollisionDecisions) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  const std::filesystem::path empty_ambient_root =
      temporary.path() / "empty-ambient";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));
  ASSERT_TRUE(std::filesystem::create_directory(empty_ambient_root));

  const auto materialized = deployment::test::
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  const deployment::RetainedPlanningModelQuery planning_query{
      materialized.bundle->retained_bundle_id(),
      products.planning_collision_model.product_id};
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const deployment::RetainedModelMapQuery map_query{
      materialized.bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id};
  const auto retained = load_lite6_planning_model(package_root, planning_query);
  const auto model_map = load_lite6_model_map(package_root, map_query);
  model_map.require_same_bundle_as(retained);

  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto *arm_binding =
      model_map.find_binding(component_id, command_target_id);
  const auto *arm_target =
      model_map.find_target(component_id, command_target_id);
  ASSERT_NE(arm_binding, nullptr);
  ASSERT_NE(arm_target, nullptr);
  const auto arm_group = std::find_if(
      arm_target->model_groups.begin(), arm_target->model_groups.end(),
      [](const deployment::CompositionTargetGroupResolution &group) {
        return group.model_group_role_id.value() == "planning_group";
      });
  ASSERT_NE(arm_group, arm_target->model_groups.end());
  std::vector<std::string> arm_joint_names;
  for (const auto &resource : arm_binding->resources) {
    ASSERT_EQ(resource.coordinates.size(), 1U);
    EXPECT_FALSE(resource.coordinates.front().derived_mimic);
    arm_joint_names.push_back(resource.coordinates.front().compiled_joint_name);
  }
  ASSERT_EQ(arm_joint_names.size(), 6U);

  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  ScopedCurrentDirectory no_ambient_lookup(empty_ambient_root);
  roboplan::Scene planning_scene(
      "retained_lite6_cross_consumer", std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths());
  auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_cross_consumer", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(), arm_group->compiled_group_name);
  ManyForgeCollisionEngine collision_engine(
      shared_collision_model, ManyForgeCollisionEngine::make_shared_scene_cache(
                                  shared_collision_model));
  const auto collision_projector =
      RetainedCollisionWorldProjector::create_verified(
          shared_collision_model, *materialized.bundle, model_map, retained);

  const core::SceneResourceId scene_resource_id("retained-model-obstacle");
  const std::string scene_body_id = "scene/retained-model-obstacle/shape/0";
  std::set<core::CollisionBodyId> component_body_ids;
  std::set<std::string> projected_collision_frames;
  std::size_t projected_geometry_count = 0U;
  for (const auto &body :
       collision_projector.component_projection().component_bodies) {
    ASSERT_EQ(body.owner_kind, core::CollisionOwnerKind::kComponentModelBody);
    ASSERT_FALSE(body.geometries.empty());
    projected_geometry_count += body.geometries.size();
    ASSERT_TRUE(component_body_ids.insert(body.body_id).second);
    ASSERT_TRUE(projected_collision_frames.insert(body.frame_id).second);
  }

  std::set<std::string> retained_collision_frames;
  for (const auto &geometry :
       planning_scene.getCollisionModel().geometryObjects) {
    ASSERT_LT(geometry.parentFrame, planning_scene.getModel().frames.size());
    retained_collision_frames.insert(
        planning_scene.getModel().frames[geometry.parentFrame].name);
  }
  EXPECT_EQ(projected_collision_frames, retained_collision_frames);
  EXPECT_EQ(projected_geometry_count,
            planning_scene.getCollisionModel().geometryObjects.size());

  const auto make_snapshot = [&](const std::uint64_t revision_id,
                                 const double fixture_x_m,
                                 const double fixture_extent_m) {
    const auto logical_scene = make_collision_scene_snapshot(
        materialized.bundle->value().cell_graph.cell_id, revision_id + 1U,
        scene_resource_id, fixture_x_m, fixture_extent_m);
    auto built = collision_projector.project(logical_scene, revision_id, 0,
                                             "retained-lite6-cross-consumer");
    if (!built.ok()) {
      throw std::logic_error(
          "cannot construct retained-model parity snapshot: " +
          built.error().message);
    }
    return built.value();
  };

  const Eigen::Vector4d color(0.2, 0.4, 0.8, 1.0);
  const std::string fixture_name = "retained_model_world_fixture";
  const auto add_world_fixture = [&](const double x_m, const double extent_m) {
    Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
    pose(0, 3) = x_m;
    const auto added = planning_scene.addBoxGeometry(
        fixture_name, "universe", roboplan::Box(extent_m, extent_m, extent_m),
        pose, color);
    ASSERT_TRUE(added.has_value()) << added.error();
  };
  add_world_fixture(10.0, 0.2);
  auto snapshot = make_snapshot(1U, 10.0, 0.2);

  const auto compare_configuration = [&](std::string query_id,
                                         const std::vector<std::string> &names,
                                         const std::vector<double> &values,
                                         const bool expected_collision) {
    ASSERT_EQ(names.size(), values.size());
    Eigen::VectorXd planning_positions =
        Eigen::VectorXd::Zero(planning_scene.getModel().nq);
    for (std::size_t index = 0U; index < names.size(); ++index) {
      const auto joint_id = planning_scene.getModel().getJointId(names[index]);
      ASSERT_LT(joint_id, planning_scene.getModel().idx_qs.size());
      planning_positions[planning_scene.getModel().idx_qs[joint_id]] =
          values[index];
    }
    const bool planning_collision =
        planning_scene.hasCollisions(planning_positions);

    core::CollisionQuery query;
    query.query_id = std::move(query_id);
    query.collision_world_revision = snapshot.revision;
    query.robot_state.joint_names = names;
    query.robot_state.positions_rad = values;
    const auto result =
        collision_engine.check(snapshot, query, core::CollisionPolicySnapshot{},
                               core::CollisionCancellationToken{});
    std::string diagnostic_text;
    for (const auto &[key, value] : result.raw_diagnostics) {
      diagnostic_text += key + "=" + value + ";";
    }
    EXPECT_EQ(result.engine_status, core::CollisionEngineStatus::kOk)
        << diagnostic_text;
    EXPECT_EQ(result.in_collision, planning_collision);
    EXPECT_EQ(result.decision, planning_collision
                                   ? core::CollisionDecisionResult::kFail
                                   : core::CollisionDecisionResult::kPass);
    EXPECT_EQ(planning_collision, expected_collision);
    if (expected_collision) {
      ASSERT_FALSE(result.normalized_pairs.empty());
      bool saw_scene_contact = false;
      for (const auto &pair : result.normalized_pairs) {
        const bool body_a_known = pair.body_a == scene_body_id ||
                                  component_body_ids.count(pair.body_a) == 1U;
        const bool body_b_known = pair.body_b == scene_body_id ||
                                  component_body_ids.count(pair.body_b) == 1U;
        EXPECT_TRUE(body_a_known);
        EXPECT_TRUE(body_b_known);
        saw_scene_contact = saw_scene_contact || pair.body_a == scene_body_id ||
                            pair.body_b == scene_body_id;
      }
      EXPECT_TRUE(saw_scene_contact);
    } else {
      EXPECT_TRUE(result.normalized_pairs.empty());
    }
  };

  const std::vector<double> home(arm_joint_names.size(), 0.0);
  const std::vector<double> historical_rest{0.0, -0.785, 0.785, 0.0, 1.57, 0.0};
  compare_configuration("lite6-home", arm_joint_names, home, false);
  compare_configuration("lite6-historical-rest", arm_joint_names,
                        historical_rest, false);

  const std::string gripper_master =
      compiled_symbol(model_map, deployment::OccurrenceSymbolKind::kJoint,
                      "right_finger_joint");
  compare_configuration("lite6-open-gripper", {gripper_master}, {0.004}, false);

  const auto removed = planning_scene.removeGeometry(fixture_name);
  ASSERT_TRUE(removed.has_value()) << removed.error();
  add_world_fixture(0.0, 4.0);
  snapshot = make_snapshot(2U, 0.0, 4.0);
  compare_configuration("lite6-positive-collision", arm_joint_names, home,
                        true);

  const core::CommandTargetId gripper_target_id(
      std::string(deployment::test::kLite6GripperTargetId));
  const auto *gripper_target =
      model_map.find_target(component_id, gripper_target_id);
  ASSERT_NE(gripper_target, nullptr);
  const auto carrier_frame = std::find_if(
      gripper_target->frames.begin(), gripper_target->frames.end(),
      [](const deployment::CompositionTargetFrameResolution &frame) {
        return frame.frame_role_id.value() == "tcp";
      });
  ASSERT_NE(carrier_frame, gripper_target->frames.end());
  const auto *carrier = std::get_if<deployment::CompositionComponentModelFrame>(
      &carrier_frame->frame);
  ASSERT_NE(carrier, nullptr);

  core::LogicalSceneSnapshotDTO carried_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 4U,
      core::SceneResourceId("carried-part"), 0.0, 0.02);
  carried_scene.objects.front().support_contacts = {
      core::SupportContactDTO{.support_object_id = "support-table"},
  };
  carried_scene.objects.push_back(core::SceneObjectSnapshotDTO{
      core::SceneResourceId("support-table"),
      {core::BoxShapeDTO{{0.5, 0.5, 0.05}}},
      core::CellFixedSceneObjectPlacementDTO{
          materialized.bundle->value().cell_graph.cell_id,
          {{10.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
      },
      std::nullopt,
      {},
  });
  carried_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          carried_scene);
  auto detached_policy_result =
      collision_projector.project_effective_policy(carried_scene, 2U);
  ASSERT_TRUE(detached_policy_result.ok())
      << detached_policy_result.error().message;
  EXPECT_TRUE(detached_policy_result.value().support_contacts.empty());
  EXPECT_NE(
      std::find(
          detached_policy_result.value().allowed_static_static_pairs.begin(),
          detached_policy_result.value().allowed_static_static_pairs.end(),
          (core::CollisionPairId{"scene/carried-part/shape/0",
                                 "scene/support-table/shape/0"})),
      detached_policy_result.value().allowed_static_static_pairs.end());
  EXPECT_TRUE(std::any_of(
      detached_policy_result.value().allowed_static_static_pairs.begin(),
      detached_policy_result.value().allowed_static_static_pairs.end(),
      [&](const auto &pair) {
        const bool names_fixed_scene =
            pair.body_a == "scene/carried-part/shape/0" ||
            pair.body_b == "scene/carried-part/shape/0";
        const auto &other = pair.body_a == "scene/carried-part/shape/0"
                                ? pair.body_b
                                : pair.body_a;
        return names_fixed_scene && component_body_ids.count(other) == 1U;
      }));
  core::CarriedObjectAttachmentDTO attachment{
      core::CarriedObjectAttachmentId("carried-part-attachment"),
      core::CarriedObjectAttachmentRevision(std::string(kZeroDigest)),
      core::SceneResourceId("carried-part"),
      component_id,
      gripper_target_id,
      core::ComponentSceneFrameRefDTO{
          component_id,
          carrier->model_instance_id,
          core::FrameRoleId("tcp"),
      },
      {{0.0, 0.0, 0.04}, {0.0, 0.0, 0.0, 1.0}},
      {core::CollisionBodyRoleId("gripper_collision")},
  };
  attachment.carried_object_attachment_revision =
      core::logical_scene_identity::ComputeCarriedObjectAttachmentRevision(
          attachment);
  carried_scene.objects.front().placement =
      core::CarriedSceneObjectPlacementDTO{
          attachment.carried_object_attachment_id,
          attachment.carried_object_attachment_revision,
      };
  carried_scene.carried_object_attachments = {attachment};
  carried_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          carried_scene);
  ASSERT_TRUE(carried_scene.locally_valid());

  auto carried_world_result = collision_projector.project(
      carried_scene, 3U, 0, "retained-lite6-cross-consumer");
  ASSERT_TRUE(carried_world_result.ok())
      << carried_world_result.error().message;
  const auto &carried_world = carried_world_result.value();
  const auto carried_body =
      std::find_if(carried_world.bodies.begin(), carried_world.bodies.end(),
                   [](const core::CollisionWorldBody &body) {
                     return body.body_id == "scene/carried-part/shape/0";
                   });
  ASSERT_NE(carried_body, carried_world.bodies.end());
  ASSERT_TRUE(carried_body->parent_body_id.has_value());
  EXPECT_EQ(component_body_ids.count(*carried_body->parent_body_id), 1U);
  EXPECT_NE(carried_body->frame_id,
            collision_projector.model_map().compiled_cell_frame_name);

  auto carried_policy_result =
      collision_projector.project_effective_policy(carried_scene, 2U);
  ASSERT_TRUE(carried_policy_result.ok())
      << carried_policy_result.error().message;
  const auto &carried_policy = carried_policy_result.value();
  ASSERT_EQ(
      carried_policy.allowed_touch_link_pairs.size(),
      gripper_target->collision_bodies.front().compiled_link_names.size());
  for (const auto &pair : carried_policy.allowed_touch_link_pairs) {
    EXPECT_TRUE(pair.body_a == carried_body->body_id ||
                pair.body_b == carried_body->body_id);
  }
  ASSERT_EQ(carried_policy.support_contacts.size(), 1U);
  EXPECT_EQ(carried_policy.support_contacts.front().pair,
            (core::CollisionPairId{"scene/carried-part/shape/0",
                                   "scene/support-table/shape/0"}));

  core::CollisionQuery carried_query;
  carried_query.query_id = "lite6-carried-object-ingestion";
  carried_query.collision_world_revision = carried_world.revision;
  carried_query.robot_state.joint_names = arm_joint_names;
  carried_query.robot_state.positions_rad = home;
  const auto carried_result =
      collision_engine.check(carried_world, carried_query, carried_policy,
                             core::CollisionCancellationToken{});
  EXPECT_EQ(carried_result.engine_status, core::CollisionEngineStatus::kOk);

  core::LogicalSceneSnapshotDTO no_touch_scene = carried_scene;
  no_touch_scene.carried_object_attachments.front().touch_body_roles.clear();
  no_touch_scene.carried_object_attachments.front()
      .carried_object_attachment_revision =
      core::logical_scene_identity::ComputeCarriedObjectAttachmentRevision(
          no_touch_scene.carried_object_attachments.front());
  auto &no_touch_placement = std::get<core::CarriedSceneObjectPlacementDTO>(
      no_touch_scene.objects.front().placement);
  no_touch_placement.carried_object_attachment_revision =
      no_touch_scene.carried_object_attachments.front()
          .carried_object_attachment_revision;
  no_touch_scene.revision.revision_id = 5U;
  no_touch_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          no_touch_scene);
  auto no_touch_world = collision_projector.project(
      no_touch_scene, 4U, 0, "retained-lite6-cross-consumer");
  ASSERT_TRUE(no_touch_world.ok()) << no_touch_world.error().message;
  EXPECT_NE(no_touch_world.value().revision.content_hash,
            carried_world.revision.content_hash);

  const auto initial_publish_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 2U,
      core::SceneResourceId("carried-part"), 0.0, 0.02);
  auto publisher_result = RetainedCollisionWorldPublisher::create(
      collision_projector, "retained-lite6-publisher", initial_publish_scene,
      RetainedCollisionWorldPublicationContext{
          core::CollisionWorldMutationSource::kDeploymentApply,
          {},
          10,
          {},
          "retained_product_consumer",
          std::nullopt,
      });
  ASSERT_TRUE(publisher_result.ok()) << publisher_result.error().message;
  const auto publisher = publisher_result.value();
  const auto initial_world = publisher->snapshot();
  ASSERT_NE(initial_world, nullptr);
  EXPECT_EQ(initial_world, publisher->snapshot());
  EXPECT_EQ(initial_world->revision.revision_id, 1U);
  EXPECT_FALSE(initial_world->bodies.back().parent_body_id.has_value());
  const auto initial_inventory =
      publisher->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(initial_inventory.has_value());
  ASSERT_TRUE(initial_inventory->projection_binding.valid());
  EXPECT_EQ(initial_inventory->projection_binding.projection_revision
                .retained_bundle_id,
            materialized.bundle->retained_bundle_id());
  EXPECT_EQ(
      initial_inventory->projection_binding.projection_revision.graph_revision,
      materialized.bundle->value().cell_graph.graph_revision);
  EXPECT_EQ(initial_inventory->bundle.world, initial_world);

  const core::ProjectionLeaseId operation_lease("operation.native.lease.1");
  const auto retained_inventory =
      publisher->retain_scene_snapshot_lease_inventory(operation_lease);
  ASSERT_TRUE(retained_inventory.has_value());
  EXPECT_EQ(retained_inventory->projection_binding.projection_lease_id,
            operation_lease);
  EXPECT_EQ(retained_inventory->bundle.world, initial_world);
  auto retention_stats = publisher->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(retention_stats.retained_count, 1U);
  ASSERT_EQ(retention_stats.retained_ids_oldest_first.size(), 1U);
  EXPECT_EQ(retention_stats.retained_ids_oldest_first.front(), operation_lease);

  auto attached_publication = publisher->publish(
      carried_scene,
      RetainedCollisionWorldPublicationContext{
          core::CollisionWorldMutationSource::kCarriedObjectAttach,
          {core::CollisionBodyId("scene/carried-part/shape/0")},
          20,
          {},
          "scene_attach_carried",
          std::string("attach-correlation"),
      });
  ASSERT_TRUE(attached_publication.ok())
      << attached_publication.error().message;
  EXPECT_EQ(attached_publication.value().revision_id, 2U);
  EXPECT_EQ(initial_world->revision.revision_id, 1U);
  const auto advanced_inventory =
      publisher->current_scene_snapshot_lease_inventory();
  ASSERT_TRUE(advanced_inventory.has_value());
  EXPECT_NE(advanced_inventory->bundle.world, initial_world);
  EXPECT_EQ(advanced_inventory->bundle.world->revision.revision_id, 2U);
  const auto repeated_retention =
      publisher->retain_scene_snapshot_lease_inventory(operation_lease);
  ASSERT_TRUE(repeated_retention.has_value());
  EXPECT_EQ(repeated_retention->projection_binding,
            retained_inventory->projection_binding);
  EXPECT_EQ(repeated_retention->bundle.world, initial_world);
  const auto published_runtime =
      publisher->published_runtime_collision_query_bundle();
  ASSERT_NE(published_runtime, nullptr);
  ASSERT_NE(published_runtime->world, nullptr);
  ASSERT_NE(published_runtime->policy, nullptr);
  EXPECT_TRUE(published_runtime->lookup_order_valid);
  EXPECT_TRUE(published_runtime->distance_selection_order_valid);
  EXPECT_EQ(published_runtime->policy->allowed_touch_link_pairs,
            carried_policy.allowed_touch_link_pairs);

  const auto ledger = publisher->recent_ledger();
  ASSERT_EQ(ledger.size(), 2U);
  EXPECT_EQ(ledger[0].revision.revision_id, 2U);
  EXPECT_EQ(ledger[0].previous_revision_id, 1U);
  EXPECT_EQ(ledger[0].mutation_source,
            core::CollisionWorldMutationSource::kCarriedObjectAttach);
  EXPECT_EQ(ledger[0].owner_sequence, 2U);
  EXPECT_EQ(ledger[1].revision.revision_id, 1U);
  EXPECT_EQ(ledger[1].previous_revision_id, 0U);

  auto stale_publication = publisher->publish(
      carried_scene, RetainedCollisionWorldPublicationContext{
                         core::CollisionWorldMutationSource::kSceneEdit,
                         {},
                         30,
                         {},
                         "stale_scene_replay",
                         std::nullopt,
                     });
  EXPECT_FALSE(stale_publication.ok());
  EXPECT_EQ(publisher->published_collision_world_revision().revision_id, 2U);
  EXPECT_EQ(publisher->recent_ledger().size(), 2U);

  publisher->release_scene_snapshot_lease_inventory(operation_lease);
  publisher->release_scene_snapshot_lease_inventory(operation_lease);
  retention_stats = publisher->scene_snapshot_lease_retention_stats();
  EXPECT_EQ(retention_stats.retained_count, 0U);
  EXPECT_TRUE(retention_stats.retained_ids_oldest_first.empty());
}

TEST(RetainedProductCrossConsumer,
     PlannerSceneProjectionCommitsLeasesAndRebuildsExactSnapshots) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  const auto materialized = deployment::test::
      materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
          package_root);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &products = materialized.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  const auto retained = load_lite6_planning_model(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.planning_collision_model.product_id});
  const auto model_map = load_lite6_model_map(
      package_root, {materialized.bundle->retained_bundle_id(),
                     products.symbol_role_maps.front().product_id});
  auto stage =
      RetainedPlanningModelStage::create_verified(retained, staging_parent);
  const core::ControllableComponentId component_id(
      std::string(deployment::test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(deployment::test::kLite6ArmTargetId));
  const auto *target = model_map.find_target(component_id, command_target_id);
  ASSERT_NE(target, nullptr);
  ASSERT_FALSE(target->model_groups.empty());
  auto shared_collision_model = ManyForgeCollisionEngine::load_shared_model(
      "retained_lite6_projection_state_machine", stage.loader_urdf_path(),
      stage.srdf_path(), stage.package_paths(),
      target->model_groups.front().compiled_group_name);
  const auto collision_projector =
      RetainedCollisionWorldProjector::create_verified(
          shared_collision_model, *materialized.bundle, model_map, retained);

  const auto initial_scene = make_collision_scene_snapshot(
      materialized.bundle->value().cell_graph.cell_id, 2U,
      core::SceneResourceId("first-object"), 10.0, 0.1);
  const core::ProjectionRevisionDTO initial_revision{
      materialized.bundle->retained_bundle_id(),
      materialized.bundle->value().cell_graph.graph_revision,
      initial_scene.revision};
  const core::ProjectionNativeRuntimeBindingDTO native_binding{
      core::CapabilityProviderBindingId("lite6_joint_planner_binding"),
      core::CapabilityProviderBindingRevision("planner-binding-revision"),
      core::RuntimeUnitId("native-execution-host"),
      core::RuntimeEpoch("native-execution-host-epoch-1")};
  std::int64_t clock_value = 10;
  bool fail_clock = false;
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      collision_projector, products.planning_collision_model, native_binding,
      initial_scene,
      RetainedPlannerSceneProjectionConfig{
          "retained-lite6-transactional-projection", 8U, 8U},
      [&clock_value, &fail_clock] {
        if (fail_clock) {
          throw std::runtime_error("injected projection clock failure");
        }
        return clock_value++;
      });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  auto projection = projection_result.value();
  ASSERT_TRUE(
      projection->ProjectionHealth().healthy_for_target(initial_revision));

  const core::ProjectionLeaseId predecessor_lease(
      "z.operation.predecessor.oldest");
  auto retained_predecessor =
      projection->RetainProjection(initial_revision, predecessor_lease);
  ASSERT_TRUE(retained_predecessor.ok())
      << retained_predecessor.error().message;
  const auto predecessor_world = retained_predecessor.value().bundle.world;
  ASSERT_NE(predecessor_world, nullptr);
  const core::ProjectionLeaseId second_predecessor_lease(
      "a.operation.predecessor.newer");
  auto retained_second_predecessor =
      projection->RetainProjection(initial_revision, second_predecessor_lease);
  ASSERT_TRUE(retained_second_predecessor.ok())
      << retained_second_predecessor.error().message;
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats()
                .retained_ids_oldest_first,
            (std::vector<core::ProjectionLeaseId>{predecessor_lease,
                                                  second_predecessor_lease}));

  core::SceneObjectSnapshotDTO second_object{
      core::SceneResourceId("second-object"),
      {core::SphereShapeDTO{0.05}},
      core::CellFixedSceneObjectPlacementDTO{
          initial_scene.cell_id, {{9.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}}},
      std::nullopt,
      {}};
  auto candidate_scene = initial_scene;
  candidate_scene.transaction_sequence = 2U;
  candidate_scene.topology_epoch = 2U;
  candidate_scene.objects.push_back(second_object);
  candidate_scene.revision.revision_id = 3U;
  candidate_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          candidate_scene);
  const core::ProjectionRevisionDTO candidate_revision{
      initial_revision.retained_bundle_id, initial_revision.graph_revision,
      candidate_scene.revision};
  const core::SceneTransactionDTO transaction{
      core::RequestId("projection-add-second-object"),
      {},
      2U,
      initial_revision,
      candidate_revision,
      core::ContentDigest(std::string(kZeroDigest)),
      core::ResolvedUpsertSceneObjectDTO{second_object},
      core::ContentDigest(std::string(kZeroDigest))};

  const auto committed = projection->ApplyTransaction(transaction);
  ASSERT_TRUE(committed.presence_valid());
  EXPECT_EQ(committed.result, core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_TRUE(
      committed.resulting_health.healthy_for_target(candidate_revision));
  ASSERT_NE(projection->logical_snapshot(), nullptr);
  EXPECT_EQ(*projection->logical_snapshot(), candidate_scene);
  const auto current_world = projection->snapshot();
  ASSERT_NE(current_world, nullptr);
  EXPECT_NE(current_world, predecessor_world);
  EXPECT_EQ(retained_predecessor.value().bundle.world, predecessor_world);

  const auto replay = projection->ApplyTransaction(transaction);
  EXPECT_EQ(replay.transaction_sequence, committed.transaction_sequence);
  EXPECT_EQ(replay.acknowledged_projection_revision,
            committed.acknowledged_projection_revision);
  EXPECT_EQ(replay.native_projection_generation,
            committed.native_projection_generation);
  EXPECT_EQ(projection->snapshot(), current_world);

  const auto refused_healthy_rebuild =
      projection->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, initial_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              initial_scene)});
  EXPECT_EQ(refused_healthy_rebuild.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(
      projection->ProjectionHealth().healthy_for_target(candidate_revision));
  EXPECT_EQ(projection->snapshot(), current_world);

  auto conflicting = transaction;
  conflicting.request_id = core::RequestId("conflicting-reuse");
  const auto conflict = projection->ApplyTransaction(conflicting);
  EXPECT_EQ(conflict.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(conflict.rollback_proven);
  EXPECT_EQ(projection->snapshot(), current_world);

  auto policy_scene = candidate_scene;
  policy_scene.transaction_sequence = 3U;
  policy_scene.policy_epoch = 1U;
  policy_scene.configured_collision_policy.policy_id = "reduced-diagnostics";
  policy_scene.configured_collision_policy.require_full_diagnostics = false;
  ++policy_scene.configured_collision_policy.revision.revision_id;
  policy_scene.configured_collision_policy.revision.content_hash =
      core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
          policy_scene.configured_collision_policy)
          .value();
  policy_scene.revision.revision_id = 4U;
  policy_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          policy_scene);
  const core::ProjectionRevisionDTO policy_revision{
      candidate_revision.retained_bundle_id, candidate_revision.graph_revision,
      policy_scene.revision};
  auto mismatched_effective = policy_scene.configured_collision_policy;
  mismatched_effective.require_full_diagnostics = true;
  mismatched_effective.revision.content_hash =
      core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
          mismatched_effective)
          .value();
  const core::SceneTransactionDTO mismatched_policy_transaction{
      core::RequestId("projection-mismatched-effective-policy"),
      {},
      3U,
      candidate_revision,
      policy_revision,
      core::ContentDigest(std::string(kZeroDigest)),
      core::ResolvedCollisionPolicyDTO{policy_scene.configured_collision_policy,
                                       std::move(mismatched_effective)},
      std::nullopt};
  const auto rejected_policy =
      projection->ApplyTransaction(mismatched_policy_transaction);
  EXPECT_EQ(rejected_policy.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(rejected_policy.rollback_proven);
  EXPECT_EQ(projection->snapshot(), current_world);
  EXPECT_EQ(*projection->logical_snapshot(), candidate_scene);

  auto uncertain_scene = candidate_scene;
  auto third_object = second_object;
  third_object.scene_resource_id = core::SceneResourceId("third-object");
  uncertain_scene.objects.push_back(third_object);
  uncertain_scene.transaction_sequence = 3U;
  uncertain_scene.topology_epoch = 3U;
  uncertain_scene.revision.revision_id = 4U;
  uncertain_scene.revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          uncertain_scene);
  const core::ProjectionRevisionDTO uncertain_revision{
      candidate_revision.retained_bundle_id, candidate_revision.graph_revision,
      uncertain_scene.revision};
  const core::SceneTransactionDTO uncertain_transaction{
      core::RequestId("projection-injected-uncertain-effect"),
      {},
      3U,
      candidate_revision,
      uncertain_revision,
      core::ContentDigest(std::string(kZeroDigest)),
      core::ResolvedUpsertSceneObjectDTO{third_object},
      core::ContentDigest(std::string(kZeroDigest))};
  fail_clock = true;
  const auto uncertain = projection->ApplyTransaction(uncertain_transaction);
  fail_clock = false;
  EXPECT_EQ(uncertain.result,
            core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain);
  EXPECT_EQ(projection->ProjectionHealth().state,
            core::ProjectionHealthState::kDesynchronized);
  EXPECT_EQ(projection->snapshot(), nullptr);
  EXPECT_FALSE(projection->collision_policy_snapshot().has_value());
  EXPECT_FALSE(
      projection->current_scene_snapshot_lease_inventory().has_value());
  EXPECT_EQ(projection->published_runtime_collision_query_bundle(), nullptr);
  EXPECT_FALSE(
      projection->RetainProjection(initial_revision, predecessor_lease).ok());

  const auto recovered =
      projection->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  ASSERT_TRUE(recovered.presence_valid());
  EXPECT_TRUE(
      recovered.resulting_health.healthy_for_target(candidate_revision));
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats().retained_count,
            2U);
  const auto recovered_predecessor =
      projection->RetainProjection(initial_revision, predecessor_lease);
  ASSERT_TRUE(recovered_predecessor.ok())
      << recovered_predecessor.error().message;
  EXPECT_EQ(recovered_predecessor.value().bundle.world, predecessor_world);

  projection->ReleaseProjection(predecessor_lease);
  projection->ReleaseProjection(second_predecessor_lease);
  EXPECT_EQ(projection->scene_snapshot_lease_retention_stats().retained_count,
            0U);

  std::int64_t rebuild_clock = 100;
  auto uninitialized_result =
      RetainedPlannerSceneProjection::CreateUninitialized(
          collision_projector, products.planning_collision_model,
          native_binding,
          RetainedPlannerSceneProjectionConfig{
              "retained-lite6-rebuilt-projection", 8U, 8U},
          [&rebuild_clock] { return rebuild_clock++; });
  ASSERT_TRUE(uninitialized_result.ok())
      << uninitialized_result.error().message;
  auto uninitialized = uninitialized_result.value();
  EXPECT_EQ(uninitialized->ProjectionHealth().state,
            core::ProjectionHealthState::kUninitialized);
  EXPECT_EQ(uninitialized->snapshot(), nullptr);
  EXPECT_FALSE(uninitialized->collision_policy_snapshot().has_value());
  EXPECT_FALSE(
      uninitialized->current_scene_snapshot_lease_inventory().has_value());
  EXPECT_EQ(uninitialized->published_runtime_collision_query_bundle(), nullptr);
  EXPECT_FALSE(
      uninitialized
          ->RetainProjection(candidate_revision,
                             core::ProjectionLeaseId("uninitialized-lease"))
          .ok());
  const auto rebuilt =
      uninitialized->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  ASSERT_TRUE(rebuilt.presence_valid());
  EXPECT_FALSE(rebuilt.transaction_sequence.has_value());
  EXPECT_TRUE(rebuilt.resulting_health.healthy_for_target(candidate_revision));
  ASSERT_NE(uninitialized->snapshot(), nullptr);
  EXPECT_EQ(*uninitialized->logical_snapshot(), candidate_scene);
  const auto rebuilt_generation = rebuilt.native_projection_generation;
  const auto rebuilt_world = uninitialized->snapshot();
  const auto rebuilt_replay =
      uninitialized->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, products.planning_collision_model,
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  ASSERT_TRUE(rebuilt_replay.presence_valid());
  EXPECT_EQ(rebuilt_replay.native_projection_generation, rebuilt_generation);
  EXPECT_EQ(uninitialized->snapshot(), rebuilt_world);

  auto mismatched_product = products.planning_collision_model;
  mismatched_product.product_id = core::ProductId("another-product");
  const auto rejected_rebuild =
      uninitialized->RebuildProjection(core::NativeProjectionRebuildDTO{
          native_binding, candidate_revision, std::move(mismatched_product),
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene)});
  EXPECT_EQ(rejected_rebuild.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  EXPECT_TRUE(rejected_rebuild.rollback_proven);
  EXPECT_TRUE(
      uninitialized->ProjectionHealth().healthy_for_target(candidate_revision));

  std::int64_t bootstrap_clock = 200;
  auto bootstrap_projection_result =
      RetainedPlannerSceneProjection::CreateUninitialized(
          collision_projector, products.planning_collision_model,
          native_binding,
          RetainedPlannerSceneProjectionConfig{
              "retained-lite6-bootstrap-projection", 8U, 8U},
          [&bootstrap_clock] { return bootstrap_clock++; });
  ASSERT_TRUE(bootstrap_projection_result.ok())
      << bootstrap_projection_result.error().message;
  auto bootstrap_projection = bootstrap_projection_result.value();
  const core::NativeProjectionBootstrapStatusQueryDTO bootstrap_query{
      core::BootstrapId("native-bootstrap"), core::BootstrapGeneration(3U),
      native_binding, candidate_revision};
  const auto bootstrap_absent =
      bootstrap_projection->BootstrapProjectionStatus(bootstrap_query);
  EXPECT_EQ(bootstrap_absent.state,
            core::NativeProjectionBootstrapStatusState::kAbsent);
  const core::NativeProjectionBootstrapRebuildDTO bootstrap_request{
      bootstrap_query.bootstrap_id,
      bootstrap_query.bootstrap_generation,
      native_binding,
      candidate_revision,
      products.planning_collision_model,
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene))};
  const auto bootstrap_rebuilt =
      bootstrap_projection->RebuildBootstrapProjection(bootstrap_request);
  ASSERT_TRUE(bootstrap_rebuilt.presence_valid());
  EXPECT_EQ(bootstrap_rebuilt.state,
            core::NativeProjectionBootstrapRebuildState::kRebuilt);
  EXPECT_TRUE(bootstrap_rebuilt.resulting_health.healthy_for_target(
      candidate_revision));
  const auto bootstrap_replay =
      bootstrap_projection->RebuildBootstrapProjection(bootstrap_request);
  EXPECT_EQ(bootstrap_replay.state,
            core::NativeProjectionBootstrapRebuildState::kAlreadyExact);
  EXPECT_EQ(bootstrap_replay.native_projection_generation,
            bootstrap_rebuilt.native_projection_generation);
  const auto bootstrap_status =
      bootstrap_projection->BootstrapProjectionStatus(bootstrap_query);
  EXPECT_EQ(bootstrap_status.state,
            core::NativeProjectionBootstrapStatusState::kRebuilt);

  const core::NativeProjectionRetireDTO retire_request{
      core::ApplyId("remove-native-host"),
      core::ApplyGeneration(4U),
      core::ProjectionStageGeneration(1U),
      core::ProjectionTransitionRowDTO{
          native_binding, core::ProjectionTransitionDisposition::kRemoved,
          candidate_revision},
      candidate_revision,
      candidate_revision};
  const auto retire_pending = bootstrap_projection->ProjectionRetireStatus(
      core::NativeProjectionRetireStatusQueryDTO{retire_request});
  EXPECT_EQ(retire_pending.state,
            core::NativeProjectionRetireStatusState::kPending);
  const auto retired = bootstrap_projection->RetireProjection(retire_request);
  ASSERT_TRUE(retired.presence_valid());
  EXPECT_EQ(retired.state, core::NativeProjectionRetireResultState::kRetired);
  EXPECT_EQ(bootstrap_projection->snapshot(), nullptr);
  const auto retired_replay =
      bootstrap_projection->RetireProjection(retire_request);
  EXPECT_EQ(retired_replay.state,
            core::NativeProjectionRetireResultState::kRetired);
  const auto retired_status = bootstrap_projection->ProjectionRetireStatus(
      core::NativeProjectionRetireStatusQueryDTO{retire_request});
  EXPECT_EQ(retired_status.state,
            core::NativeProjectionRetireStatusState::kRetired);

  const core::ProjectionNativeRuntimeBindingDTO added_binding{
      core::CapabilityProviderBindingId("lite6_added_planner_binding"),
      core::CapabilityProviderBindingRevision("added-binding-revision"),
      core::RuntimeUnitId("added-native-execution-host"),
      core::RuntimeEpoch("added-native-execution-host-epoch-1")};
  std::int64_t stage_clock = 300;
  auto added_projection_result =
      RetainedPlannerSceneProjection::CreateUninitialized(
          collision_projector, products.planning_collision_model, added_binding,
          RetainedPlannerSceneProjectionConfig{
              "retained-lite6-added-projection", 8U, 8U},
          [&stage_clock] { return stage_clock++; });
  ASSERT_TRUE(added_projection_result.ok())
      << added_projection_result.error().message;
  auto added_projection = added_projection_result.value();
  const core::NativeProjectionStageDTO stage_request{
      core::ApplyId("add-native-host"),
      core::ApplyGeneration(5U),
      core::ProjectionStageGeneration(2U),
      core::ProjectionTransitionRowDTO{
          added_binding, core::ProjectionTransitionDisposition::kAdded,
          std::nullopt},
      candidate_revision,
      candidate_revision,
      products.planning_collision_model,
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              candidate_scene))};
  const auto staged = added_projection->StageProjection(stage_request);
  ASSERT_TRUE(staged.presence_valid());
  EXPECT_EQ(staged.state, core::NativeProjectionStageResultState::kStaged);
  EXPECT_EQ(added_projection->snapshot(), nullptr);
  const core::NativeProjectionStatusQueryDTO stage_query{
      stage_request.apply_id, stage_request.apply_generation,
      stage_request.projection_stage_generation, stage_request.transition_row};
  const auto staged_status =
      added_projection->ProjectionStageStatus(stage_query);
  EXPECT_EQ(staged_status.state, core::NativeProjectionStatusState::kStaged);
  const core::NativeProjectionActivationDTO activation_request{
      stage_request.apply_id,
      stage_request.apply_generation,
      stage_request.projection_stage_generation,
      stage_request.transition_row,
      stage_request.expected_predecessor_projection_revision,
      stage_request.candidate_projection_revision};
  const auto activated =
      added_projection->ActivateStagedProjection(activation_request);
  ASSERT_TRUE(activated.presence_valid());
  EXPECT_EQ(activated.state,
            core::NativeProjectionActivationResultState::kActivated);
  EXPECT_TRUE(added_projection->ProjectionHealth().healthy_for_target(
      candidate_revision));
  EXPECT_NE(added_projection->snapshot(), nullptr);
  const auto activated_replay =
      added_projection->ActivateStagedProjection(activation_request);
  EXPECT_EQ(activated_replay.state,
            core::NativeProjectionActivationResultState::kActivated);
  const auto activated_status =
      added_projection->ProjectionStageStatus(stage_query);
  EXPECT_EQ(activated_status.state,
            core::NativeProjectionStatusState::kActivated);
}

TEST(RetainedProductCrossConsumer,
     PlannerSceneProjectionPreparesCrossBundleCandidateBeforeStage) {
  ScopedTemporaryRoot temporary;
  const std::filesystem::path package_root =
      temporary.path() / "retained-package";
  const std::filesystem::path staging_parent =
      temporary.path() / "private-stages";
  ASSERT_TRUE(std::filesystem::create_directory(package_root));
  ASSERT_TRUE(std::filesystem::create_directory(staging_parent));

  constexpr std::string_view cell_id = "i6_projection_transition_cell";
  const auto predecessor = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  const auto candidate = deployment::test::
      materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
          package_root, cell_id);
  ASSERT_NE(predecessor.bundle, nullptr);
  ASSERT_NE(candidate.bundle, nullptr);
  ASSERT_NE(predecessor.bundle->retained_bundle_id(),
            candidate.bundle->retained_bundle_id());

  const auto load_projector =
      [&](const deployment::test::DurableLite6RetainedConsumerFixture &fixture,
          std::string model_name) {
        const auto &products = fixture.bundle->value().compiled_products;
        EXPECT_EQ(products.symbol_role_maps.size(), 1U);
        const auto retained = load_lite6_planning_model(
            package_root, {fixture.bundle->retained_bundle_id(),
                           products.planning_collision_model.product_id});
        const auto model_map = load_lite6_model_map(
            package_root, {fixture.bundle->retained_bundle_id(),
                           products.symbol_role_maps.front().product_id});
        auto model_stage = RetainedPlanningModelStage::create_verified(
            retained, staging_parent);
        const auto *target = model_map.find_target(
            core::ControllableComponentId(
                std::string(deployment::test::kLite6ControllableComponentId)),
            core::CommandTargetId(
                std::string(deployment::test::kLite6ArmTargetId)));
        EXPECT_NE(target, nullptr);
        if (target == nullptr || target->model_groups.empty()) {
          throw std::logic_error(
              "cross-bundle fixture has no Lite6 arm model group");
        }
        auto shared_model = ManyForgeCollisionEngine::load_shared_model(
            std::move(model_name), model_stage.loader_urdf_path(),
            model_stage.srdf_path(), model_stage.package_paths(),
            target->model_groups.front().compiled_group_name);
        return std::pair{
            RetainedCollisionWorldProjector::create_verified(
                shared_model, *fixture.bundle, model_map, retained),
            std::move(shared_model)};
      };

  auto [predecessor_projector, predecessor_shared_model] =
      load_projector(predecessor, "i6_projection_predecessor");
  auto [candidate_projector, candidate_shared_model] =
      load_projector(candidate, "i6_projection_candidate");
  const auto initial_scene = make_collision_scene_snapshot(
      core::CellId(std::string(cell_id)), 2U,
      core::SceneResourceId("cross-bundle-object"), 2.0, 0.1);
  const core::ProjectionRevisionDTO predecessor_revision{
      predecessor.bundle->retained_bundle_id(),
      predecessor.bundle->value().cell_graph.graph_revision,
      initial_scene.revision};
  const core::ProjectionRevisionDTO candidate_revision{
      candidate.bundle->retained_bundle_id(),
      candidate.bundle->value().cell_graph.graph_revision,
      initial_scene.revision};
  const core::ProjectionNativeRuntimeBindingDTO binding{
      core::CapabilityProviderBindingId("cross_bundle_planner_binding"),
      core::CapabilityProviderBindingRevision("cross-bundle-binding-revision"),
      core::RuntimeUnitId("cross_bundle_native_execution"),
      core::RuntimeEpoch("cross-bundle-native-epoch-1")};
  std::int64_t clock = 400;
  auto projection_result = RetainedPlannerSceneProjection::CreateInitialized(
      std::move(predecessor_projector),
      predecessor.bundle->value().compiled_products.planning_collision_model,
      binding, initial_scene,
      RetainedPlannerSceneProjectionConfig{
          "cross-bundle-transactional-projection", 8U, 8U},
      [&clock] { return clock++; });
  ASSERT_TRUE(projection_result.ok()) << projection_result.error().message;
  auto projection = std::move(projection_result.value());
  EXPECT_EQ(projection->collision_shared_model(), predecessor_shared_model);
  const auto predecessor_world = projection->snapshot();
  ASSERT_NE(predecessor_world, nullptr);

  const core::NativeProjectionStageDTO request{
      core::ApplyId("cross-bundle-apply"),
      core::ApplyGeneration(8U),
      core::ProjectionStageGeneration(3U),
      core::ProjectionTransitionRowDTO{
          binding, core::ProjectionTransitionDisposition::kRetained,
          predecessor_revision},
      predecessor_revision,
      candidate_revision,
      candidate.bundle->value().compiled_products.planning_collision_model,
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              initial_scene))};
  const auto unprepared = projection->StageProjection(request);
  EXPECT_EQ(unprepared.state,
            core::NativeProjectionStageResultState::kRejected);
  ASSERT_TRUE(unprepared.error.has_value());
  EXPECT_EQ(unprepared.error->details.at("reason"),
            "native_projection_candidate_model_not_prepared");
  EXPECT_EQ(projection->snapshot(), predecessor_world);

  const auto prepared = projection->PrepareCandidateProjection(
      std::move(candidate_projector), request.planning_collision_model);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  const auto staged = projection->StageProjection(request);
  ASSERT_TRUE(staged.presence_valid());
  EXPECT_EQ(staged.state, core::NativeProjectionStageResultState::kStaged);
  EXPECT_EQ(projection->snapshot(), predecessor_world);
  const auto staged_health = projection->ProjectionHealth();
  EXPECT_EQ(staged_health.state, core::ProjectionHealthState::kHealthy);
  EXPECT_EQ(staged_health.active_projection_revision, predecessor_revision);
  EXPECT_TRUE(staged_health.staged_candidate.has_value());

  const core::NativeProjectionActivationDTO activation{
      request.apply_id,
      request.apply_generation,
      request.projection_stage_generation,
      request.transition_row,
      request.expected_predecessor_projection_revision,
      request.candidate_projection_revision};
  const auto activated = projection->ActivateStagedProjection(activation);
  ASSERT_TRUE(activated.presence_valid());
  EXPECT_EQ(activated.state,
            core::NativeProjectionActivationResultState::kActivated);
  EXPECT_TRUE(
      projection->ProjectionHealth().healthy_for_target(candidate_revision));
  EXPECT_EQ(projection->collision_shared_model(), candidate_shared_model);
  EXPECT_NE(projection->snapshot(), predecessor_world);
}

} // namespace
} // namespace manyforge::roboplan_backend
