#include "ros_managed_runtime_host.hpp"
#include <manyforge_deployment/execution_runtime_contract.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_component_manager/isaac_site_settings.hpp>
#include <manyforge_component_manager/manager_named_apply_coordinator.hpp>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/time/host_boot_identity.hpp>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
#include <manyforge_roboplan/retained_measured_robot_geometry.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_component_owner.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_native_view_predecessor.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include "managed_ros2_component_installation.hpp"
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <manyforge_roboplan/roboplan_scene_factory.hpp>
#include <manyforge_scene/logical_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <fstream>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "execution_host_posix.hpp"
#include "execution_host_commands.hpp"
#include "execution_host_responses.hpp"
#include "execution_host_scene_bridge.hpp"
#include "execution_host_surface.hpp"
#include "ros_host_composition.hpp"
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>

#include "cell_admission_command_codec.hpp"
#include "kernel_apply_fence_rpc_internal.hpp"
#include "execution_motion_target_limits_internal.hpp"
#include "managed_kernel_apply_fence_owner.hpp"
#include "managed_projection_registry.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include "native_projection_rpc_internal.hpp"
#include "owner_settlement_readers.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include "selected_runtime_composition_rpc_internal.hpp"
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>

#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
#include "focused_pick_place_planning_fixture.hpp"
#endif

namespace manyforge::roboplan_backend::host_detail {

Composition::~Composition() {
    if (prepared_controller_launch && !controller_launch_adopted)
      static_cast<void>(host_controller_launch_supervisor().discard_candidate(
          *prepared_controller_launch));
  }

// A failed fence keeps engine/runtime pins, but its uninstalled process
  // has no active owner. Borrowed and already-adopted launches remain owned.
  void Composition::discard_unadopted_controller_launches() {
    if (prepared_controller_launch && !controller_launch_adopted)
      static_cast<void>(host_controller_launch_supervisor().discard_candidate(
          *prepared_controller_launch));
    for (auto &peer : participant_compositions)
      peer->discard_unadopted_controller_launches();
  }

[[nodiscard]] core::ResourceAddressedExecutionKernel *
  Composition::active_addressed_kernel() const noexcept {
    if (cell_runtime != nullptr) {
      return &cell_runtime->addressed_kernel();
    }
    return runtime != nullptr ? runtime->active_addressed_kernel() : nullptr;
  }

[[nodiscard]] const void *Composition::component_runtime_identity() const noexcept {
    return runtime != nullptr
               ? static_cast<const void *>(runtime.get())
               : static_cast<const void *>(gripper_runtime.get());
  }

[[nodiscard]] std::vector<core::RuntimeUnitEpochBindingDTO>
  Composition::host_runtime_unit_epochs() const {
    if (runtime != nullptr)
      return runtime->motion_binding.value().runtime_unit_epochs;
    if (gripper_runtime != nullptr)
      // binding() returns a value; copy its epoch list so callers never retain
      // a reference into that temporary.
      return gripper_runtime->binding().value().runtime_unit_epochs;
    Fail("native component has no runtime-unit identity source");
  }

[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
  Composition::component_registrations() const {
    if (runtime != nullptr)
      return RetainedNativeComponentOwner(runtime).registrations();
    if (gripper_runtime != nullptr)
      return RetainedNativeComponentOwner(gripper_runtime).registrations();
    Fail("native component has no execution registration owner");
  }

Composition::Composition(
      core::ControllableComponentId selected_component_id,
      std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
      std::shared_ptr<const deployment::ProgramBindingContract>
          selected_program,
      std::shared_ptr<const deployment::ProgramBinding> selected_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_gripper_program_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_motion_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_gripper_binding,
      std::optional<core::JointLimitsDTO> selected_direct_control_joint_limits,
      CanonicalValue selected_direct_control_motion_descriptor,
      CanonicalValue selected_direct_control_gripper_descriptor,
      std::shared_ptr<const RetainedPlanningModelStage> selected_stage,
      std::shared_ptr<RetainedNativeRuntime> selected_runtime,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation,
      std::optional<core::TargetRoleId> selected_status_motion_target_role_id,
      std::optional<core::TargetRoleId> selected_status_gripper_target_role_id)
      : controllable_component_id(std::move(selected_component_id)),
        bundle(std::move(selected_bundle)),
        program(std::move(selected_program)),
        program_binding(std::move(selected_binding)),
        direct_control_binding(std::move(selected_direct_control_binding)),
        direct_control_gripper_program_binding(
            std::move(selected_direct_control_gripper_program_binding)),
        direct_control_motion_binding(
            std::move(selected_direct_control_motion_binding)),
        direct_control_gripper_binding(
            std::move(selected_direct_control_gripper_binding)),
        direct_control_joint_limits(
            std::move(selected_direct_control_joint_limits)),
        direct_control_motion_descriptor(
            std::move(selected_direct_control_motion_descriptor)),
        direct_control_gripper_descriptor(
            std::move(selected_direct_control_gripper_descriptor)),
        stage(std::move(selected_stage)), runtime(std::move(selected_runtime)),
        projection_revision(std::move(selected_projection)),
        publication_generation(selected_generation),
        status_motion_target_role_id(
            std::move(selected_status_motion_target_role_id)),
        status_gripper_target_role_id(
            std::move(selected_status_gripper_target_role_id)) {}

void ExecutionCompositionAccess::replace_current(Composition *replacement) {
    // Caller holds mutex exclusively and keeps the predecessor alive. A new
    // Program/Scene publication may still use the same physical controllers;
    // only a controller actually retired by the component delta loses its
    // observed-state source. Waking a retained channel would falsely report
    // a controller disconnect to readers waiting on that stable source.
    if (current != nullptr && current != replacement) {
      std::set<const RetainedNativeRuntime *> retained_motion;
      std::set<const RetainedNativeGripperRuntime *> retained_grippers;
      const auto include_retained = [&](const Composition &composition) {
        if (composition.runtime != nullptr)
          retained_motion.insert(composition.runtime.get());
        if (composition.gripper_runtime != nullptr)
          retained_grippers.insert(composition.gripper_runtime.get());
      };
      if (replacement != nullptr) {
        include_retained(*replacement);
        for (const auto &participant : replacement->participant_compositions)
          include_retained(*participant);
      }
      const auto wake_if_retired = [&](const Composition &composition) {
        if (composition.runtime != nullptr &&
            !retained_motion.contains(composition.runtime.get())) {
          if (composition.runtime->motion_state_channel != nullptr)
            composition.runtime->motion_state_channel
                ->notify_composition_replaced();
          if (composition.runtime->gripper_state_channel != nullptr)
            composition.runtime->gripper_state_channel
                ->notify_composition_replaced();
        }
        if (composition.gripper_runtime != nullptr &&
            !retained_grippers.contains(composition.gripper_runtime.get())) {
          if (const auto channel = composition.gripper_runtime->state_channel();
              channel != nullptr)
            channel->notify_composition_replaced();
        }
      };
      wake_if_retired(*current);
      for (const auto &participant : current->participant_compositions)
        wake_if_retired(*participant);
    }
    current = replacement;
  }
} // namespace manyforge::roboplan_backend::host_detail

namespace manyforge::roboplan_backend {
using namespace host_detail;
namespace {

namespace scene = manyforge::scene;

[[nodiscard]] std::vector<RoboPlanJointLimitInput>
RoboPlanJointLimitsFromRetainedModel(
    const deployment::UrdfDocumentIr &urdf) {
  std::vector<RoboPlanJointLimitInput> result;
  result.reserve(urdf.joints.size());
  for (const auto &joint : urdf.joints) {
    if (joint.type == deployment::UrdfJointType::kFixed ||
        joint.type == deployment::UrdfJointType::kFloating ||
        joint.type == deployment::UrdfJointType::kPlanar ||
        joint.mimic.has_value()) {
      continue;
    }
    Require(joint.limit.has_value(),
            "retained movable joint '" + joint.name +
                "' has no explicit dynamics limits");
    const auto &limit = *joint.limit;
    Require(std::isfinite(limit.velocity) && limit.velocity > 0.0,
            "retained movable joint '" + joint.name +
                "' has no positive velocity limit");

    RoboPlanJointLimitInput row;
    row.joint_name = joint.name;
    row.maximum_velocity = {limit.velocity};
    if (joint.type == deployment::UrdfJointType::kRevolute ||
        joint.type == deployment::UrdfJointType::kPrismatic) {
      Require(limit.authored_lower.has_value() &&
                  limit.authored_upper.has_value(),
              "retained bounded joint '" + joint.name +
                  "' has incomplete position limits");
      row.minimum_position = std::vector<double>{limit.lower};
      row.maximum_position = std::vector<double>{limit.upper};
    }
    if (limit.acceleration.has_value()) {
      row.maximum_acceleration =
          std::vector<double>{*limit.acceleration};
    }
    result.push_back(std::move(row));
  }
  std::sort(result.begin(), result.end(), [](const auto &left,
                                              const auto &right) {
    return left.joint_name < right.joint_name;
  });
  Require(std::adjacent_find(
              result.begin(), result.end(), [](const auto &left,
                                                const auto &right) {
                return left.joint_name == right.joint_name;
              }) == result.end(),
          "retained model repeats a movable joint limit");
  return result;
}

// The RoboPlan worker reloads this base model by path whenever it restarts;
// the stage that owns those files must outlive every worker generation.
[[nodiscard]] RoboPlanWorkerModelDTO NativeWorkerBaseModel(
    const RetainedPlanningModelStage &stage,
    std::vector<RoboPlanJointLimitInput> joint_limits) {
  return RoboPlanWorkerModelDTO{"manyforge_i6_native_execution_worker",
                                stage.loader_urdf_path(),
                                stage.srdf_path(),
                                stage.package_paths(),
                                std::move(joint_limits),
                                {},
                                {}};
}

// One candidate RoboPlan scene and its exact state projector for the given
// bindings, built from the staged candidate whole-cell planning model.
struct CandidateRetainedModelScene final {
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
};

template <typename RetainedBundle, typename ModelMap, typename PlanningModel>
[[nodiscard]] CandidateRetainedModelScene BuildCandidateRetainedModelScene(
    std::string_view scene_name, const RetainedPlanningModelStage &stage,
    const PlanningModel &planning_model, const RetainedBundle &bundle,
    const ModelMap &model_map,
    std::vector<core::RuntimeParticipantBinding> bindings) {
  const auto srdf = planning_model.srdf_bytes();
  const auto documents = deployment::parse_model_documents_for_projection(
      planning_model.urdf_bytes(),
      srdf.empty() ? std::optional<std::string_view>{}
                   : std::optional<std::string_view>{srdf});
  CandidateRetainedModelScene result;
  result.scene = make_roboplan_scene(RoboPlanSceneInput{
      std::string(scene_name), std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(),
      RoboPlanJointLimitsFromRetainedModel(documents.urdf)});
  result.scene_mutex = std::make_shared<std::mutex>();
  result.state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          result.scene, result.scene_mutex, bundle, model_map, planning_model,
          std::move(bindings), 0));
  return result;
}

struct ManagedComponentProjectionSet final {
  core::ProjectionRevisionDTO projection_revision;
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      by_component;
  // Representatives do not own every provider required by the published model.
  // Keep the complete exact selection alive through build and installation.
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required_projection_pins;
};

[[nodiscard]] std::optional<ManagedComponentProjectionSet>
SelectManagedComponentProjections(const ManagedRuntimeChildConfig &config,
                                  const deployment::RetainedBundle &bundle,
                                  const ManagedProjectionRegistry &projections,
                                  const core::ProjectionRevisionDTO &expected,
                                  const bool allow_current_scene_revision) {
  const auto unavailable = [](const std::string &detail) {
    static std::mutex mutex;
    static std::string last_detail;
    std::lock_guard lock(mutex);
    if (last_detail != detail) {
      last_detail = detail;
      std::cerr << "managed component projection selection: " << detail
                << '\n';
    }
    return std::optional<ManagedComponentProjectionSet>{};
  };
  ManagedComponentProjectionSet selected;
  bool revision_selected = false;
  std::shared_ptr<RetainedPlannerSceneProjection> cell_projection;
  std::set<core::ControllableComponentId> motion_projection_components;
  const auto supported_component = [&](const core::ControllableComponentId &id) {
    return std::any_of(bundle.value().cell_graph.control_bindings.begin(),
                       bundle.value().cell_graph.control_bindings.end(),
                       [&](const auto &binding) {
                         return binding.controllable_component_id == id &&
                                binding.provider_runtime_unit_id ==
                                    core::RuntimeUnitId(config.runtime_unit_id) &&
                                (binding.capability_id.value() ==
                                     "execution.trajectory.execute" ||
                                 binding.capability_id.value() ==
                                     "tool.gripper.command");
                       });
  };
  for (const auto &provider : bundle.value().capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            provider.capability_id, provider.capability_definition_revision)) {
      continue;
    }
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : provider.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      Require(runtime != nullptr && !runtime->empty(),
              "managed joint planner has a malformed runtime endpoint");
      runtime_sources.insert(*runtime);
    }
    if (runtime_sources.size() != 1U ||
        runtime_sources.begin()->value() != config.runtime_unit_id) {
      continue;
    }
    const core::ProjectionNativeRuntimeBindingDTO binding{
        provider.capability_provider_binding_id,
        provider.capability_provider_binding_revision,
        core::RuntimeUnitId(config.runtime_unit_id),
        core::RuntimeEpoch(config.runtime_epoch)};
    auto revision = expected;
    auto projection = projections.FindActiveProjection(revision, binding);
    if (projection == nullptr && allow_current_scene_revision) {
      const auto current =
          projections.FindCurrentProjectionRevision(expected, binding);
      if (!current.has_value())
        return unavailable("no current revision for " +
                           binding.provider_binding_id.value());
      revision = *current;
      projection = projections.FindActiveProjection(revision, binding);
    }
    if (projection == nullptr)
      return unavailable("no active projection for " +
                         binding.provider_binding_id.value());
    selected.required_projection_pins.push_back(projection);
    if (cell_projection == nullptr)
      cell_projection = projection;
    if (!revision_selected) {
      selected.projection_revision = revision;
      revision_selected = true;
    } else {
      if (selected.projection_revision != revision)
        return unavailable("component projections have different revisions");
    }
    if (const auto *target =
            std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
                &provider.target);
        target != nullptr &&
        supported_component(target->controllable_component_id)) {
      // A motion owner must use its own joint-goal projection. Other native
      // providers may share that component but cannot silently substitute
      // their projection for the planner's bound one.
      if (provider.capability_id.value() == "planning.motion.joint_goal") {
        Require(motion_projection_components
                    .insert(target->controllable_component_id)
                    .second,
                "managed motion component has multiple joint planners");
        selected.by_component[target->controllable_component_id] = projection;
      } else {
        selected.by_component.try_emplace(target->controllable_component_id,
                                          projection);
      }
    }
  }
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id ==
            core::RuntimeUnitId(config.runtime_unit_id) &&
        binding.capability_id.value() == "execution.trajectory.execute" &&
        !motion_projection_components.contains(
            binding.controllable_component_id))
      return unavailable("motion component has no active native projection");
  }
  if (!revision_selected) {
    selected.projection_revision = expected;
    return selected;
  }
  // A gripper with an independent controller need not have a motion planner.
  // Its frame/Scene view is the same cell projection already selected for the
  // motion owners; this does not create a planner or a projection lease for
  // the gripper.
  Require(cell_projection != nullptr,
          "selected native projection has no cell model");
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id !=
            core::RuntimeUnitId(config.runtime_unit_id) ||
        (binding.capability_id.value() != "execution.trajectory.execute" &&
         binding.capability_id.value() != "tool.gripper.command"))
      continue;
    selected.by_component.try_emplace(binding.controllable_component_id,
                                      cell_projection);
  }
  return selected;
}

[[nodiscard]] int
RunManagedRuntimeChild(const ManagedRuntimeChildConfig &config);
[[nodiscard]] RoboPlanCandidateBindingDTO
CandidateBinding(const core::RuntimeParticipantBinding &runtime) {
  const auto &value = runtime.value();
  Require(value.model_groups.size() == 1U,
          "motion binding has no sole retained model group");
  const auto &group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &candidate) {
        return candidate.model_instance_id == group.model_instance_id;
      });
  Require(model != value.model_bindings.end(),
          "motion model group has no retained model revision");
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  resources.reserve(value.resources.size());
  compiled_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    compiled_joint_names.push_back(resource.compiled_resource_name);
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
      std::move(compiled_joint_names), std::move(frames)};
}

[[nodiscard]] core::LogicalSceneSnapshotDTO
EmptyScene(const core::CellId &cell_id, const core::SceneRevision &revision) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::LogicalSceneSnapshotDTO snapshot{revision, cell_id,           0U, 0U,
                                         0U,       std::move(policy), {}, {}};
  Require(core::logical_scene_identity::ComputeLogicalSceneContentHash(
              snapshot) == revision.content_hash &&
              snapshot.locally_valid(),
          "active scene revision is not the exact canonical empty scene; "
          "live scene consumption starts at N5");
  return snapshot;
}

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO
ActiveFacts(const deployment::RuntimeProgramTargetBinding &primary,
            std::span<const deployment::RuntimeProgramTargetBinding> supporting,
            const core::RuntimeEpoch &runtime_epoch,
            core::ActiveControlConfigurationId active_configuration_id) {
  Require(primary.desired_route().has_value(),
          "runtime binding has no desired I6 route");
  std::set<core::RuntimeUnitId> runtime_units(
      primary.component().runtime_unit_ids.begin(),
      primary.component().runtime_unit_ids.end());
  if (primary.control_domain().has_value()) {
    runtime_units.insert(primary.control_domain()->runtime_unit_ids.begin(),
                         primary.control_domain()->runtime_unit_ids.end());
  }
  std::map<core::ProviderInstanceId, core::RuntimeUnitId> providers;
  const auto insert_provider = [&](const core::ProviderInstanceId &provider,
                                   const core::RuntimeUnitId &runtime_unit) {
    const auto [found, inserted] = providers.emplace(provider, runtime_unit);
    Require(inserted || found->second == runtime_unit,
            "runtime provider is assigned to conflicting runtime units");
    runtime_units.insert(runtime_unit);
  };
  if (primary.control_binding().has_value()) {
    insert_provider(primary.control_binding()->provider_instance_id,
                    primary.control_binding()->provider_runtime_unit_id);
  }
  if (primary.capability_provider_binding().has_value()) {
    insert_provider(
        primary.capability_provider_binding()->provider_instance_id,
        retained_provider_runtime_unit(*primary.capability_provider_binding()));
  }
  for (const auto &support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      insert_provider(
          support.capability_provider_binding()->provider_instance_id,
          retained_provider_runtime_unit(
              *support.capability_provider_binding()));
    }
  }
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  runtime_epochs.reserve(runtime_units.size());
  for (const auto &runtime_unit : runtime_units) {
    runtime_epochs.push_back({runtime_unit, runtime_epoch});
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(providers.size());
  for (const auto &[provider, runtime_unit] : providers) {
    provider_epochs.push_back({provider, runtime_unit, runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      *primary.desired_route(), std::move(active_configuration_id),
      core::ActiveControlConfigurationGeneration(1U), std::move(runtime_epochs),
      std::move(provider_epochs)};
}

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
BuildControlBindingRepresentatives(
    const deployment::ProgramBinding &direct_control_binding,
    const core::RuntimeParticipantBinding &primary_binding,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const core::RuntimeEpoch &runtime_epoch,
    const core::ActiveControlConfigurationId &active_configuration_id) {
  const auto &primary = primary_binding.value();
  Require(primary.control_path.has_value(),
          "direct-control primary has no exact control path");
  const auto &primary_path = *primary.control_path;
  const auto &targets = direct_control_binding.value().target_role_bindings;
  const auto target =
      std::find_if(targets.begin(), targets.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   primary.controllable_component_id &&
               candidate.command_target_id == primary.command_target_id;
      });
  Require(target != targets.end() &&
              std::find_if(std::next(target), targets.end(),
                           [&](const auto &candidate) {
                             return candidate.controllable_component_id ==
                                        primary.controllable_component_id &&
                                    candidate.command_target_id ==
                                        primary.command_target_id;
                           }) == targets.end(),
          "direct-control physical target is absent or ambiguous");

  std::vector<core::RuntimeParticipantBinding> representatives;
  for (const auto &capability : target->capability_bindings) {
    if (!capability.control_binding.has_value()) {
      continue;
    }
    if (capability.capability_id == primary.capability_id) {
      representatives.push_back(primary_binding);
      continue;
    }
    const auto resolved = deployment::resolve_runtime_program_target(
        direct_control_binding, target->target_role_id,
        capability.capability_id, bundle, model_map);
    auto sealed = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            resolved,
            ActiveFacts(resolved, {}, runtime_epoch, active_configuration_id)));
    if (!sealed.ok()) {
      Fail("direct-control control-binding representative failed "
           "verification: " +
           sealed.error().message);
    }
    representatives.push_back(std::move(sealed.value()));
  }
  std::sort(representatives.begin(), representatives.end(),
            [](const auto &left, const auto &right) {
              return left.value().control_path->control_binding_id <
                     right.value().control_path->control_binding_id;
            });

  const auto &desired = bundle.value().desired_control_configurations;
  const auto configuration =
      std::find_if(desired.begin(), desired.end(), [&](const auto &candidate) {
        return candidate.control_domain_id == primary_path.control_domain_id;
      });
  Require(configuration != desired.end() &&
              std::find_if(std::next(configuration), desired.end(),
                           [&](const auto &candidate) {
                             return candidate.control_domain_id ==
                                    primary_path.control_domain_id;
                           }) == desired.end() &&
              representatives.size() == configuration->binding_routes.size(),
          "direct-control target does not cover its exact desired domain");
  for (const auto &route : configuration->binding_routes) {
    const auto found = std::find_if(
        representatives.begin(), representatives.end(),
        [&](const auto &candidate) {
          const auto &path = *candidate.value().control_path;
          return path.control_binding_id == route.control_binding_id &&
                 path.control_binding_revision ==
                     route.control_binding_revision &&
                 path.control_route_id == route.control_route_id;
        });
    Require(found != representatives.end(),
            "direct-control target omits one desired control binding");
  }
  return representatives;
}

[[nodiscard]] CanonicalValue
ManualControlContractValue(const core::RuntimeParticipantBindingDTO &binding) {
  CanonicalValue::Array supporting_providers;
  supporting_providers.reserve(
      binding.supporting_capability_provider_paths.size());
  for (const auto &path : binding.supporting_capability_provider_paths) {
    supporting_providers.emplace_back(CanonicalValue::Object{
        {"capability_definition_revision",
         CanonicalValue(path.capability_definition_revision.value())},
        {"capability_id", CanonicalValue(path.capability_id.value())},
        {"capability_provider_binding_id",
         CanonicalValue(path.capability_provider_binding_id.value())},
        {"capability_provider_binding_revision",
         CanonicalValue(path.capability_provider_binding_revision.value())},
        {"provider_manifest_id",
         CanonicalValue(path.provider_manifest_id.value())},
        {"provider_manifest_revision",
         CanonicalValue(path.provider_manifest_revision.value())},
    });
  }
  CanonicalValue command_schema(nullptr);
  if (binding.command_schema.has_value()) {
    command_schema = CanonicalValue(CanonicalValue::Object{
        {"command_schema_id",
         CanonicalValue(binding.command_schema->command_schema_id.value())},
        {"command_schema_revision",
         CanonicalValue(
             binding.command_schema->command_schema_revision.value())},
    });
  }
  CanonicalValue control_path(nullptr);
  if (binding.control_path.has_value()) {
    const auto &path = *binding.control_path;
    control_path = CanonicalValue(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(path.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(path.control_binding_revision.value())},
        {"control_interface_contract_id",
         CanonicalValue(path.control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(path.control_interface_revision.value())},
        {"controller_topology_id",
         CanonicalValue(path.controller_topology_id.value())},
        {"stop_evidence_policy_id",
         CanonicalValue(path.stop_evidence_policy_id.value())},
        {"stop_evidence_policy_revision",
         CanonicalValue(path.stop_evidence_policy_revision.value())},
        {"topology_revision", CanonicalValue(path.topology_revision.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"command_schema", std::move(command_schema)},
      {"control_path", std::move(control_path)},
      {"operation_capability_definition_revision",
       CanonicalValue(binding.capability_definition_revision.value())},
      {"operation_capability_id",
       CanonicalValue(binding.capability_id.value())},
      {"supporting_providers", CanonicalValue(std::move(supporting_providers))},
  });
}

struct ManualControlDescriptors final {
  CanonicalValue motion;
  CanonicalValue gripper;
};

[[nodiscard]] CanonicalValue
CanonicalNullableNumber(const std::optional<double> &value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(value.value());
}

[[nodiscard]] CanonicalValue
CanonicalNullableText(const std::optional<std::string> &value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(*value);
}

[[nodiscard]] CanonicalValue BuildManualControlGripperDescriptor(
    const core::RuntimeParticipantBinding &gripper_binding,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::vector<deployment::ros2_control_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents) {
  const auto &gripper = gripper_binding.value();
  Require(gripper.resources.size() == 1U,
          "manual-control descriptor crosses its exact resource inventory");
  const auto &resource = gripper.resources.front();
  CanonicalValue::Array followers;
  if (gripper.control_path.has_value()) {
    const auto *coordinate_map = model_map.find_binding(
        gripper.controllable_component_id, gripper.command_target_id,
        gripper.control_path->control_binding_id);
    Require(coordinate_map != nullptr,
            "manual-control gripper descriptor has no retained coordinate map");
    const auto row = std::find_if(
        coordinate_map->resources.begin(), coordinate_map->resources.end(),
        [&](const auto &candidate) {
          return candidate.resource_id == resource.resource_id;
        });
    Require(row != coordinate_map->resources.end(),
            "manual-control gripper descriptor has no exact resource row");
    Require(resource.source_joint_name.has_value()
                ? !row->coordinates.empty() &&
                      !row->component_local_coordinate_kind.has_value()
                : row->coordinates.empty() &&
                      row->component_local_coordinate_kind ==
                          resource.coordinate_kind,
            "manual-control gripper descriptor crosses coordinate semantics");
    for (const auto &coordinate : row->coordinates) {
      if (!coordinate.derived_mimic) {
        continue;
      }
      followers.emplace_back(CanonicalValue::Object{
          {"compiled_coordinate_name",
           CanonicalValue(coordinate.compiled_joint_name)},
          {"multiplier", CanonicalValue(coordinate.multiplier)},
          {"offset", CanonicalValue(coordinate.offset)},
          {"source_coordinate_name",
           CanonicalValue(coordinate.source_joint_name)},
      });
    }
  }

  std::map<std::string, CanonicalValue> named_presets;
  std::optional<core::ModelInstanceId> gripper_model_instance_id;
  std::optional<std::string> source_group_name;
  if (documents.srdf.has_value() && resource.source_joint_name.has_value()) {
    const auto group = std::find_if(
        gripper.model_groups.begin(), gripper.model_groups.end(),
        [&](const auto &candidate) {
          return std::find(candidate.expanded_compiled_joint_names.begin(),
                           candidate.expanded_compiled_joint_names.end(),
                           resource.compiled_resource_name) !=
                 candidate.expanded_compiled_joint_names.end();
        });
    Require(
        group != gripper.model_groups.end() &&
            std::find_if(
                std::next(group), gripper.model_groups.end(),
                [&](const auto &candidate) {
                  return std::find(
                             candidate.expanded_compiled_joint_names.begin(),
                             candidate.expanded_compiled_joint_names.end(),
                             resource.compiled_resource_name) !=
                         candidate.expanded_compiled_joint_names.end();
                }) == gripper.model_groups.end(),
        "manual-control gripper resource has no unique retained group");
    const auto source_group = std::find_if(
        model_map.symbols().begin(), model_map.symbols().end(),
        [&](const auto &symbol) {
          return symbol.source.model_instance_id == group->model_instance_id &&
                 symbol.source.kind ==
                     deployment::OccurrenceSymbolKind::kSrdfGroup &&
                 symbol.compiled_name == group->compiled_group_name;
        });
    Require(source_group != model_map.symbols().end(),
            "manual-control gripper group has no retained source identity");
    gripper_model_instance_id = group->model_instance_id;
    source_group_name = source_group->source.source_name;
    std::map<std::string, std::string> source_state_names;
    for (const auto &symbol : model_map.symbols()) {
      if (symbol.source.model_instance_id != group->model_instance_id ||
          symbol.source.kind !=
              deployment::OccurrenceSymbolKind::kSrdfGroupState) {
        continue;
      }
      Require(source_state_names
                  .emplace(symbol.compiled_name, symbol.source.source_name)
                  .second,
              "manual-control preset has no unique retained source name");
    }
    std::map<std::string, std::string> source_coordinate_names;
    for (const auto &coordinate : model_map.coordinates()) {
      if (coordinate.model_instance_id != group->model_instance_id) {
        continue;
      }
      Require(source_coordinate_names
                  .emplace(coordinate.compiled_joint_name,
                           coordinate.source_joint_name)
                  .second,
              "manual-control preset coordinate is not unique");
    }
    for (const auto &state : documents.srdf->group_states) {
      if (state.group_name != group->compiled_group_name) {
        continue;
      }
      const auto master = std::find_if(
          state.joints.begin(), state.joints.end(), [&](const auto &joint) {
            return joint.joint_name == resource.compiled_resource_name;
          });
      if (master == state.joints.end()) {
        continue;
      }
      const auto symbol = source_state_names.find(state.name);
      Require(symbol != source_state_names.end(),
              "manual-control preset has no unique retained source name");
      CanonicalValue::Array coordinates;
      coordinates.reserve(state.joints.size());
      for (const auto &joint : state.joints) {
        const auto source = source_coordinate_names.find(joint.joint_name);
        Require(source != source_coordinate_names.end(),
                "manual-control preset names an unknown retained coordinate");
        coordinates.emplace_back(CanonicalValue::Object{
            {"position", CanonicalValue(joint.value)},
            {"source_coordinate_name", CanonicalValue(source->second)},
        });
      }
      Require(
          named_presets
              .emplace(
                  symbol->second,
                  CanonicalValue(CanonicalValue::Object{
                      {"coordinates", CanonicalValue(std::move(coordinates))},
                      {"master_position", CanonicalValue(master->value)},
                      {"name", CanonicalValue(symbol->second)},
                  }))
              .second,
          "manual-control preset source name is not unique");
    }
  }
  CanonicalValue::Array presets;
  presets.reserve(named_presets.size());
  for (auto &entry : named_presets) {
    presets.emplace_back(std::move(entry.second));
  }

  const auto authored_configuration = std::find_if(
      gripper_target_configurations.begin(),
      gripper_target_configurations.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   gripper.controllable_component_id &&
               candidate.command_target_id == gripper.command_target_id;
      });
  const auto has_authored_configuration =
      authored_configuration != gripper_target_configurations.end();
  if (has_authored_configuration) {
    Require(gripper_model_instance_id.has_value() &&
                source_group_name.has_value(),
            "authored gripper configuration has no exact retained model group");
    Require(
        named_presets.contains(authored_configuration->open_state_ref) &&
            named_presets.contains(authored_configuration->closed_state_ref),
        "authored gripper state reference is not a retained SRDF preset");
    const auto require_source_link = [&](const std::string &source_name,
                                         const std::string_view field) {
      Require(
          std::count_if(model_map.symbols().begin(), model_map.symbols().end(),
                        [&](const auto &symbol) {
                          return symbol.source.model_instance_id ==
                                     *gripper_model_instance_id &&
                                 symbol.source.kind ==
                                     deployment::OccurrenceSymbolKind::kLink &&
                                 symbol.source.source_name == source_name;
                        }) == 1,
          "authored gripper " + std::string(field) +
              " is not one exact retained source link");
    };
    require_source_link(authored_configuration->mount_frame, "mount frame");
    require_source_link(authored_configuration->attach_frame, "attach frame");
    require_source_link(authored_configuration->tcp_frame, "TCP frame");
    for (const auto &link : authored_configuration->touch_links) {
      require_source_link(link, "touch link");
    }
    for (const auto &frame : authored_configuration->auxiliary_frames) {
      require_source_link(frame, "auxiliary frame");
    }
  }
  CanonicalValue::Array touch_links;
  CanonicalValue::Array auxiliary_frames;
  std::optional<std::string> description;
  std::optional<std::string> actuation_kind;
  std::optional<std::string> output_mode;
  std::optional<std::string> open_state_ref;
  std::optional<std::string> closed_state_ref;
  std::optional<double> command_duration_s;
  std::optional<std::string> mount_frame;
  std::optional<std::string> attach_frame;
  std::optional<std::string> tcp_frame;
  if (has_authored_configuration) {
    description = authored_configuration->description;
    actuation_kind = authored_configuration->actuation_kind;
    output_mode = authored_configuration->output_mode == core::GripperOutputMode::kNone ? "none" : "commanded";
    open_state_ref = authored_configuration->open_state_ref;
    closed_state_ref = authored_configuration->closed_state_ref;
    command_duration_s = authored_configuration->command_duration_s;
    mount_frame = authored_configuration->mount_frame;
    attach_frame = authored_configuration->attach_frame;
    tcp_frame = authored_configuration->tcp_frame;
    touch_links.reserve(authored_configuration->touch_links.size());
    for (const auto &link : authored_configuration->touch_links) {
      touch_links.emplace_back(link);
    }
    auxiliary_frames.reserve(authored_configuration->auxiliary_frames.size());
    for (const auto &frame : authored_configuration->auxiliary_frames) {
      auxiliary_frames.emplace_back(frame);
    }
  }

  std::optional<std::string> state_authority_id;
  std::optional<std::string> state_authority_class;
  std::optional<std::string> control_interface_contract_id;
  if (gripper.control_path.has_value()) {
    state_authority_id = gripper.control_path->state_authority_id.value();
    state_authority_class = std::string(
        core::to_string(gripper.control_path->state_authority_class));
    control_interface_contract_id =
        gripper.control_path->control_interface_contract_id.value();
  }

  const auto coordinate_kind =
      resource.coordinate_kind == core::CoordinateKind::kAngular ? "angular_rad"
                                                                 : "linear_m";
  return CanonicalValue(CanonicalValue::Object{
      {"actuation_kind", CanonicalNullableText(actuation_kind)},
      {"output_mode", CanonicalNullableText(output_mode)},
      {"attach_frame", CanonicalNullableText(attach_frame)},
      {"auxiliary_frames", CanonicalValue(std::move(auxiliary_frames))},
      {"closed_state_ref", CanonicalNullableText(closed_state_ref)},
      {"command_target_id", CanonicalValue(gripper.command_target_id.value())},
      {"command_duration_s", CanonicalNullableNumber(command_duration_s)},
      {"command_kind", CanonicalValue("gripper_position")},
      {"component_role_id", CanonicalValue(gripper.component_role_id.value())},
      {"completion_policy", CanonicalValue(actuation_kind == "pneumatic"
          ? (output_mode == "none" ? "no_output" : "output_delivery")
          : "goal_fenced_target_reached")},
      {"control_interface_contract_id",
       CanonicalNullableText(control_interface_contract_id)},
      {"contracts", ManualControlContractValue(gripper)},
      {"controllable_component_id",
       CanonicalValue(gripper.controllable_component_id.value())},
      {"coordinate_kind", CanonicalValue(coordinate_kind)},
      {"description", CanonicalNullableText(description)},
      {"grasp_confirmation_policy",
       CanonicalValue(actuation_kind == "pneumatic" ? "not_required" : "binding_feedback_required")},
      {"mimic_followers", CanonicalValue(std::move(followers))},
      {"max_position", gripper_limits.has_value()
                           ? CanonicalValue(gripper_limits->upper_position)
                           : CanonicalValue(nullptr)},
      {"min_position", gripper_limits.has_value()
                           ? CanonicalValue(gripper_limits->lower_position)
                           : CanonicalValue(nullptr)},
      {"mount_frame", CanonicalNullableText(mount_frame)},
      {"named_presets", CanonicalValue(std::move(presets))},
      {"open_state_ref", CanonicalNullableText(open_state_ref)},
      {"schema_version",
       CanonicalValue("manyforge.manual_control_gripper_descriptor.v3")},
      {"source_coordinate_name",
       CanonicalValue(resource.source_joint_name.has_value()
                          ? *resource.source_joint_name
                          : resource.compiled_resource_name)},
      {"source_group_name", CanonicalNullableText(source_group_name)},
      {"source_kind", CanonicalValue(resource.source_joint_name.has_value()
                                         ? "model_role"
                                         : "component_local")},
      {"state_authority_class", CanonicalNullableText(state_authority_class)},
      {"state_authority_id", CanonicalNullableText(state_authority_id)},
      {"target_role_id", CanonicalValue(gripper.target_role_id.value())},
      {"tcp_frame", CanonicalNullableText(tcp_frame)},
      {"touch_links", CanonicalValue(std::move(touch_links))},
      {"transport_kind", CanonicalValue("resolved_binding")},
      {"unit", CanonicalValue(coordinate_kind == std::string_view("angular_rad")
                                  ? "rad"
                                  : "m")},
  });
}

[[nodiscard]] ManualControlDescriptors BuildManualControlDescriptors(
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const core::JointLimitsDTO &motion_limits,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &motion_participant_policy,
    const std::optional<double> &deployment_maximum_tcp_linear_speed_mps,
    const deployment::execution_runtime_contract::
        OrdinaryMotionJerkSettings *ordinary_jerk_settings,
    const std::vector<deployment::execution_runtime_contract::
                          Configuration::DirectControlJointLimit>
        &direct_control_joint_limits,
    const std::vector<deployment::ros2_control_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents) {
  const auto &motion = motion_binding.value();
  Require(
      motion.resources.size() == motion_limits.joint_names.size() &&
          motion.resources.size() == motion_limits.position_lower_rad.size() &&
          motion.resources.size() == motion_limits.position_upper_rad.size() &&
          motion.resources.size() == motion_limits.velocity_max_rad_s.size() &&
          (motion_limits.acceleration_max_rad_s2.empty() ||
           motion.resources.size() ==
               motion_limits.acceleration_max_rad_s2.size()),
      "manual-control descriptor crosses its exact resource inventory");

  CanonicalValue::Array frames;
  frames.reserve(motion.frame_bindings.size());
  for (const auto &frame : motion.frame_bindings) {
    frames.emplace_back(CanonicalValue::Object{
        {"compiled_frame_name", CanonicalValue(frame.compiled_frame_name)},
        {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
        {"model_instance_id", CanonicalValue(frame.model_instance_id.value())},
        {"model_revision", CanonicalValue(frame.model_revision.value())},
    });
  }
  CanonicalValue::Array joints;
  joints.reserve(motion.resources.size());
  for (std::size_t index = 0U; index < motion.resources.size(); ++index) {
    const auto &resource = motion.resources[index];
    Require(resource.source_joint_name.has_value(),
            "manual-control motion descriptor has a non-model coordinate");
    Require(motion_limits.joint_names[index] == resource.compiled_resource_name,
            "manual-control motion descriptor crosses its exact joint order");
    joints.emplace_back(CanonicalValue::Object{
        {"compiled_joint_name",
         CanonicalValue(resource.compiled_resource_name)},
        {"max_acceleration_rad_s2",
         motion_limits.acceleration_max_rad_s2.empty()
             ? CanonicalValue(nullptr)
             : CanonicalValue(motion_limits.acceleration_max_rad_s2[index])},
        {"max_position_rad",
         CanonicalValue(motion_limits.position_upper_rad[index])},
        {"max_velocity_rad_s",
         CanonicalValue(motion_limits.velocity_max_rad_s[index])},
        {"min_position_rad",
         CanonicalValue(motion_limits.position_lower_rad[index])},
        {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
        {"unit", CanonicalValue("rad")},
    });
  }
  CanonicalValue operational_limit(nullptr);
  if (deployment_maximum_tcp_linear_speed_mps.has_value()) {
    operational_limit = CanonicalValue(CanonicalValue::Object{
        {"maximum_tcp_linear_speed_mps",
         CanonicalValue(*deployment_maximum_tcp_linear_speed_mps)},
    });
  }
  CanonicalValue::Array operational_joint_limits;
  CanonicalValue::Array ordinary_jerk_joints;
  operational_joint_limits.reserve(motion.resources.size());
  for (std::size_t index = 0U; index < motion.resources.size(); ++index) {
    const auto &resource = motion.resources[index];
    const auto authored = std::find_if(
        direct_control_joint_limits.begin(), direct_control_joint_limits.end(),
        [&](const auto &candidate) {
          return candidate.controllable_component_id ==
                     motion.controllable_component_id &&
                 candidate.command_target_id == motion.command_target_id &&
                 resource.source_joint_name.has_value() &&
                 candidate.source_joint_name == *resource.source_joint_name;
        });
    std::optional<double> authored_velocity;
    std::optional<double> authored_acceleration;
    std::optional<double> authored_jerk;
    if (authored != direct_control_joint_limits.end()) {
      authored_velocity = authored->max_velocity_rad_s;
      authored_acceleration = authored->max_acceleration_rad_s2;
      authored_jerk = authored->max_jerk_rad_s3;
    }
    const auto model_acceleration =
        motion_limits.acceleration_max_rad_s2.empty()
            ? std::optional<double>{}
            : std::optional<double>{
                  motion_limits.acceleration_max_rad_s2[index]};
    auto effective_acceleration = model_acceleration;
    if (authored_acceleration.has_value() &&
        (!effective_acceleration.has_value() ||
         authored_acceleration.value() < effective_acceleration.value())) {
      effective_acceleration = authored_acceleration;
    }
    auto effective_velocity = motion_limits.velocity_max_rad_s[index];
    if (authored_velocity.has_value()) {
      effective_velocity =
          std::min(effective_velocity, authored_velocity.value());
    }
    auto effective_jerk = CanonicalNullableNumber(authored_jerk);
    if (motion_participant_policy &&
        motion_participant_policy->value().ordinary_motion_jerk) {
      const auto &profile =
          *motion_participant_policy->value().ordinary_motion_jerk;
      if (!profile.enabled) {
        effective_jerk = CanonicalValue(nullptr);
      } else {
        const double ceiling =
            std::get<core::AngularJerkDTO>(
                profile.limits.at(*resource.source_joint_name))
                .value_rad_s3;
        effective_jerk = CanonicalValue(
            authored_jerk ? std::min(*authored_jerk, ceiling) : ceiling);
      }
    }
    if (ordinary_jerk_settings != nullptr) {
      const auto &settings = *ordinary_jerk_settings;
      const auto *profile =
          motion_participant_policy &&
                  motion_participant_policy->value().ordinary_motion_jerk
              ? &*motion_participant_policy->value().ordinary_motion_jerk
              : nullptr;
      if (settings.enabled) {
        Require(profile != nullptr && profile->enabled,
                "enabled ordinary jerk descriptor requires its resolved policy");
      } else {
        effective_jerk = CanonicalValue(nullptr);
      }
      const auto setting = std::find_if(
          settings.joint_limits.begin(), settings.joint_limits.end(),
          [&](const auto &row) {
            return row.source_joint_name == *resource.source_joint_name;
          });
      const auto *imported =
          setting != settings.joint_limits.end() && setting->imported
              ? &*setting->imported
              : nullptr;
      const char *source =
          !settings.enabled ? "off"
          : setting != settings.joint_limits.end() && setting->override_rad_s3
              ? "override"
          : imported != nullptr && imported->enabled ? "imported"
                                                     : "preset";
      ordinary_jerk_joints.emplace_back(CanonicalValue::Object{
          {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
          {"source", CanonicalValue(source)},
          {"maximum_rad_s3",
           settings.enabled
               ? CanonicalValue(
                     std::get<core::AngularJerkDTO>(
                         profile->limits.at(*resource.source_joint_name))
                         .value_rad_s3)
               : CanonicalValue(nullptr)},
          {"preset_rad_s3",
           model_acceleration
               ? CanonicalValue(core::motion_jerk_preset_limit(
                     *model_acceleration, settings.preset_reference_time_s))
               : CanonicalValue(nullptr)},
          {"imported",
           imported != nullptr
               ? CanonicalValue(CanonicalValue::Object{
                     {"maximum_rad_s3",
                      CanonicalValue(imported->maximum_rad_s3)},
                     {"enabled", CanonicalValue(imported->enabled)},
                     {"machine_required",
                      CanonicalValue(imported->machine_required)},
                     {"source_id", CanonicalValue(imported->source_id)},
                     {"source_digest",
                      CanonicalValue(imported->source_digest.value())}})
               : CanonicalValue(nullptr)},
      });
    }
    operational_joint_limits.emplace_back(CanonicalValue::Object{
        {"deployment_max_acceleration_rad_s2",
         CanonicalNullableNumber(authored_acceleration)},
        {"deployment_max_jerk_rad_s3", CanonicalNullableNumber(authored_jerk)},
        {"deployment_max_velocity_rad_s",
         CanonicalNullableNumber(authored_velocity)},
        {"effective_max_acceleration_rad_s2",
         CanonicalNullableNumber(effective_acceleration)},
        {"effective_max_jerk_rad_s3", std::move(effective_jerk)},
        {"effective_max_velocity_rad_s", CanonicalValue(effective_velocity)},
        {"model_max_acceleration_rad_s2",
         CanonicalNullableNumber(model_acceleration)},
        {"model_max_velocity_rad_s",
         CanonicalValue(motion_limits.velocity_max_rad_s[index])},
        {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
    });
  }
  for (const auto &authored : direct_control_joint_limits) {
    if (authored.controllable_component_id !=
            motion.controllable_component_id ||
        authored.command_target_id != motion.command_target_id) {
      continue;
    }
    Require(std::any_of(motion.resources.begin(), motion.resources.end(),
                        [&](const auto &resource) {
                          return resource.source_joint_name.has_value() &&
                                 *resource.source_joint_name ==
                                     authored.source_joint_name;
                        }),
            "direct-control joint limit names an unknown source joint");
  }
  CanonicalValue motion_descriptor(CanonicalValue::Object{
      {"command_target_id", CanonicalValue(motion.command_target_id.value())},
      {"component_role_id", CanonicalValue(motion.component_role_id.value())},
      {"contracts", ManualControlContractValue(motion)},
      {"controllable_component_id",
       CanonicalValue(motion.controllable_component_id.value())},
      {"frames", CanonicalValue(std::move(frames))},
      {"joints", CanonicalValue(std::move(joints))},
      {"operational_joint_limits",
       CanonicalValue(std::move(operational_joint_limits))},
      {"operational_limit", std::move(operational_limit)},
      {"ordinary_jerk",
       ordinary_jerk_settings != nullptr
           ? CanonicalValue(CanonicalValue::Object{
                 {"enabled", CanonicalValue(ordinary_jerk_settings->enabled)},
                 {"preset_reference_time_s",
                  CanonicalValue(
                      ordinary_jerk_settings->preset_reference_time_s)},
                 {"joint_limits",
                  CanonicalValue(std::move(ordinary_jerk_joints))}})
           : CanonicalValue(nullptr)},
      {"schema_version",
       CanonicalValue("manyforge.manual_control_motion_descriptor.v5")},
      {"target_role_id", CanonicalValue(motion.target_role_id.value())},
  });

  if (!gripper_binding.has_value()) {
    Require(!gripper_limits.has_value(),
            "manual-control gripper limits have no physical gripper binding");
    Require(std::none_of(
                gripper_target_configurations.begin(),
                gripper_target_configurations.end(), [&](const auto &candidate) {
                  return candidate.controllable_component_id ==
                         motion.controllable_component_id;
                }),
            "manual-control gripper configuration has no physical gripper binding");
    return {std::move(motion_descriptor), CanonicalValue(nullptr)};
  }

  return {std::move(motion_descriptor),
          BuildManualControlGripperDescriptor(
              *gripper_binding, gripper_limits,
              gripper_target_configurations, model_map, documents)};
}



[[nodiscard]] std::map<core::ControllableComponentId, const Composition *>
IndexNativeComponentCompositions(const Composition &cell) {
  std::map<core::ControllableComponentId, const Composition *> result;
  const auto include = [&](const Composition &composition) {
    Require(composition.component_runtime_identity() != nullptr &&
                (composition.runtime == nullptr) !=
                    (composition.gripper_runtime == nullptr),
            "native component composition has no sole runtime owner");
    Require(result.emplace(composition.controllable_component_id,
                           &composition).second,
            "native cell contains duplicate component runtime owners");
  };
  include(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr,
            "native cell contains a missing component runtime owner");
    include(*participant);
  }
  return result;
}

// Exact existing scalar owner/configuration facts distinguish an intentionally
// absent physical launch from a lost Commanded/arm process. Never infer None
// from a null process, a target name, or unavailable feedback.
[[nodiscard]] bool IsLocalNoOutputGripperOwner(const Composition& owner) {
  if (owner.runtime || !owner.gripper_runtime || !owner.direct_control_gripper_binding)
    return false;
  const auto binding = owner.gripper_runtime->binding();
  const auto* backend = owner.gripper_runtime->backend();
  const auto& target = binding.value().command_target_id.value();
  return backend && binding.value().controllable_component_id ==
                        owner.direct_control_gripper_binding->value().controllable_component_id &&
         binding.value().command_target_id ==
             owner.direct_control_gripper_binding->value().command_target_id &&
         backend->gripper_actuation_kind(target) == core::GripperActuationKind::kPneumatic &&
         backend->gripper_output_mode(target) == core::GripperOutputMode::kNone;
}

struct VerifiedLocalNoneProcessOwner final {
  std::shared_ptr<RetainedNativeGripperRuntime> runtime;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
  core::RuntimeParticipantBinding binding;
  core::RuntimeRouteConfigurationReadbackDTO route;
  component_manager::ManagerComponentProcessOccurrenceDTO occurrence;
};

[[nodiscard]] VerifiedLocalNoneProcessOwner ReadVerifiedLocalNoneProcessOwner(
    const Composition& owner,
    const component_manager::ManagerComponentProcessInventoryRequestDTO& request) {
  Require(IsLocalNoOutputGripperOwner(owner) && !owner.controller_process &&
              owner.bundle && owner.cell_runtime && owner.ros2_control_runtime_handle &&
              owner.active_addressed_kernel() == &owner.cell_runtime->addressed_kernel(),
          "local None process occurrence has no exact installed logical owner");
  const auto registration = owner.gripper_runtime->registration();
  const auto& binding = registration.binding;
  const auto& value = binding.value();
  const auto* backend = owner.gripper_runtime->backend();
  const auto* actual_backend = dynamic_cast<const ros2_control_execution::Ros2GripperCommandBackend*>(backend);
  Require(actual_backend && value.control_path &&
              registration.engine == owner.gripper_runtime->participant_engine() &&
              registration.stop_execution_port == owner.gripper_runtime->participant_engine() &&
              registration.stop_readiness &&
              registration.stop_readiness->value().expected_controller_domain.physical_expectation ==
                  core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone &&
              owner.ros2_control_runtime_handle->OwnsGripper(backend, binding) &&
              value.retained_bundle_id == request.predecessor_projection_revision.retained_bundle_id &&
              value.graph_revision == request.predecessor_projection_revision.graph_revision &&
              value.runtime_unit_epochs == std::vector<core::RuntimeUnitEpochBindingDTO>{
                  {request.runtime_unit_id, request.runtime_epoch}},
          "local None process occurrence crosses its actual binding/engine/Stop owner");
  const auto logical = actual_backend->gripper_transport_readiness(value.command_target_id.value());
  Require(logical.configured && logical.logical_owner_ready &&
              logical.output_mode == core::GripperOutputMode::kNone,
          "local None process occurrence has no current configured logical backend");
  const auto& path = *value.control_path;
  std::vector<core::ActiveControlConfigurationBindingRouteDTO> expected_routes;
  std::vector<core::RouteReadinessPublicationRowDTO> expected_readiness;
  std::set<core::ControlBindingId> actual_ids;
  const auto registrations = owner.gripper_runtime->registrations();
  const auto& source_bindings = owner.bundle->value().cell_graph.control_bindings;
  for (const auto& row : registrations) {
    const auto& alias = row.binding.value();
    Require(alias.control_path && retained_bindings_share_backend_owner(binding, row.binding) &&
                row.engine == registration.engine && row.stop_execution_port == registration.stop_execution_port &&
                row.stop_readiness && row.stop_readiness->value().expected_controller_domain.physical_expectation ==
                    core::ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone &&
                (alias.capability_id == value.capability_id ||
                 (alias.capability_id.value() == "tool.gripper.feedback" && !alias.command_schema && !row.intrinsic_direct_control)),
            "local None occurrence has an alias outside its exact command owner");
    const auto& alias_path = *alias.control_path;
    Require(actual_ids.insert(alias_path.control_binding_id).second &&
                std::count_if(source_bindings.begin(), source_bindings.end(), [&](const auto& source) {
                  return source.control_binding_id == alias_path.control_binding_id &&
                      source.control_binding_revision == alias_path.control_binding_revision &&
                      source.controllable_component_id == alias.controllable_component_id &&
                      source.command_target_id == alias.command_target_id && source.capability_id == alias.capability_id &&
                      source.capability_definition_revision == alias.capability_definition_revision &&
                      source.control_domain_id == alias_path.control_domain_id &&
                      source.provider_runtime_unit_id == request.runtime_unit_id;
                }) == 1,
            "local None occurrence has an unknown or duplicated source binding");
    expected_routes.push_back({alias_path.control_binding_id, alias_path.control_binding_revision, alias_path.control_route_id});
    expected_readiness.push_back({alias.controllable_component_id,
        row.stop_readiness->value().component_lifecycle_generation,
        alias_path.control_binding_id, alias_path.control_binding_revision,
        row.stop_readiness->value().resolved_stop_base_plan.resolved_stop_base_plan_id});
  }
  Require(std::count_if(source_bindings.begin(), source_bindings.end(), [&](const auto& source) {
            return source.controllable_component_id == value.controllable_component_id;
          }) == static_cast<std::ptrdiff_t>(registrations.size()),
          "local None occurrence omits source-declared component bindings");
  std::sort(expected_routes.begin(), expected_routes.end(), [](const auto& left, const auto& right) {
    return left.control_binding_id < right.control_binding_id;
  });
  std::sort(expected_readiness.begin(), expected_readiness.end(), core::CanonicalRouteReadinessPublicationRowLess);
  auto route = owner.active_addressed_kernel()->current_runtime_route_configuration(path.control_domain_id);
  Require(route.ok() && route.value(),
          "local None process occurrence has no installed Kernel route");
  const auto& configuration = route.value()->active_configuration;
  Require(configuration.state == core::ActiveControlConfigurationStateDTO::kReconciled &&
              configuration.control_domain_id == path.control_domain_id &&
              configuration.control_domain_revision == path.control_domain_revision &&
              configuration.controller_topology_id == path.controller_topology_id &&
              configuration.topology_revision == path.topology_revision &&
              configuration.activation_set_id == path.activation_set_id &&
              configuration.active_control_configuration_id == path.active_control_configuration_id &&
              configuration.active_control_configuration_generation == path.active_control_configuration_generation &&
              configuration.source_runtime_unit_id == request.runtime_unit_id &&
              configuration.source_runtime_epoch == request.runtime_epoch &&
              configuration.binding_routes == expected_routes &&
              route.value()->readiness_rows == expected_readiness,
          "local None process occurrence differs from the exact Kernel route/Stop readback");
  component_manager::ManagerComponentProcessOccurrenceDTO occurrence{
      value.controllable_component_id, {}, 0, component_manager::ProcessPresenceState::kAbsent,
      core::ControllerLaunchCleanupState::kNotRequested, {},
      deployment::derive_component_runtime_identity(*owner.bundle, value.controllable_component_id),
      component_manager::ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal,
      component_manager::ManagerPneumaticNoneProcessOwnerDTO{
          value.command_target_id, path.control_binding_id, path.control_binding_revision}};
  const auto verified = component_manager::ValidateManagerPneumaticNoneProcessOwner(
      *owner.bundle, request, occurrence);
  Require(verified.ok(), verified.ok() ? std::string{} : verified.error().message);
  return {owner.gripper_runtime, owner.ros2_control_runtime_handle, binding,
          std::move(*route.value()), std::move(occurrence)};
}

[[nodiscard]] std::optional<std::string> RetainedControllerLaunchExit(
    const std::shared_ptr<ControllerLaunchProcess> &process) {
  if (!process)
    return std::nullopt;
  const auto reason = process->exited();
  if (!reason)
    return std::nullopt;
  const auto evidence = process->exit_evidence();
  return *reason + " pid=" + std::to_string(process->pid()) +
         (evidence ? " observed_utc=" + evidence->observed_utc : "");
}

void VerifyRetainedNativePredecessors(
    const Composition *active,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  const auto components = active == nullptr
                              ? std::map<core::ControllableComponentId,
                                         const Composition *>{}
                              : IndexNativeComponentCompositions(*active);
  static_cast<void>(request);
  if (active != nullptr)
    Require(active->bundle &&
                deployment::verify_retained_bundle_identity(*active->bundle),
            "actual_installed_state_unknown: installed component product is "
            "not verified");
  for (const auto &[id, owner] : components)
    Require(
        owner->bundle && owner->component_runtime_identity() &&
            deployment::derive_component_runtime_identity(*owner->bundle, id),
        "actual_installed_state_unknown: installed component lacks its real "
        "owner identity");
  return;
}

[[nodiscard]] const Composition *FindActualRetainedComponentOwner(
    const core::ControllableComponentId &component,
    const Composition &installed, const Composition *predecessor,
    const std::span<const Composition *const> other_predecessors) {
  const auto find = [&](const Composition *root) -> const Composition * {
    if (!root)
      return nullptr;
    const auto owners = IndexNativeComponentCompositions(*root);
    const auto found = owners.find(component);
    return found != owners.end() &&
                   found->second->component_runtime_identity() ==
                       installed.component_runtime_identity()
               ? found->second
               : nullptr;
  };
  if (const auto *owner = find(predecessor))
    return owner;
  for (const auto *root : other_predecessors)
    if (const auto *owner = find(root))
      return owner;
  return nullptr;
}

[[nodiscard]] core::NativeApplyPlanRequestDTO BuildNativeApplyPlanRequest(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const Composition &candidate, const Composition *predecessor,
    const std::span<const core::RuntimeParticipantBinding> representatives,
    [[maybe_unused]] const std::span<const Composition *const>
        other_predecessors = {}) {
  Require(candidate.bundle != nullptr &&
              candidate.component_runtime_identity() != nullptr &&
              candidate.active_addressed_kernel() != nullptr,
          "native Apply has no installed candidate or stable Kernel");
  const auto candidate_components = IndexNativeComponentCompositions(candidate);
  const auto predecessor_components =
      predecessor == nullptr
          ? std::map<core::ControllableComponentId, const Composition *>{}
          : IndexNativeComponentCompositions(*predecessor);
  core::NativeApplyPlanRequestDTO plan{
      {request.apply_id, request.apply_generation},
      candidate.bundle->retained_bundle_id(),
      candidate.bundle->value().cell_graph.graph_revision,
      {}, {}, {}, {}, {}, {}};
  // The immutable authored delta is echoed in the receipt. Runtime lifetime
  // follows the owner actually installed, including a restarted authored
  // Retain.
  for (const auto &[component_id, after] : candidate_components) {
    if (FindActualRetainedComponentOwner(component_id, *after, predecessor,
                                         other_predecessors))
      plan.retained_components.push_back(component_id);
  }
  for (const auto &[component_id, before] : predecessor_components) {
    static_cast<void>(before);
    if (!candidate_components.contains(component_id))
      plan.removed_components.push_back(component_id);
  }
  for (const auto *root : other_predecessors) {
    if (!root)
      continue;
    for (const auto &[component_id, before] :
         IndexNativeComponentCompositions(*root)) {
      static_cast<void>(before);
      if (!candidate_components.contains(component_id))
        plan.removed_components.push_back(component_id);
    }
  }
  std::sort(plan.removed_components.begin(), plan.removed_components.end());
  plan.removed_components.erase(std::unique(plan.removed_components.begin(),
                                            plan.removed_components.end()),
                                plan.removed_components.end());
  for (const auto &[component_id, _] : candidate_components)
    plan.components.push_back(component_id);
  const auto &desired_routes =
      candidate.bundle->value().desired_control_configurations;
  for (const auto &desired : desired_routes) {
    const auto physical = std::find_if(
        representatives.begin(), representatives.end(), [&](const auto &binding) {
          return binding.value().control_path.has_value() &&
                 binding.value().control_path->control_domain_id ==
                     desired.control_domain_id;
        });
    Require(physical != representatives.end(),
            "native candidate domain has no compiled physical binding");
    plan.compiled_routes.push_back(
        {desired.control_domain_id,
         physical->value()
             .control_path->active_control_configuration_generation});
    if (!std::binary_search(plan.retained_components.begin(),
                            plan.retained_components.end(),
                            physical->value().controllable_component_id))
      continue;
    const auto current = candidate.active_addressed_kernel()
                             ->current_runtime_route_configuration(
                                 desired.control_domain_id);
    Require(current.ok() && current.value().has_value() &&
                current.value()->operation_admission_open,
            "native retained domain has no current Kernel route");
    plan.retained_routes.push_back(current.value()->active_configuration);
  }
  if (predecessor != nullptr) {
    for (const auto &desired :
         predecessor->bundle->value().desired_control_configurations) {
      if (std::none_of(desired_routes.begin(), desired_routes.end(),
                       [&](const auto &row) {
                         return row.control_domain_id ==
                                desired.control_domain_id;
                       }))
        plan.removed_domains.push_back(desired.control_domain_id);
    }
  }
  return plan;
}

[[nodiscard]] std::vector<component_manager::ManagerRuntimeComponentReceiptDTO>
ReadNativeInstalledComponentReceipts(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    const Composition *candidate, const Composition *predecessor,
    [[maybe_unused]] const std::span<const Composition *const>
        other_predecessors = {}) {
  const auto current = candidate == nullptr
                           ? std::map<core::ControllableComponentId,
                                      const Composition *>{}
                           : IndexNativeComponentCompositions(*candidate);
  const auto previous = predecessor == nullptr
                            ? std::map<core::ControllableComponentId,
                                       const Composition *>{}
                            : IndexNativeComponentCompositions(*predecessor);
  [[maybe_unused]] const bool product_changed =
      request.predecessor_projection_revision.retained_bundle_id !=
          request.candidate_projection_revision.retained_bundle_id ||
      request.predecessor_projection_revision.graph_revision !=
          request.candidate_projection_revision.graph_revision;
  std::vector<component_manager::ManagerRuntimeComponentReceiptDTO> receipts;
  for (const auto &row : request.component_delta.components) {
    if (row.disposition == deployment::ApplyComponentDisposition::kRemove)
      continue;
    const auto installed = current.find(row.controllable_component_id);
    Require(candidate != nullptr && installed != current.end(),
            "native installed component receipt has no live owner");
    const auto &component_id = installed->second->controllable_component_id;
    Require(component_id == row.controllable_component_id,
            "native installed composition has another component identity");
    const auto runtime_identity = deployment::derive_component_runtime_identity(
        *candidate->bundle, component_id);
    Require(runtime_identity.has_value() &&
                runtime_identity == row.candidate_runtime_identity,
            "native installed component differs from the candidate product");
    bool has_component_registration = false;
    for (const auto &registration :
         installed->second->component_registrations()) {
      const auto &binding = registration.binding.value();
      if (binding.controllable_component_id != component_id) {
        continue;
      }
      has_component_registration = true;
      Require(binding.retained_bundle_id ==
                      request.candidate_projection_revision.retained_bundle_id &&
                  binding.graph_revision ==
                      request.candidate_projection_revision.graph_revision,
              "native component registration is sealed to another product");
      Require(std::any_of(
                  binding.runtime_unit_epochs.begin(),
                  binding.runtime_unit_epochs.end(), [&](const auto &epoch) {
                    return epoch.runtime_unit_id == request.runtime_unit_id &&
                           epoch.runtime_epoch == request.runtime_epoch;
                  }),
              "native component registration has no current execution-host epoch");
    }
    Require(has_component_registration,
            "native installed component has no runtime participant registrations");
    [[maybe_unused]] std::vector<core::RuntimeUnitEpochBindingDTO> epochs{
        {request.runtime_unit_id, request.runtime_epoch}};
    [[maybe_unused]] const auto predecessor_owner =
        previous.find(row.controllable_component_id);
    const auto *actual_predecessor = FindActualRetainedComponentOwner(
        component_id, *installed->second, predecessor, other_predecessors);
    const auto &view = *installed->second;
    Require((view.runtime != nullptr || view.gripper_runtime != nullptr) &&
                view.ros2_control_runtime_handle != nullptr,
            "ROS installed receipt has no owned runtime handle");
    // Successful route installation can advance the live configuration
    // generation beyond the Composition's construction-time manual bindings.
    // Receipt ownership uses the current runtime bindings and registrations.
    const std::optional<core::RuntimeParticipantBinding> current_motion =
        view.runtime ? std::optional(view.runtime->motion_binding) : std::nullopt;
    const std::optional<core::RuntimeParticipantBinding> current_tool =
        view.runtime ? view.runtime->gripper_binding
                     : std::optional(view.gripper_runtime->binding());
    auto receipt = detail::ReadRos2ComponentReceipt(
        request, component_id, *runtime_identity,
        *view.ros2_control_runtime_handle,
        view.runtime ? view.runtime->execution.get() : nullptr,
        view.runtime ? view.runtime->gripper.get() : view.gripper_runtime->backend(),
        current_motion, current_tool,
        view.component_registrations(),
        view.runtime ? view.runtime->participant_engine_for(true) : nullptr,
        view.runtime ? view.runtime->participant_engine_for(false) : view.gripper_runtime->participant_engine(),
        actual_predecessor
            ? actual_predecessor->ros2_control_runtime_handle.get()
            : nullptr,
        RetainedControllerLaunchExit(view.controller_process));
    Require(receipt.valid(), "native component receipt is invalid");
    receipts.push_back(std::move(receipt));
  }
  Require(component_manager::ComponentReceiptsMatchRequest(request, receipts),
          "native installed component receipts differ from the Apply decision");
  return receipts;
}

[[maybe_unused]] void
AttachProductPickPlacePlanningOwner(Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.scene_authority_proxy != nullptr &&
              composition.runtime != nullptr &&
              composition.active_addressed_kernel() != nullptr,
          "production pick/place join has no scene or addressed owner");
  if (composition.pick_place_planning_owner != nullptr) {
    return;
  }
  composition.pick_place_planning_owner =
      std::make_unique<core::AddressedPickPlacePlanningOwner>(
          *composition.active_addressed_kernel(),
          *composition.scene_authority_proxy,
          *composition.scene_authority_proxy,
          *composition.planner_scene_projection,
          composition.runtime->planning_scene_synchronizer.get());
}

void InstallDirectSceneFrameResolver(ManagedProjectionRegistry &projections,
                                     Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.bundle != nullptr &&
              composition.scene_frame_resolver != nullptr,
          "command-capable native projection has no direct frame resolver");
  projections.InstallActiveFrameResolverSet(*composition.bundle,
                                            composition.projection_revision,
                                            composition.scene_frame_resolver);
}



[[nodiscard]] CanonicalValue
LaunchIncidentOutput(ControllerLaunchOutputSnapshot output) {
  const auto unsigned_value = [](const std::uint64_t value) {
    return value <= 9'007'199'254'740'991ULL
               ? CanonicalValue(static_cast<double>(value))
               : CanonicalValue(std::to_string(value));
  };
  std::uint64_t invalid_text_bytes{};
  const bool escaped_tail = !content_identity::is_utf8_nfc(output.tail);
  if (escaped_tail) {
    // Preserve otherwise unrepresentable bytes as reversible ASCII escapes.
    // Valid NFC UTF8 takes the unchanged path above.
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    for (const auto byte : output.tail) {
      if (static_cast<unsigned char>(byte) >= 0x80U) {
        const auto value = static_cast<unsigned char>(byte);
        escaped += "\\x";
        escaped += hex[value >> 4U];
        escaped += hex[value & 0x0fU];
        ++invalid_text_bytes;
      } else if (byte == '\\') {
        escaped += "\\\\";
      } else {
        escaped += byte;
      }
    }
    output.tail = std::move(escaped);
  }
  // Keep the most recent text. JSON escaping counts against the approved
  // encoded-byte cap; every trim boundary preserves a UTF8 codepoint or a
  // complete generated ASCII escape token.
  auto encoded = content_identity::canonical_json(CanonicalValue(output.tail));
  if (encoded.size() > 128U * 1024U) {
    const auto encoded_byte = [](const unsigned char byte) -> std::size_t {
      if (byte == '\b' || byte == '\t' || byte == '\n' || byte == '\f' ||
          byte == '\r' || byte == '"' || byte == '\\')
        return 2U;
      return byte < 0x20U ? 6U : 1U;
    };
    auto remaining = encoded.size();
    std::size_t removed{};
    while (remaining > 128U * 1024U && removed < output.tail.size()) {
      const auto token_bytes =
          escaped_tail && output.tail[removed] == '\\'
              ? (output.tail[removed + 1U] == 'x' ? 4U : 2U)
              : 1U;
      const auto token_end = removed + token_bytes;
      while (removed < token_end)
        remaining -=
            encoded_byte(static_cast<unsigned char>(output.tail[removed++]));
    }
    while (removed < output.tail.size() &&
           (static_cast<unsigned char>(output.tail[removed]) & 0xC0U) == 0x80U)
      ++removed;
    output.tail.erase(0U, removed);
    output.truncated = true;
  }
  const auto line_count =
      output.tail.empty() ? 0U
                          : static_cast<std::size_t>(std::count(
                                output.tail.begin(), output.tail.end(), '\n')) +
                                (output.tail.back() == '\n' ? 0U : 1U);
  return CanonicalValue(CanonicalValue::Object{
           {"tail", CanonicalValue(std::move(output.tail))},
           {"line_count", CanonicalValue(static_cast<double>(line_count))},
           {"truncated", CanonicalValue(output.truncated)},
           {"dropped_forward_lines",
            unsigned_value(output.dropped_forward_lines)},
           {"dropped_forward_bytes",
            unsigned_value(output.dropped_forward_bytes)},
           {"invalid_text_bytes", unsigned_value(invalid_text_bytes)}});
}

[[nodiscard]] std::shared_ptr<const CanonicalValue>
ControllerLaunchIncidentSnapshot(ControllerLaunchProcess &process,
                                 const ControllerLaunchExitEvidence &exit,
                                 const deployment::RetainedBundle &bundle,
                                 const std::string &runtime_id,
                                 const std::string &runtime_epoch) {
  const auto optional_text = [](const std::optional<std::string> &value) {
    return value ? CanonicalValue(*value) : CanonicalValue(nullptr);
  };
  const auto optional_bool = [](const std::optional<bool> value) {
    return value ? CanonicalValue(*value) : CanonicalValue(nullptr);
  };
  const auto optional_number = [](const std::optional<int> value) {
    return value ? CanonicalValue(static_cast<double>(*value))
                 : CanonicalValue(nullptr);
  };
  const auto unsigned_value = [](const std::uint64_t value) {
    return value <= 9'007'199'254'740'991ULL
               ? CanonicalValue(static_cast<double>(value))
               : CanonicalValue(std::to_string(value));
  };
  const auto &command = process.command();
  CanonicalValue::Array argv;
  for (const auto &argument : command.argv)
    argv.emplace_back(argument);
  const ControllerLaunchReadinessEvidence unknown;
  const auto &readiness = exit.readiness ? *exit.readiness : unknown;
  const auto configuration =
      deployment::ros2_control_execution_runtime_contract::
          component_configuration_projection(
              bundle, core::RuntimeUnitId(runtime_id),
              core::ControllableComponentId(exit.controllable_component_id));
  auto snapshot = std::make_shared<const CanonicalValue>(CanonicalValue::Object{
      {"exit", CanonicalValue(CanonicalValue::Object{
                   {"status", optional_number(exit.exit_status)},
                   {"signal", optional_number(exit.signal)},
                   {"core_dumped", CanonicalValue(exit.core_dumped)},
                   {"observed_utc", CanonicalValue(exit.observed_utc)}})},
      {"context",
       CanonicalValue(CanonicalValue::Object{
           {"component_id", CanonicalValue(exit.controllable_component_id)},
           {"request_context", CanonicalValue(exit.request_context)},
           {"runtime_unit_id", CanonicalValue(runtime_id)},
           {"runtime_epoch", CanonicalValue(runtime_epoch)},
           {"bundle_id", CanonicalValue(bundle.retained_bundle_id().value())},
           {"graph_revision",
            CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
           {"generator_identity", CanonicalValue(nullptr)},
           {"launch_identity", CanonicalValue(exit.launch_identity)}})},
      {"readiness",
       CanonicalValue(CanonicalValue::Object{
           {"known", optional_bool(readiness.known)},
           {"controller_manager_present",
            optional_bool(readiness.controller_manager_present)},
           {"controller_observed",
            optional_bool(readiness.controller_observed)},
           {"controller_loaded", optional_bool(readiness.controller_loaded)},
           {"controller_bound", optional_bool(readiness.controller_bound)},
           {"arm_controller_active",
            optional_bool(readiness.arm_controller_active)},
           {"hardware_ready", optional_bool(readiness.hardware_ready)},
           {"joint_sample_available",
            optional_bool(readiness.joint_sample_available)},
           {"binding_epoch", readiness.binding_epoch
                                 ? unsigned_value(*readiness.binding_epoch)
                                 : CanonicalValue(nullptr)},
           {"sample_age_ns", readiness.sample_age_ns
                                 ? content_identity::nonnegative_int64_value(
                                       *readiness.sample_age_ns)
                                 : CanonicalValue(nullptr)},
           {"captured_utc", readiness.captured_utc.empty()
                                ? CanonicalValue(nullptr)
                                : CanonicalValue(readiness.captured_utc)}})},
      {"settings",
       CanonicalValue(CanonicalValue::Object{
           {"argv", CanonicalValue(std::move(argv))},
           {"controller_profile", CanonicalValue(command.controller_profile)},
           {"runtime_namespace", CanonicalValue(command.runtime_namespace)},
           {"arm_controller_name", CanonicalValue(command.arm_controller_name)},
           {"gripper_controller_name",
            optional_text(command.gripper_controller_name)},
           {"frame_prefix", CanonicalValue(command.frame_prefix)},
           {"controllers_yaml", CanonicalValue(command.controllers_yaml)},
           {"retained_configuration_json",
            CanonicalValue(content_identity::canonical_json(configuration))}})},
      {"generated",
       CanonicalValue(CanonicalValue::Object{
           {"robot_description_sha256",
            CanonicalValue(exit.robot_description_sha256)},
           {"controllers_sha256", CanonicalValue(exit.controllers_sha256)}})},
      {"output", LaunchIncidentOutput(process.output_snapshot())},
      {"core_dump_link", CanonicalValue(nullptr)}});
  Require(core::ComponentIncidentDiagnosticSnapshotShapeIsValid(*snapshot),
          "controller launch incident snapshot exceeded its closed bound");
  return snapshot;
}
std::function<ControllerLaunchReadinessEvidence()>
CachedControllerLaunchReadinessSource(
    const std::shared_ptr<RetainedRos2ControlRuntimeHandle> &handle) {
  return [weak = std::weak_ptr<RetainedRos2ControlRuntimeHandle>(handle)] {
    ControllerLaunchReadinessEvidence facts;
    const auto handle = weak.lock();
    if (!handle)
      return facts;
    const auto activity = handle->arm_activity();
    facts.controller_manager_present = activity.controller_manager_present;
    facts.controller_observed = activity.observed;
    if (activity.observed) {
      facts.controller_loaded = activity.controller_loaded;
      facts.arm_controller_active = activity.controller_active;
      facts.hardware_ready = activity.hardware_active;
    }
    if (const auto port = handle->current_port()) {
      facts.known = true;
      const auto readiness = port->ReadReadiness();
      facts.controller_bound = readiness.controller_bound;
      facts.binding_epoch = readiness.binding_epoch.value();
      const auto sample = port->ReadSample();
      facts.joint_sample_available = sample.has_value();
      if (sample)
        facts.sample_age_ns = std::max<core::TimePoint>(
            0, WallClock{}.now() - sample->local_receipt_stamp_ns);
    }
    return facts;
  };
}


struct RetainedNativeComponentSnapshot final {
  std::shared_ptr<RetainedNativeRuntime> runtime;
  std::shared_ptr<RetainedNativeGripperRuntime> gripper_runtime;
  std::shared_ptr<const RetainedPlanningModelStage> stage;
  // The cell planning-model product `stage` was staged from.
  core::RetainedProductDTO planning_model_product;
  std::optional<core::JointLimitsDTO> joint_limits;
  CanonicalValue motion_descriptor;
  CanonicalValue gripper_descriptor;
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control_runtime_handle;
  std::shared_ptr<ControllerLaunchProcess> controller_process;
};

struct CompositionBuildInput final {
  std::filesystem::path package_root;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  core::CellId cell_id;
  core::RequestOwnerEpoch owner_epoch;
  std::optional<core::RuntimeUnitId> runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedModelMap model_map;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  std::vector<core::RuntimeParticipantBinding> motion_program_aliases;
  std::vector<core::RuntimeParticipantBinding> gripper_program_aliases;
  ManagedKernelAdmissionGate *operation_admission_gate{};
  std::shared_ptr<const deployment::ProgramBindingContract> status_program;
  std::shared_ptr<const deployment::ProgramBinding> status_program_binding;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  bool attach_addressed_owner{true};
  std::shared_ptr<WallClock> shared_clock;
  std::shared_ptr<SimpleScheduler> shared_scheduler;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::optional<core::ControllableComponentId> selected_component_id{};
  std::shared_ptr<RetainedObservationWake> observation_wake{};
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      component_planner_scene_projections{};
  // Immutable Programs generated by Program Apply may live outside the
  // retained Deployment package. This is a read-only point-lookup source.
  std::filesystem::path program_artifact_root{};
  // Present only for a Manager-selected managed composition. Startup and
  // direct product builds have no Apply delta.
  std::optional<deployment::ApplyComponentDeltaDTO> managed_component_delta{};
  std::map<core::ControllableComponentId, RetainedNativeComponentSnapshot>
      retained_component_snapshots{};
  // The selected package root may be either retained descriptor in a managed
  // child. Candidate workers receive whichever exact root was selected as FD 6.
  std::optional<int> retained_package_root_fd{};
  // Manager-opened scene-asset root (managed FD 9), shared by the collision
  // model and the candidate worker. Absent: relative scene meshes fail closed.
  std::optional<int> scene_asset_root_fd{};
  std::string controller_request_context{};
  // Captured by the stable cell owner only after the strict observer check
  // and cell barrier complete. Component construction performs no world work.
  std::optional<IsaacControllerLaunchInputs> managed_isaac_launch_inputs{};
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required_projection_pins{};
};

[[nodiscard]] core::CellAdmissionSettlementValidator
BuildOwnerSettlementValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  if (manager_apply_journal.empty() && logical_scene_database.empty() &&
      program_cycle_journal.empty() && lifecycle_journal.empty()) {
    return {};
  }
  return
      [manager_apply_journal = std::move(manager_apply_journal),
       logical_scene_database = std::move(logical_scene_database),
       program_cycle_journal = std::move(program_cycle_journal),
       lifecycle_journal = std::move(lifecycle_journal),
       cell_id = std::move(cell_id)](
          const core::CellAdmissionClaimDTO &claim,
          const core::CellAdmissionClaimReleaseDTO &release) {
        if (claim.intent.claim_kind ==
                core::CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
            claim.intent.owner_role ==
                core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_journal, cell_id, claim, release);
        }
        switch (claim.intent.claim_kind) {
        case core::CellAdmissionClaimKindDTO::kProgramCycle:
          return detail::ValidateProgramCycleSettlementReadOnly(
              program_cycle_journal, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoringMutation:
          // Composer local edits own no durable or runtime effect. Kernel's
          // exact claim/release identity validation is therefore the complete
          // settlement proof; requiring a second Composer journal would only
          // duplicate the in-memory workspace owner.
          return core::Result<void>::Ok();
        case core::CellAdmissionClaimKindDTO::kManagerApply:
          return component_manager::
              ValidateManagerApplyCellAdmissionSettlementReadOnly(
                  manager_apply_journal, cell_id, claim, release);
        case core::CellAdmissionClaimKindDTO::kComponentLifecycle:
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_journal, cell_id, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoredScene:
          return scene::ValidateAuthoredSceneCellAdmissionSettlementReadOnly(
              logical_scene_database, cell_id, claim, release);
        default:
          return core::Result<void>::Err(core::ErrorDTO{
              "invariant_violation",
              "cell-admission owner settlement reader is unavailable",
              {{"reason", "settlement_owner_reader_unavailable"}}});
        }
      };
}

[[nodiscard]] core::CellAdmissionReconcileValidator
BuildOwnerReconciliationValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  return [manager_apply_journal = std::move(manager_apply_journal),
          program_cycle_journal = std::move(program_cycle_journal),
          lifecycle_journal = std::move(lifecycle_journal),
          cell_id = std::move(cell_id)](
             const std::vector<core::CellAdmissionClaimDTO> &held_claims,
             const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
                 &current_owner_epochs,
             const core::CellAdmissionReconcileDTO &reconcile) {
    std::optional<core::CellAdmissionOwnerRoleDTO> changed_role;
    const auto note_changed_role = [&](const auto role) {
      if (changed_role.has_value() && *changed_role != role) {
        return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
            core::ErrorDTO{
                "invariant_violation",
                "owner reconciliation may replace exactly one role",
                {{"reason",
                  "multiple_owner_role_reconciliation_unsupported"}}});
      }
      changed_role = role;
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Ok({});
    };
    for (const auto &current : current_owner_epochs) {
      const auto requested = std::find_if(
          reconcile.current_owner_epochs.begin(),
          reconcile.current_owner_epochs.end(), [&](const auto &owner) {
            return owner.owner_role == current.owner_role;
          });
      if (requested == reconcile.current_owner_epochs.end() ||
          requested->owner_epoch != current.owner_epoch) {
        const auto noted = note_changed_role(current.owner_role);
        if (!noted.ok())
          return noted;
      }
    }
    for (const auto &requested : reconcile.current_owner_epochs) {
      const auto current =
          std::find_if(current_owner_epochs.begin(), current_owner_epochs.end(),
                       [&](const auto &owner) {
                         return owner.owner_role == requested.owner_role;
                       });
      if (current == current_owner_epochs.end()) {
        const auto noted = note_changed_role(requested.owner_role);
        if (!noted.ok())
          return noted;
      }
    }
    if (!changed_role.has_value()) {
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
          core::ErrorDTO{
              "invariant_violation",
              "owner reconciliation has no supported replacement",
              {{"reason", "owner_role_reconciliation_unsupported"}}});
    }
    switch (*changed_role) {
    case core::CellAdmissionOwnerRoleDTO::kProgramRunner:
      return detail::ValidateProgramRunnerOwnerReconciliationReadOnly(
          program_cycle_journal, held_claims, current_owner_epochs, reconcile);
    case core::CellAdmissionOwnerRoleDTO::kAuthoringOwner:
      return component_manager::
          ValidateManagerAuthoringOwnerReconciliationReadOnly(
              manager_apply_journal, cell_id, held_claims,
              current_owner_epochs, reconcile);
    case core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner:
      return core::ValidateCCLCCellAdmissionReconciliationReadOnly(
          lifecycle_journal, cell_id, held_claims, current_owner_epochs,
          reconcile);
    default:
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
          core::ErrorDTO{
              "invariant_violation",
              "owner role has no durable reconciliation reader",
              {{"reason", "owner_role_reconciliation_unsupported"}}});
    }
  };
}

struct NativePhysicalBindingSet final {
  // Each native component may provide either capability independently. The
  // managed cell aggregates these per-component records; this value must not
  // combine capabilities owned by two different components.
  std::shared_ptr<const deployment::ProgramBinding> motion_program;
  std::shared_ptr<const deployment::ProgramBinding> gripper_program;
  std::optional<core::RuntimeParticipantBinding> motion;
  std::optional<core::RuntimeParticipantBinding> gripper;
  std::vector<core::RuntimeParticipantBinding> representatives;
  std::optional<core::TargetRoleId> status_motion_role;
  std::optional<core::TargetRoleId> status_gripper_role;
};

template <typename PhysicalBindingInput>
[[nodiscard]] NativePhysicalBindingSet ResolveNativePhysicalBindings(
    const PhysicalBindingInput &input,
    const deployment::DirectControlProgramBindingInventory &inventory) {
  const auto select_optional_binding =
      [&](const auto &bindings, const std::string_view capability_id) {
        std::shared_ptr<const deployment::ProgramBinding> selected;
        for (const auto &binding : bindings) {
          const auto target = deployment::resolve_runtime_program_target(
              *binding,
              core::TargetRoleId(
                  std::string(deployment::kDirectControlTargetRoleId)),
              core::CapabilityId(std::string(capability_id)), *input.bundle,
              input.model_map);
          if (input.selected_component_id.has_value() &&
              target.component().controllable_component_id !=
                  *input.selected_component_id)
            continue;
          Require(selected == nullptr,
                  "native composition selection has multiple matching targets");
          selected = binding;
        }
        return selected;
      };
  auto motion_program = select_optional_binding(
      inventory.motion, "execution.trajectory.execute");
  std::optional<deployment::RuntimeProgramTargetBinding> motion_target;
  if (motion_program != nullptr) {
    motion_target = deployment::resolve_runtime_program_target(
        *motion_program,
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("execution.trajectory.execute"), *input.bundle,
        input.model_map);
  }
  auto gripper_program = select_optional_binding(inventory.gripper,
                                                 "tool.gripper.command");
  std::optional<deployment::RuntimeProgramTargetBinding> gripper_target;
  if (gripper_program != nullptr) {
    gripper_target = deployment::resolve_runtime_program_target(
        *gripper_program,
        core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("tool.gripper.command"), *input.bundle,
        input.model_map);
  }

  // A component-runtime value has exactly one lifecycle owner. Motion and
  // gripper targets may be independent components in the retained cell, but
  // they must be resolved through separate component-runtime invocations.
  if (motion_target.has_value() && gripper_target.has_value()) {
    Require(motion_target->component().controllable_component_id ==
                gripper_target->component().controllable_component_id,
            "native component binding combines capabilities from different owners");
  }
  Require(!input.selected_component_id.has_value() ||
              motion_target.has_value() || gripper_target.has_value(),
          "selected native component has no supported direct-control capability");

  std::optional<core::TargetRoleId> status_motion_role;
  std::optional<core::TargetRoleId> status_gripper_role;
  if (input.status_program_binding != nullptr) {
    const auto selected_target_for =
        [&](const std::optional<core::TargetRoleId> &role,
                                         const std::string_view capability_id)
        -> std::optional<deployment::RuntimeProgramTargetBinding> {
      if (!role.has_value()) return std::nullopt;
      const auto found = std::find_if(
          input.status_program_binding->value().target_role_bindings.begin(),
          input.status_program_binding->value().target_role_bindings.end(),
          [&](const auto &target) {
            return target.target_role_id == *role &&
                   std::any_of(target.capability_bindings.begin(),
                               target.capability_bindings.end(),
                               [&](const auto &capability) {
                                 return capability.capability_id.value() ==
                                        capability_id;
                               });
          });
      if (found ==
          input.status_program_binding->value().target_role_bindings.end()) {
        return std::nullopt;
      }
      return deployment::resolve_runtime_program_target(
          *input.status_program_binding, *role,
          core::CapabilityId(std::string(capability_id)), *input.bundle,
          input.model_map);
    };
    const auto status_role_for = [&](const auto &physical_target,
                                     const auto &selected_target,
                                     const std::optional<core::TargetRoleId>
                                         &selected_role,
                                     const std::string_view capability_name)
        -> std::optional<core::TargetRoleId> {
      if (!physical_target.has_value())
        return std::nullopt;
      const auto &physical_owner =
          physical_target->component().controllable_component_id;
      const auto &physical_command_target =
          physical_target->target_role().command_target_id;
      if (!selected_target.has_value())
        return core::TargetRoleId(physical_command_target.value());
      const bool selection_addresses_this_component =
          selected_target->component().controllable_component_id ==
              (input.selected_component_id.has_value()
                   ? *input.selected_component_id
                   : physical_owner);
      if (!selection_addresses_this_component) {
        // The selected Program may legitimately use this capability on a
        // different independently owned component. Keep this component's
        // direct-control status address tied to its own verified target.
        return core::TargetRoleId(physical_command_target.value());
      }
      Require(selected_target->component().controllable_component_id ==
                  physical_owner &&
                  selected_role.has_value() &&
                  selected_target->target_role().command_target_id ==
                      physical_command_target,
              "selected Program " + std::string(capability_name) +
                  " target does not match its native component capability");
      return *selected_role;
    };
    const auto selected_motion_target = selected_target_for(
        input.status_motion_target_role_id, "execution.trajectory.execute");
    status_motion_role = status_role_for(
        motion_target, selected_motion_target,
        input.status_motion_target_role_id, "motion");
    const auto selected_gripper_target = selected_target_for(
        input.status_gripper_target_role_id, "tool.gripper.command");
    status_gripper_role = status_role_for(
        gripper_target, selected_gripper_target,
        input.status_gripper_target_role_id, "gripper");
  } else {
    if (motion_target.has_value()) {
      status_motion_role = core::TargetRoleId(
          motion_target->target_role().command_target_id.value());
    }
    if (gripper_target.has_value()) {
      status_gripper_role = core::TargetRoleId(
          gripper_target->target_role().command_target_id.value());
    }
  }

  std::optional<core::RuntimeParticipantBinding> motion;
  if (motion_target.has_value()) {
    std::vector<deployment::RuntimeProgramTargetBinding> motion_support;
    const auto &target_role = motion_program->value().target_role_bindings.front();
    for (const std::string_view capability_id :
         {std::string_view("planning.motion.cartesian_path"),
          std::string_view("planning.motion.joint_goal"),
          std::string_view("planning.motion.pose_goal")}) {
      if (std::any_of(target_role.capability_bindings.begin(),
                      target_role.capability_bindings.end(),
                      [&](const auto &capability) {
                        return capability.capability_id.value() == capability_id;
                      })) {
        motion_support.push_back(deployment::resolve_runtime_program_target(
            *motion_program,
            core::TargetRoleId(
                std::string(deployment::kDirectControlTargetRoleId)),
            core::CapabilityId(std::string(capability_id)), *input.bundle,
            input.model_map));
      }
    }
    Require(std::any_of(motion_support.begin(), motion_support.end(),
                        [](const auto &support) {
                          return support.capability().capability_id.value() ==
                                 "planning.motion.joint_goal";
                        }),
            "direct-control motion binding has no joint planner");
    auto verified_motion = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            *motion_target,
            ActiveFacts(
                *motion_target,
                std::span<const deployment::RuntimeProgramTargetBinding>(
                    motion_support),
                input.runtime_epoch,
                core::ActiveControlConfigurationId("active.i6.native.arm")),
            std::span<const deployment::RuntimeProgramTargetBinding>(
                motion_support)));
    if (!verified_motion.ok()) {
      Fail("direct-control motion participant binding failed verification: " +
           verified_motion.error().message);
    }
    motion = std::move(verified_motion.value());
  }

  std::optional<core::RuntimeParticipantBinding> gripper;
  if (gripper_target.has_value()) {
    auto verified_gripper = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            *gripper_target,
            ActiveFacts(
                *gripper_target,
                std::span<const deployment::RuntimeProgramTargetBinding>{},
                input.runtime_epoch,
                core::ActiveControlConfigurationId(
                    "active.i6.native.gripper"))));
    if (!verified_gripper.ok()) {
      Fail("direct-control gripper participant binding failed verification: " +
           verified_gripper.error().message);
    }
    gripper = std::move(verified_gripper.value());
  }
  std::vector<core::RuntimeParticipantBinding> representatives;
  if (motion.has_value()) {
    auto motion_representatives = BuildControlBindingRepresentatives(
        *motion_program, *motion, *input.bundle, input.model_map,
        input.runtime_epoch,
        core::ActiveControlConfigurationId("active.i6.native.arm"));
    representatives.insert(
        representatives.end(),
        std::make_move_iterator(motion_representatives.begin()),
        std::make_move_iterator(motion_representatives.end()));
  }
  if (gripper.has_value()) {
    auto gripper_representatives = BuildControlBindingRepresentatives(
        *gripper_program, *gripper, *input.bundle, input.model_map,
        input.runtime_epoch,
        core::ActiveControlConfigurationId("active.i6.native.gripper"));
    representatives.insert(
        representatives.end(),
        std::make_move_iterator(gripper_representatives.begin()),
        std::make_move_iterator(gripper_representatives.end()));
  }
  std::sort(representatives.begin(), representatives.end(),
            [](const auto &left, const auto &right) {
      return left.value().control_path->control_binding_id <
             right.value().control_path->control_binding_id;
    });
  return {std::move(motion_program), std::move(gripper_program),
          std::move(motion), std::move(gripper),
          std::move(representatives), std::move(status_motion_role),
          std::move(status_gripper_role)};
}

[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input);


[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
ResealRetainedNativeCandidateRegistrations(
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &predecessor_registrations,
    const deployment::RetainedBundle &candidate_bundle,
    const std::filesystem::path &package_root,
    const std::vector<core::RuntimeParticipantBinding> &representatives) {
  const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root);
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  result.reserve(representatives.size());
  for (const auto &binding : representatives) {
    const auto &value = binding.value();
    Require(value.control_path.has_value(),
            "retained native candidate binding has no control route");
    const auto old = std::find_if(
        predecessor_registrations.begin(),
        predecessor_registrations.end(),
        [&](const auto &registration) {
          const auto &before = registration.binding.value();
          return before.controllable_component_id ==
                     value.controllable_component_id &&
                 before.command_target_id == value.command_target_id &&
                 before.capability_id == value.capability_id &&
                 before.control_path.has_value() &&
                 before.control_path->control_binding_id ==
                     value.control_path->control_binding_id;
        });
    Require(old != predecessor_registrations.end(),
            "retained native candidate has no predecessor registration");
    auto registration = *old;
    registration.binding = binding;
    if (old->stop_readiness.has_value()) {
      const auto desired = std::find_if(
          candidate_bundle.value().desired_control_configurations.begin(),
          candidate_bundle.value().desired_control_configurations.end(),
          [&](const auto &row) {
            return row.control_domain_id ==
                   value.control_path->control_domain_id;
          });
      Require(desired !=
                  candidate_bundle.value().desired_control_configurations.end(),
              "retained native candidate has no desired controller domain");
      const auto expected =
          deployment::load_verified_controller_lifecycle_expected_domain(
              candidate_bundle, artifacts, *desired,
              value.control_path->provider_runtime_unit_id,
              value.control_path->provider_runtime_epoch);
      auto readiness =
          core::rebind_runtime_participant_stop_readiness_product(
              *old->stop_readiness, old->binding, binding,
              expected.sealed());
      if (!readiness.ok())
        Fail("retained native Stop readiness cannot be re-sealed: " +
             readiness.error().message);
      registration.stop_readiness = std::move(readiness.value());
    }
    result.push_back(std::move(registration));
  }
  Require(result.size() == representatives.size(),
          "retained native candidate registration set is incomplete");
  const auto predecessor_rows = std::count_if(
      predecessor_registrations.begin(),
      predecessor_registrations.end(),
      [&](const auto &registration) {
        return !representatives.empty() &&
               registration.binding.value().controllable_component_id ==
                   representatives.front().value().controllable_component_id;
      });
  Require(static_cast<std::size_t>(predecessor_rows) == result.size(),
          "retained native candidate cannot discard a predecessor registration");
  return result;
}

[[nodiscard]] RetainedSceneFrameCoordinateAuthority
SceneFrameCoordinateAuthority(
    const deployment::RetainedBundle &bundle,
    const core::RuntimeParticipantBinding &binding) {
  const auto required = RetainedSceneFrameCoordinateAuthority::kRequiredPublication;
  const auto &value = binding.value();
  if (value.capability_id.value() != "tool.gripper.command")
    return required;
  Require(value.control_path.has_value(),
          "scene-frame gripper has no exact retained control path");
  const auto &runtime_id = value.control_path->provider_runtime_unit_id;
  const auto &units = bundle.value().cell_graph.runtime_units;
  const auto runtime = std::find_if(units.begin(), units.end(), [&](const auto &row) {
    return row.runtime_unit_id == runtime_id;
  });
  Require(runtime != units.end(),
          "scene-frame gripper has no retained provider runtime");
  if (runtime->runtime_profile_id.value() !=
      deployment::ros2_control_execution_runtime_contract::kRuntimeProfileId)
    return required;
  const auto configuration =
      deployment::ros2_control_execution_runtime_contract::require_component_configuration(
          bundle, runtime_id, value.controllable_component_id);
  return retained_ros2_control_gripper_actuation_kind(configuration, binding) ==
                 core::GripperActuationKind::kPneumatic
             ? RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic
             : required;
}

[[nodiscard]] RetainedSceneFrameStateSourceInput
SceneFrameSource(const RetainedObservedStateChannel &channel,
                 std::shared_ptr<const core::IObservedStateProvider> provider,
                 const RetainedSceneFrameCoordinateAuthority authority) {
  return {channel.query_context(), channel.source_kind(),
          authority == RetainedSceneFrameCoordinateAuthority::kRequiredPublication
              ? std::move(provider) : nullptr,
          authority};
}

[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input) {
  Require(input.selected_component_id.has_value() &&
              input.runtime_unit_id.has_value() &&
              input.status_program == nullptr &&
              input.status_program_binding == nullptr &&
              input.bundle != nullptr,
          "retained native component view is not a managed physical target");
  const auto predecessor = input.retained_component_snapshots.find(
      *input.selected_component_id);
  Require(predecessor != input.retained_component_snapshots.end() &&
              (predecessor->second.runtime != nullptr ||
               predecessor->second.gripper_runtime != nullptr) &&
              predecessor->second.stage != nullptr,
          "retained native component has no pinned predecessor owner");
  Require(input.planner_scene_projection != nullptr ||
              predecessor->second.runtime == nullptr,
          "retained motion controller has no candidate Scene projection");
  if (predecessor->second.gripper_runtime != nullptr) {
    Require(predecessor->second.runtime == nullptr,
            "retained native component has two controller owners");
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
        input.package_root);
    const auto inventory =
        deployment::compile_direct_control_program_binding_inventory(
            *input.bundle, input.model_map, catalog_store);
    auto physical = ResolveNativePhysicalBindings(input, inventory);
    Require(!physical.motion.has_value() && physical.gripper.has_value() &&
                physical.gripper_program != nullptr &&
                retained_bindings_share_backend_owner(
                    predecessor->second.gripper_runtime->binding(),
                    *physical.gripper),
            "retained gripper changed its physical capability or controller");
    auto candidate_registrations =
        ResealRetainedNativeCandidateRegistrations(
            {predecessor->second.gripper_runtime->registration()},
            *input.bundle, input.package_root, physical.representatives);
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto retained_model = source.get_verified(
        deployment::RetainedPlanningModelQuery{
            input.bundle->retained_bundle_id(),
            input.bundle->value().compiled_products.planning_collision_model
                .product_id});
    Require(retained_model.has_value() && input.collision_shared_model != nullptr,
            "retained gripper has no verified candidate model");
    const auto old_channel = std::dynamic_pointer_cast<RetainedObservedStateChannel>(
        predecessor->second.gripper_runtime->state_channel());
    Require(old_channel != nullptr,
            "retained gripper has no observed-state channel");
    // A model-backed gripper projects its observations into a scene of the
    // whole-cell model. When that model changes, its candidate projector is
    // prepared here and replaces the predecessor's under the Apply fence.
    auto gripper_stage = predecessor->second.stage;
    std::shared_ptr<RetainedKinematicStateProjector> candidate_tool_projector;
    if (old_channel->kinematic_projector() != nullptr &&
        predecessor->second.planning_model_product !=
            input.bundle->value().compiled_products.planning_collision_model) {
      auto candidate_stage = std::make_shared<const RetainedPlanningModelStage>(
          RetainedPlanningModelStage::create_verified(
              *retained_model, input.execution_run_root));
      auto candidate_scene = BuildCandidateRetainedModelScene(
          "manyforge_i6_native_gripper_host", *candidate_stage,
          *retained_model, *input.bundle, input.model_map,
          {*physical.gripper});
      const auto limits =
          predecessor->second.gripper_runtime->position_limits();
      const auto &resource = physical.gripper->value().resources.front();
      const auto joint =
          candidate_scene.scene->getJointInfo(resource.compiled_resource_name);
      Require(limits.has_value() && joint.has_value() &&
                  joint->limits.min_position.size() == 1U &&
                  joint->limits.max_position.size() == 1U &&
                  limits->coordinate_kind == resource.coordinate_kind &&
                  limits->lower_position == joint->limits.min_position[0] &&
                  limits->upper_position == joint->limits.max_position[0],
              "candidate cell model changes the retained gripper limits");
      candidate_tool_projector = std::move(candidate_scene.state_projector);
      gripper_stage = std::move(candidate_stage);
    }
    std::vector<RetainedSceneFrameStateSourceInput> state_sources;
    const auto gripper_binding =
        predecessor->second.gripper_runtime->binding();
    const auto &resources = gripper_binding.value().resources;
    if (std::any_of(resources.begin(), resources.end(),
                    [](const auto &resource) {
                      return resource.source_joint_name.has_value();
                    })) {
      state_sources.push_back(SceneFrameSource(*old_channel,
          std::shared_ptr<const core::IObservedStateProvider>(
              old_channel, old_channel->addressed_publication()),
          SceneFrameCoordinateAuthority(*input.bundle, *physical.gripper)));
    }
    auto frame_resolver = RetainedSceneFrameResolver::create_verified(
        input.collision_shared_model, *input.bundle, input.model_map,
        *retained_model, *input.runtime_unit_id, input.runtime_epoch,
        std::move(state_sources), input.selected_component_id);
    Require(frame_resolver != nullptr,
            "retained gripper has no candidate frame resolver");
    const std::vector<core::ProgramBindingId> direct_binding_ids{
        physical.gripper_program->program_binding_id()};
    auto target_resolver =
        std::make_shared<ProductBackedRuntimeParticipantResolver>(
            input.package_root, input.bundle, input.model_map,
            std::vector<core::RuntimeParticipantBinding>{*physical.gripper},
            direct_binding_ids, input.program_artifact_root);
    RetainedRuntimeParticipantResolverFactory resolver_factory =
        [package_root = input.package_root, bundle = input.bundle,
         model_map = input.model_map,
         program_artifact_root = input.program_artifact_root,
         direct_binding_ids](
            std::vector<core::RuntimeParticipantBinding> bindings) {
          auto resolver =
              std::make_shared<ProductBackedRuntimeParticipantResolver>(
                  package_root, bundle, model_map, std::move(bindings),
                  direct_binding_ids, program_artifact_root);
          return RetainedRuntimeParticipantResolver{
              [resolver](const core::OperationIntentDTO &operation,
                         const std::size_t participant_index) {
                return resolver->ResolveExecutionParticipant(
                    operation, participant_index);
              }};
        };
    auto composition = std::make_unique<Composition>(
        *input.selected_component_id, std::move(input.bundle), nullptr, nullptr,
        nullptr, std::move(physical.gripper_program), std::nullopt,
        std::move(physical.gripper), std::nullopt,
        CanonicalValue(nullptr), predecessor->second.gripper_descriptor,
        std::move(gripper_stage), nullptr,
        std::move(input.projection_revision),
        input.publication_generation, std::nullopt,
        physical.status_gripper_role);
    composition->gripper_runtime = predecessor->second.gripper_runtime;
    composition->ros2_control_runtime_handle = predecessor->second.ros2_control_runtime_handle;
    composition->candidate_tool_state_projector =
        std::move(candidate_tool_projector);
    composition->target_resolver = std::move(target_resolver);
    composition->candidate_control_representatives =
        std::move(physical.representatives);
    composition->candidate_participant_resolver_factory =
        std::move(resolver_factory);
    composition->candidate_component_registrations =
        std::move(candidate_registrations);
    composition->scene_frame_resolver = std::move(frame_resolver);
    composition->planner_scene_projection =
        std::move(input.planner_scene_projection);
    composition->required_projection_pins = std::move(input.required_projection_pins);
    if (composition->planner_scene_projection != nullptr) {
      composition->scene_authority_proxy =
          std::make_shared<ManagerSceneAuthorityProxy>(
              std::move(input.manager_run_root));
    }
    return composition;
  }
  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical = ResolveNativePhysicalBindings(input, inventory);
  Require(physical.motion.has_value() &&
              physical.status_motion_role.has_value() &&
              physical.gripper.has_value() ==
                  predecessor->second.runtime->gripper_binding.has_value(),
          "retained native controller changed its physical capability set");
  auto candidate_registrations =
      ResealRetainedNativeCandidateRegistrations(
          predecessor->second.runtime->participant_registrations,
          *input.bundle, input.package_root,
          physical.representatives);

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  const auto retained_model = planning_source.get_verified(
      deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          input.bundle->value().compiled_products.planning_collision_model
              .product_id});
  Require(retained_model.has_value() && input.collision_shared_model != nullptr,
          "retained native view has no verified candidate model");
  // Runtime identity excludes the shared cell planning model (other
  // components, passive models, root poses). The retained worker plans in
  // that whole-cell model, so a changed product stages the candidate model
  // here and replaces the worker's model under the Apply fence.
  // The same holds for every in-process model consumer (scene, state
  // projector, kinematics, validator, collision projector): all of them are
  // prepared here from the candidate model and the candidate bindings that
  // the re-seal installs, and replace the predecessor generation together.
  auto planning_stage = predecessor->second.stage;
  std::optional<RoboPlanWorkerModelDTO> candidate_planner_model;
  std::shared_ptr<const RetainedKinematicModelGeneration>
      candidate_model_generation;
  if (predecessor->second.planning_model_product !=
      input.bundle->value().compiled_products.planning_collision_model) {
    auto candidate_stage = std::make_shared<const RetainedPlanningModelStage>(
        RetainedPlanningModelStage::create_verified(*retained_model,
                                                    input.execution_run_root));
    const auto candidate_srdf = retained_model->srdf_bytes();
    const auto candidate_documents =
        deployment::parse_model_documents_for_projection(
            retained_model->urdf_bytes(),
            candidate_srdf.empty()
                ? std::optional<std::string_view>{}
                : std::optional<std::string_view>{candidate_srdf});
    candidate_planner_model = NativeWorkerBaseModel(
        *candidate_stage,
        RoboPlanJointLimitsFromRetainedModel(candidate_documents.urdf));
    std::vector<core::RuntimeParticipantBinding> projected_bindings{
        physical.motion.value()};
    if (physical.gripper.has_value() &&
        physical.gripper->value().resources.front().source_joint_name
            .has_value())
      projected_bindings.push_back(*physical.gripper);
    auto candidate_scene = BuildCandidateRetainedModelScene(
        "manyforge_i6_native_execution_host", *candidate_stage,
        *retained_model, *input.bundle, input.model_map,
        std::move(projected_bindings));
    candidate_model_generation = make_retained_motion_model_generation(
        physical.motion.value(), std::move(candidate_scene.scene),
        std::move(candidate_scene.scene_mutex),
        std::move(candidate_scene.state_projector));
    planning_stage = std::move(candidate_stage);
  }
  std::vector<RetainedSceneFrameStateSourceInput> state_sources;
  const auto add_state_source = [&](const auto &channel) {
    if (channel == nullptr)
      return;
    const auto channel_binding = channel->binding();
    const auto &resources = channel_binding.value().resources;
    if (std::none_of(resources.begin(), resources.end(),
                     [](const auto &resource) {
                       return resource.source_joint_name.has_value();
                     }))
      return;
    Require(std::all_of(resources.begin(), resources.end(),
                        [](const auto &resource) {
                          return resource.source_joint_name.has_value();
                        }),
            "retained native state channel mixes model and local resources");
    state_sources.push_back(SceneFrameSource(*channel,
        std::shared_ptr<const core::IObservedStateProvider>(
            channel, channel->addressed_publication()),
        SceneFrameCoordinateAuthority(*input.bundle, channel_binding)));
  };
  add_state_source(predecessor->second.runtime->motion_state_channel);
  if (predecessor->second.runtime->gripper_state_channel)
    add_state_source(predecessor->second.runtime->gripper_state_channel);
  auto frame_resolver = RetainedSceneFrameResolver::create_verified(
      input.collision_shared_model, *input.bundle, input.model_map,
      *retained_model, *input.runtime_unit_id, input.runtime_epoch,
      std::move(state_sources), input.selected_component_id);
  Require(frame_resolver != nullptr &&
              frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "retained native candidate frame resolver has another product");

  std::vector<core::ProgramBindingId> direct_binding_ids{
      physical.motion_program->program_binding_id()};
  if (physical.gripper_program != nullptr)
    direct_binding_ids.push_back(
        physical.gripper_program->program_binding_id());
  std::sort(direct_binding_ids.begin(), direct_binding_ids.end());
  std::vector<core::RuntimeParticipantBinding> physical_bindings{
      physical.motion.value()};
  if (physical.gripper.has_value())
    physical_bindings.push_back(*physical.gripper);
  auto target_resolver =
      std::make_shared<ProductBackedRuntimeParticipantResolver>(
          input.package_root, input.bundle, input.model_map,
          std::move(physical_bindings), direct_binding_ids,
          input.program_artifact_root);
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       direct_binding_ids = std::move(direct_binding_ids)](
          std::vector<core::RuntimeParticipantBinding> bindings) {
        auto resolver =
            std::make_shared<ProductBackedRuntimeParticipantResolver>(
                package_root, bundle, model_map, std::move(bindings),
                direct_binding_ids, program_artifact_root);
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t index) {
              return resolver->ResolveExecutionParticipant(operation, index);
            }};
      };
  auto composition = std::make_unique<Composition>(
      *input.selected_component_id, std::move(input.bundle), nullptr, nullptr,
      std::move(physical.motion_program),
      std::move(physical.gripper_program), std::move(physical.motion.value()),
      std::move(physical.gripper), predecessor->second.joint_limits,
      predecessor->second.motion_descriptor,
      predecessor->second.gripper_descriptor, std::move(planning_stage),
      predecessor->second.runtime, std::move(input.projection_revision),
      input.publication_generation, std::move(*physical.status_motion_role),
      physical.status_gripper_role);
  Require(predecessor->second.ros2_control_runtime_handle != nullptr,
          "retained ROS component has no physical handle owner");
  composition->ros2_control_runtime_handle =
      predecessor->second.ros2_control_runtime_handle;
  composition->controller_process = predecessor->second.controller_process;
  composition->controller_request_context = input.controller_request_context;
  composition->candidate_planner_model = std::move(candidate_planner_model);
  composition->candidate_model_generation =
      std::move(candidate_model_generation);
  composition->target_resolver = std::move(target_resolver);
  composition->candidate_control_representatives =
      std::move(physical.representatives);
  composition->candidate_component_registrations =
      std::move(candidate_registrations);
  composition->candidate_participant_resolver_factory =
      std::move(resolver_factory);
  composition->candidate_collision_publisher = input.collision_publisher;
  composition->candidate_collision_model = input.collision_shared_model;
  composition->candidate_runtime_collision_engine =
      std::make_shared<collision::ManyForgeCollisionEngine>(
          input.collision_shared_model,
          collision::ManyForgeCollisionEngine::make_shared_scene_cache(
              input.collision_shared_model));
  const auto validation_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  for (std::size_t index = 0;
       index < predecessor->second.runtime->validation_collision_engines.size();
       ++index) {
    composition->candidate_validation_collision_engines.push_back(
        std::make_shared<collision::ManyForgeCollisionEngine>(
            input.collision_shared_model, validation_cache));
  }
  const auto lookahead_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  for (std::size_t index = 0;
       index < predecessor->second.runtime->lookahead_collision_engines.size();
       ++index) {
    composition->candidate_lookahead_collision_engines.push_back(
        std::make_shared<collision::ManyForgeCollisionEngine>(
            input.collision_shared_model, lookahead_cache));
  }
  composition->scene_frame_resolver = std::move(frame_resolver);
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  composition->scene_authority_proxy =
      std::make_shared<ManagerSceneAuthorityProxy>(
          std::move(input.manager_run_root));
  return composition;
}


[[nodiscard]] std::unique_ptr<Composition>
BuildGripperOnlyProductComposition(CompositionBuildInput input, NativePhysicalBindingSet physical) {
  Require(!physical.motion && physical.gripper && physical.gripper_program &&
              physical.status_gripper_role && input.shared_clock && input.shared_scheduler,
          "ROS gripper-only component requires its exact physical target and stable cell time");
  const auto binding = *physical.gripper;
  const auto& value = binding.value();
  Require(value.resources.size() == 1U && value.model_groups.size() == 1U &&
              value.resources.front().source_joint_name && value.control_path,
          "ROS gripper-only component requires one genuine model-backed scalar joint");
  const auto local = input.runtime_unit_id.value_or(value.runtime_unit_epochs.front().runtime_unit_id);
  const auto configuration = deployment::ros2_control_execution_runtime_contract::require_configuration(
      *input.bundle, local);
  const auto* selected = deployment::ros2_control_execution_runtime_contract::find_controller_profile(
      configuration, value.controllable_component_id);
  const auto authored = std::find_if(configuration.gripper_target_configurations.begin(),
      configuration.gripper_target_configurations.end(), [&](const auto& row) {
        return row.controllable_component_id == value.controllable_component_id &&
               row.command_target_id == value.command_target_id;
      });
  Require(authored != configuration.gripper_target_configurations.end(),
          "ROS gripper-only component has no exact authored gripper configuration");
  const bool no_output = authored->output_mode == core::GripperOutputMode::kNone;
  Require(!no_output || authored->actuation_kind == "pneumatic",
          "No output requires its strict pneumatic gripper target");
  Require(selected && (no_output || (selected->mock_profile && !selected->isaac_profile)),
          "ROS gripper-only component requires its selected Mock controller profile");
  std::optional<MockControllerProfileFacts> profile;
  if (!no_output) {
    profile = mock_controller_profile_facts(*selected->mock_profile);
    Require(profile->gripper_required && profile->gripper_controller_name,
            "ROS gripper-only profile has no real gripper controller");
  }
  const deployment::FilesystemVerifiedRetainedPlanningModelSource model_source(input.package_root);
  const auto model = model_source.get_verified({input.bundle->retained_bundle_id(),
      input.bundle->value().compiled_products.planning_collision_model.product_id});
  Require(model.has_value() && input.collision_shared_model,
          "ROS gripper-only component has no verified cell geometry");
  auto stage = std::make_shared<const RetainedPlanningModelStage>(
      RetainedPlanningModelStage::create_verified(*model, input.execution_run_root));
  auto projected = BuildCandidateRetainedModelScene("manyforge_ros_gripper_host", *stage,
      *model, *input.bundle, input.model_map, {binding});
  const auto joint = projected.scene->getJointInfo(value.resources.front().compiled_resource_name);
  Require(joint && joint->limits.min_position.size() == 1U && joint->limits.max_position.size() == 1U,
          "ROS gripper-only component has no exact scalar model limits");
  const RetainedGripperPositionLimits limits{value.resources.front().coordinate_kind,
      joint->limits.min_position[0], joint->limits.max_position[0]};
  auto channel = std::make_shared<RetainedObservedStateChannel>(binding, projected.state_projector,
      retained_ros2_control_gripper_state_config(binding), input.shared_clock.get(), input.observation_wake);
  Require(channel->initialize_from_projection(input.shared_clock->now()).applied(),
          "ROS gripper-only component could not seed its genuine model projection");
  const auto observed = projected.state_projector->current_state(
      value.control_path->control_binding_id, value.control_path->control_binding_revision);
  Require(observed.ok() && observed.value().positions.size() == 1U,
          "ROS gripper-only component has no exact projected initial scalar");
  const auto& position = observed.value().positions.front();
  const double initial = std::holds_alternative<core::AngularPositionDTO>(position)
      ? std::get<core::AngularPositionDTO>(position).value_rad : std::get<core::LinearPositionDTO>(position).value_m;
  const auto instance = std::find_if(input.bundle->value().cell_graph.model_instances.begin(),
      input.bundle->value().cell_graph.model_instances.end(), [&](const auto& row) {
        return row.model_instance_id == value.model_groups.front().model_instance_id;
      });
  Require(instance != input.bundle->value().cell_graph.model_instances.end(),
          "ROS gripper-only source model instance is missing");
  const auto source = std::find_if(model->model_source_urdfs().begin(), model->model_source_urdfs().end(),
      [&](const auto& row) { return row.model_asset_id == instance->model_asset_id &&
                                  row.model_revision == instance->model_revision; });
  Require(source != model->model_source_urdfs().end(), "ROS gripper-only source URDF is missing");
  struct UnadoptedLaunch final {
    const ControllerLaunchSupervisor::Ensured* receipt{};
    ~UnadoptedLaunch() {
      if (receipt) static_cast<void>(host_controller_launch_supervisor().discard_candidate(*receipt));
    }
  };
  std::optional<ControllerLaunchSupervisor::Ensured> launch;
  UnadoptedLaunch owned_launch{};
  if (!no_output) {
    ComponentControllerLaunchInput artifacts;
    artifacts.artifacts_root = input.execution_run_root / "controller-launch-artifacts" /
        value.controllable_component_id.value();
    artifacts.model_urdf = source->urdf_bytes;
    artifacts.gripper_binding = binding;
    artifacts.gripper_projected_position = initial;
    artifacts.mock_profile = selected->mock_profile;
    artifacts.frame_prefix = selected->frame_prefix;
    const auto written = write_component_controller_launch_artifacts(
        std::optional<core::RuntimeParticipantBinding>{}, artifacts);
    const auto command = mock_controller_launch_command(written);
    const auto acquired = host_controller_launch_supervisor().ensure(command, written.directory,
        input.controller_request_context.empty() ? "Bootstrap runtime=" + local.value() +
        " epoch=" + input.runtime_epoch.value() : input.controller_request_context);
    UnadoptedLaunch acquired_guard{&acquired};
    launch = acquired; // the local receipt remains guarded if this copy throws
    owned_launch.receipt = &*launch;
    acquired_guard.receipt = nullptr;
  }
  auto handle = std::make_shared<RetainedRos2ControlRuntimeHandle>();
  if (launch) handle->InstallControllerProcessFaultSource(
      ControllerLaunchSupervisor::fault_source(launch->process, CachedControllerLaunchReadinessSource(handle)),
      [process = launch->process] { return core::Result<std::optional<std::string>>::Ok(
          ProjectControllerLaunchFault(process->ReadCachedFaultState())); });
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_ros2_control_execution_backend";
  config.node_namespace = controller_runtime_namespace(value.controllable_component_id.value());
  config.executor_thread_count = 3U;
  if (no_output) {
    ros2_control_execution::Ros2GripperActionConfig local;
    local.group_id = value.model_groups.front().compiled_group_name;
    local.joint_name = value.resources.front().compiled_resource_name;
    local.state_joint_name = *value.resources.front().source_joint_name;
    config.gripper = std::move(local);
  } else {
    configure_retained_mock_gripper_action(config, binding, *profile,
        config.node_namespace, config.node_namespace + "/controller_manager");
  }
  configure_retained_gripper_output_mode(config, configuration, binding);
  config.defer_ros_start = input.managed_component_delta.has_value();
  config.measured_receiver_clock = input.shared_clock;
  auto backend = make_retained_ros2_control_gripper_backend(std::move(config), binding, channel,
      *input.shared_clock, *input.shared_scheduler, handle);
  RetainedNativeGripperRuntimeInput gripper_input{
      binding, std::move(backend), input.shared_clock, channel, nullptr, {}, true,
      [handle](const auto& target, const auto& state, const auto&) {
        return handle->RebindGripperRoute(target, state);
      }, input.shared_scheduler, limits};
  const RetainedMeasuredJointStateLane measured_lane{
      controller_runtime_namespace(value.controllable_component_id.value()) + "/joint_states",
      1'000'000'000LL, core::StateAuthorityId(std::string(kMeasuredJointStateAuthorityId)),
      std::string(kMeasuredJointStateProfileId)};
  const bool pneumatic_gripper = retained_ros2_control_gripper_actuation_kind(configuration, binding) ==
      core::GripperActuationKind::kPneumatic;
  gripper_input.prepare_measured_sources = [handle, clock = input.shared_clock, measured_lane, pneumatic_gripper](
      const core::RuntimeParticipantBinding& current, const core::ActiveControlConfigurationDTO& configuration)
      -> core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>> {
    using Result = core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>;
    if (pneumatic_gripper)
      return Result::Ok(std::make_shared<const RetainedPreparedRobotMeasuredSources>(
          RetainedPreparedRobotMeasuredSources{{current.value().controllable_component_id, {}}, [] {}}));
    auto descriptor = make_retained_ros2_control_measured_state_descriptor(current, configuration, measured_lane);
    if (!descriptor.ok()) return Result::Err(descriptor.error());
    auto staged = handle->StageMeasuredStateSources({descriptor.value()}, clock,
        std::optional<core::RuntimeParticipantBinding>{}, std::optional(current));
    if (!staged.ok()) return Result::Err(staged.error());
    if (!staged.value()) return Result::Ok(nullptr);
    auto publication = std::move(staged.value());
    core::RobotMeasuredStateSourceView sources{current.value().controllable_component_id, {}};
    for (std::size_t index = 0; index < publication->descriptors().size(); ++index)
      sources.sources.push_back({publication->descriptors()[index], publication->readers()[index]});
    return Result::Ok(std::make_shared<const RetainedPreparedRobotMeasuredSources>(
        RetainedPreparedRobotMeasuredSources{std::move(sources), [publication] { publication->EnablePublication(); }}));
  };
  gripper_input.control_binding_representatives = physical.representatives;
  auto owner = std::make_shared<RetainedNativeGripperRuntime>(std::move(gripper_input));
  const auto srdf = model->srdf_bytes();
  const auto documents = deployment::parse_model_documents_for_projection(model->urdf_bytes(),
      srdf.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{srdf});
  auto descriptor = BuildManualControlGripperDescriptor(binding, limits,
      configuration.gripper_target_configurations, input.model_map, documents);
  const auto make_resolver = [package_root = input.package_root, bundle = input.bundle,
      model_map = input.model_map, program_artifact_root = input.program_artifact_root,
      program_id = physical.gripper_program->program_binding_id()](auto bindings) {
    return std::make_shared<ProductBackedRuntimeParticipantResolver>(package_root, bundle,
        model_map, std::move(bindings), std::vector<core::ProgramBindingId>{program_id}, program_artifact_root);
  };
  auto resolver = make_resolver(std::vector<core::RuntimeParticipantBinding>{binding});
  std::vector<RetainedSceneFrameStateSourceInput> frame_sources;
  frame_sources.push_back(SceneFrameSource(*channel,
      std::shared_ptr<const core::IObservedStateProvider>(channel, channel->addressed_publication()),
      SceneFrameCoordinateAuthority(*input.bundle, binding)));
  auto frames = RetainedSceneFrameResolver::create_verified(input.collision_shared_model, *input.bundle,
      input.model_map, *model, local, input.runtime_epoch, std::move(frame_sources),
      input.selected_component_id);
  auto composition = std::make_unique<Composition>(value.controllable_component_id, input.bundle,
      input.status_program, input.status_program_binding, nullptr, physical.gripper_program,
      std::nullopt, binding, std::nullopt, CanonicalValue(nullptr), std::move(descriptor),
      stage, nullptr, input.projection_revision, input.publication_generation, std::nullopt,
      physical.status_gripper_role);
  composition->gripper_runtime = std::move(owner);
  composition->ros2_control_runtime_handle = std::move(handle);
  composition->measured_joint_state_lane = measured_lane;
  composition->controller_process = launch ? launch->process : nullptr;
  composition->controller_request_context = input.controller_request_context;
  composition->prepared_controller_launch = launch;
  owned_launch.receipt = nullptr;
  composition->target_resolver = std::move(resolver);
  composition->candidate_participant_resolver_factory = [make_resolver](auto bindings) {
    auto owner = make_resolver(std::move(bindings));
    return RetainedRuntimeParticipantResolver{[owner](const auto& operation, const std::size_t index) {
      return owner->ResolveExecutionParticipant(operation, index);
    }};
  };
  composition->scene_frame_resolver = std::move(frames);
  composition->candidate_control_representatives = std::move(physical.representatives);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  return composition;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildSingleProductComposition(CompositionBuildInput input) {
  if (input.selected_component_id.has_value() &&
      input.retained_component_snapshots.contains(*input.selected_component_id))
    return BuildRetainedProductComposition(std::move(input));
  if (!input.attach_addressed_owner &&
      (input.shared_clock == nullptr) != (input.shared_scheduler == nullptr)) {
    throw std::invalid_argument("native runtime clock and scheduler "
                                "capabilities must be supplied together");
  }
  Require(input.bundle != nullptr &&
              input.bundle->value().cell_graph.cell_id == input.cell_id &&
              (!input.runtime_unit_id.has_value() ||
               !input.runtime_unit_id->empty()) &&
              !input.runtime_epoch.empty() &&
              input.model_map.query().retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.graph_revision ==
                  input.bundle->value().cell_graph.graph_revision &&
              input.publication_generation.nonzero() &&
              (input.status_program == nullptr) ==
                  (input.status_program_binding == nullptr) &&
              (input.planner_scene_projection == nullptr ||
               !input.manager_run_root.empty()),
          "native physical composition inputs do not share one active "
          "product");
  const auto &products = input.bundle->value().compiled_products;
  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  auto direct_control_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical_bindings =
      ResolveNativePhysicalBindings(input, direct_control_inventory);
  if (!physical_bindings.motion && physical_bindings.gripper)
    return BuildGripperOnlyProductComposition(std::move(input), std::move(physical_bindings));
  Require(physical_bindings.motion.has_value() &&
              physical_bindings.status_motion_role.has_value(),
          "motion-backed native composition has no motion target");
  auto direct_control_binding = std::move(physical_bindings.motion_program);
  auto direct_control_gripper_program_binding =
      std::move(physical_bindings.gripper_program);
  auto direct_control_motion_binding = std::move(physical_bindings.motion);
  auto direct_control_gripper_binding = std::move(physical_bindings.gripper);
  auto control_binding_representatives =
      std::move(physical_bindings.representatives);
  input.status_motion_target_role_id =
      std::move(*physical_bindings.status_motion_role);
  input.status_gripper_target_role_id =
      physical_bindings.status_gripper_role;
  auto local_runtime_unit_id = input.runtime_unit_id;
  if (!local_runtime_unit_id.has_value()) {
    const auto &runtime_units =
        direct_control_motion_binding.value().value().runtime_unit_epochs;
    Require(runtime_units.size() == 1U,
            "standalone native execution cannot infer one local runtime "
            "unit");
    local_runtime_unit_id = runtime_units.front().runtime_unit_id;
  }
  const auto contains_local_runtime = [&](const auto &binding) {
    const auto &runtime_units = binding.value().runtime_unit_epochs;
    return std::find_if(runtime_units.begin(), runtime_units.end(),
                        [&](const auto &runtime_unit) {
                          return runtime_unit.runtime_unit_id ==
                                     *local_runtime_unit_id &&
                                 runtime_unit.runtime_epoch ==
                                     input.runtime_epoch;
                        }) != runtime_units.end();
  };
  Require(contains_local_runtime(direct_control_motion_binding.value()) &&
              (!direct_control_gripper_binding.has_value() ||
               contains_local_runtime(*direct_control_gripper_binding)),
          "native execution local runtime is absent from its exact "
          "participant bindings");
  const auto retained_runtime_configuration =
      deployment::ros2_control_execution_runtime_contract::
          require_configuration(*input.bundle, *local_runtime_unit_id);
  std::optional<double> deployment_motion_target_limit;
  std::vector<core::ControlBindingDTO> motion_limit_bindings;
  motion_limit_bindings.reserve(direct_control_inventory.motion.size());
  for (const auto &binding : direct_control_inventory.motion) {
    const auto target = deployment::resolve_runtime_program_target(
        *binding,
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("execution.trajectory.execute"), *input.bundle,
        input.model_map);
    Require(target.control_binding().has_value(),
            "motion target has no exact control binding");
    motion_limit_bindings.push_back(*target.control_binding());
  }
  constexpr bool whole_runtime_motion_limits = true;
  std::vector<std::pair<core::ControllableComponentId,
                        core::CommandTargetId>> configured_motion_limits;
  configured_motion_limits.reserve(
      retained_runtime_configuration.motion_target_limits.size());
  std::transform(
      retained_runtime_configuration.motion_target_limits.begin(),
      retained_runtime_configuration.motion_target_limits.end(),
      std::back_inserter(configured_motion_limits), [](const auto &limit) {
        return std::pair{limit.controllable_component_id,
                         limit.command_target_id};
      });
  Require(execution_motion_target_limit_keys_match(
              motion_limit_bindings, configured_motion_limits,
              *local_runtime_unit_id,
              direct_control_motion_binding.value().value().controllable_component_id,
              whole_runtime_motion_limits),
          "execution Deployment must contain exactly one TCP-speed maximum for "
          "every motion-capable target and no unknown target");
  const auto motion_limit =
      std::find_if(retained_runtime_configuration.motion_target_limits.begin(),
                   retained_runtime_configuration.motion_target_limits.end(),
                   [&](const auto &candidate) {
                     return candidate.controllable_component_id ==
                                direct_control_motion_binding.value()
                                    .value()
                                    .controllable_component_id &&
                            candidate.command_target_id ==
                                direct_control_motion_binding.value()
                                    .value()
                                    .command_target_id;
                   });
  Require(motion_limit !=
              retained_runtime_configuration.motion_target_limits.end(),
          "execution Deployment has no limit for the exact motion target");
  deployment_motion_target_limit = motion_limit->maximum_tcp_linear_speed_mps;
  auto selected_initial_joint_positions =
      retained_runtime_configuration.initial_joint_positions_rad;
  const auto selected_resource_count =
      direct_control_motion_binding.value().value().resources.size();
  if (selected_initial_joint_positions.size() != selected_resource_count) {
    struct InitialPositionSlice final {
      core::ControllableComponentId component_id;
      std::size_t count{};
    };
    std::vector<InitialPositionSlice> slices;
    for (const auto &binding : direct_control_inventory.motion) {
      const auto target = deployment::resolve_runtime_program_target(
          *binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId("execution.trajectory.execute"), *input.bundle,
          input.model_map);
      const auto verified = core::verify_runtime_participant_binding(
          deployment::build_runtime_participant_binding_dto(
              target, ActiveFacts(target, {}, input.runtime_epoch,
                                  core::ActiveControlConfigurationId(
                                      "active.i6.native.arm"))));
      Require(verified.ok(), "native initial-state target is invalid");
      slices.push_back({target.component().controllable_component_id,
                        verified.value().value().resources.size()});
    }
    std::sort(slices.begin(), slices.end(),
              [](const auto &left, const auto &right) {
                return left.component_id < right.component_id;
              });
    const auto total = std::accumulate(
        slices.begin(), slices.end(), std::size_t{0},
        [](const auto sum, const auto &slice) { return sum + slice.count; });
    Require(total == selected_initial_joint_positions.size() &&
                input.selected_component_id.has_value(),
            "native execution initial state differs from the cell motion "
            "resource order");
    std::size_t offset = 0U;
    for (const auto &slice : slices) {
      if (slice.component_id == *input.selected_component_id) {
        selected_initial_joint_positions = std::vector<double>(
            retained_runtime_configuration.initial_joint_positions_rad.begin() +
                static_cast<std::ptrdiff_t>(offset),
            retained_runtime_configuration.initial_joint_positions_rad.begin() +
                static_cast<std::ptrdiff_t>(offset + slice.count));
        break;
      }
      offset += slice.count;
    }
    Require(selected_initial_joint_positions.size() == selected_resource_count,
            "native execution initial-state slice is unavailable");
  }
  Require(selected_initial_joint_positions.size() == selected_resource_count,
          "native initial-state selection differs from the exact motion binding");
  const auto execution_tick_period_ns = static_cast<core::Duration>(
      std::llround(1'000'000'000.0 /
                   static_cast<double>(
                       retained_runtime_configuration.execution_tick_hz)));
  const auto collision_monitor_period_ns = static_cast<core::Duration>(
      std::llround(retained_runtime_configuration.collision_monitor_period_ms *
                   1'000'000.0));
  Require(execution_tick_period_ns > 0 && collision_monitor_period_ns > 0,
          "native execution retained runtime cadences are invalid");

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  auto retained_model =
      planning_source.get_verified(deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          products.planning_collision_model.product_id});
  Require(retained_model.has_value(),
          "active retained planning model is absent or invalid");
  const auto retained_srdf = retained_model->srdf_bytes();
  const auto retained_model_documents =
      deployment::parse_model_documents_for_projection(
          retained_model->urdf_bytes(),
          retained_srdf.empty()
              ? std::optional<std::string_view>{}
              : std::optional<std::string_view>{retained_srdf});
  const auto roboplan_joint_limits = RoboPlanJointLimitsFromRetainedModel(
      retained_model_documents.urdf);
  auto stage = RetainedPlanningModelStage::create_verified(
      *retained_model, input.execution_run_root);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "manyforge_i6_native_execution_host",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), roboplan_joint_limits});
  auto scene_mutex = std::make_shared<std::mutex>();
  std::vector<core::RuntimeParticipantBinding> projected_state_bindings{
      direct_control_motion_binding.value()};
  if (direct_control_gripper_binding.has_value() &&
      direct_control_gripper_binding->value()
          .resources.front()
          .source_joint_name.has_value()) {
    projected_state_bindings.push_back(*direct_control_gripper_binding);
  }
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *input.bundle, input.model_map, *retained_model,
          std::move(projected_state_bindings), 0));

  auto publisher = std::move(input.collision_publisher);
  auto collision_model = std::move(input.collision_shared_model);
  if (publisher == nullptr) {
    const auto &group =
        direct_control_motion_binding.value().value().model_groups.front();
    // The retained package root is not a scene-mesh root: relative scene
    // meshes resolve only below the separately approved scene-asset root.
    collision_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_i6_native_execution_host", stage.loader_urdf_path(),
        stage.srdf_path(), stage.package_paths(), group.compiled_group_name,
        input.scene_asset_root_fd);
    auto collision_projector = RetainedCollisionWorldProjector::create_verified(
        collision_model, *input.bundle, input.model_map, *retained_model);
    const auto logical_scene =
        EmptyScene(input.bundle->value().cell_graph.cell_id,
                   input.projection_revision.scene_revision);
    auto publisher_result = RetainedCollisionWorldPublisher::create(
        std::move(collision_projector), "i6-native-execution-host",
        logical_scene,
        RetainedCollisionWorldPublicationContext{
            core::CollisionWorldMutationSource::kDeploymentApply,
            {},
            WallClock{}.now(),
            {},
            "i6_native_execution_host_startup",
            std::nullopt},
        [] { return WallClock{}.now(); });
    if (!publisher_result.ok()) {
      Fail("retained collision publication failed: " +
           publisher_result.error().message);
    }
    publisher = std::move(publisher_result.value());
  }
  Require(publisher != nullptr && collision_model != nullptr,
          "native composition has no authoritative collision projection");
  const auto inventory = publisher->current_scene_snapshot_lease_inventory();
  Require(inventory.has_value() &&
              inventory->projection_binding.projection_revision ==
                  input.projection_revision,
          "native collision publisher does not expose the active product "
          "projection");

  auto candidate_binding =
      CandidateBinding(direct_control_motion_binding.value());
  auto candidate_client =
      std::make_unique<ProcessContainedRoboPlanCandidateClient>(
          NativeWorkerBaseModel(stage, roboplan_joint_limits),
          candidate_binding,
          RoboPlanCandidateComputerConfig{
              .rng_seed = retained_runtime_configuration.planner_rng_seed},
          ProcessContainedCandidateClientConfig{
              .worker_executable = input.candidate_worker,
              .require_retained_projection = true,
              .scene_asset_root_fd = input.scene_asset_root_fd,
              .retained_package_root_fd = input.retained_package_root_fd,
              // The worker must load exactly the scene-mesh bytes this
              // composition's collision model loaded (or first pinned).
              .scene_mesh_content_digest =
                  [collision_model](const std::string &uri) {
                    return collision::ManyForgeCollisionEngine::
                        scene_mesh_content_digest(collision_model, uri);
                  }});
  Require(candidate_client->requires_retained_projection(),
          "production native planner must consume the exact retained scene "
          "projection");
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding,
      std::move(candidate_client));
  std::vector<core::ProgramBindingId> intrinsic_direct_bindings{
      direct_control_binding->program_binding_id()};
  if (direct_control_gripper_program_binding != nullptr) {
    intrinsic_direct_bindings.push_back(
        direct_control_gripper_program_binding->program_binding_id());
  }
  const auto make_participant_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       intrinsic_direct_bindings = std::move(intrinsic_direct_bindings)](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            intrinsic_direct_bindings, program_artifact_root);
      };
  const RetainedRuntimeParticipantResolverFactory participant_resolver_factory =
      [make_participant_resolver](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        auto resolver = make_participant_resolver(std::move(physical_bindings));
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t participant_index) {
              return resolver->ResolveExecutionParticipant(operation,
                                                           participant_index);
            }};
      };
  std::vector<core::RuntimeParticipantBinding> target_physical_bindings{
      direct_control_motion_binding.value()};
  if (direct_control_gripper_binding.has_value())
    target_physical_bindings.push_back(*direct_control_gripper_binding);
  auto target_resolver =
      make_participant_resolver(std::move(target_physical_bindings));
  RetainedRuntimeParticipantResolver participant_resolver =
      [resolver = target_resolver](const core::OperationIntentDTO &operation,
                                   const std::size_t participant_index) {
        return resolver->ResolveExecutionParticipant(operation,
                                                     participant_index);
      };
  const auto reference_collision_model = collision_model;
  const auto reference_runtime_unit_id = *local_runtime_unit_id;
  const auto reference_runtime_epoch = input.runtime_epoch;
  const auto reference_component_id = input.selected_component_id;
  RetainedNativeRuntimeInput runtime_input{
      direct_control_motion_binding.value(),
      direct_control_gripper_binding,
      {},
      {},
      std::move(input.motion_program_aliases),
      std::move(input.gripper_program_aliases),
      scene,
      scene_mutex,
      state_projector,
      publisher,
      collision_model,
      std::move(planner),
      [&, reference_collision_model, reference_runtime_unit_id,
       reference_runtime_epoch, reference_component_id](const auto &channels) {
        std::vector<RetainedSceneFrameStateSourceInput> state_sources;
        state_sources.reserve(channels.size());
        for (const auto &channel : channels) {
          const auto binding = channel->binding();
          const auto &resources = binding.value().resources;
          const bool has_model_resource = std::any_of(
              resources.begin(), resources.end(), [](const auto &resource) {
                return resource.source_joint_name.has_value();
              });
          if (!has_model_resource) {
            continue;
          }
          Require(std::all_of(resources.begin(), resources.end(),
                              [](const auto &resource) {
                                return resource.source_joint_name.has_value();
                              }),
                  "native scene-frame state channel mixes model and "
                  "component-local resources");
          state_sources.push_back(SceneFrameSource(*channel,
              std::shared_ptr<const core::IObservedStateProvider>(
                  channel, channel->addressed_publication()),
              SceneFrameCoordinateAuthority(*input.bundle, binding)));
        }
        return RetainedSceneFrameResolver::create_verified(
            reference_collision_model, *input.bundle, input.model_map,
            *retained_model, reference_runtime_unit_id, reference_runtime_epoch,
            std::move(state_sources), reference_component_id);
      },
      std::move(participant_resolver),
      participant_resolver_factory,
      input.operation_admission_gate,
      {},
      {},
      {},
      {},
      {}};
  runtime_input.control_binding_representatives =
      std::move(control_binding_representatives);
  runtime_input.observation_wake = std::move(input.observation_wake);
  runtime_input.deployment_maximum_tcp_linear_speed_mps =
      deployment_motion_target_limit;
  // Q37 (as main): the Deployment-authored fault Stop profile is the motion
  // target's fault profile, read directly; absent means none.
  runtime_input.fault_stop_profile_from_deployment = true;
  for (const auto &authored :
       retained_runtime_configuration.default_fault_stop_profiles) {
    if (authored.controllable_component_id ==
            runtime_input.motion_binding.value().controllable_component_id &&
        authored.command_target_id ==
            runtime_input.motion_binding.value().command_target_id) {
      runtime_input.deployment_fault_stop_profile = authored.profile;
    }
  }
  if (input.planner_scene_projection != nullptr) {
    runtime_input.planning_scene_synchronizer =
        std::make_shared<ManagerPlanningSceneSynchronizer>(
            input.manager_run_root, input.bundle->retained_bundle_id(),
            input.bundle->value().cell_graph.graph_revision,
            input.publication_generation, publisher);
  }
  // Q63-Q69: this component's generated controller robot_description and
  // controllers file, written to a per-activation, per-component directory
  // of the host run root before the lane is composed; their hashes and the
  // exact launch contract are activation evidence.
  // Q78/§7.5: the mock lane's controller launch is then started (or kept,
  // Q21) by this Manager-owned host before the lane's backend binds, so CCLC
  // finds the arm controller loaded inactive and activates it as before.
  std::shared_ptr<ControllerLaunchProcess> controller_process;
  const auto discard_launch = [](ControllerLaunchSupervisor::Ensured *launch) {
    if (launch != nullptr) {
      static_cast<void>(
          host_controller_launch_supervisor().discard_candidate(*launch));
      delete launch;
    }
  };
  std::unique_ptr<ControllerLaunchSupervisor::Ensured, decltype(discard_launch)>
      prepared_launch(nullptr, discard_launch);
  std::map<std::string, double, std::less<>> mock_cancel_by_source_joint;
  bool mock_lane = false;
  {
    const auto &motion_value = direct_control_motion_binding.value().value();
    const auto *selected_profile =
        deployment::ros2_control_execution_runtime_contract::
            find_controller_profile(retained_runtime_configuration,
                                    motion_value.controllable_component_id);
    Require(selected_profile != nullptr,
            "ROS component has no controller profile in its runtime unit");
    Require(motion_value.model_groups.size() == 1U,
            "ROS component has no sole model group");
    const auto &model_instance_id =
        motion_value.model_groups.front().model_instance_id;
    const auto &model_instances =
        input.bundle->value().cell_graph.model_instances;
    const auto model_instance = std::find_if(
        model_instances.begin(), model_instances.end(),
        [&](const auto &candidate) {
          return candidate.model_instance_id == model_instance_id;
        });
    Require(model_instance != model_instances.end(),
            "ROS component model instance is absent from the retained graph");
    const auto &source_urdfs = retained_model->model_source_urdfs();
    const auto source_urdf = std::find_if(
        source_urdfs.begin(), source_urdfs.end(), [&](const auto &candidate) {
          return candidate.model_asset_id == model_instance->model_asset_id &&
                 candidate.model_revision == model_instance->model_revision;
        });
    Require(source_urdf != source_urdfs.end(),
            "ROS component model source URDF is absent from the retained "
            "planning closure");
    const auto path_token = [](std::string text) {
      for (auto &character : text) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '-' || character == '_' ||
                          character == '.';
        if (!safe) character = '_';
      }
      return text;
    };
    ComponentControllerLaunchInput artifact_input;
    artifact_input.artifacts_root =
        input.execution_run_root / "controller-launch-artifacts" /
        path_token(input.bundle->retained_bundle_id().value()) /
        path_token(input.runtime_epoch.value());
    artifact_input.gripper_binding = direct_control_gripper_binding;
    artifact_input.isaac_profile = selected_profile->isaac_profile;
    artifact_input.mock_profile = selected_profile->mock_profile;
    if (selected_profile->isaac_profile.has_value()) {
      artifact_input.isaac_lane =
          retained_isaac_controller_lane(*selected_profile->isaac_profile);
    } else {
      artifact_input.initial_joint_positions_rad =
          selected_initial_joint_positions;
      const deployment::FilesystemVerifiedRetainedProductArtifactSource
          stop_artifacts(input.package_root);
      artifact_input.controlled_stop_max_deceleration_rad_s2 =
          deployment::retained_controlled_stop_max_deceleration_rad_s2(
              *input.bundle, stop_artifacts,
              direct_control_motion_binding.value());
      if (direct_control_gripper_binding.has_value() &&
          direct_control_gripper_binding->value()
              .resources.front()
              .source_joint_name.has_value()) {
        // As `main`'s native lane seeds its gripper (retained model
        // projection of the gripper binding's source joint).
        const auto &gripper_path =
            *direct_control_gripper_binding->value().control_path;
        const auto projected = state_projector->current_state(
            gripper_path.control_binding_id,
            gripper_path.control_binding_revision);
        Require(projected.ok() && projected.value().positions.size() == 1U,
                "ROS mock gripper has no retained model projection");
        const auto &position = projected.value().positions.front();
        artifact_input.gripper_projected_position =
            std::holds_alternative<core::AngularPositionDTO>(position)
                ? std::get<core::AngularPositionDTO>(position).value_rad
                : std::get<core::LinearPositionDTO>(position).value_m;
      }
    }
    if (direct_control_gripper_binding) {
      const auto row = std::find_if(retained_runtime_configuration.gripper_target_configurations.begin(),
          retained_runtime_configuration.gripper_target_configurations.end(), [&](const auto& configured) {
            return configured.controllable_component_id == direct_control_gripper_binding->value().controllable_component_id &&
                   configured.command_target_id == direct_control_gripper_binding->value().command_target_id;
          });
      Require(row != retained_runtime_configuration.gripper_target_configurations.end(),
              "ROS controller artifacts have no exact authored gripper output choice");
      artifact_input.gripper_output_mode = row->output_mode;
    }
    artifact_input.model_urdf = source_urdf->urdf_bytes;
    artifact_input.frame_prefix = selected_profile->frame_prefix;
    const auto written =
        write_component_controller_launch_artifacts(
            direct_control_motion_binding.value(), artifact_input);
    const auto evidence = controller_launch_artifacts_evidence(written);
    {
      std::ofstream record(written.directory /
                               "controller_launch_artifacts.json",
                           std::ios::binary | std::ios::trunc);
      record << evidence << '\n';
      Require(static_cast<bool>(record),
              "ROS controller launch evidence could not be written");
    }
    std::cerr << "manyforge controller launch artifacts: " << evidence
              << std::endl;
    if (written.lane == ControllerLaunchLane::kMock) {
      mock_lane = true;
      // Q48: the same per-joint cancel bounds the generated controllers file
      // carries, keyed by the binding's own resource-to-source-joint map.
      for (const auto &[resource_id, limit] :
           artifact_input.controlled_stop_max_deceleration_rad_s2) {
        const auto &resources = motion_value.resources;
        const auto resource = std::find_if(
            resources.begin(), resources.end(), [&](const auto &candidate) {
              return candidate.resource_id == resource_id;
            });
        Require(resource != resources.end() &&
                    resource->source_joint_name.has_value(),
                "ROS mock Stop limit names a resource outside its arm");
        mock_cancel_by_source_joint.emplace(*resource->source_joint_name,
                                            limit);
      }
      const auto command = mock_controller_launch_command(written);
      const auto ensured = host_controller_launch_supervisor().ensure(
          command, written.directory,
          input.controller_request_context.empty()
              ? "Bootstrap runtime=" +
                    (input.runtime_unit_id ? input.runtime_unit_id->value()
                                           : std::string("unmanaged")) +
                    " epoch=" + input.runtime_epoch.value()
              : input.controller_request_context);
      prepared_launch.reset(new ControllerLaunchSupervisor::Ensured(ensured));
      controller_process = ensured.process;
      std::cerr << "manyforge controller launch: component="
                << written.controllable_component_id << " decision="
                << (ensured.decision == ControllerLaunchDecision::kKeep
                        ? "kept"
                        : ensured.decision == ControllerLaunchDecision::kStart
                              ? "started"
                              : "restarted")
                << " pid=" << ensured.process->pid() << std::endl;
    } else if (input.managed_isaac_launch_inputs) {
      const auto &cell_inputs = *input.managed_isaac_launch_inputs;
      Require(cell_inputs.manager_cell_world_owned,
              "managed Isaac component requires completed cell-world preflight");
      Require(artifact_input.isaac_lane.has_value(),
              "managed Isaac component has no retained controller lane");
      const auto command = isaac_controller_launch_command(
          written, *artifact_input.isaac_lane, cell_inputs);
      const auto ensured = host_controller_launch_supervisor().ensure(
          command, written.directory,
          input.controller_request_context.empty()
              ? "Bootstrap runtime=" +
                    (input.runtime_unit_id ? input.runtime_unit_id->value()
                                           : std::string("unmanaged")) +
                    " epoch=" + input.runtime_epoch.value()
              : input.controller_request_context);
      prepared_launch.reset(new ControllerLaunchSupervisor::Ensured(ensured));
      controller_process = ensured.process;
      std::cerr << "manyforge controller launch: component="
                << written.controllable_component_id << " decision="
                << (ensured.decision == ControllerLaunchDecision::kKeep
                        ? "kept"
                        : ensured.decision == ControllerLaunchDecision::kStart
                              ? "started"
                              : "restarted")
                << " pid=" << ensured.process->pid() << std::endl;
    } else {
      // Unmanaged developer launch remains external. Managed construction
      // receives the cell owner's qualified inputs rather than recreating
      // its barrier/observer inside each robot launch.
      std::cerr << "manyforge controller launch: component="
                << written.controllable_component_id
                << " Isaac launch requires qualified cell-world inputs"
                << std::endl;
    }
  }
  auto ros2_control_runtime_handle =
      std::make_shared<RetainedRos2ControlRuntimeHandle>();
  if (controller_process != nullptr) {
    ros2_control_runtime_handle->InstallControllerProcessFaultSource(
        ControllerLaunchSupervisor::fault_source(
            controller_process, CachedControllerLaunchReadinessSource(
                                    ros2_control_runtime_handle)),
        [controller_process] {
          return core::Result<std::optional<std::string>>::Ok(
              ProjectControllerLaunchFault(
                  controller_process->ReadCachedFaultState()));
        });
  }
  auto ros2_control_backend_config =
      mock_lane
          ? make_retained_mock_ros2_control_backend_config(
                retained_runtime_configuration,
                direct_control_motion_binding.value(),
                direct_control_gripper_binding, mock_cancel_by_source_joint)
          : make_retained_isaac_ros2_control_backend_config(
                retained_runtime_configuration,
                direct_control_motion_binding.value(),
                direct_control_gripper_binding);
  // The addressed wrapper historically creates its own time capabilities.
  // A subordinate runtime retains the exact supplied pair, or creates both.
  const auto measured_receiver_clock = !input.attach_addressed_owner &&
                                               input.shared_clock != nullptr
                                           ? input.shared_clock
                                           : std::make_shared<WallClock>();
  ros2_control_backend_config.measured_receiver_clock = measured_receiver_clock;
  const auto measured_lane =
      ros2_control_backend_config.arm.measured_joint_state_lane;
  const bool pneumatic_gripper = ros2_control_backend_config.gripper &&
      ros2_control_backend_config.gripper->actuation_kind == core::GripperActuationKind::kPneumatic;
  runtime_input.prepare_measured_sources =
      [ros2_control_runtime_handle, measured_receiver_clock, measured_lane, pneumatic_gripper](
          const core::RuntimeParticipantBinding &motion,
          const std::optional<core::RuntimeParticipantBinding> &gripper,
          const std::vector<core::ActiveControlConfigurationDTO> &configurations)
      -> core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>> {
    using Result =
        core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>;
    if (!measured_lane) {
      // Build-owned absence is unavailable measurement, not an additional
      // commanded-route or configuration Apply admission condition.
      return Result::Ok(nullptr);
    }
    std::vector<core::MeasuredStateSourceDescriptor> descriptors;
    bool source_configuration_absent = false;
    const auto add_binding = [&](const core::RuntimeParticipantBinding &binding)
        -> core::Result<void> {
      const auto &value = binding.value();
      // Component-local resources do not represent model geometry. The
      // independently complete inventory still covers all model-backed rows.
      if (std::all_of(value.resources.begin(), value.resources.end(),
                      [](const auto &row) { return !row.source_joint_name; }))
        return core::Result<void>::Ok();
      if (!value.control_path)
        return core::Result<void>::Err(
            {"measured_source_unavailable", "physical control path is absent",
             {{"reason", "measured_control_path_missing"}}});
      const auto configuration = std::find_if(
          configurations.begin(), configurations.end(), [&](const auto &row) {
            return row.control_domain_id == value.control_path->control_domain_id;
          });
      if (configuration == configurations.end()) {
        // Actual absence is not a foreign/malformed association. The
        // otherwise valid selected route still publishes without qualifying
        // measurement while another required domain is not yet current.
        source_configuration_absent = true;
        return core::Result<void>::Ok();
      }
      auto descriptor = make_retained_ros2_control_measured_state_descriptor(
          binding, *configuration, *measured_lane);
      if (!descriptor.ok()) return core::Result<void>::Err(descriptor.error());
      descriptors.push_back(std::move(descriptor.value()));
      return core::Result<void>::Ok();
    };
    auto added = add_binding(motion);
    if (!added.ok()) return Result::Err(added.error());
    if (gripper && !pneumatic_gripper) {
      added = add_binding(*gripper);
      if (!added.ok()) return Result::Err(added.error());
    }
    if (source_configuration_absent) return Result::Ok(nullptr);
    auto staged = ros2_control_runtime_handle->StageMeasuredStateSources(
        descriptors, measured_receiver_clock, motion, gripper);
    if (!staged.ok()) return Result::Err(staged.error());
    if (!staged.value()) return Result::Ok(nullptr);
    if (staged.value()->descriptors().size() != staged.value()->readers().size())
      return Result::Err(
          {"measured_source_unavailable", "prepared measured source rows differ",
           {{"reason", "measured_prepared_scope_mismatch"}}});
    core::RobotMeasuredStateSourceView sources{motion.value().controllable_component_id, {}};
    for (std::size_t row = 0; row < staged.value()->descriptors().size(); ++row)
      sources.sources.push_back(
          {staged.value()->descriptors()[row], staged.value()->readers()[row]});
    auto publication = std::move(staged.value());
    return Result::Ok(std::make_shared<const RetainedPreparedRobotMeasuredSources>(
        RetainedPreparedRobotMeasuredSources{
            std::move(sources),
            [publication] { publication->EnablePublication(); }}));
  };
  ros2_control_backend_config.defer_ros_start = input.managed_component_delta.has_value();
  runtime_input.motion_state_channel_config =
      retained_ros2_control_motion_state_config(ros2_control_backend_config);
  if (direct_control_gripper_binding.has_value()) {
    runtime_input.gripper_state_channel_config =
        retained_ros2_control_gripper_state_config(
            *direct_control_gripper_binding);
  }
  runtime_input.backend_factory = make_retained_ros2_control_backend_factory(
      std::move(ros2_control_backend_config), ros2_control_runtime_handle);
  auto kernel_config = core::ExecutionKernelConfig{};
  kernel_config.collision_monitor_period_ns = collision_monitor_period_ns;
  const auto milliseconds_to_ns = [](const double value) {
    return static_cast<core::Duration>(std::llround(value * 1'000'000.0));
  };
  if (retained_runtime_configuration.runtime_collision_immediate_query_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_immediate_query_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_immediate_query_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_query_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_lookahead_query_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_lookahead_query_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_cycle_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_lookahead_cycle_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_lookahead_cycle_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_s
          .has_value()) {
    kernel_config.runtime_collision_lookahead_horizon_ns =
        static_cast<core::Duration>(std::llround(
            *retained_runtime_configuration.runtime_collision_lookahead_s *
            1'000'000'000.0));
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_max_samples
          .has_value()) {
    kernel_config.runtime_collision_lookahead_max_samples =
        *retained_runtime_configuration.runtime_collision_lookahead_max_samples;
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_workers
          .has_value()) {
    kernel_config.runtime_collision_lookahead_workers = static_cast<int>(
        *retained_runtime_configuration.runtime_collision_lookahead_workers);
    kernel_config.runtime_collision_lookahead_workers_declared = true;
  }
  if (retained_runtime_configuration.runtime_dynamic_body_max_age_cap_ms
          .has_value()) {
    kernel_config.runtime_dynamic_body_max_age_cap_ns = milliseconds_to_ns(
        *retained_runtime_configuration.runtime_dynamic_body_max_age_cap_ms);
  }
  if (retained_runtime_configuration.validation_collision_workers.has_value()) {
    kernel_config.validation_collision_workers = static_cast<int>(
        *retained_runtime_configuration.validation_collision_workers);
    kernel_config.validation_collision_workers_declared = true;
  }
  RetainedNativeRuntimeConfig runtime_config{
      input.owner_epoch,
      input.cell_id,
      input.execution_state_root / std::string(core::kOperationJournalFilename),
      1024U,
      core::CacheConfig{},
      std::move(kernel_config),
      SimpleSchedulerConfig::from_environment(),
      execution_tick_period_ns,
      std::move(selected_initial_joint_positions),
      kMaintainedNativeStopSurveillancePolicy,
      {},
      {},
      {},
      {}};
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
  const std::vector<core::RequiredManagerLivenessReceiverDTO> fixture_receivers;
  runtime_config.manager_liveness_required_set =
      core::ManagerLivenessRequiredSetDTO{
          input.cell_id,
          input.bundle->retained_bundle_id(),
          core::ComputeRequiredRuntimeSetDigest(fixture_receivers),
          core::RuntimeUnitId("standalone-admission-fixture.manager"),
          core::RuntimeEpoch("standalone-admission-fixture.manager-epoch"),
          core::RequestOwnerEpoch("standalone-admission-fixture.manager-owner"),
          fixture_receivers};
#endif
  const auto manager_apply_journal =
      input.manager_state_root /
      std::string(component_manager::kManagerApplyJournalFilename);
  runtime_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          manager_apply_journal, input.logical_scene_database,
          input.program_cycle_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  runtime_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          manager_apply_journal, input.program_cycle_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  auto receiver_scheduler = !input.attach_addressed_owner &&
                                    input.shared_scheduler != nullptr
                                ? input.shared_scheduler
                                : std::make_shared<SimpleScheduler>(runtime_config.scheduler);
  auto runtime = input.attach_addressed_owner
                     ? create_retained_native_runtime(
                           std::move(runtime_input), std::move(runtime_config),
                           measured_receiver_clock, std::move(receiver_scheduler))
                     : create_retained_native_runtime_participants(
                           std::move(runtime_input), std::move(runtime_config),
                           measured_receiver_clock, std::move(receiver_scheduler));
  auto direct_scene_frame_resolver =
      std::dynamic_pointer_cast<RetainedSceneFrameResolver>(
          runtime->reference_resolver);
  Require(direct_scene_frame_resolver != nullptr &&
              direct_scene_frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "native physical composition did not retain its exact direct frame "
          "resolver");
  if (input.attach_addressed_owner &&
      input.operation_admission_gate != nullptr) {
    input.operation_admission_gate->BindResourceTable(
        runtime->addressed_kernel->resource_admission_table());
  }
  auto direct_control_joint_limits = runtime->kinematic_model->joint_limits(
      direct_control_motion_binding.value()
          .value()
          .controllable_component_id.value(),
      direct_control_motion_binding.value().value().command_target_id.value());
  if (!direct_control_joint_limits.ok()) {
    Fail("direct-control joint limits are unavailable: " +
         direct_control_joint_limits.error().message);
  }
  auto manual_control_descriptors = BuildManualControlDescriptors(
      direct_control_motion_binding.value(), direct_control_gripper_binding,
      direct_control_joint_limits.value(), runtime->gripper_position_limits,
      runtime->motion_participant_policy,
      runtime->deployment_maximum_tcp_linear_speed_mps,
      nullptr, {}, retained_runtime_configuration.gripper_target_configurations,
      input.model_map, retained_model_documents);
  const auto selected_component_id =
      input.selected_component_id.value_or(
          direct_control_motion_binding.value()
              .value()
              .controllable_component_id);
  auto composition = std::make_unique<Composition>(
      selected_component_id,
      std::move(input.bundle), std::move(input.status_program),
      std::move(input.status_program_binding),
      std::move(direct_control_binding),
      std::move(direct_control_gripper_program_binding),
      std::move(direct_control_motion_binding.value()),
      std::move(direct_control_gripper_binding),
      std::move(direct_control_joint_limits.value()),
      std::move(manual_control_descriptors.motion),
      std::move(manual_control_descriptors.gripper),
      std::make_shared<const RetainedPlanningModelStage>(std::move(stage)),
      std::move(runtime), std::move(input.projection_revision),
      input.publication_generation,
      std::move(input.status_motion_target_role_id),
      std::move(input.status_gripper_target_role_id));
  composition->ros2_control_runtime_handle =
      std::move(ros2_control_runtime_handle);
  composition->measured_joint_state_lane = measured_lane;
  composition->controller_process = std::move(controller_process);
  composition->controller_request_context = input.controller_request_context;
  if (prepared_launch) {
    composition->prepared_controller_launch = *prepared_launch;
    delete prepared_launch.release();
  }
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->required_projection_pins = std::move(input.required_projection_pins);
  composition->scene_frame_resolver = std::move(direct_scene_frame_resolver);
  composition->target_resolver = std::move(target_resolver);
  if (composition->planner_scene_projection != nullptr) {
    composition->scene_authority_proxy =
        std::make_shared<ManagerSceneAuthorityProxy>(
            std::move(input.manager_run_root));
  }
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  auto fixture = process_fixture::MakeFocusedPickPlacePlanningFixture();
  composition->pick_place_planning_fixture_lifetime =
      std::move(fixture.lifetime);
  composition->pick_place_planning_owner = std::move(fixture.owner);
#else
  if (input.attach_addressed_owner) {
    AttachProductPickPlacePlanningOwner(*composition);
  }
#endif
  return composition;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildProductComposition(CompositionBuildInput input) {
  if (input.managed_component_delta.has_value()) {
    Require(input.bundle != nullptr && input.managed_component_delta->valid(),
            "managed composition has an invalid component delta or bundle");
    std::vector<core::ControllableComponentId> selected_bundle_components;
    selected_bundle_components.reserve(
        input.bundle->value().cell_graph.controllable_components.size());
    for (const auto &component :
         input.bundle->value().cell_graph.controllable_components) {
      selected_bundle_components.push_back(
          component.controllable_component_id);
    }
    std::sort(selected_bundle_components.begin(),
              selected_bundle_components.end());

    std::vector<core::ControllableComponentId> delta_candidate_components;
    for (const auto &change : input.managed_component_delta->components) {
      if (change.disposition == deployment::ApplyComponentDisposition::kRemove)
        continue;
      delta_candidate_components.push_back(change.controllable_component_id);
    }
    Require(delta_candidate_components == selected_bundle_components,
            "managed component delta rows differ from the selected bundle");
  }

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  const auto sorted_component_ids = [&](const auto &bindings,
                                        const std::string_view capability_id) {
    std::vector<core::ControllableComponentId> component_ids;
    component_ids.reserve(bindings.size());
    for (const auto &binding : bindings) {
      const auto target = deployment::resolve_runtime_program_target(
          *binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId(std::string(capability_id)), *input.bundle,
          input.model_map);
      component_ids.push_back(target.component().controllable_component_id);
    }
    std::sort(component_ids.begin(), component_ids.end());
    Require(std::adjacent_find(component_ids.begin(), component_ids.end()) ==
                component_ids.end(),
            "native direct-control inventory repeats a capability for one "
            "component");
    return component_ids;
  };
  const auto motion_component_ids = sorted_component_ids(
      inventory.motion, "execution.trajectory.execute");
  const auto gripper_component_ids =
      sorted_component_ids(inventory.gripper, "tool.gripper.command");
  Require(!motion_component_ids.empty() || !gripper_component_ids.empty(),
          "ROS composition requires at least one physical capability binding");
  // A scalar-only cell has no motion Scene projection. Its frames still use
  // the complete immutable retained model, with the same source/asset checks.
  if (motion_component_ids.empty() && input.collision_shared_model == nullptr) {
    auto selected = input;
    selected.selected_component_id = gripper_component_ids.front();
    const auto physical = ResolveNativePhysicalBindings(selected, inventory);
    Require(!physical.motion && physical.gripper &&
                physical.gripper->value().model_groups.size() == 1U &&
                physical.gripper->value().retained_bundle_id ==
                    input.bundle->retained_bundle_id() &&
                physical.gripper->value().graph_revision ==
                    input.bundle->value().cell_graph.graph_revision &&
                input.model_map.query().retained_bundle_id ==
                    input.bundle->retained_bundle_id(),
            "scalar cell geometry has no exact retained target and model group");
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto model = source.get_verified(
        {input.bundle->retained_bundle_id(),
         input.bundle->value().compiled_products.planning_collision_model.product_id});
    Require(model.has_value(),
            "scalar cell geometry has no verified retained model");
    auto stage = std::make_shared<const RetainedPlanningModelStage>(
        RetainedPlanningModelStage::create_verified(*model, input.execution_run_root));
    auto collision_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_ros_scalar_cell", stage->loader_urdf_path(),
        stage->srdf_path(), stage->package_paths(),
        physical.gripper->value().model_groups.front().compiled_group_name,
        input.scene_asset_root_fd);
    // Keep the loader's private asset tree alive with this existing shared
    // model. The pair retires the model before its stage; no projection or
    // state authority is created, and retained component scene stages stay put.
    const auto *model_pointer = collision_model.get();
    auto lifetime = std::make_shared<const std::pair<
        std::shared_ptr<const RetainedPlanningModelStage>,
        collision::ManyForgeCollisionEngine::SharedModelPtr>>(
            std::move(stage), std::move(collision_model));
    input.collision_shared_model =
        collision::ManyForgeCollisionEngine::SharedModelPtr(
            std::move(lifetime), model_pointer);
  }
  if (motion_component_ids.empty() && gripper_component_ids.size() == 1U) {
    input.selected_component_id = gripper_component_ids.front();
    if (input.retained_component_snapshots.contains(gripper_component_ids.front()))
      return BuildRetainedProductComposition(std::move(input));
    return BuildSingleProductComposition(std::move(input));
  }
  if (inventory.motion.size() == 1U && inventory.gripper.size() <= 1U &&
      std::includes(motion_component_ids.begin(), motion_component_ids.end(),
                    gripper_component_ids.begin(), gripper_component_ids.end())) {
    input.selected_component_id = motion_component_ids.front();
    if (input.retained_component_snapshots.contains(
            motion_component_ids.front()))
      return BuildRetainedProductComposition(std::move(input));
    return BuildSingleProductComposition(std::move(input));
  }
  Require(
      input.runtime_unit_id.has_value() && !input.attach_addressed_owner &&
          (!input.component_planner_scene_projections.empty() ||
           motion_component_ids.empty()) &&
          input.shared_clock != nullptr && input.shared_scheduler != nullptr &&
          input.status_program == nullptr &&
          input.status_program_binding == nullptr,
      "multi-robot native composition requires the managed shared-owner path");

  auto component_ids = motion_component_ids;
  component_ids.insert(component_ids.end(), gripper_component_ids.begin(), gripper_component_ids.end());
  std::sort(component_ids.begin(), component_ids.end());
  component_ids.erase(std::unique(component_ids.begin(), component_ids.end()), component_ids.end());
  // Preserve the existing motion root when present; independent scalar
  // components add owners without changing any arm's local runtime.
  if (!motion_component_ids.empty()) {
    const auto first_motion = std::find(component_ids.begin(), component_ids.end(), motion_component_ids.front());
    std::rotate(component_ids.begin(), first_motion, std::next(first_motion));
  }

  std::vector<std::unique_ptr<Composition>> compositions;
  compositions.reserve(component_ids.size());
  if (input.observation_wake == nullptr)
    input.observation_wake = std::make_shared<RetainedObservationWake>();
  for (const auto &component_id : component_ids) {
    auto selected = input;
    selected.selected_component_id = component_id;
    const auto projection =
        selected.component_planner_scene_projections.find(component_id);
    if (projection != selected.component_planner_scene_projections.end()) {
      Require(projection->second != nullptr,
              "native component has a null Scene projection");
      selected.collision_publisher = projection->second;
      selected.collision_shared_model =
          projection->second->collision_shared_model();
      selected.planner_scene_projection = projection->second;
    } else {
      Require(!std::binary_search(motion_component_ids.begin(),
                                  motion_component_ids.end(), component_id) &&
                  selected.collision_shared_model != nullptr,
              "native motion component has no Scene projection");
      selected.collision_publisher.reset();
      selected.planner_scene_projection.reset();
    }
    const auto retain_component = [&](auto &aliases) {
      std::erase_if(aliases, [&](const auto &binding) {
        return binding.value().controllable_component_id != component_id;
      });
    };
    retain_component(selected.motion_program_aliases);
    retain_component(selected.gripper_program_aliases);
    if (selected.retained_component_snapshots.contains(component_id))
      compositions.push_back(
          BuildRetainedProductComposition(std::move(selected)));
    else
      compositions.push_back(
          BuildSingleProductComposition(std::move(selected)));
  }

  std::vector<core::ProgramBindingId> intrinsic_direct_bindings;
  intrinsic_direct_bindings.reserve(inventory.motion.size() +
                                    inventory.gripper.size());
  for (const auto &binding : inventory.motion)
    intrinsic_direct_bindings.push_back(binding->program_binding_id());
  for (const auto &binding : inventory.gripper)
    intrinsic_direct_bindings.push_back(binding->program_binding_id());
  std::sort(intrinsic_direct_bindings.begin(), intrinsic_direct_bindings.end());
  intrinsic_direct_bindings.erase(std::unique(intrinsic_direct_bindings.begin(),
                                              intrinsic_direct_bindings.end()),
                                  intrinsic_direct_bindings.end());
  const auto make_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       intrinsic_direct_bindings = std::move(intrinsic_direct_bindings)](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            intrinsic_direct_bindings, program_artifact_root);
      };
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      [make_resolver](std::vector<core::RuntimeParticipantBinding> bindings) {
        auto resolver = make_resolver(std::move(bindings));
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t participant_index) {
              return resolver->ResolveExecutionParticipant(operation,
                                                           participant_index);
            }};
      };

  auto primary = std::move(compositions.front());
  compositions.erase(compositions.begin());
  const bool has_retained_owner =
      !input.retained_component_snapshots.empty();
  if (!primary->runtime)
    primary->candidate_participant_resolver_factory = resolver_factory;
  if (!has_retained_owner) {
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers;
    peers.reserve(compositions.size());
    for (const auto &composition : compositions) {
      if (composition->runtime != nullptr)
        peers.push_back(composition->runtime);
    }
    if (primary->runtime != nullptr)
      join_retained_native_runtime_participants(primary->runtime,
                                                std::move(peers),
                                                resolver_factory);
  } else {
    primary->candidate_participant_resolver_factory = resolver_factory;
  }
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  physical_bindings.reserve(component_ids.size() * 2U);
  if (primary->direct_control_motion_binding.has_value())
    physical_bindings.push_back(*primary->direct_control_motion_binding);
  if (primary->direct_control_gripper_binding.has_value())
    physical_bindings.push_back(*primary->direct_control_gripper_binding);
  for (const auto &composition : compositions) {
    if (composition->direct_control_motion_binding.has_value())
      physical_bindings.push_back(*composition->direct_control_motion_binding);
    if (composition->direct_control_gripper_binding.has_value())
      physical_bindings.push_back(*composition->direct_control_gripper_binding);
  }
  primary->target_resolver = make_resolver(std::move(physical_bindings));
  primary->participant_compositions = std::move(compositions);
  // Scene is a cell authority. Its component-relative frames may refer to
  // any installed robot, not just the component selected as this cell's
  // addressed-Kernel root. Keep each engine's local resolver separate, but
  // install one verified cell resolver over every live state source.
  {
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto retained_model = source.get_verified(
        deployment::RetainedPlanningModelQuery{
            input.bundle->retained_bundle_id(),
            input.bundle->value().compiled_products.planning_collision_model
                .product_id});
    Require(retained_model.has_value() && input.runtime_unit_id.has_value() &&
                input.collision_shared_model != nullptr,
            "native cell frame resolver has no candidate model or owner");
    std::vector<RetainedSceneFrameStateSourceInput> state_sources;
    const auto include_channel = [&](const auto &channel) {
      if (channel == nullptr)
        return;
      const auto *retained_channel =
          dynamic_cast<const RetainedObservedStateChannel *>(channel.get());
      Require(retained_channel != nullptr,
              "native cell frame source is not route-aware");
      const auto channel_binding = retained_channel->binding();
      const auto &resources = channel_binding.value().resources;
      if (std::none_of(resources.begin(), resources.end(),
                       [](const auto &resource) {
                         return resource.source_joint_name.has_value();
                       }))
        return;
      Require(std::all_of(resources.begin(), resources.end(),
                          [](const auto &resource) {
                            return resource.source_joint_name.has_value();
                          }),
              "native cell frame source mixes local and model coordinates");
      state_sources.push_back(SceneFrameSource(*retained_channel,
          std::shared_ptr<const core::IObservedStateProvider>(
              channel, channel->addressed_publication()),
          SceneFrameCoordinateAuthority(*input.bundle, channel_binding)));
    };
    const auto include_owner = [&](const Composition &view) {
      Require(view.component_runtime_identity() != nullptr,
              "native cell frame resolver has no component owner");
      if (view.runtime != nullptr) {
        include_channel(view.runtime->motion_state_channel);
        include_channel(view.runtime->gripper_state_channel);
      } else {
        include_channel(view.gripper_runtime->state_channel());
      }
    };
    include_owner(*primary);
    for (const auto &peer : primary->participant_compositions)
      include_owner(*peer);
    auto cell_frame_resolver = RetainedSceneFrameResolver::create_verified(
        input.collision_shared_model, *input.bundle, input.model_map,
        *retained_model, *input.runtime_unit_id, input.runtime_epoch,
        std::move(state_sources));
    Require(cell_frame_resolver != nullptr,
            "native cell frame resolver could not be built");
    // A component's Program may name a frame on another installed component.
    // Candidate views all use the same verified cell source set. A retained
    // engine is re-sealed only at fenced installation; fresh engines are
    // private and may receive the resolver now.
    const auto install_candidate_resolver = [&](Composition &view) {
      view.scene_frame_resolver = cell_frame_resolver;
      if (input.retained_component_snapshots.contains(
              view.controllable_component_id))
        return;
      if (view.gripper_runtime != nullptr)
        return;
      Require(view.runtime != nullptr &&
                  view.runtime->reference_resolver_proxy != nullptr,
              "native candidate has no frame-resolution owner");
      view.runtime->reference_resolver = cell_frame_resolver;
      view.runtime->reference_resolver_proxy->ReplaceTarget(
          cell_frame_resolver);
    };
    install_candidate_resolver(*primary);
    for (auto &peer : primary->participant_compositions)
      install_candidate_resolver(*peer);
  }
  return primary;
}

[[nodiscard]] std::vector<core::ControllableComponentId>
RetainedNativeComponentIds(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  std::vector<core::ControllableComponentId> ids;
  for (const auto &change : request.component_delta.components) {
    if (change.disposition == deployment::ApplyComponentDisposition::kRetain)
      ids.push_back(change.controllable_component_id);
  }
  Require(std::is_sorted(ids.begin(), ids.end()) &&
              std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "retained native Apply repeats a component identity");
  return ids;
}

void ResealRetainedNativeCandidateOwners(
    Composition &candidate, const Composition &predecessor,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  auto old = IndexNativeComponentCompositions(predecessor);
  auto next = IndexNativeComponentCompositions(candidate);
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto previous = old.find(component_id);
    const auto selected = next.find(component_id);
    Require(previous != old.end() && selected != next.end() &&
                previous->second->component_runtime_identity() != nullptr &&
                previous->second->component_runtime_identity() ==
                    selected->second->component_runtime_identity() &&
                selected->second->scene_frame_resolver != nullptr &&
                !selected->second->candidate_component_registrations.empty(),
            "retained native owner differs from the planned predecessor");
    auto &view = *selected->second;
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native component lost the exact parent Apply fence");
    if (view.gripper_runtime != nullptr) {
      Require(view.runtime == nullptr &&
                  !view.candidate_component_registrations.empty() &&
                  view.direct_control_gripper_binding.has_value(),
              "retained gripper candidate has no exact physical participant");
      std::vector<core::RuntimeParticipantBinding> representatives;
      std::vector<std::optional<core::RuntimeParticipantStopReadiness>> readiness;
      for (const auto& row : view.candidate_component_registrations) {
        representatives.push_back(row.binding);
        readiness.push_back(row.stop_readiness);
      }
      auto staged = view.gripper_runtime->PrepareRegistrations(representatives, readiness);
      Require(staged.ok(), staged.ok() ? std::string{} : staged.error().message);
      const auto primary = std::find_if(staged.value().begin(), staged.value().end(), [&](const auto& row) {
        return row.binding.value() == view.direct_control_gripper_binding->value();
      });
      Require(primary != staged.value().end(), "retained gripper candidate omits its exact primary command");
      const auto channel =
          std::dynamic_pointer_cast<RetainedObservedStateChannel>(
              view.gripper_runtime->state_channel());
      Require(channel != nullptr,
              "retained gripper has no rebindable observed-state owner");
      Require(predecessor.cell_runtime != nullptr &&
                  predecessor.cell_runtime->clock() != nullptr,
              "retained gripper has no stable cell clock");
      const auto rebound = channel->RebindRoute(
          *view.direct_control_gripper_binding,
          predecessor.cell_runtime->clock()->now());
      if (!rebound.ok())
        Fail("retained gripper state route cannot be re-sealed: " +
             rebound.error().message);
      const auto installed = view.gripper_runtime->RebindRoute(
          *view.direct_control_gripper_binding, channel,
          primary->stop_readiness);
      if (!installed.ok())
        Fail("retained gripper controller cannot be re-sealed: " +
             installed.error().message);
      view.gripper_runtime->AdoptVerifiedRegistrations(std::move(staged.value()));
      continue;
    }
    Require(view.runtime != nullptr &&
                view.direct_control_motion_binding.has_value(),
            "retained motion candidate has no physical owner");
    RetainedNativeRuntimeResealInput reseal{
        *view.direct_control_motion_binding,
        view.direct_control_gripper_binding,
        {},
        {},
        view.candidate_control_representatives,
        view.candidate_component_registrations,
        {},
        nullptr,
        view.scene_frame_resolver,
        request.configuration_fence_id,
        request.configuration_fence_scope,
        admission_gate};
    auto result = reseal_retained_native_runtime_for_apply(*view.runtime,
                                                           std::move(reseal));
    if (!result.ok()) {
      Fail("retained native owner cannot be re-sealed under Apply: " +
           result.error().message);
    }
  }
}

void RefreshRetainedNativeCandidateCollision(
    Composition &candidate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native collision candidate repeats an owner");
  }
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end() &&
                found->second->component_runtime_identity() != nullptr,
            "retained native collision owner is absent");
    auto &view = *found->second;
    // A standalone gripper has no planner or collision-engine resources to
    // reconfigure. Its geometry changes through the Scene projection below.
    if (view.gripper_runtime != nullptr)
      continue;
    auto &runtime = *view.runtime;
    Require(runtime.participant_engine != nullptr &&
                view.candidate_collision_publisher != nullptr &&
                view.candidate_collision_model != nullptr &&
                view.candidate_runtime_collision_engine != nullptr,
            "retained native candidate collision resources are incomplete");
    core::CollisionPipelineResourceOwners owners{
        view.candidate_collision_publisher,
        view.candidate_runtime_collision_engine,
        view.candidate_validation_collision_engines,
        view.candidate_lookahead_collision_engines};
    auto installed = runtime.participant_engine
                         ->replace_collision_resources_under_apply_fence(
                             request.configuration_fence_id,
                             request.configuration_fence_scope, admission_gate,
                             std::move(owners));
    if (!installed.ok())
      Fail("retained native collision refresh failed: " +
           installed.error().message);
    auto readback = runtime.participant_engine->collision_resource_set_readback();
    const auto candidate_policy =
        view.candidate_collision_publisher->collision_policy_snapshot();
    Require(readback.ok() && readback.value() == installed.value() &&
                readback.value().resources.snapshot_publisher ==
                    view.candidate_collision_publisher &&
                readback.value().resources.runtime_monitor_engine ==
                    view.candidate_runtime_collision_engine &&
                readback.value().resources.validation_engines ==
                    view.candidate_validation_collision_engines &&
                readback.value().resources.runtime_lookahead_engines ==
                    view.candidate_lookahead_collision_engines &&
                candidate_policy.has_value() &&
                readback.value().coordinator_policy_snapshot != nullptr &&
                *readback.value().coordinator_policy_snapshot ==
                    *candidate_policy,
            "retained native collision readback differs from the candidate");
    runtime.collision_publisher = view.candidate_collision_publisher;
    runtime.runtime_collision_engine =
        view.candidate_runtime_collision_engine;
    runtime.validation_collision_engines =
        view.candidate_validation_collision_engines;
    runtime.lookahead_collision_engines =
        view.candidate_lookahead_collision_engines;
  }
}

// A retained controller keeps its identities (controller, Kernel, planner
// client, observed sources) but every model-dependent computation follows the
// candidate's whole-cell planning model: the worker's base model, the
// in-process scene with its kinematics, validator and collision projector,
// and each model-backed state projector. The candidate composition owns the
// stage whose files the replacement worker loads. Collision resources were
// installed just before; this completes one coherent model generation and
// reads it back before admission can reopen. A failure is rethrown as the
// Apply failure; nothing is restored and the fence keeps admission closed.
void RefreshRetainedNativeCandidateModels(
    Composition &candidate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate,
    core::IClock &clock) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native model candidate repeats an owner");
  }
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end(),
            "retained native model owner is absent");
    auto &view = *found->second;
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native model refresh lost the exact parent Apply fence");
    if (view.gripper_runtime != nullptr) {
      if (view.candidate_tool_state_projector == nullptr)
        continue;
      const auto channel =
          std::dynamic_pointer_cast<RetainedObservedStateChannel>(
              view.gripper_runtime->state_channel());
      const auto *backend = view.gripper_runtime->backend();
      Require(channel != nullptr && backend != nullptr,
              "retained gripper has no model-backed observed-state owner");
      Require(backend
                      ->get_gripper_status(view.gripper_runtime->binding()
                                               .value()
                                               .command_target_id.value())
                      .state != core::GripperCommandState::kExecuting,
              "retained gripper has active command work during model refresh");
      const auto replaced = channel->ReplaceKinematicProjector(
          view.candidate_tool_state_projector, clock.now());
      if (!replaced.ok())
        Fail("retained gripper rejected the candidate cell model: " +
             replaced.error().message);
      Require(channel->kinematic_projector() ==
                  view.candidate_tool_state_projector,
              "retained gripper does not read back the candidate model");
      view.candidate_tool_state_projector.reset();
      continue;
    }
    Require(view.candidate_planner_model.has_value() ==
                (view.candidate_model_generation != nullptr),
            "retained native model candidate is incomplete");
    if (!view.candidate_planner_model.has_value())
      continue;
    Require(view.runtime != nullptr && view.runtime->planner != nullptr &&
                view.stage != nullptr &&
                view.candidate_planner_model->urdf_path ==
                    view.stage->loader_urdf_path(),
            "retained native planner has no staged candidate model");
    const auto replaced =
        view.runtime->planner->ReplaceRetainedModel(*view.candidate_planner_model);
    if (!replaced.ok())
      Fail("retained native planner rejected the candidate planning model: " +
           replaced.error().message);
    const auto refreshed = refresh_retained_native_runtime_model_for_apply(
        *view.runtime,
        RetainedNativeRuntimeModelRefreshInput{
            view.candidate_model_generation, request.configuration_fence_id,
            request.configuration_fence_scope, admission_gate});
    if (!refreshed.ok())
      Fail("retained native model consumers rejected the candidate cell "
           "model: " +
           refreshed.error().message);
    view.candidate_planner_model.reset();
    view.candidate_model_generation.reset();
  }
}

[[nodiscard]] std::unique_ptr<Composition>
BuildComposition(const HostConfig &config,
                 const core::RequestOwnerEpoch &owner_epoch,
                 const core::RuntimeEpoch &runtime_epoch,
                 ManagedKernelAdmissionGate *operation_admission_gate) {
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation.nonzero(),
          "active retained-bundle publication is absent");
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  const deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      config.package_root);
  auto bundle =
      bundle_source.get_verified(receipt.request.candidate_retained_bundle_id);
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->value().cell_graph.graph_revision ==
                  receipt.request.candidate_graph_revision &&
              bundle->retained_bundle_id() ==
                  receipt.request.candidate_projection_revision
                      .retained_bundle_id &&
              bundle->value().cell_graph.graph_revision ==
                  receipt.request.candidate_projection_revision.graph_revision,
          "active publication and retained bundle disagree");
  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "N3 native host requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      config.package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "active retained model map is absent or invalid");

  const deployment::FilesystemVerifiedProgramBindingSource binding_repository(
      config.package_root);
  auto program_binding = binding_repository.get_verified(
      core::ProgramBindingId(config.program_binding_id), *bundle, *model_map);
  Require(program_binding != nullptr,
          "exact retained Program Binding is absent or invalid");
  const deployment::FilesystemVerifiedProgramSource program_repository(
      config.package_root);
  auto program = program_repository.get_verified(
      program_binding->value().program_id,
      program_binding->value().program_revision, *bundle);
  Require(program != nullptr,
          "Program and exact retained Program Binding disagree");

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      config.package_root);
  auto direct_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *bundle, *model_map, catalog_store);
  if (direct_inventory.motion.empty()) {
    Require(direct_inventory.gripper.size() == 1U &&
                config.motion_target_role_id.empty() &&
                !config.gripper_target_role_id.empty(),
            "native no-motion composition requires one explicitly selected gripper target");
    // Let the component inventory choose the gripper-only owner. It creates
    // no motion engine or planner and is attached to the stable cell Kernel by
    // the same managed bootstrap path as every other component.
    return BuildProductComposition(CompositionBuildInput{
        config.package_root,
        config.execution_state_root,
        config.execution_run_root,
        config.candidate_worker,
        core::CellId(config.cell_id),
        owner_epoch,
        std::nullopt,
        runtime_epoch,
        std::move(bundle),
        std::move(*model_map),
        receipt.request.candidate_projection_revision,
        receipt.publication_generation,
        {},
        {},
        {},
        {},
        operation_admission_gate,
        std::move(program),
        std::move(program_binding),
        std::nullopt,
        core::TargetRoleId(config.gripper_target_role_id),
        true,
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
    });
  }
  Require(direct_inventory.motion.size() == 1U &&
              direct_inventory.gripper.size() <= 1U,
          "native Program aliases have no sole physical motion target or have "
          "ambiguous gripper targets");
  const auto physical_motion = deployment::resolve_runtime_program_target(
      *direct_inventory.motion.front(),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  std::optional<deployment::RuntimeProgramTargetBinding> physical_gripper;
  if (!direct_inventory.gripper.empty()) {
    physical_gripper = deployment::resolve_runtime_program_target(
        *direct_inventory.gripper.front(),
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
  }
  Require(!config.motion_target_role_id.empty(),
          "native Program has no selected motion target role");
  std::vector<deployment::RuntimeProgramTargetBinding> physical_support;
  const auto &selected_target = program_binding->value().target_role_bindings;
  const auto selected_motion = deployment::resolve_runtime_program_target(
      *program_binding, core::TargetRoleId(config.motion_target_role_id),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  const auto selected_motion_role = std::find_if(
      selected_target.begin(), selected_target.end(), [&](const auto &role) {
        return role.target_role_id ==
               core::TargetRoleId(config.motion_target_role_id);
      });
  Require(selected_motion_role != selected_target.end(),
          "native Program motion target role is absent");
  for (const auto &capability : selected_motion_role->capability_bindings) {
    if (capability.capability_id.value().starts_with("planning.motion.")) {
      physical_support.push_back(deployment::resolve_runtime_program_target(
          *program_binding, core::TargetRoleId(config.motion_target_role_id),
          capability.capability_id, *bundle, *model_map));
    }
  }
  const auto motion_alias = core::verify_runtime_participant_binding(
      deployment::build_runtime_participant_binding_dto(
          selected_motion,
          ActiveFacts(
              selected_motion,
              std::span<const deployment::RuntimeProgramTargetBinding>(
                  physical_support),
              runtime_epoch,
              core::ActiveControlConfigurationId("active.i6.native.arm")),
          std::span<const deployment::RuntimeProgramTargetBinding>(
              physical_support)));
  Require(motion_alias.ok(), "native Program motion alias failed verification");

  std::vector<core::RuntimeParticipantBinding> motion_aliases;
  motion_aliases.push_back(motion_alias.value());
  std::vector<core::RuntimeParticipantBinding> gripper_aliases;
  const bool selected_program_has_gripper = std::any_of(
      selected_target.begin(), selected_target.end(), [](const auto &role) {
        return std::any_of(role.capability_bindings.begin(),
                           role.capability_bindings.end(), [](const auto &cap) {
                             return cap.capability_id.value() ==
                                    "tool.gripper.command";
                           });
      });
  if (selected_program_has_gripper) {
    Require(!config.gripper_target_role_id.empty() &&
                physical_gripper.has_value(),
            "native Program gripper target is not physically available");
    const auto selected_gripper = deployment::resolve_runtime_program_target(
        *program_binding, core::TargetRoleId(config.gripper_target_role_id),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
    Require(selected_gripper.component().controllable_component_id ==
                    physical_gripper->component().controllable_component_id &&
                selected_gripper.target_role().command_target_id ==
                    physical_gripper->target_role().command_target_id,
            "native Program gripper alias does not match its physical target");
    const auto gripper_alias = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            selected_gripper, ActiveFacts(selected_gripper, {}, runtime_epoch,
                                          core::ActiveControlConfigurationId(
                                              "active.i6.native.gripper"))));
    Require(gripper_alias.ok(),
            "native Program gripper alias failed verification");
    gripper_aliases.push_back(gripper_alias.value());
  }
  Require(physical_motion.component().controllable_component_id ==
                  selected_motion.component().controllable_component_id &&
              physical_motion.target_role().command_target_id ==
                  selected_motion.target_role().command_target_id &&
              (!physical_gripper.has_value() ||
               physical_gripper->component().controllable_component_id ==
                   physical_motion.component().controllable_component_id) &&
              (!selected_program_has_gripper ||
               (physical_gripper.has_value() &&
                physical_gripper->component().controllable_component_id ==
                    selected_motion.component().controllable_component_id)),
          "native Program aliases cross their physical component");

  return BuildProductComposition(CompositionBuildInput{
      config.package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      owner_epoch,
      std::nullopt,
      runtime_epoch,
      std::move(bundle),
      std::move(*model_map),
      receipt.request.candidate_projection_revision,
      receipt.publication_generation,
      {},
      {},
      std::move(motion_aliases),
      std::move(gripper_aliases),
      operation_admission_gate,
      std::move(program),
      std::move(program_binding),
      core::TargetRoleId(config.motion_target_role_id),
      config.gripper_target_role_id.empty()
          ? std::optional<core::TargetRoleId>{}
          : std::optional<core::TargetRoleId>{
                core::TargetRoleId(config.gripper_target_role_id)},
      true,
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
  });
}


[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
PhysicalControlBindings(const Composition &composition) {
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  if (composition.runtime != nullptr) {
    physical_bindings =
        RetainedNativeComponentOwner(composition.runtime).physical_bindings();
  } else if (composition.gripper_runtime != nullptr) {
    physical_bindings = RetainedNativeComponentOwner(
                            composition.gripper_runtime)
                            .physical_bindings();
  }
  for (const auto &participant : composition.participant_compositions) {
    auto nested = PhysicalControlBindings(*participant);
    physical_bindings.insert(physical_bindings.end(),
                             std::make_move_iterator(nested.begin()),
                             std::make_move_iterator(nested.end()));
  }
  return physical_bindings;
}

[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
NativeCellComponentRegistrations(const Composition &cell) {
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  std::set<core::ControllableComponentId> owners;
  const auto append = [&](const Composition &view) {
    Require(owners.insert(view.controllable_component_id).second,
            "native cell repeats a component registration owner");
    auto rows = view.component_registrations();
    Require(!rows.empty(), "native cell component has no registrations");
    result.insert(result.end(), std::make_move_iterator(rows.begin()),
                  std::make_move_iterator(rows.end()));
  };
  append(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing component owner");
    append(*participant);
  }
  return result;
}

[[nodiscard]] core::RuntimeParticipantAdmissionResolver
NativeCellParticipantResolver(
    const Composition &cell,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations) {
  Require(cell.target_resolver != nullptr,
          "native cell has no product-backed participant resolver");
  auto resolver = cell.target_resolver;
  RetainedRuntimeParticipantResolver binding_resolver =
      [resolver](const core::OperationIntentDTO &operation,
                 const std::size_t index) {
        return resolver->ResolveExecutionParticipant(operation, index);
      };
  return make_retained_cell_kernel_participant_resolver(
      std::move(binding_resolver), registrations);
}

void AppendNativeGripperStopSources(
    const Composition &cell,
    std::vector<RetainedStopSurveillanceSourceInput> &result) {
  const auto add_gripper = [&](const Composition &view) {
    if (view.gripper_runtime == nullptr)
      return;
    auto source = build_retained_gripper_stop_surveillance_source(
        view.gripper_runtime->binding(),
        view.gripper_runtime->stop_readiness(),
        view.gripper_runtime->state_channel(),
        view.gripper_runtime->participant_engine());
    if (source.has_value() &&
        std::none_of(result.begin(), result.end(), [&](const auto &row) {
          return row.binding.value() == source->binding.value();
        }))
      result.push_back(std::move(*source));
  };
  add_gripper(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing Stop participant");
    add_gripper(*participant);
  }
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
NativeCellStopSources(const Composition &cell) {
  std::vector<RetainedStopSurveillanceSourceInput> result;
  if (cell.runtime != nullptr && cell.runtime->stop_surveillance_owner != nullptr)
    result = cell.runtime->stop_surveillance_owner->SnapshotSources();
  std::set<core::ControllableComponentId> independent_grippers;
  const auto note_gripper = [&](const Composition &view) {
    if (view.gripper_runtime != nullptr)
      independent_grippers.insert(view.controllable_component_id);
  };
  note_gripper(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing Stop participant");
    note_gripper(*participant);
  }
  std::erase_if(result, [&](const auto &row) {
    return independent_grippers.contains(
        row.binding.value().controllable_component_id);
  });
  AppendNativeGripperStopSources(cell, result);
  if (!result.empty())
    Require(RetainedStopSurveillanceOwner::ValidateSources(result).ok(),
            "native cell has invalid Stop observation sources");
  return result;
}

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
CellControlBindingRepresentatives(const Composition &composition) {
  auto representatives =
      composition.candidate_control_representatives.empty()
          ? (composition.runtime != nullptr
                 ? composition.runtime->control_binding_representatives
                 : std::vector<core::RuntimeParticipantBinding>{
                       composition.gripper_runtime->binding()})
          : composition.candidate_control_representatives;
  for (const auto &participant : composition.participant_compositions) {
    auto nested = CellControlBindingRepresentatives(*participant);
    representatives.insert(representatives.end(),
                           std::make_move_iterator(nested.begin()),
                           std::make_move_iterator(nested.end()));
  }
  return representatives;
}

[[nodiscard]] core::Result<void> VerifyNativeEngineForDomain(
    const Composition &composition, const core::ControlDomainId &domain) {
  if (composition.runtime != nullptr) {
    for (const auto &registration :
         composition.runtime->participant_registrations) {
      const auto &path = registration.binding.value().control_path;
      if (path && path->control_domain_id == domain &&
          registration.engine != nullptr &&
          registration.stop_execution_port != nullptr) {
        return core::Result<void>::Ok();
      }
    }
  }
  if (composition.gripper_runtime != nullptr) {
    const auto registration = composition.gripper_runtime->registration();
    const auto &path = registration.binding.value().control_path;
    if (path && path->control_domain_id == domain &&
        registration.engine != nullptr &&
        registration.stop_execution_port != nullptr) {
      return core::Result<void>::Ok();
    }
  }
  for (const auto &participant : composition.participant_compositions) {
    if (VerifyNativeEngineForDomain(*participant, domain).ok()) {
      return core::Result<void>::Ok();
    }
  }
  return core::Result<void>::Err(
      {"native_lifecycle_engine_unavailable",
       "the exact native participant engine is not installed for the control domain",
       {{"reason", "native_engine_or_stop_port_missing"},
        {"control_domain_id", domain.value()}}});
}

void DetachRetainedCCLC(Composition &composition);

// Both hosts adopt exactly what the Kernel reads back at the planned
// generations; a ROS plan's Declared component reads back command-closed.
[[nodiscard]] core::NativeApplyFactAdoptionDTO ReadNativeApplyFacts(
    Composition &composition,
    const core::ActiveBundlePublicationReceiptDTO &publication,
    const core::NativeApplyPlanDTO &plan) {
  Require(publication.valid() &&
              publication.publication_generation ==
                  composition.publication_generation &&
              publication.request.candidate_retained_bundle_id ==
                  composition.bundle->retained_bundle_id() &&
              plan.candidate_retained_bundle_id ==
                  composition.bundle->retained_bundle_id(),
          "native CCLC adoption does not name the installed candidate");
  core::NativeApplyFactAdoptionDTO facts{publication, {}, {}};
  const auto planned_component = [&](const core::ControllableComponentId &id) {
    const auto found = std::find_if(
        plan.components.begin(), plan.components.end(),
        [&](const auto &row) { return row.component_id == id; });
    Require(found != plan.components.end(),
            "native CCLC adoption has an unplanned component");
    return *found;
  };
  const auto representatives = CellControlBindingRepresentatives(composition);
  std::set<core::ControllableComponentId> ready_components;
  for (const auto &desired :
       composition.bundle->value().desired_control_configurations) {
    const auto engine = VerifyNativeEngineForDomain(
        composition, desired.control_domain_id);
    if (!engine.ok()) {
      Fail("native CCLC adoption has no exact engine: " +
           engine.error().message);
    }
    const auto current = composition.active_addressed_kernel()
                             ->current_runtime_route_configuration(
                                 desired.control_domain_id);
    Require(current.ok() && current.value().has_value(),
            "native CCLC adoption has no route readback");
    const auto &active = current.value()->active_configuration;
    const auto planned_route = std::find_if(
        plan.routes.begin(), plan.routes.end(), [&](const auto &row) {
          return row.control_domain_id == desired.control_domain_id;
        });
    Require(planned_route != plan.routes.end() &&
                active.active_control_configuration_generation ==
                    planned_route->generation,
            "native CCLC adoption route generation differs from the plan");
    Require(active.locally_valid() &&
                active.state ==
                    core::ActiveControlConfigurationStateDTO::kReconciled &&
                active.control_domain_id == desired.control_domain_id &&
                active.control_domain_revision ==
                    desired.control_domain_revision &&
                active.activation_set_id == desired.activation_set_id &&
                active.controller_topology_id ==
                    desired.controller_topology_id &&
                active.topology_revision == desired.topology_revision &&
                active.binding_routes.size() == desired.binding_routes.size() &&
                current.value()->readiness_rows.size() ==
                    desired.binding_routes.size(),
            "native CCLC adoption route differs from retained intent");
    for (const auto &route : desired.binding_routes) {
      Require(std::any_of(active.binding_routes.begin(),
                          active.binding_routes.end(), [&](const auto &row) {
                            return row.control_binding_id ==
                                       route.control_binding_id &&
                                   row.control_binding_revision ==
                                       route.control_binding_revision &&
                                   row.control_route_id == route.control_route_id;
                          }),
              "native CCLC adoption is missing a retained binding route");
      const auto representative = std::find_if(
          representatives.begin(), representatives.end(), [&](const auto &row) {
            return row.value().control_path.has_value() &&
                   row.value().control_path->control_domain_id ==
                       desired.control_domain_id &&
                   row.value().control_path->control_binding_id ==
                       route.control_binding_id &&
                   row.value().control_path->control_binding_revision ==
                       route.control_binding_revision;
          });
      Require(representative != representatives.end(),
              "native CCLC adoption has no physical binding representative");
      ready_components.insert(
          representative->value().controllable_component_id);
      // Route currency and component command eligibility are independent.
      // A Disabled component keeps its verified route but cannot dispatch.
      Require(current.value()->operation_admission_open,
              "native CCLC adoption route is not current");
    }
    facts.current_routes.push_back(active);
  }
  for (const auto &component :
       composition.bundle->value().cell_graph.controllable_components) {
    Require(ready_components.erase(component.controllable_component_id) == 1U,
            "native CCLC adoption has a component without verified routes");
    const auto readiness = composition.active_addressed_kernel()
                               ->component_command_readiness(
                                   component.controllable_component_id);
    Require(readiness.ok() &&
                readiness.value().component_lifecycle_generation ==
                    planned_component(component.controllable_component_id)
                        .generation &&
                readiness.value().command_eligible ==
                    (planned_component(component.controllable_component_id)
                         .state == core::ComponentLifecycleStateDTO::kActive),
            "native CCLC adoption command eligibility differs from the plan");
    facts.components.push_back(
        planned_component(component.controllable_component_id));
  }
  Require(ready_components.empty(),
          "native CCLC adoption contains an undeclared route component");
  std::sort(facts.components.begin(), facts.components.end(),
            [](const auto &a, const auto &b) {
              return a.component_id < b.component_id;
            });
  std::sort(facts.current_routes.begin(), facts.current_routes.end(),
            [](const auto &a, const auto &b) {
              return a.control_domain_id < b.control_domain_id;
            });
  return facts;
}


// No-motion route/readiness seams borrow the same stable cell Kernel and
// retain component engines directly; neither callback captures Composition.
void EnsureGripperOnlyCellPorts(Composition& composition) {
  // Arm roots derive exact scalar delegates from their current installed
  // inventory/factory; no per-Apply map or captured predecessor is stored.
  if (composition.runtime || composition.cclc_cell_route_publication_owner) return;
  Require(composition.cell_runtime && composition.bundle &&
              composition.candidate_participant_resolver_factory,
          "no-motion CCLC has no exact stable cell/product resolver factory");
  auto* const cell = composition.cell_runtime;
  const auto owners = IndexNativeComponentCompositions(composition);
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> grippers;
  for (const auto& [component, view] : owners) {
    static_cast<void>(component);
    Require(view->gripper_runtime != nullptr && !view->runtime,
            "no-motion CCLC contains a foreign physical owner");
    grippers.push_back(view->gripper_runtime);
  }
  const auto resolver_for = [factory = composition.candidate_participant_resolver_factory,
                             grippers](const auto& rows) {
    std::vector<core::RuntimeParticipantBinding> physical;
    for (const auto& gripper : grippers) {
      const auto actual = gripper->registration();
      const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& value) {
        return value.binding.value() == actual.binding.value() &&
               value.engine == actual.engine && value.stop_execution_port == actual.stop_execution_port;
      });
      if (row == rows.end())
        throw std::invalid_argument("no-motion resolver lost its exact physical owner");
      physical.push_back(row->binding);
    }
    auto binding_resolver = factory(std::move(physical));
    if (!binding_resolver)
      throw std::invalid_argument("no-motion resolver factory returned no resolver");
    return make_retained_cell_kernel_participant_resolver(std::move(binding_resolver), rows);
  };
  auto candidate = [cell, resolver_for](const auto&, const auto& replacements) {
    using Result = core::Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>;
    auto rows = cell->participant_registrations();
    for (const auto& row : replacements) {
      const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& before) {
        return before.engine == row.engine && before.binding.value().control_path && row.binding.value().control_path &&
               before.binding.value().controllable_component_id == row.binding.value().controllable_component_id &&
               before.binding.value().command_target_id == row.binding.value().command_target_id &&
               before.binding.value().capability_id == row.binding.value().capability_id &&
               before.binding.value().control_path->control_binding_id == row.binding.value().control_path->control_binding_id;
      });
      if (found == rows.end()) return Result::Err(
          {"missing_reference", "scalar candidate has no exact installed registration", {}});
      *found = row;
    }
    auto resolver = resolver_for(rows);
    return Result::Ok({std::move(rows), std::move(resolver), {}});
  };
  std::map<core::ControlDomainId, std::shared_ptr<RetainedNativeGripperRoutePublicationOwner>> routes;
  for (const auto& gripper : grippers) {
    const auto binding = gripper->binding();
    Require(binding.value().control_path.has_value(), "scalar route owner has no physical domain");
    auto port = std::make_shared<RetainedNativeGripperRoutePublicationOwner>(
        gripper, *cell->clock(), cell->addressed_kernel(), candidate, cell);
    Require(routes.emplace(binding.value().control_path->control_domain_id, port).second,
            "scalar route inventory repeats a domain owner");
  }
  Require(composition.gripper_runtime != nullptr, "no-motion CCLC has no primary scalar owner");
  const auto primary_binding = composition.gripper_runtime->binding();
  const auto primary = routes.find(primary_binding.value().control_path->control_domain_id);
  Require(primary != routes.end(), "no-motion route map omits its primary owner");
  auto root_route = primary->second;
  routes.erase(primary);
  root_route->BindParticipantPeers(root_route, std::move(routes));
  composition.cclc_cell_route_publication_owner = root_route;
  composition.cclc_cell_command_readiness_port = std::make_unique<RetainedNativeCellCommandReadinessPort>(
      *cell, [grippers = std::move(grippers), resolver_for, root_route](const auto& publication, const auto& current) {
        using Result = core::Result<RetainedNativeCellReadinessCandidate>;
        // This operation-local lease survives the synchronous existing port's
        // validation, publication, exact readback and all cache adoption. It
        // is never stored in a resolver/context/source; same thread releases it.
        auto transition = std::make_shared<std::unique_lock<std::mutex>>(root_route->CellTransitionMutex());
        RetainedNativeCellReadinessCandidate candidate;
        candidate.registrations = current;
        for (auto& row : candidate.registrations) {
          if (row.binding.value().controllable_component_id != publication.controllable_component_id ||
              !row.stop_readiness) continue;
          auto sealed = core::rebind_runtime_participant_stop_readiness_generation(*row.stop_readiness,
              publication.component_lifecycle_generation);
          if (!sealed.ok()) return Result::Err(sealed.error());
          row.stop_readiness = std::move(sealed.value());
        }
        std::vector<std::tuple<std::shared_ptr<RetainedNativeGripperRuntime>,
            std::optional<core::RuntimeParticipantStopReadiness>,
            std::vector<core::RuntimeParticipantExecutionRegistration>>> staged;
        for (const auto& gripper : grippers) {
          const auto actual = gripper->registration();
          if (actual.binding.value().controllable_component_id != publication.controllable_component_id) continue;
          std::vector<core::RuntimeParticipantExecutionRegistration> aliases;
          std::optional<core::RuntimeParticipantStopReadiness> primary_readiness;
          for (const auto& registered : gripper->registrations()) {
            const auto row = std::find_if(candidate.registrations.begin(), candidate.registrations.end(),
                [&](const auto& value) {
              return value.binding.value() == registered.binding.value() &&
                     value.engine == registered.engine && value.stop_execution_port == registered.stop_execution_port;
            });
            if (row == candidate.registrations.end())
              return Result::Err({"missing_reference", "readiness candidate lost its exact scalar alias", {}});
            aliases.push_back(*row);
            if (row->binding.value() == actual.binding.value()) primary_readiness = row->stop_readiness;
          }
          staged.emplace_back(gripper, std::move(primary_readiness), std::move(aliases));
        }
        candidate.participant_resolver = resolver_for(candidate.registrations);
        candidate.adopt_component_owners = [root_route, transition = std::move(transition),
            staged = std::move(staged)]() mutable noexcept {
          for (auto& [gripper, readiness, registrations] : staged) {
            gripper->AdoptVerifiedReadiness(std::move(readiness));
            gripper->AdoptVerifiedRegistrations(std::move(registrations));
          }
        };
        return Result::Ok(std::move(candidate));
      });
}

// Prepare the same actual product used by the installed facade. Original
// delegated runtimes borrow its catalog/ports, so attachment must retain it.
void EnsureInstalledRetainedRos2ControlLifecycleProduct(
    Composition &composition, const std::filesystem::path &package_root) {
  if (composition.cclc_lifecycle_product) {
    Require(composition.cclc_provider_manifest_source != nullptr,
            "installed ROS lifecycle product lost its verified provider source");
    return;
  }
  Require((composition.runtime || composition.gripper_runtime) && composition.cell_runtime &&
              composition.active_addressed_kernel() != nullptr,
          "installed ROS lifecycle product requires the stable runtime and Kernel");
  auto provider_manifest_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProviderManifestSource>(package_root);
  auto physical_bindings = PhysicalControlBindings(composition);
  std::vector<std::shared_ptr<RetainedRos2ControlRuntimeHandle>> handles;
  for (const auto &[component_id, view] : IndexNativeComponentCompositions(composition)) {
    static_cast<void>(component_id);
    Require(view->ros2_control_runtime_handle != nullptr,
            "retained ROS lifecycle has no bound execution adapter");
    handles.push_back(view->ros2_control_runtime_handle);
  }
  auto product = RetainedRos2ControlLifecycleProduct::Create(
      composition.bundle, *provider_manifest_source, physical_bindings, handles,
      *(composition.runtime ? composition.runtime->clock : composition.cell_runtime->clock()),
      1'000'000'000LL);
  if (!product.ok())
    Fail("native lifecycle product cannot be composed: " + product.error().message);
  composition.cclc_provider_manifest_source = std::move(provider_manifest_source);
  composition.cclc_lifecycle_product = std::move(product.value());
}

using HostCCLCReadinessDependencies = RetainedNativeCCLCReadinessDependencies;

// Invoked only at product construction after the existing route adoption.
// The actual route owner remains the source association authority; the map
// supplies complete geometry independently of source availability.
[[nodiscard]] HostCCLCReadinessDependencies
BuildInstalledCCLCReadinessDependencies(
    const Composition &composition, const std::filesystem::path &package_root) {
  using CaptureResult =
      core::Result<core::ComponentLifecycleReadinessSourceView>;
  const auto unavailable = [](core::ErrorDTO error) -> HostCCLCReadinessDependencies {
    return {nullptr, [error = std::move(error)](
        const core::ComponentLifecycleCurrentSnapshotDTO &) {
      return CaptureResult::Err(error);
    }};
  };
  const auto primary = composition.runtime;
  if ((!primary && !composition.gripper_runtime) || composition.bundle == nullptr)
    return unavailable({"measured_source_unavailable", "the primary product owner is absent",
                        {{"reason", "measured_primary_owner_missing"}}});
  const auto &products = composition.bundle->value().compiled_products;
  if (products.symbol_role_maps.size() != 1U)
    return unavailable({"measured_source_unavailable", "the exact retained model map is absent",
                        {{"reason", "measured_model_map_missing"}}});
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(package_root);
  const auto model_map = map_source.get_verified(
      {composition.bundle->retained_bundle_id(),
       products.symbol_role_maps.front().product_id});
  if (!model_map)
    return unavailable({"measured_source_unavailable", "the retained model map is unavailable",
                        {{"reason", "measured_model_map_unavailable"}}});

  std::set<core::ControllableComponentId> model_owners;
  for (const auto &coordinate : model_map->coordinates())
    if (coordinate.owner_component_id &&
        coordinate.coordinate_kind != deployment::UrdfCoordinateKind::kNone)
      model_owners.insert(*coordinate.owner_component_id);
  std::vector<core::ControllableComponentId> robots;
  for (const auto &component : composition.bundle->value().cell_graph.controllable_components)
    if (model_owners.erase(component.controllable_component_id) != 0U)
      robots.push_back(component.controllable_component_id);
  if (!model_owners.empty())
    return unavailable({"measured_source_unavailable", "the model geometry has a foreign owner",
                        {{"reason", "measured_model_membership_mismatch"}}});

  std::map<core::ControllableComponentId, std::shared_ptr<RetainedNativeRuntime>> runtimes;
  std::map<core::ControllableComponentId, std::shared_ptr<RetainedNativeGripperRuntime>> grippers;
  for (const auto &[component, view] : IndexNativeComponentCompositions(composition)) {
    if (view == nullptr || (!view->runtime && !view->gripper_runtime))
      return unavailable({"measured_source_unavailable", "the installed component owner is absent",
                          {{"reason", "measured_component_owner_missing"}}});
    if (view->runtime) runtimes.emplace(component, view->runtime);
    if (view->gripper_runtime) grippers.emplace(component, view->gripper_runtime);
    if (std::find(robots.begin(), robots.end(), component) == robots.end())
      continue;
    // Verified physical/model rows supply immutable coordinate names below.
    // Dynamic source authority comes only from the current route capture, so
    // an adopted predecessor epoch cannot poison the static geometry map.
  }
  const auto physical = PhysicalControlBindings(composition);
  std::vector<core::RuntimeParticipantBinding> pneumatic;
  for (const auto& binding : physical) {
    if (binding.value().capability_id.value() != "tool.gripper.command") continue;
    const auto component = binding.value().controllable_component_id;
    const auto motion_owner = runtimes.find(component);
    const auto scalar_owner = grippers.find(component);
    const auto* backend = motion_owner != runtimes.end() ? motion_owner->second->gripper.get()
        : scalar_owner != grippers.end() ? scalar_owner->second->backend() : nullptr;
    Require(backend != nullptr, "qualified gripper geometry has no exact configured backend owner");
    if (backend->gripper_actuation_kind(binding.value().command_target_id.value()) ==
        core::GripperActuationKind::kPneumatic) pneumatic.push_back(binding);
  }
  auto geometry = QualifyRetainedRobotMeasuredGeometry(*model_map, robots, physical, {}, pneumatic);
  if (!geometry.ok()) {
    const auto &error = geometry.error();
    std::cerr << "retained measured geometry unavailable code=" << error.code;
    for (const auto *key : {"reason", "component_id", "control_binding_id",
                            "command_target_id", "source_joint_name",
                            "compiled_joint_name"}) {
      const auto field = error.details.find(key);
      if (field != error.details.end())
        std::cerr << " " << key << "=" << field->second;
    }
    std::cerr << "\n";
    return unavailable(error);
  }
  auto inventory = std::make_shared<const core::ConfiguredRobotMeasuredStateInventory>(
      std::move(geometry.value()));
  deployment::CCLCReadinessSourceCapture capture =
      [primary, runtimes = std::move(runtimes), grippers = std::move(grippers)](
          const core::ComponentLifecycleCurrentSnapshotDTO &snapshot) -> CaptureResult {
    // The existing motion runtime owns the complete arm-and-tool inventory.
    // The scalar owner is used only when no motion runtime exists.
    const auto found = runtimes.find(snapshot.controllable_component_id);
    if (found == runtimes.end()) {
      const auto tool = grippers.find(snapshot.controllable_component_id);
      if (tool != grippers.end())
        return tool->second->CaptureReadinessSources(snapshot, tool->second);
      return CaptureResult::Err({"measured_source_unavailable",
          "the component has no exact installed runtime owner",
          {{"reason", "measured_component_owner_missing"}}});
    }
    auto captured = found->second->CaptureReadinessSources(snapshot, found->second);
    if (!captured.ok()) return captured;
    // The primary owns the cell root. A peer's raw root pointer by itself
    // cannot pin that root while the operation-local validator runs.
    const auto validate = std::move(captured.value().validate_current);
    captured.value().validate_current = [primary, validate] {
      return validate ? validate() : core::Result<void>::Err(
          {"measured_source_unavailable", "source association validator is absent",
           {{"reason", "measured_source_validator_missing"}}});
    };
    return captured;
  };
  return {std::move(inventory), std::move(capture)};
}
[[nodiscard]] RetainedNativeCCLCRuntimeConfig
WithInstalledReadinessConstruction(
    RetainedNativeCCLCRuntimeConfig config, const Composition &composition,
    const std::filesystem::path &package_root) {
  config.prepare_readiness_dependencies = [&composition, package_root] {
    return BuildInstalledCCLCReadinessDependencies(composition, package_root);
  };
  return config;
}

void PrepareInstalledMeasuredSourcesAfterContext(const Composition &composition) noexcept {
  try {
    for (const auto &[component, view] : IndexNativeComponentCompositions(composition)) {
      static_cast<void>(component);
      if (!view) continue;
      if (!view->runtime && view->gripper_runtime) {
        auto prepared = view->gripper_runtime->PrepareCurrentMeasuredSources();
        if (!prepared.ok()) std::cerr << "manyforge measured gripper source unavailable: component="
            << component.value() << " code=" << prepared.error().code << " message=" << prepared.error().message << '\n';
        continue;
      }
      if (!view->runtime) continue;
      const auto bindings = RetainedNativeComponentOwner(view->runtime).physical_bindings();
      const bool has_geometry = std::any_of(bindings.begin(), bindings.end(),
          [](const auto &binding) {
        return std::any_of(binding.value().resources.begin(), binding.value().resources.end(),
                           [](const auto &resource) { return resource.source_joint_name.has_value(); });
      });
      if (has_geometry) {
        // Stage only after the existing context factory qualified its closure.
        // Missing measurement does not add Apply/lifecycle admission rules.
        const auto prepared = view->runtime->PrepareCurrentMeasuredSources();
        if (!prepared.ok()) {
          std::cerr << "manyforge measured source preparation: component=" << component.value()
                    << " code=" << prepared.error().code
                    << " message=" << prepared.error().message << '\n';
        }
      }
    }
  } catch (const std::exception &error) {
    // This new optional measurement preparation must not convert an already
    // qualified context into an Apply failure. Existing capture stays absent.
    try { std::cerr << "manyforge measured source preparation failed: " << error.what() << '\n'; }
    catch (...) {}
  } catch (...) {
    try { std::cerr << "manyforge measured source preparation failed: unknown exception\n"; }
    catch (...) {}
  }
}


void AttachRetainedNativeCCLC(Composition &composition,
                              const HostConfig &config,
                              ManagedKernelApplyFenceOwner &fence_owner,
                              const std::optional<core::ConfigurationFenceStatusQueryDTO>
                                  &parent_apply = std::nullopt,
                              const std::optional<core::RequestOwnerEpoch>
                                  &quiescent_predecessor = std::nullopt) {
  core::IClock *owner_clock = nullptr;
  core::IScheduler *owner_scheduler = nullptr;
  Require(composition.cell_runtime && (composition.runtime || composition.gripper_runtime),
          "CCLC attachment requires the stable cell and an actual component owner");
  owner_clock = composition.cell_runtime->clock().get();
  owner_scheduler = composition.cell_runtime->scheduler().get();
  EnsureGripperOnlyCellPorts(composition);
  Require(composition.active_addressed_kernel() != nullptr &&
              owner_clock != nullptr && owner_scheduler != nullptr,
          "native CCLC attachment requires one stable addressed Kernel");

  auto &kernel = *composition.active_addressed_kernel();
  if (composition.cclc_runtime) {
    Require(composition.cclc_attached.load(std::memory_order_acquire),
            "constructed native CCLC is not attached");
    return;
  }
  Require(kernel.SetRouteRecoveryPending(true).ok(),
          "route recovery could not retain closed admission");
  composition.route_recovery_pending.store(true, std::memory_order_release);

  auto active_publication_reader =
      std::make_unique<component_manager::SqliteActiveBundlePublicationStore>(
          config.manager_state_root /
              std::string(
                  component_manager::kActiveBundlePublicationStoreFilename),
          core::CellId(config.cell_id));
  const auto integrity = active_publication_reader->VerifyIntegrity();
  Require(integrity.current_generation == composition.publication_generation,
          "native CCLC active publication changed during composition");
  auto active_publication =
      active_publication_reader->ReadActiveBundlePublication(
          integrity.current_generation);
  Require(
      active_publication.ok() && active_publication.value().valid() &&
          active_publication.value().request.candidate_retained_bundle_id ==
              composition.bundle->retained_bundle_id() &&
          active_publication.value()
                  .request.candidate_projection_revision.retained_bundle_id ==
              composition.projection_revision.retained_bundle_id &&
          active_publication.value()
                  .request.candidate_projection_revision.graph_revision ==
              composition.projection_revision.graph_revision &&
          active_publication.value()
                  .request.candidate_projection_revision.scene_revision
                  .scene_id ==
              composition.projection_revision.scene_revision.scene_id &&
          active_publication.value().request.candidate_graph_revision ==
              composition.bundle->value().cell_graph.graph_revision,
      "native CCLC cannot re-read the exact active product");

  // Both hosts adopt their own recorded plan while the parent Apply fence is
  // still held. A ROS plan adopts its Declared components and the routes it
  // actually published; ordinary CCLC Prepare/Enable reopens afterwards.
  if (parent_apply.has_value()) {
    fence_owner.RunWithExactHeldSuccessor(
        *parent_apply, active_publication.value(), [&] {
          core::SqliteCCLCJournal journal(
              config.execution_state_root /
                  std::string(core::kCCLCJournalFilename),
              core::CellId(config.cell_id));
          const auto plan = journal.LoadNativeApplyPlan(
              {parent_apply->owner_transaction.apply_id,
               parent_apply->owner_transaction.apply_generation});
          if (!plan.ok() || !plan.value().has_value()) {
            Fail("native CCLC has no recorded plan for this Apply");
          }
          auto facts = ReadNativeApplyFacts(composition,
                                            active_publication.value(),
                                            *plan.value());
          const auto adopted = journal.AdoptNativeApplyFacts(facts);
          if (!adopted.ok()) {
            Fail("native CCLC could not adopt exact Apply facts: " +
                 adopted.error().message);
          }
          journal.Close();
        });
  }

  auto artifact_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProductArtifactSource>(
      config.package_root);
  EnsureInstalledRetainedRos2ControlLifecycleProduct(composition, config.package_root);
  auto *const lifecycle_product_view = composition.cclc_lifecycle_product.get();

  deployment::CCLCRuntimeCompositionConfig cclc_config{
      config.execution_state_root,
      config.execution_run_root,
      core::CellId(config.cell_id),
      {core::RequestOwnerEpoch(RandomToken("cclc-owner-")), 1024U, 2U,
       10'000'000LL, 30'000'000'000LL}};
  cclc_config.coordinator.quiescent_predecessor = quiescent_predecessor;
  cclc_config.preparation_failure_source = config.preparation_failure_source;
  cclc_config.lifecycle_target_driver.read_output_drop_counts =
      [](const core::ControllableComponentId &component)
      -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    const auto counts = host_controller_launch_supervisor()
                            .component_output_counters(component.value());
    if (!counts)
      return std::nullopt;
    return std::pair{counts->dropped_forward_lines,
                     counts->dropped_forward_bytes};
  };
  cclc_config.on_incidents_pruned =
      [](const core::ComponentIncidentPruneNotice &notice) {
        host_controller_launch_supervisor().enqueue_host_log(
            "INFO manyforge CCLC closed incidents pruned: count=" +
            std::to_string(notice.count) +
            " oldest_sequence=" + std::to_string(notice.oldest_sequence) +
            " newest_sequence=" + std::to_string(notice.newest_sequence) +
            "\n");
      };
  auto cclc = [&]() -> core::Result<
      std::unique_ptr<deployment::CCLCRuntimeComposition>> {
    if (!composition.runtime) {
      auto& route = *composition.cclc_cell_route_publication_owner;
      auto& readiness = *composition.cclc_cell_command_readiness_port;
      auto current = [&kernel](const core::ControlDomainId& domain) {
        auto read = kernel.current_runtime_route_configuration(domain);
        if (!read.ok()) return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Err(read.error());
        return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
            read.value() ? std::optional(read.value()->active_configuration) : std::nullopt);
      };
      if (parent_apply || quiescent_predecessor) {
        auto created = CreateRetainedNativeCCLCRuntime(
            WithInstalledReadinessConstruction({std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
            composition.bundle, *active_publication_reader, std::move(active_publication.value()), *artifact_source,
            lifecycle_product_view->catalog_source(), lifecycle_product_view->brand_ports(),
            CellControlBindingRepresentatives(composition), kernel, fence_owner, route,
            *owner_clock, *owner_scheduler, std::move(current), lifecycle_product_view->lifecycle_snapshot_source(), &readiness);
        if (!created.ok()) return created;
        const auto started = created.value()->Start();
        if (!started.ok()) return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::Err(started.error());
        return created;
      }
      auto* const exact_route = dynamic_cast<RetainedNativeGripperRoutePublicationOwner*>(&route);
      Require(exact_route, "gripper-only restart has another route owner");
      return CreateRecoveredRetainedNativeCellCCLCRuntime(
          WithInstalledReadinessConstruction({std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
          composition.bundle, *active_publication_reader, std::move(active_publication.value()), *artifact_source,
          lifecycle_product_view->catalog_source(), lifecycle_product_view->brand_ports(),
          CellControlBindingRepresentatives(composition), kernel, fence_owner, route, readiness, kernel,
          *owner_clock, *owner_scheduler,
          [exact_route](const auto& predecessor) { return exact_route->AdoptAndReadBackRouteConfigurationPredecessor(predecessor); },
          [cell = composition.cell_runtime] { return cell->ReconcileCommandFreeOrphans(); },
          composition.cclc_cell_route_reconciler, lifecycle_product_view->lifecycle_snapshot_source());
    }
    if (parent_apply || quiescent_predecessor) {
      // A live parent installation already carries its actual routes and
      // readiness. Reconstruct only the immutable facade dependencies; the
      // startup recovery overload would adopt/reconcile unrelated effects.
      auto current_configuration = [&runtime = *composition.runtime](
          const core::ControlDomainId &domain) {
        return runtime.ReadCurrentPublishedRouteConfiguration(domain);
      };
      auto created = CreateRetainedNativeCCLCRuntime(
          WithInstalledReadinessConstruction(
              {std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
          composition.bundle, *active_publication_reader,
          std::move(active_publication.value()), *artifact_source,
          lifecycle_product_view->catalog_source(),
          lifecycle_product_view->brand_ports(),
          CellControlBindingRepresentatives(composition), kernel, fence_owner,
          *composition.runtime, *owner_clock, *owner_scheduler,
          std::move(current_configuration),
          lifecycle_product_view->lifecycle_snapshot_source(),
          composition.runtime.get());
      if (!created.ok()) return created;
      const auto started = created.value()->Start();
      if (!started.ok())
        return core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>::Err(
            started.error());
      return created;
    }
    return CreateRetainedNativeCCLCRuntime(
        WithInstalledReadinessConstruction(
            {std::move(cclc_config), 1'000'000'000LL}, composition, config.package_root),
        composition.bundle, *active_publication_reader,
        std::move(active_publication.value()), *artifact_source,
        lifecycle_product_view->catalog_source(),
        lifecycle_product_view->brand_ports(),
        CellControlBindingRepresentatives(composition), *composition.runtime,
        *composition.active_addressed_kernel(), fence_owner
        ,
        lifecycle_product_view->lifecycle_snapshot_source()
    );
  }();
  if (!cclc.ok()) {
    Fail("native CCLC composition failed: " + cclc.error().message);
  }
  PrepareInstalledMeasuredSourcesAfterContext(composition);


  composition.cclc_active_publication_reader =
      std::move(active_publication_reader);
  composition.cclc_artifact_source = std::move(artifact_source);
  composition.cclc_runtime = std::move(cclc.value());
  composition.cclc_attached.store(true, std::memory_order_release);
}

void DetachRetainedCCLC(Composition &composition) {
  composition.cclc_attached.store(false, std::memory_order_release);
  composition.route_recovery_pending.store(false, std::memory_order_release);
  composition.cclc_cell_route_reconciler = {};
  if (composition.runtime != nullptr) {
    composition.runtime->stop_recovery_configuration_source = {};
    composition.runtime->reconcile_controller_routes = {};
  }
  composition.cclc_runtime.reset();
  composition.cclc_cell_command_readiness_port.reset();
  composition.cclc_cell_route_publication_owner.reset();
  composition.cclc_lifecycle_product.reset();
  composition.cclc_provider_manifest_source.reset();
  composition.cclc_artifact_source.reset();
  composition.cclc_active_publication_reader.reset();
}

struct ManagedBundleAtPinnedRoot final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::filesystem::path package_root;
};

/// Resolve an exact retained bundle from the two directory descriptors pinned
/// for this managed child. The selected package root has precedence; the
/// activation root is only the explicitly supplied fallback, never an
/// ambient filesystem search path.
[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadManagedBundleAtPinnedRoot(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id) {
  const std::array<std::filesystem::path, 2U> roots{
      config.package_root, config.program_artifact_root};
  for (std::size_t index = 0U; index < roots.size(); ++index) {
    const auto &root = roots[index];
    if (root.empty() || (index == 1U && root == roots.front())) continue;
    const deployment::FilesystemVerifiedRetainedBundleSource source(root);
    auto bundle = source.get_verified(retained_bundle_id);
    if (bundle != nullptr)
      return ManagedBundleAtPinnedRoot{std::move(bundle), root};
  }
  return std::nullopt;
}

[[nodiscard]] core::NativeApplyParentPreparationFailureSource
MakeNativeApplyPreparationFailureSource(
    const ManagedRuntimeChildConfig &config,
    core::ResourceAddressedExecutionKernel &kernel) {
  // These roots belong to the main-scope managed child, and the stable Kernel
  // outlives every CCLC composition. No original-live effect validator is
  // reconstructed here: replacements may observe this immutable history.
  return [config, &kernel](const core::NativeApplyPlanDTO &plan)
             -> core::Result<bool> {
    const auto candidate = LoadManagedBundleAtPinnedRoot(
        config, plan.candidate_retained_bundle_id);
    if (!candidate) return core::Result<bool>::Ok(false);
    const auto manager_journal = config.manager_state_root /
        std::string(component_manager::kManagerApplyJournalFilename);
    const auto parent = component_manager::ReadNativeApplyParentStatusReadOnly(
        manager_journal, core::CellId(config.cell_id), plan, *candidate->bundle);
    if (!parent.ok()) return core::Result<bool>::Err(parent.error());
    const auto &stored = parent.value().intent.intent;
    // Read the existing full ledger entry rather than guessing the Q91 scope.
    const auto claim = stored.parent_authoring_claim
        ? stored.parent_authoring_claim
        : kernel.ReadHeldCellAdmissionClaim(
              core::CellAdmissionClaimKindDTO::kManagerApply);
    if (!claim) return core::Result<bool>::Ok(false);
    const core::CellAdmissionRecoveryReceiptQueryDTO query{
        claim->intent, *claim, std::nullopt};
    const auto receipt = kernel.GetCellAdmissionRecoveryReceipt(query);
    if (!receipt.ok()) return core::Result<bool>::Err(receipt.error());
    const auto observed =
        component_manager::ReadNativeApplyPreparationFailureReadOnly(
            manager_journal, core::CellId(config.cell_id), plan,
            *candidate->bundle, *claim, query, receipt.value());
    if (!observed.ok()) return core::Result<bool>::Err(observed.error());
    return core::Result<bool>::Ok(observed.value().has_value());
  };
}

// A borrowed compiler view, without composition/Scene/admission owners. The
// same resolver accepts this view and the actual CompositionBuildInput.
struct CandidatePhysicalBindingInput final {
  const std::shared_ptr<const deployment::RetainedBundle> &bundle;
  const deployment::VerifiedRetainedModelMap &model_map;
  const core::RuntimeEpoch &runtime_epoch;
  std::optional<core::ControllableComponentId> selected_component_id;
  const deployment::ProgramBinding *status_program_binding{};
  std::optional<core::TargetRoleId> status_motion_target_role_id{};
  std::optional<core::TargetRoleId> status_gripper_target_role_id{};
};

struct ManagedCandidateLifecycleComponentClosure final {
  core::ControllableComponentId component_id;
  std::vector<core::ExpectedControlDomainRevisionDTO> candidate_expected_domains;
  std::vector<core::ResolvedLifecycleControlDomainScopeDTO> qualified_candidate_domains;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> candidate_provider_occurrences;
  std::vector<core::BrandLifecyclePlanCatalogDTO> lifecycle_plan_catalogs;
};

struct ManagedCandidateLifecyclePlanningClosure final {
  core::ProjectionRevisionDTO candidate_projection_revision;
  core::ActiveBundlePublicationGeneration anticipated_publication_generation;
  // Compiled ActiveFacts floors/paths only, never installed or ready readback.
  std::vector<core::RuntimeParticipantBinding> compiled_physical_bindings;
  std::vector<ManagedCandidateLifecycleComponentClosure> components;
};

// Captured only by the original synchronous fresh Acquire branch. Durable
// plan/status reads on a replacement never reconstruct this live continuation.
// Its facade is borrowed from the main cell lifetime, which cannot be recreated
// until every accepted child and original managed caller has fully drained.
struct ManagedOriginalApplyBringUpCapture final {
  component_manager::PersistedManagerApplyBringUpPlan sealed_plan;
  core::CellAdmissionClaimDTO parent_claim;
  core::ActiveBundlePublicationReceiptDTO predecessor_publication;
  core::ProjectionRevisionDTO predecessor_projection_revision;
  ManagedBundleAtPinnedRoot predecessor;
  ManagedBundleAtPinnedRoot candidate;
  ManagedCandidateLifecyclePlanningClosure candidate_closure;
  deployment::CCLCRuntimeComposition *original_facade{};
};

[[nodiscard]] core::Result<ManagedCandidateLifecyclePlanningClosure>
BuildManagedCandidateLifecyclePlanningClosure(
    const ManagedRuntimeChildConfig &config,
    const ManagedBundleAtPinnedRoot &candidate,
    const core::ProjectionRevisionDTO &revision,
    const core::ActiveBundlePublicationGeneration anticipated_publication_generation) {
  using Closure = ManagedCandidateLifecyclePlanningClosure;
  try {
    Require(candidate.bundle && deployment::verify_retained_bundle_identity(*candidate.bundle) &&
                candidate.bundle->value().cell_graph.cell_id == core::CellId(config.cell_id) &&
                revision.valid() && revision.retained_bundle_id == candidate.bundle->retained_bundle_id() &&
                revision.graph_revision == candidate.bundle->value().cell_graph.graph_revision &&
                anticipated_publication_generation.nonzero() &&
                !config.runtime_unit_id.empty() && !config.runtime_epoch.empty(),
            "candidate lifecycle compiler inputs do not share the pinned product");
    const auto &bundle = *candidate.bundle;
    const auto &graph = bundle.value().cell_graph;
    const core::RuntimeUnitId runtime_unit_id(config.runtime_unit_id);
    const auto runtime_configuration =
        deployment::ros2_control_execution_runtime_contract::require_configuration(bundle, runtime_unit_id);
    // Validate host-local ownership before the resolver's ActiveFacts uses
    // this host epoch. A foreign retained unit must never receive it.
    for (const auto &component : graph.controllable_components) {
      Require(std::find(component.runtime_unit_ids.begin(), component.runtime_unit_ids.end(), runtime_unit_id) !=
                  component.runtime_unit_ids.end() &&
                  deployment::ros2_control_execution_runtime_contract::find_controller_profile(
                      runtime_configuration, component.controllable_component_id) != nullptr,
              "candidate component has no controller profile in this host runtime unit");
    }
    Require(std::all_of(graph.control_bindings.begin(), graph.control_bindings.end(),
                [&](const auto &binding) { return binding.provider_runtime_unit_id == runtime_unit_id; }),
            "candidate lifecycle provider belongs to another runtime unit");
    Closure result{revision, anticipated_publication_generation, {}, {}};
    if (graph.controllable_components.empty()) {
      Require(graph.control_bindings.empty() && graph.control_domains.empty() &&
                  bundle.value().desired_control_configurations.empty(),
              "empty candidate has nonempty lifecycle control membership");
      return core::Result<Closure>::Ok(std::move(result));
    }
    const auto &products = bundle.value().compiled_products;
    Require(products.symbol_role_maps.size() == 1U,
            "candidate lifecycle compiler requires the exact retained model map");
    const deployment::FilesystemVerifiedRetainedModelMapSource map_source(candidate.package_root);
    auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
        bundle.retained_bundle_id(), products.symbol_role_maps.front().product_id});
    Require(model_map.has_value(), "candidate retained model map is absent or invalid");
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(candidate.package_root);
    const auto inventory = deployment::compile_direct_control_program_binding_inventory(
        bundle, *model_map, catalog_store);
    const core::RuntimeEpoch runtime_epoch(config.runtime_epoch);
    std::map<core::ControllableComponentId, std::set<core::ControlDomainId>> component_domains;
    for (const auto &component : graph.controllable_components) {
      const auto [entry, inserted] = component_domains.emplace(
          component.controllable_component_id, std::set<core::ControlDomainId>{});
      Require(inserted, "candidate compiler has duplicate component membership");
      CandidatePhysicalBindingInput input{candidate.bundle, *model_map, runtime_epoch,
          entry->first, nullptr, std::nullopt, std::nullopt};
      auto physical = ResolveNativePhysicalBindings(input, inventory);
      result.compiled_physical_bindings.insert(result.compiled_physical_bindings.end(),
          std::make_move_iterator(physical.representatives.begin()),
          std::make_move_iterator(physical.representatives.end()));
    }
    for (const auto &binding : graph.control_bindings) {
      const auto component = component_domains.find(binding.controllable_component_id);
      Require(component != component_domains.end(), "candidate binding has no retained component");
      component->second.insert(binding.control_domain_id);
    }
    Require(std::all_of(result.compiled_physical_bindings.begin(), result.compiled_physical_bindings.end(),
                [](const auto &row) { return row.value().control_path.has_value(); }),
            "candidate compiler produced an unaddressed physical binding");
    std::sort(result.compiled_physical_bindings.begin(), result.compiled_physical_bindings.end(),
        [](const auto &lhs, const auto &rhs) {
          return lhs.value().control_path->control_binding_id < rhs.value().control_path->control_binding_id;
        });
    std::map<core::ControlBindingId, const core::RuntimeParticipantBinding *> compiled;
    for (const auto &binding : result.compiled_physical_bindings) {
      const auto &value = binding.value();
      Require(value.retained_bundle_id == bundle.retained_bundle_id() &&
                  value.graph_revision == graph.graph_revision && value.control_path &&
                  compiled.emplace(value.control_path->control_binding_id, &binding).second,
              "candidate compiled representatives are stale, absent or duplicated");
    }
    struct DomainClosure final {
      core::ExpectedControlDomainRevisionDTO expected;
      core::ResolvedLifecycleControlDomainScopeDTO scope;
      core::ResolvedLifecycleProviderOccurrenceDTO provider;
      core::BrandLifecyclePlanCatalogDTO catalog;
    };
    std::map<core::ControlDomainId, DomainClosure> domains;
    std::set<core::ControlBindingId> consumed;
    const deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(candidate.package_root);
    const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(candidate.package_root);
    for (const auto &desired : bundle.value().desired_control_configurations) {
      const auto domain = std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
          [&](const auto &row) { return row.control_domain_id == desired.control_domain_id; });
      Require(domain != graph.control_domains.end() &&
                  domain->control_domain_revision == desired.control_domain_revision &&
                  domain->controller_topology_id == desired.controller_topology_id &&
                  domain->topology_revision == desired.topology_revision,
              "candidate desired domain differs from its retained topology");
      std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
      std::set<core::ResourceId> resources;
      const core::RuntimeControlPathDTO *provider = nullptr;
      for (const auto &route : desired.binding_routes) {
        const auto representative = compiled.find(route.control_binding_id);
        Require(representative != compiled.end() && consumed.insert(route.control_binding_id).second,
                "candidate desired route has no unique compiled representative");
        const auto &value = representative->second->value();
        const auto &path = *value.control_path;
        const auto graph_binding = std::find_if(graph.control_bindings.begin(), graph.control_bindings.end(),
            [&](const auto &row) { return row.control_binding_id == route.control_binding_id; });
        Require(graph_binding != graph.control_bindings.end() &&
                    graph_binding->controllable_component_id == value.controllable_component_id &&
                    graph_binding->control_binding_revision == route.control_binding_revision &&
                    graph_binding->control_domain_id == desired.control_domain_id &&
                    graph_binding->controller_topology_id == desired.controller_topology_id &&
                    graph_binding->topology_revision == desired.topology_revision &&
                    graph_binding->command_target_id == value.command_target_id &&
                    graph_binding->capability_id == value.capability_id &&
                    graph_binding->provider_instance_id == path.provider_instance_id &&
                    graph_binding->provider_manifest_id == path.provider_manifest_id &&
                    graph_binding->provider_manifest_revision == path.provider_manifest_revision &&
                    graph_binding->provider_runtime_unit_id == path.provider_runtime_unit_id &&
                    std::find(graph_binding->legal_control_route_ids.begin(),
                              graph_binding->legal_control_route_ids.end(), route.control_route_id) !=
                        graph_binding->legal_control_route_ids.end() &&
                    path.control_domain_id == desired.control_domain_id &&
                    path.control_domain_revision == desired.control_domain_revision &&
                    path.controller_topology_id == desired.controller_topology_id &&
                    path.topology_revision == desired.topology_revision &&
                    path.control_binding_revision == route.control_binding_revision &&
                    path.control_route_id == route.control_route_id &&
                    path.provider_runtime_unit_id == runtime_unit_id &&
                    path.provider_runtime_epoch == runtime_epoch,
                "candidate compiled route differs from its retained binding/domain");
        if (provider) {
          Require(std::tie(provider->provider_instance_id, provider->provider_manifest_id,
                           provider->provider_manifest_revision, provider->provider_runtime_unit_id,
                           provider->provider_runtime_epoch) ==
                      std::tie(path.provider_instance_id, path.provider_manifest_id,
                               path.provider_manifest_revision, path.provider_runtime_unit_id,
                               path.provider_runtime_epoch),
                  "candidate domain spans multiple lifecycle provider occurrences");
        } else {
          provider = &path;
        }
        bindings.push_back({route.control_binding_id, route.control_binding_revision, route.control_route_id});
      }
      // Scope includes every domain binding/resource, including another
      // component's share. A selected-component filter would truncate it.
      std::size_t graph_binding_count = 0U;
      for (const auto &binding : graph.control_bindings) {
        if (binding.control_domain_id != desired.control_domain_id) continue;
        ++graph_binding_count;
        for (const auto &resource : binding.resource_bindings) resources.insert(resource.resource_id);
      }
      Require(provider && graph_binding_count == bindings.size() && !resources.empty(),
              "candidate desired domain does not close all retained bindings/resources");
      // Reuse the existing sealed product decoder; no synthetic topology or
      // controller catalog is accepted from an inspection request.
      static_cast<void>(deployment::load_verified_controller_lifecycle_expected_domain(
          bundle, artifacts, desired, provider->provider_runtime_unit_id, provider->provider_runtime_epoch));
      const auto manifest = manifests.get_verified({bundle.retained_bundle_id(),
          provider->provider_manifest_id, provider->provider_manifest_revision});
      Require(manifest.has_value(), "candidate retained lifecycle manifest is absent");
      auto catalog = make_retained_ros2_control_lifecycle_catalog(manifest->value());
      if (!catalog.ok()) return core::Result<Closure>::Err(catalog.error());
      std::sort(bindings.begin(), bindings.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.control_binding_id < rhs.control_binding_id;
      });
      const core::ExpectedActiveControlConfigurationDTO absent_configuration{
          core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
      core::ExpectedControlDomainRevisionDTO expected{desired.control_domain_id,
          desired.control_domain_revision, desired.controller_topology_id, desired.topology_revision,
          absent_configuration, bindings};
      core::ResolvedLifecycleControlDomainScopeDTO scope{desired.control_domain_id,
          desired.control_domain_revision, desired.controller_topology_id, desired.topology_revision,
          std::nullopt, std::move(bindings), std::vector<core::ResourceId>(resources.begin(), resources.end())};
      core::ResolvedLifecycleProviderOccurrenceDTO occurrence{desired.control_domain_id,
          desired.control_domain_revision, provider->provider_instance_id, provider->provider_manifest_id,
          provider->provider_manifest_revision, provider->provider_runtime_unit_id,
          provider->provider_runtime_epoch, catalog.value().catalog_digest};
      Require(expected.locally_valid() && scope.locally_valid() && occurrence.locally_valid() &&
                  core::ExpectedDomainMatchesResolvedScope(expected, scope),
              "candidate lifecycle domain closure is not one valid exact value");
      Require(domains.emplace(desired.control_domain_id, DomainClosure{
          std::move(expected), std::move(scope), std::move(occurrence), std::move(catalog.value())}).second,
          "candidate has duplicate desired domains");
    }
    Require(consumed.size() == compiled.size() && domains.size() == graph.control_domains.size(),
            "candidate compiler has unused physical bindings or omitted domains");
    for (const auto &[component, ids] : component_domains) {
      Require(!ids.empty() && ids.size() <= core::kMaxLifecycleExpectedDomains,
              "candidate component has no bounded lifecycle domain closure");
      ManagedCandidateLifecycleComponentClosure closure{component, {}, {}, {}, {}};
      std::size_t binding_count = 0U;
      for (const auto &id : ids) {
        const auto domain = domains.find(id);
        Require(domain != domains.end(), "candidate component domain has no compiled lifecycle closure");
        const auto count = domain->second.expected.expected_control_bindings.size();
        Require(count <= core::kMaxLifecycleExpectedBindings - binding_count,
                "candidate component binding closure exceeds the existing bound");
        binding_count += count;
        closure.candidate_expected_domains.push_back(domain->second.expected);
        closure.qualified_candidate_domains.push_back(domain->second.scope);
        closure.candidate_provider_occurrences.push_back(domain->second.provider);
        closure.lifecycle_plan_catalogs.push_back(domain->second.catalog);
      }
      result.components.push_back(std::move(closure));
    }
    return core::Result<Closure>::Ok(std::move(result));
  } catch (const std::exception &error) {
    return core::Result<Closure>::Err({"candidate_lifecycle_planning_failed", error.what(),
        {{"reason", "verified_candidate_closure_unavailable"}}});
  }
}

struct ManagedInstalledLifecycleComponentClosure final {
  std::vector<core::ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers;
  std::vector<core::BrandLifecyclePortRegistrationDTO> ports;
};

// Called only while the original live parent pins this installed Composition.
// No compiled desired configuration is substituted for missing Kernel truth.
[[nodiscard]] core::Result<ManagedInstalledLifecycleComponentClosure>
ReadInstalledManagedCandidateLifecycleComponentClosure(
    const Composition &composition,
    const ManagedCandidateLifecycleComponentClosure &qualified) {
  using Closure = ManagedInstalledLifecycleComponentClosure;
  try {
    Require(composition.cclc_lifecycle_product && composition.active_addressed_kernel() &&
                IndexNativeComponentCompositions(composition).contains(qualified.component_id) &&
                !qualified.qualified_candidate_domains.empty() &&
                qualified.qualified_candidate_domains.size() == qualified.candidate_provider_occurrences.size() &&
                qualified.qualified_candidate_domains.size() == qualified.lifecycle_plan_catalogs.size(),
            "installed lifecycle component has no actual product/Kernel closure");
    const auto physical_bindings = PhysicalControlBindings(composition);
    const auto &product = *composition.cclc_lifecycle_product;
    Closure result;
    for (std::size_t index = 0U; index < qualified.qualified_candidate_domains.size(); ++index) {
      const auto &candidate = qualified.qualified_candidate_domains[index];
      const auto port = std::find_if(product.brand_ports().begin(), product.brand_ports().end(),
          [&](const auto &row) { return row.provider_occurrence.control_domain_id == candidate.control_domain_id; });
      Require(port != product.brand_ports().end() && port->port &&
                  port->provider_occurrence == qualified.candidate_provider_occurrences[index],
              "installed lifecycle provider differs from the qualified candidate");
      const auto &provider = port->provider_occurrence;
      const auto catalog = product.catalog_source().LoadExactLifecyclePlanCatalog(
          provider.provider_manifest_id, provider.provider_manifest_revision,
          provider.lifecycle_plan_catalog_digest);
      Require(catalog.ok() && catalog.value() == qualified.lifecycle_plan_catalogs[index],
              "installed lifecycle catalog differs from the qualified candidate");
      const auto current = composition.active_addressed_kernel()->current_runtime_route_configuration(
          candidate.control_domain_id);
      Require(current.ok() && current.value() && current.value()->operation_admission_open,
              "installed lifecycle domain has no current Kernel route readiness");
      const auto &configuration = current.value()->active_configuration;
      Require(configuration.locally_valid() &&
                  configuration.state == core::ActiveControlConfigurationStateDTO::kReconciled &&
                  configuration.source_runtime_unit_id == provider.provider_runtime_unit_id &&
                  configuration.source_runtime_epoch == provider.provider_runtime_epoch &&
                  current.value()->readiness_rows.size() == configuration.binding_routes.size(),
              "installed lifecycle route has another provider or incomplete Stop readiness");
      std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
      std::set<core::ResourceId> resources;
      for (const auto &route : configuration.binding_routes) {
        bool found = false;
        for (const auto &physical : physical_bindings) {
          const auto &value = physical.value();
          if (!value.control_path || value.control_path->control_domain_id != candidate.control_domain_id ||
              value.control_path->control_binding_id != route.control_binding_id) continue;
          const auto &path = *value.control_path;
          Require(value.retained_bundle_id == composition.bundle->retained_bundle_id() &&
                      value.graph_revision == composition.bundle->value().cell_graph.graph_revision &&
                      path.control_domain_revision == configuration.control_domain_revision &&
                      path.controller_topology_id == configuration.controller_topology_id &&
                      path.topology_revision == configuration.topology_revision &&
                      path.control_binding_revision == route.control_binding_revision &&
                      path.control_route_id == route.control_route_id &&
                      path.activation_set_id == configuration.activation_set_id &&
                      path.active_control_configuration_id == configuration.active_control_configuration_id &&
                      path.active_control_configuration_generation == configuration.active_control_configuration_generation &&
                      path.provider_instance_id == provider.provider_instance_id &&
                      path.provider_manifest_id == provider.provider_manifest_id &&
                      path.provider_manifest_revision == provider.provider_manifest_revision &&
                      path.provider_runtime_unit_id == provider.provider_runtime_unit_id &&
                      path.provider_runtime_epoch == provider.provider_runtime_epoch,
                  "installed physical binding differs from its current Kernel route/provider");
          found = true;
          for (const auto &resource : value.resources) resources.insert(resource.resource_id);
        }
        Require(found, "installed Kernel route has no actual physical binding");
        bindings.push_back({route.control_binding_id, route.control_binding_revision, route.control_route_id});
      }
      core::ResolvedLifecycleControlDomainScopeDTO actual{
          configuration.control_domain_id, configuration.control_domain_revision,
          configuration.controller_topology_id, configuration.topology_revision, configuration,
          std::move(bindings), std::vector<core::ResourceId>(resources.begin(), resources.end())};
      auto expected = candidate;
      expected.active_control_configuration = configuration;
      Require(actual.locally_valid() && actual == expected,
              "installed lifecycle domain closure differs from the qualified candidate");
      result.domains.push_back(std::move(actual));
      result.providers.push_back(provider);
      result.ports.push_back(*port);
    }
    return core::Result<Closure>::Ok(std::move(result));
  } catch (const std::exception &error) {
    return core::Result<Closure>::Err({"installed_lifecycle_closure_unavailable", error.what(),
        {{"reason", "actual_installed_product_or_route_unavailable"}}});
  }
}
// Assemble observation data from the actual original caller's qualified
// values. Heldness/lifetime and effect permission are verified by the existing
// live parent services and CCLC admission; this value creates neither.
[[maybe_unused, nodiscard]] core::Result<core::ApplyComponentBringUpEvidenceDTO>
BuildManagedApplyComponentBringUpEvidence(
    const component_manager::PersistedManagerApplyBringUpPlan &sealed,
    const component_manager::ManagerApplyBringUpChildRequestDTO &child,
    const core::ApplyComponentBringUpAuthorityDTO &authority,
    const core::ActiveBundlePublicationReceiptDTO &actual_predecessor_head,
    const ManagedCandidateLifecycleComponentClosure &candidate,
    const std::optional<core::ComponentLifecycleCurrentSnapshotDTO> &actual_predecessor,
    const std::optional<core::ApplyComponentBringUpRecoveryEvidenceDTO> &actual_recovery,
    const std::optional<core::SelectedControllerOccurrenceDTO> &actual_predecessor_occurrence,
    const std::optional<core::SelectedControllerOccurrenceDTO> &actual_successor_floor,
    bool qualified_candidate_has_arm) {
  using Evidence = core::ApplyComponentBringUpEvidenceDTO;
  const auto invalid = [](std::string reason) {
    return core::Result<Evidence>::Err(core::ErrorDTO{
        "apply_bring_up_evidence_invalid",
        "original delegated child does not close its qualified component evidence",
        {{"reason", std::move(reason)}}});
  };
  try {
    const auto &plan = sealed.plan;
    const core::ApplyComponentBringUpReferenceDTO parent{
        {plan.apply_id, plan.apply_generation}, sealed.plan_digest};
    if (!plan.valid() || !child.valid() ||
        std::find(plan.children.begin(), plan.children.end(), child) == plan.children.end() ||
        component_manager::ComputeManagerApplyBringUpPlanDigest(plan) != sealed.plan_digest ||
        !authority.valid() || authority.parent != parent || !actual_predecessor_head.valid() ||
        actual_predecessor_head.durable_public_pointer_content_digest != plan.predecessor_public_pointer_digest ||
        actual_predecessor_head.publication_generation !=
            plan.expected_publication.expected_previous_publication_generation ||
        actual_predecessor_head.request.candidate_graph_revision != child.intent.expected_graph_revision ||
        plan.expected_publication.candidate_projection_revision != child.candidate_projection_revision ||
        authority.held_fence_query.manager_runtime_epoch != child.manager_runtime_epoch ||
        authority.parent_claim.intent.expected_admission_generation != child.intent.expected_admission_generation ||
        candidate.component_id != child.intent.controllable_component_id ||
        candidate.candidate_expected_domains != child.intent.expected_control_domains ||
        candidate.qualified_candidate_domains.size() != child.intent.expected_control_domains.size() ||
        candidate.candidate_provider_occurrences.size() != candidate.qualified_candidate_domains.size() ||
        candidate.lifecycle_plan_catalogs.size() != candidate.qualified_candidate_domains.size() ||
        !child.intent.apply_generation_selection) {
      return invalid("original_child_parent_head_or_candidate_mismatch");
    }
    const auto &selection = *child.intent.apply_generation_selection;
    const auto expected_predecessor = selection.predecessor_generation
        ? selection.predecessor_generation->value()
        : selection.owner_qualified_predecessor ? selection.owner_qualified_predecessor->generation.value() : 0U;
    if (!selection.locally_valid() || child.intent.expected_component_lifecycle_generation.value() != expected_predecessor ||
        (actual_predecessor &&
         (!selection.owner_qualified_predecessor ||
          selection.owner_qualified_predecessor->generation != actual_predecessor->component_lifecycle_generation ||
          selection.owner_qualified_predecessor->snapshot_digest != actual_predecessor->snapshot_digest)) ||
        (!actual_predecessor && selection.owner_qualified_predecessor)) {
      return invalid("original_owner_generation_selection_mismatch");
    }
    core::ConfigurationFenceScopeDTO selected_scope{
        {child.intent.controllable_component_id}, {}, {}, {}};
    std::map<core::ControlDomainId, core::ConfigurationFenceDomainScopeDTO> domains;
    std::map<core::ControlBindingId, core::ConfigurationFenceBindingScopeDTO> bindings;
    std::set<core::ResourceId> resources;
    if (actual_predecessor) {
      if (!core::VerifyComponentLifecycleCurrentSnapshot(*actual_predecessor).ok() ||
          actual_predecessor->controllable_component_id != child.intent.controllable_component_id ||
          actual_predecessor->active_publication_receipt != actual_predecessor_head) {
        return invalid("actual_predecessor_snapshot_mismatch");
      }
      for (const auto &domain : actual_predecessor->domains) {
        domains.emplace(domain.control_domain_id, core::ConfigurationFenceDomainScopeDTO{
            domain.control_domain_id, domain.control_domain_revision, std::nullopt});
        for (const auto &binding : domain.current_control_bindings) {
          auto [entry, inserted] = bindings.emplace(binding.control_binding_id,
              core::ConfigurationFenceBindingScopeDTO{
                  binding.control_binding_id, binding.control_binding_revision, std::nullopt});
          if (!inserted && entry->second.predecessor_control_binding_revision != binding.control_binding_revision)
            return invalid("predecessor_binding_revision_conflict");
        }
        resources.insert(domain.current_resources.begin(), domain.current_resources.end());
      }
    }
    std::vector<core::ControlDomainId> candidate_ids;
    for (std::size_t index = 0U; index < candidate.qualified_candidate_domains.size(); ++index) {
      const auto &domain = candidate.qualified_candidate_domains[index];
      if (!core::ExpectedDomainMatchesResolvedScope(child.intent.expected_control_domains[index], domain))
        return invalid("qualified_candidate_domain_mismatch");
      candidate_ids.push_back(domain.control_domain_id);
      auto [entry, inserted] = domains.emplace(domain.control_domain_id,
          core::ConfigurationFenceDomainScopeDTO{
              domain.control_domain_id, std::nullopt, domain.control_domain_revision});
      if (!inserted) entry->second.candidate_control_domain_revision = domain.control_domain_revision;
      // The compiler's full domain closure includes shared bindings/resources.
      // Never reduce it to representatives whose component ID is selected.
      for (const auto &binding : domain.current_control_bindings) {
        auto [current, added] = bindings.emplace(binding.control_binding_id,
            core::ConfigurationFenceBindingScopeDTO{
                binding.control_binding_id, std::nullopt, binding.control_binding_revision});
        if (!added) {
          if (current->second.candidate_control_binding_revision &&
              current->second.candidate_control_binding_revision != binding.control_binding_revision)
            return invalid("candidate_binding_revision_conflict");
          current->second.candidate_control_binding_revision = binding.control_binding_revision;
        }
      }
      resources.insert(domain.current_resources.begin(), domain.current_resources.end());
    }
    for (const auto &[id, domain] : domains) { static_cast<void>(id); selected_scope.domains.push_back(domain); }
    for (const auto &[id, binding] : bindings) { static_cast<void>(id); selected_scope.bindings.push_back(binding); }
    selected_scope.resources.assign(resources.begin(), resources.end());
    if (!selected_scope.valid() || actual_predecessor_occurrence.has_value() != actual_successor_floor.has_value() ||
        (!actual_predecessor && (actual_recovery || actual_predecessor_occurrence)))
      return invalid("selected_scope_or_occurrence_shape_mismatch");
    if (actual_recovery &&
        (actual_recovery->predecessor_snapshot_digest != actual_predecessor->snapshot_digest ||
         !actual_predecessor_occurrence))
      return invalid("actual_recovery_predecessor_mismatch");
    const auto &parent_scope = authority.held_fence_query.expected_scope;
    if (!std::binary_search(parent_scope.controllable_component_ids.begin(),
            parent_scope.controllable_component_ids.end(), child.intent.controllable_component_id) ||
        !std::includes(parent_scope.resources.begin(), parent_scope.resources.end(),
            selected_scope.resources.begin(), selected_scope.resources.end()))
      return invalid("selected_component_or_resources_outside_parent_scope");
    for (const auto &domain : selected_scope.domains) {
      const auto held = std::lower_bound(parent_scope.domains.begin(), parent_scope.domains.end(),
          domain.control_domain_id, [](const auto &row, const auto &id) { return row.control_domain_id < id; });
      if (held == parent_scope.domains.end() || held->control_domain_id != domain.control_domain_id ||
          (domain.predecessor_control_domain_revision &&
           domain.predecessor_control_domain_revision != held->predecessor_control_domain_revision) ||
          (domain.candidate_control_domain_revision &&
           domain.candidate_control_domain_revision != held->candidate_control_domain_revision))
        return invalid("selected_domain_outside_parent_scope");
    }
    for (const auto &binding : selected_scope.bindings) {
      const auto held = std::lower_bound(parent_scope.bindings.begin(), parent_scope.bindings.end(),
          binding.control_binding_id, [](const auto &row, const auto &id) { return row.control_binding_id < id; });
      if (held == parent_scope.bindings.end() || held->control_binding_id != binding.control_binding_id ||
          (binding.predecessor_control_binding_revision &&
           binding.predecessor_control_binding_revision != held->predecessor_control_binding_revision) ||
          (binding.candidate_control_binding_revision &&
           binding.candidate_control_binding_revision != held->candidate_control_binding_revision))
        return invalid("selected_binding_outside_parent_scope");
    }
    if (actual_predecessor_occurrence) {
      if (actual_predecessor_occurrence->domains.size() != actual_predecessor->domains.size())
        return invalid("actual_predecessor_occurrence_membership_mismatch");
      for (std::size_t index = 0U; index < actual_predecessor->domains.size(); ++index)
        if (actual_predecessor_occurrence->domains[index].control_domain_id !=
            actual_predecessor->domains[index].control_domain_id)
          return invalid("actual_predecessor_occurrence_membership_mismatch");
      const auto computed = core::ComputeSelectedControllerSuccessorOccurrence(
          *actual_predecessor_occurrence, candidate_ids, qualified_candidate_has_arm);
      if (!computed.ok() || computed.value() != *actual_successor_floor)
        return invalid("actual_occurrence_floor_mismatch");
    }
    const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
        child.intent, child.candidate_projection_revision, child.candidate_retained_bundle_digest,
        child.candidate_retained_bundle_bytes, child.runtime_unit_id, child.runtime_epoch,
        child.manager_runtime_epoch, parent);
    return core::Result<Evidence>::Ok({authority, std::move(selected_scope), digest,
        child.candidate_projection_revision, child.candidate_retained_bundle_digest,
        child.candidate_retained_bundle_bytes, {child.runtime_unit_id, child.runtime_epoch},
        actual_recovery, actual_predecessor_occurrence, actual_successor_floor});
  } catch (const std::exception &error) {
    return core::Result<Evidence>::Err(core::ErrorDTO{
        "apply_bring_up_evidence_invalid", error.what(),
        {{"reason", "original_qualified_evidence_unavailable"}}});
  }
}


// Mechanical original-acceptance extraction only. This helper neither proves
// the parent remains held nor grants Continue, installation, or publication.
[[maybe_unused, nodiscard]] core::Result<core::ApplyComponentBringUpAcceptedReceiptDTO>
MakeManagedApplyComponentBringUpAcceptedReceipt(
    const component_manager::PersistedManagerApplyBringUpPlan &sealed,
    const component_manager::ManagerApplyBringUpChildRequestDTO &child,
    const core::CCLCLifecycleRequestResultDTO &result) {
  using Receipt = core::ApplyComponentBringUpAcceptedReceiptDTO;
  const auto invalid = [](std::string reason) {
    return core::Result<Receipt>::Err(core::ErrorDTO{
        "apply_bring_up_receipt_invalid",
        "delegated acceptance differs from its original sealed child",
        {{"reason", std::move(reason)}}});
  };
  try {
    const auto &plan = sealed.plan;
    const auto original = std::find(plan.children.begin(), plan.children.end(), child);
    if (!plan.valid() || !child.valid() || original == plan.children.end() ||
        component_manager::ComputeManagerApplyBringUpPlanDigest(plan) != sealed.plan_digest ||
        !result.locally_valid() || result.incident_recovery || result.rejection ||
        !result.acceptance || !result.status) {
      return invalid("sealed_child_or_result_mismatch");
    }
    const auto &status = *result.status;
    if (!status.apply_bring_up || status.selected_bring_up ||
        !status.accepted_intent.replay_equal(child.intent) ||
        status.status_owner_epoch != child.intent.request_context.request_owner_epoch ||
        status.reconciliation_required || status.error) {
      return invalid("original_owner_status_mismatch");
    }
    const auto &evidence = *status.apply_bring_up;
    const core::ApplyComponentBringUpReferenceDTO parent{
        {plan.apply_id, plan.apply_generation}, sealed.plan_digest};
    const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
        child.intent, child.candidate_projection_revision,
        child.candidate_retained_bundle_digest, child.candidate_retained_bundle_bytes,
        child.runtime_unit_id, child.runtime_epoch, child.manager_runtime_epoch, parent);
    if (evidence.parent_authority.parent != parent ||
        evidence.candidate_projection_revision != child.candidate_projection_revision ||
        evidence.candidate_retained_bundle_digest != child.candidate_retained_bundle_digest ||
        evidence.candidate_retained_bundle_bytes != child.candidate_retained_bundle_bytes ||
        evidence.original_execution_host.runtime_unit_id != child.runtime_unit_id ||
        evidence.original_execution_host.runtime_epoch != child.runtime_epoch ||
        evidence.parent_authority.held_fence_query.manager_runtime_epoch != child.manager_runtime_epoch ||
        evidence.original_request_payload_digest != digest) {
      return invalid("original_parent_candidate_or_epoch_mismatch");
    }
    // Result acceptance may carry a replay disposition. The status preserves
    // the actual durable acceptance that linked native planning joins exactly.
    return core::Result<Receipt>::Ok({child.intent.request_context,
        child.intent.controllable_component_id, status.acceptance, digest});
  } catch (const std::exception &error) {
    return core::Result<Receipt>::Err(core::ErrorDTO{
        "apply_bring_up_receipt_invalid", error.what(),
        {{"reason", "sealed_child_or_result_unavailable"}}});
  }
}



// The managed cell keeps the actual facade and its immutable construction
// dependencies independently of its current physical Composition. Moving
// these owners does not rotate the namespace, rebuild a registry or change
// accepted request evidence. The pinned runtimes keep the old borrowed route
// and observation ports alive until that facade has completely stopped.
// One installed product lifetime, owned by the immutable CCLC context. The
// current/retired Composition and this pin share the same compiled ROS product.
// Product callbacks retain actual installed runtime owners, never a mutable
// pointer to whichever Composition later becomes current.
struct InstalledManagedCCLCProductLifetime final {
  std::shared_ptr<WallClock> clock;
  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtimes;
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> grippers;
  std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> projections;
  std::vector<std::shared_ptr<ControllerLaunchProcess>> processes;
  std::shared_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      provider_source;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifacts;
  std::optional<core::BuildOwnedLifecyclePlanCatalogSource> empty_catalog;
  std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>
      empty_route_publication_owner;
  std::shared_ptr<deployment::IRouteConfigurationKernelPublicationOwner> component_route_owner;
  std::shared_ptr<RetainedNativeCellCommandReadinessPort> component_readiness_owner;
  // Destroy the borrowing compiled product before its provider source.
  std::shared_ptr<RetainedRos2ControlLifecycleProduct> lifecycle_product;
};

// The same verified empty dependencies serve initial construction and later
// preparation on the existing facade. They never manufacture a physical owner.
[[nodiscard]] std::shared_ptr<InstalledManagedCCLCProductLifetime>
MakeEmptyInstalledManagedCCLCProductLifetime(
    const ManagedRuntimeChildConfig &config,
    const std::shared_ptr<const deployment::RetainedBundle> &bundle,
    const core::ActiveBundlePublicationReceiptDTO &head,
    const std::shared_ptr<WallClock> &clock) {
  Require(bundle && clock && head.valid() &&
              head.request.candidate_retained_bundle_id == bundle->retained_bundle_id() &&
              head.request.candidate_graph_revision == bundle->value().cell_graph.graph_revision &&
              bundle->value().cell_graph.cell_id == core::CellId(config.cell_id),
          "empty cell product dependencies lost their exact bundle/head");
  auto route = MakeVerifiedEmptyCellRoutePublicationOwner(*bundle);
  Require(route.ok(), "empty cell CCLC product is not genuinely empty");
  auto catalog = core::BuildOwnedLifecyclePlanCatalogSource::Make({});
  Require(catalog.ok(), "empty cell lifecycle catalog cannot be constructed");
  auto lifetime = std::make_shared<InstalledManagedCCLCProductLifetime>();
  lifetime->clock = clock;
  lifetime->publication_reader =
      std::make_unique<component_manager::SqliteActiveBundlePublicationStore>(
          config.manager_state_root /
              std::string(component_manager::kActiveBundlePublicationStoreFilename),
          core::CellId(config.cell_id));
  const auto integrity = lifetime->publication_reader->VerifyIntegrity();
  const auto current = lifetime->publication_reader->ReadActiveBundlePublication(
      integrity.current_generation);
  Require(current.ok() && current.value() == head,
          "empty cell CCLC publication changed before construction");
  const auto selected = LoadManagedBundleAtPinnedRoot(config, bundle->retained_bundle_id());
  Require(selected && selected->bundle->canonical_bytes() == bundle->canonical_bytes(),
          "empty cell CCLC product is not at its pinned root");
  lifetime->artifacts = std::make_unique<
      deployment::FilesystemVerifiedRetainedProductArtifactSource>(selected->package_root);
  lifetime->empty_catalog = std::move(catalog.value());
  lifetime->empty_route_publication_owner = std::move(route.value());
  return lifetime;
}

struct ManagedCellCCLCLifetime final {
  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtime_pins;
  std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> gripper_pins;
  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      active_publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifact_source;
  std::shared_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      provider_manifest_source;
  std::optional<core::BuildOwnedLifecyclePlanCatalogSource> empty_catalog;
  std::shared_ptr<RetainedRos2ControlLifecycleProduct> lifecycle_product;
  std::shared_ptr<deployment::IRouteConfigurationKernelPublicationOwner>
      route_publication_owner;
  std::shared_ptr<RetainedNativeCellCommandReadinessPort> readiness_port;
  // Immutable construction head. Candidate installation never retargets the
  // original owner or retroactively changes its accepted request evidence.
  std::optional<core::ActiveBundlePublicationReceiptDTO> publication;
  // Original preparation for the existing installed-request reconciliation
  // lane. The facade remains the only current-context owner.
  std::shared_ptr<const deployment::CCLCRuntimeComposition::PreparedProductContext>
      pending_product;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      pending_product_request;
  std::optional<core::RequestOwnerEpoch> pending_product_owner;
  // Declared last: stop/drain the real facade before any borrowed dependency.
  std::unique_ptr<deployment::CCLCRuntimeComposition> facade;

  [[nodiscard]] core::Result<std::shared_ptr<const
      deployment::CCLCRuntimeComposition::PreparedProductContext>>
  PrepareInstalledProduct(Composition &composition,
                          const ManagedRuntimeChildConfig &config,
                          const core::ActiveBundlePublicationReceiptDTO &head) {
    Require(facade && clock && (composition.runtime || composition.gripper_runtime) &&
                composition.cell_runtime && composition.cell_runtime->clock() == clock && head.valid() &&
                head.publication_generation == composition.publication_generation &&
                head.request.candidate_retained_bundle_id ==
                    composition.bundle->retained_bundle_id() &&
                head.request.candidate_projection_revision == composition.projection_revision,
            "CCLC product preparation lost its actual installed owner or publication");
    const auto selected = LoadManagedBundleAtPinnedRoot(
        config, composition.bundle->retained_bundle_id());
    Require(selected && selected->bundle->canonical_bytes() ==
                            composition.bundle->canonical_bytes(),
            "CCLC product preparation lost the pinned candidate package");
    EnsureInstalledRetainedRos2ControlLifecycleProduct(composition, selected->package_root);
    auto lifetime = std::make_shared<InstalledManagedCCLCProductLifetime>();
    lifetime->clock = clock;
    lifetime->provider_source = composition.cclc_provider_manifest_source;
    lifetime->lifecycle_product = composition.cclc_lifecycle_product;
    lifetime->publication_reader = std::make_unique<
        component_manager::SqliteActiveBundlePublicationStore>(
        config.manager_state_root /
            std::string(component_manager::kActiveBundlePublicationStoreFilename),
        core::CellId(config.cell_id));
    lifetime->artifacts = std::make_unique<
        deployment::FilesystemVerifiedRetainedProductArtifactSource>(selected->package_root);
    for (const auto &[id, view] : IndexNativeComponentCompositions(composition)) {
      static_cast<void>(id);
      Require(view && (view->runtime || view->gripper_runtime) &&
                  (!view->runtime || view->runtime->clock == clock) &&
                  (view->controller_process || IsLocalNoOutputGripperOwner(*view)) &&
                  view->ros2_control_runtime_handle,
              "CCLC installed product has an incomplete physical lifetime");
      if (view->runtime) lifetime->runtimes.push_back(view->runtime);
      if (view->gripper_runtime) lifetime->grippers.push_back(view->gripper_runtime);
      if (view->controller_process)
        lifetime->processes.push_back(view->controller_process);
      lifetime->projections.insert(lifetime->projections.end(),
          view->required_projection_pins.begin(), view->required_projection_pins.end());
    }
    Require((!lifetime->runtimes.empty() || !lifetime->grippers.empty()) && lifetime->provider_source &&
                lifetime->lifecycle_product,
            "CCLC installed product has no genuine occurrence closure");
    const auto runtime = composition.runtime;
    deployment::CCLCRuntimeCompositionConfig context_config{
        config.execution_state_root, config.execution_run_root,
        core::CellId(config.cell_id),
        {facade->coordinator().request_owner_epoch(), 1024U, 2U,
         10'000'000LL, 30'000'000'000LL}};
    // This reuses the existing native derivation's freshness/configuration
    // qualification only. It never constructs or starts another owner.
    if (!runtime) {
      EnsureGripperOnlyCellPorts(composition);
      lifetime->component_route_owner = composition.cclc_cell_route_publication_owner;
      lifetime->component_readiness_owner = composition.cclc_cell_command_readiness_port;
      auto* const kernel = composition.active_addressed_kernel();
      auto prepared = PrepareRetainedNativeCCLCProductContext(
          *facade, WithInstalledReadinessConstruction({std::move(context_config), 1'000'000'000LL},
              composition, selected->package_root), composition.bundle,
          *lifetime->publication_reader, head, *lifetime->artifacts,
          lifetime->lifecycle_product->catalog_source(), lifetime->lifecycle_product->brand_ports(),
          CellControlBindingRepresentatives(composition), *clock, *lifetime->component_route_owner, lifetime,
          [kernel](const core::ControlDomainId& domain) {
            auto current = kernel->current_runtime_route_configuration(domain);
            if (!current.ok()) return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Err(current.error());
            return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
                current.value() ? std::optional(current.value()->active_configuration) : std::nullopt);
          }, lifetime->lifecycle_product->lifecycle_snapshot_source(), lifetime->component_readiness_owner.get());
      if (prepared.ok()) PrepareInstalledMeasuredSourcesAfterContext(composition);
      return prepared;
    }
    auto prepared = PrepareRetainedNativeCCLCProductContext(
        *facade, WithInstalledReadinessConstruction(
            {std::move(context_config), 1'000'000'000LL}, composition,
            selected->package_root), composition.bundle,
        *lifetime->publication_reader, head, *lifetime->artifacts,
        lifetime->lifecycle_product->catalog_source(),
        lifetime->lifecycle_product->brand_ports(),
        CellControlBindingRepresentatives(composition), *clock, *runtime, lifetime,
        [runtime](const core::ControlDomainId &domain) {
          return runtime->ReadCurrentPublishedRouteConfiguration(domain);
        }, lifetime->lifecycle_product->lifecycle_snapshot_source(), runtime.get());
    if (prepared.ok()) PrepareInstalledMeasuredSourcesAfterContext(composition);
    return prepared;
  }

  [[nodiscard]] core::Result<std::shared_ptr<const
      deployment::CCLCRuntimeComposition::PreparedProductContext>>
  PrepareEmptyProduct(const ManagedRuntimeChildConfig &config,
                      std::shared_ptr<const deployment::RetainedBundle> bundle,
                      const core::ActiveBundlePublicationReceiptDTO &head) {
    Require(facade && clock, "empty product preparation lost its original facade");
    auto lifetime = MakeEmptyInstalledManagedCCLCProductLifetime(config, bundle, head, clock);
    return facade->PrepareProductContext({
        std::move(bundle), *lifetime->publication_reader, head, *lifetime->artifacts,
        *lifetime->empty_catalog, {}, {}, {}, {}, {}, {}, nullptr,
        *lifetime->empty_route_publication_owner, lifetime});
  }

  void TakeAttached(Composition &composition,
                    const core::ActiveBundlePublicationReceiptDTO &head) {
    Require(!facade && composition.cclc_runtime && composition.cell_runtime &&
                (composition.runtime || composition.gripper_runtime) &&
                composition.cclc_attached.load(std::memory_order_acquire),
            "cell CCLC transfer requires one actual attached facade");
    Require(head.valid() &&
                head.publication_generation == composition.publication_generation &&
                head.request.candidate_projection_revision == composition.projection_revision &&
                head.request.candidate_retained_bundle_id == composition.bundle->retained_bundle_id(),
            "cell CCLC transfer differs from its immutable construction head");
    std::vector<std::shared_ptr<RetainedNativeRuntime>> pins;
    std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> tools;
    const auto pin = [&](const Composition& owner) {
      Require(owner.runtime || owner.gripper_runtime, "cell CCLC transfer has an incomplete component dependency");
      if (owner.runtime) pins.push_back(owner.runtime);
      if (owner.gripper_runtime) tools.push_back(owner.gripper_runtime);
    };
    pin(composition);
    for (const auto& peer : composition.participant_compositions) {
      Require(peer != nullptr, "cell CCLC transfer has a missing peer");
      pin(*peer);
    }
    publication = head;
    runtime_pins = std::move(pins);
    gripper_pins = std::move(tools);
    clock = composition.cell_runtime->clock();
    scheduler = composition.cell_runtime->scheduler();
    active_publication_reader =
        std::move(composition.cclc_active_publication_reader);
    artifact_source = std::move(composition.cclc_artifact_source);
    provider_manifest_source =
        std::move(composition.cclc_provider_manifest_source);
    lifecycle_product = std::move(composition.cclc_lifecycle_product);
    route_publication_owner =
        std::move(composition.cclc_cell_route_publication_owner);
    readiness_port = std::move(composition.cclc_cell_command_readiness_port);
    facade = std::move(composition.cclc_runtime);
  }

  void CreateEmpty(const ManagedRuntimeChildConfig &config,
                   std::shared_ptr<const deployment::RetainedBundle> bundle,
                   const core::ActiveBundlePublicationReceiptDTO &head,
                   core::ResourceAddressedExecutionKernel &kernel,
                   ManagedKernelApplyFenceOwner &fence_owner,
                   std::shared_ptr<WallClock> owner_clock,
                   std::shared_ptr<SimpleScheduler> owner_scheduler,
                   const std::optional<core::RequestOwnerEpoch> &quiescent_predecessor = {}) {
    Require(!facade && bundle && owner_clock && owner_scheduler,
            "empty cell CCLC requires the original cell dependencies");
    auto lifetime = MakeEmptyInstalledManagedCCLCProductLifetime(config, bundle, head, owner_clock);
    clock = std::move(owner_clock);
    scheduler = std::move(owner_scheduler);
    active_publication_reader = std::move(lifetime->publication_reader);
    artifact_source = std::move(lifetime->artifacts);
    empty_catalog = std::move(lifetime->empty_catalog);
    route_publication_owner = std::move(lifetime->empty_route_publication_owner);
    deployment::CCLCRuntimeCompositionConfig cclc_config{
        config.execution_state_root, config.execution_run_root,
        core::CellId(config.cell_id),
        {core::RequestOwnerEpoch(RandomToken("cclc-owner-")), 1024U, 2U,
         10'000'000LL, 30'000'000'000LL}};
    cclc_config.coordinator.quiescent_predecessor = quiescent_predecessor;
    cclc_config.preparation_failure_source =
        MakeNativeApplyPreparationFailureSource(config, kernel);
    auto created = deployment::CCLCRuntimeComposition::Create(
        std::move(cclc_config), std::move(bundle), *active_publication_reader,
        head, *artifact_source, *empty_catalog, {}, {}, {}, {}, {},
        kernel, fence_owner, *route_publication_owner, *clock, *scheduler);
    if (!created.ok()) Fail("empty cell CCLC construction failed: " + created.error().message);
    publication = head;
    facade = std::move(created.value());
    const auto started = facade->Start();
    if (!started.ok()) Fail("empty cell CCLC start failed: " + started.error().message);
  }
};

[[nodiscard]] std::filesystem::path ManagedProgramArtifactRoot(
    const ManagedRuntimeChildConfig &config,
    const std::filesystem::path &selected_package_root) {
  return config.program_artifact_root == selected_package_root
             ? std::filesystem::path{}
             : config.program_artifact_root;
}

[[nodiscard]] int ManagedPackageRootDescriptor(
    const ManagedRuntimeChildConfig &config,
    const std::filesystem::path &selected_package_root) {
  if (selected_package_root == config.package_root) {
    return config.package_root_fd;
  }
  if (!config.program_artifact_root.empty() &&
      selected_package_root == config.program_artifact_root) {
    return config.program_artifact_root_fd;
  }
  Fail("managed candidate package root is not one of the pinned descriptors");
}

[[nodiscard]] bool AttachCurrentPublishedCCLCIfReady(
    Composition &composition, const ManagedRuntimeChildConfig &config,
    ManagedKernelApplyFenceOwner &fence_owner,
    const std::optional<core::ConfigurationFenceStatusQueryDTO>
        &parent_apply = std::nullopt) {
  auto &kernel = *composition.active_addressed_kernel();
  if (composition.cclc_attached.load(std::memory_order_acquire))
    return true;
  Require(kernel.SetRouteRecoveryPending(true).ok(),
          "route attachment could not close admission");
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  publication_store.Close();
  if (integrity.current_generation != composition.publication_generation) {
    return false;
  }
  Require(publication.ok() && publication.value().valid(),
          "native CCLC attachment cannot verify current publication");
  const auto active_bundle = LoadManagedBundleAtPinnedRoot(
      config, publication.value().request.candidate_retained_bundle_id);
  Require(active_bundle.has_value(),
          "native CCLC attachment cannot load its published retained bundle");
  HostConfig attachment_config;
  attachment_config.package_root = active_bundle->package_root;
  attachment_config.manager_state_root = config.manager_state_root;
  attachment_config.execution_state_root = config.execution_state_root;
  attachment_config.execution_run_root = config.execution_run_root;
  attachment_config.candidate_worker = config.candidate_worker;
  attachment_config.cell_id = config.cell_id;
  attachment_config.preparation_failure_source =
      MakeNativeApplyPreparationFailureSource(config, kernel);
  AttachRetainedNativeCCLC(composition, attachment_config, fence_owner,
                           parent_apply);
  return composition.cclc_attached.load(std::memory_order_acquire);
}

[[nodiscard]] std::optional<CompositionBuildInput>
TryBuildCurrentManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    ManagedKernelAdmissionGate &admission_gate,
    bool &predecessor_composition_pending) {
  predecessor_composition_pending = false;
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  if (!integrity.current_generation.nonzero()) {
    publication_store.Close();
    return std::nullopt;
  }
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "managed active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  // Verify the published bundle before deciding which projection providers
  // this runtime unit actually requires. A gripper-only unit may require no
  // native planner projection at all.
  auto selected_bundle = LoadManagedBundleAtPinnedRoot(
      config, receipt.request.candidate_retained_bundle_id);
  if (!selected_bundle.has_value()) {
    Fail("managed active publication names an unavailable retained bundle: " +
         receipt.request.candidate_retained_bundle_id.value());
  }
  auto bundle = std::move(selected_bundle->bundle);
  const auto &selected_package_root = selected_bundle->package_root;
  const auto &published_projection_revision =
      receipt.request.candidate_projection_revision;
  if (bundle->value().cell_graph.cell_id != core::CellId(config.cell_id) ||
      bundle->value().cell_graph.graph_revision !=
          receipt.request.candidate_graph_revision ||
      bundle->retained_bundle_id() !=
          published_projection_revision.retained_bundle_id ||
      bundle->value().cell_graph.graph_revision !=
          published_projection_revision.graph_revision) {
    Fail("managed active publication and retained bundle disagree: "
         "publication_bundle=" +
         receipt.request.candidate_retained_bundle_id.value() +
         ", publication_graph=" +
         receipt.request.candidate_graph_revision.value() +
         ", projection_bundle=" +
         published_projection_revision.retained_bundle_id.value() +
         ", projection_graph=" +
         published_projection_revision.graph_revision.value() +
         ", loaded_bundle=" + bundle->retained_bundle_id().value() +
         ", loaded_graph=" +
         bundle->value().cell_graph.graph_revision.value());
  }
  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }
  if (!projections.AllRequiredProjectionsHealthy(
          *bundle, receipt.request.candidate_projection_revision)) {
    predecessor_composition_pending = true;
    return std::nullopt;
  }

  auto component_projections = SelectManagedComponentProjections(
      config, *bundle, projections,
      receipt.request.candidate_projection_revision, true);
  if (!component_projections.has_value()) {
    predecessor_composition_pending = true;
    return std::nullopt;
  }
  const auto projection_revision = component_projections->projection_revision;
  auto projection = component_projections->by_component.empty()
                        ? nullptr
                        : component_projections->by_component.begin()->second;
  // The model map is still mandatory when no motion planner is present: it
  // seals the gripper's component/target identity and any authored frames.
  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      selected_package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");
  auto collision_model = projection != nullptr
                             ? projection->collision_shared_model()
                             : collision::ManyForgeCollisionEngine::SharedModelPtr{};
  Require(projection == nullptr || collision_model != nullptr,
          "managed active projection has no collision model");

  auto planner_scene_projection = projection;
  auto input = CompositionBuildInput{
      selected_package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
      std::move(bundle),
      std::move(*model_map),
      projection_revision,
      receipt.publication_generation,
      std::move(projection),
      std::move(collision_model),
      {},
      {},
      &admission_gate,
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      true,
      {},
      {},
      config.manager_state_root,
      config.manager_run_root,
      config.logical_scene_database,
      config.program_cycle_journal,
      std::move(planner_scene_projection),
      {},
      {},
      std::move(component_projections->by_component),
      ManagedProgramArtifactRoot(config, selected_package_root),
      {},
      {},
      ManagedPackageRootDescriptor(config, selected_package_root),
      config.scene_asset_root_fd,
      [&receipt, &config] {
        if (const auto *apply =
                std::get_if<core::NamedApplyPublicationTransactionDTO>(
                    &receipt.request.transaction_ref))
          return "Apply=" + apply->apply_id.value() + " generation=" +
                 std::to_string(apply->apply_generation.value());
        return "Bootstrap runtime=" + config.runtime_unit_id +
               " epoch=" + config.runtime_epoch;
      }(),
  };
  input.required_projection_pins = std::move(component_projections->required_projection_pins);
  return input;
}

[[nodiscard]] bool CurrentPublicationIsInstalledSuccessor(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  const core::ActiveBundlePublicationGeneration successor_generation(
      predecessor_generation + 1U);
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  if (integrity.current_generation ==
      request.expected_predecessor_publication_generation) {
    publication_store.Close();
    return false;
  }
  Require(integrity.current_generation == successor_generation,
          "managed composition public pointer is neither predecessor nor "
          "exact successor");
  const auto publication =
      publication_store.ReadActiveBundlePublication(successor_generation);
  publication_store.Close();
  Require(publication.ok() && publication.value().valid(),
          "managed composition successor publication cannot be verified");
  const auto *transaction =
      std::get_if<core::NamedApplyPublicationTransactionDTO>(
          &publication.value().request.transaction_ref);
  Require(transaction != nullptr && transaction->apply_id == request.apply_id &&
              transaction->apply_generation == request.apply_generation &&
              publication.value().request.candidate_projection_revision ==
                  request.candidate_projection_revision &&
              publication.value()
                      .request.expected_previous_publication_generation ==
                  std::optional<core::ActiveBundlePublicationGeneration>{
                      request.expected_predecessor_publication_generation},
          "managed composition successor belongs to another Apply");
  return true;
}

[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadExactManagedCandidateBundle(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  Require(request.valid() &&
              request.runtime_unit_id.value() == config.runtime_unit_id &&
              request.runtime_epoch.value() == config.runtime_epoch,
          "managed composition request names another runtime child");
  auto selected = LoadManagedBundleAtPinnedRoot(
      config, request.candidate_projection_revision.retained_bundle_id);
  const auto bundle = selected.has_value() ? selected->bundle : nullptr;
  const auto canonical_bytes =
      bundle != nullptr ? bundle->canonical_bytes() : std::string_view{};
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->value().cell_graph.graph_revision ==
                  request.candidate_projection_revision.graph_revision &&
              bundle->retained_bundle_id() ==
                  request.candidate_projection_revision.retained_bundle_id &&
              !canonical_bytes.empty() &&
              canonical_bytes.size() ==
                  request.retained_bundle_canonical_byte_size &&
              core::ContentDigest(
                  content_identity::sha256_digest(canonical_bytes)) ==
                  request.retained_bundle_canonical_content_digest,
          "managed selected retained bundle identity or materialization "
          "differs");
  return selected;
}

struct ManagedBootstrapStopProduct final {
  std::vector<core::ResourceId> resource_inventory;
  bool requires_physical_composition{};
};

[[nodiscard]] ManagedBootstrapStopProduct LoadManagedBootstrapStopProduct(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id,
    const core::GraphRevision &graph_revision) {
  const auto selected =
      LoadManagedBundleAtPinnedRoot(config, retained_bundle_id);
  Require(selected.has_value() && selected->bundle != nullptr &&
              selected->bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              selected->bundle->retained_bundle_id() == retained_bundle_id &&
              selected->bundle->value().cell_graph.graph_revision ==
                  graph_revision,
          "managed bootstrap Stop policy differs from the retained product");

  std::set<core::ResourceId> resources;
  for (const auto &binding :
       selected->bundle->value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id.value() != config.runtime_unit_id)
      continue;
    for (const auto &resource : binding.resource_bindings)
      resources.insert(resource.resource_id);
  }
  Require(resources.size() <= core::kMaxKernelResourceSlots,
          "managed bootstrap resource inventory exceeds the Kernel bound");
  return {
      {resources.begin(), resources.end()},
      !selected->bundle->value().cell_graph.controllable_components.empty()};
}

[[nodiscard]] std::optional<CompositionBuildInput> BuildManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    ManagedKernelAdmissionGate &admission_gate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    std::shared_ptr<WallClock> shared_clock,
    std::shared_ptr<SimpleScheduler> shared_scheduler) {
  auto selected_candidate = LoadExactManagedCandidateBundle(config, request);
  Require(selected_candidate.has_value(),
          "managed selected retained bundle is unavailable from pinned roots");
  auto bundle = std::move(selected_candidate->bundle);
  const auto &candidate_package_root = selected_candidate->package_root;
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation ==
                  request.expected_predecessor_publication_generation &&
              integrity.current_pointer_digest ==
                  request.expected_predecessor_public_pointer_digest.value(),
          "managed composition predecessor publication changed before "
          "installation");
  const auto publication = publication_store.ReadActiveBundlePublication(
      request.expected_predecessor_publication_generation);
  Require(publication.ok() && publication.value().valid() &&
              publication.value().publication_generation ==
                  request.expected_predecessor_publication_generation &&
              publication.value().durable_public_pointer_content_digest ==
                  request.expected_predecessor_public_pointer_digest,
          "managed predecessor publication cannot be verified");
  publication_store.Close();

  const auto predecessor_location = LoadManagedBundleAtPinnedRoot(
      config, request.predecessor_projection_revision.retained_bundle_id);
  const auto predecessor = predecessor_location.has_value()
                               ? predecessor_location->bundle
                               : nullptr;
  Require(predecessor != nullptr &&
              predecessor->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              predecessor->value().cell_graph.graph_revision ==
                  request.predecessor_projection_revision.graph_revision &&
              deployment::derive_apply_component_delta(*predecessor, *bundle) ==
                  request.component_delta,
          "managed component delta differs from the verified predecessor and candidate");

  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }

  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      candidate_package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");

  auto component_projections = SelectManagedComponentProjections(
      config, *bundle, projections, request.candidate_projection_revision,
      false);
  Require(component_projections.has_value(),
          "managed selected robot projections are not active at the candidate "
          "revision");
  auto projection = component_projections->by_component.empty()
                        ? nullptr
                        : component_projections->by_component.begin()->second;
  auto collision_model = projection != nullptr
                             ? projection->collision_shared_model()
                             : collision::ManyForgeCollisionEngine::SharedModelPtr{};
  Require(projection == nullptr || collision_model != nullptr,
          "managed active projection has no collision model");

  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  auto planner_scene_projection = projection;
  auto input = CompositionBuildInput{
      candidate_package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
      std::move(bundle),
      std::move(*model_map),
      request.candidate_projection_revision,
      core::ActiveBundlePublicationGeneration(predecessor_generation + 1U),
      std::move(projection),
      std::move(collision_model),
      {},
      {},
      &admission_gate,
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      false,
      std::move(shared_clock),
      std::move(shared_scheduler),
      config.manager_state_root,
      config.manager_run_root,
      config.logical_scene_database,
      config.program_cycle_journal,
      std::move(planner_scene_projection),
      {},
      {},
      std::move(component_projections->by_component),
      ManagedProgramArtifactRoot(config, candidate_package_root),
      request.component_delta,
      {},
      ManagedPackageRootDescriptor(config, candidate_package_root),
      config.scene_asset_root_fd,
      "Apply=" + request.apply_id.value() +
          " generation=" + std::to_string(request.apply_generation.value()),
  };
  input.required_projection_pins = std::move(component_projections->required_projection_pins);
  return input;
}

[[nodiscard]] bool IsExactRetainedNativeViewApply(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  return !request.component_delta.components.empty() &&
         request.predecessor_projection_revision.retained_bundle_id ==
             request.candidate_projection_revision.retained_bundle_id &&
         request.predecessor_projection_revision.graph_revision ==
             request.candidate_projection_revision.graph_revision &&
         std::all_of(request.component_delta.components.begin(),
                     request.component_delta.components.end(),
                     [](const auto &row) {
                       return row.disposition ==
                              deployment::ApplyComponentDisposition::kRetain;
                     });
}

void RefreshRetainedNativePublicationView(
    Composition &cell,
    const std::map<core::ControllableComponentId,
                   std::shared_ptr<RetainedPlannerSceneProjection>>
        &projections,
    const std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> &required_projection_pins,
    const core::ProjectionRevisionDTO &revision,
    const core::ActiveBundlePublicationGeneration generation,
    const std::filesystem::path &manager_run_root) {
  // Copy before any view mutation; allocation failure retains the old owners.
  auto replacement_pins = required_projection_pins;
  const auto replace_pins = [&] {
    for (auto &peer : cell.participant_compositions)
      peer->required_projection_pins.clear();
    cell.required_projection_pins = std::move(replacement_pins);
  };
  std::map<core::ControllableComponentId, Composition *> components;
  components.emplace(cell.controllable_component_id, &cell);
  for (auto &peer : cell.participant_compositions) {
    Require(peer != nullptr &&
                components.emplace(peer->controllable_component_id,
                                   peer.get()).second,
            "retained native publication has duplicate component owners");
  }
  if (projections.empty()) {
    // No component advertises a native planner projection. Scene revision is
    // still owned by the canonical Scene; there is no planner to re-anchor.
    for (const auto &[component_id, composition] : components) {
      static_cast<void>(component_id);
      Require(composition->gripper_runtime != nullptr &&
                  composition->runtime == nullptr &&
                  composition->planner_scene_projection == nullptr,
              "unprojected retained view contains a motion controller");
      composition->projection_revision = revision;
      composition->publication_generation = generation;
    }
    replace_pins();
    return;
  }
  Require(components.size() == projections.size(),
          "retained native publication projection set changed components");
  // First validate the entire candidate set. A failed validation does not
  // re-anchor even one planner; any later effect still runs under admission.
  for (const auto &[component_id, composition] : components) {
    const auto projection = projections.find(component_id);
    Require(projection != projections.end() && projection->second != nullptr &&
                composition->component_runtime_identity() != nullptr &&
                composition->planner_scene_projection == projection->second,
            "retained native controller has no stable Scene projection owner");
    if (composition->runtime != nullptr) {
      Require(composition->runtime->planner != nullptr &&
                  composition->runtime->planning_scene_synchronizer != nullptr,
              "retained motion controller has no stable planning owner");
    }
    const auto inventory =
        projection->second->current_scene_snapshot_lease_inventory();
    Require(inventory.has_value() &&
                inventory->projection_binding.projection_revision == revision,
            "retained native projection does not match the candidate Scene");
  }
  for (const auto &[component_id, composition] : components) {
    const auto &projection = projections.at(component_id);
    const auto inventory =
        projection->current_scene_snapshot_lease_inventory();
    if (composition->runtime != nullptr) {
      auto &runtime = *composition->runtime;
      const auto rebound = runtime.planner->RebindProjection(
          runtime.planner->projection_binding(), inventory->projection_binding);
      if (!rebound.ok())
        Fail("retained native planner rejected the candidate Scene: " +
             rebound.error().message);
      runtime.planning_scene_synchronizer->ReplaceTarget(
          std::make_shared<ManagerPlanningSceneSynchronizer>(
              manager_run_root, revision.retained_bundle_id,
              revision.graph_revision, generation, projection));
    }
    composition->projection_revision = revision;
    composition->publication_generation = generation;
  }
  replace_pins();
}

// Stable host-owned occurrences. Component Composition roots never own or
// retire these processes. Manager has already checked the immutable observer
// input and activates its expectation between ordinary Prepare and Finalize.
class ManagedIsaacCellProcesses final {
public:
  using Facts = component_manager::ManagerIsaacPoseObserverLaunchFactsDTO;
  explicit ManagedIsaacCellProcesses(const ManagedRuntimeChildConfig &config)
      : config_(config) {}
  ~ManagedIsaacCellProcesses() {
    std::shared_ptr<CellLaunchProcess> process;
    {
      std::lock_guard lock(state_mutex_);
      process = std::move(observer_);
    }
    if (process)
      static_cast<void>(host_controller_launch_supervisor().retire_cell_process(process));
  }

  /// Capture the current occurrence once. Inspect grants no effect and never
  /// regenerates detection time, settings or output when persistence retries.
  [[nodiscard]] std::optional<component_manager::ManagerCellObserverObservationDTO>
  observe_current(deployment::CCLCRuntimeComposition *const cclc = nullptr) {
    std::lock_guard transition_lock(transition_mutex_);
    std::lock_guard lock(state_mutex_);
    if (!facts_) return std::nullopt;
    using Presence = component_manager::ProcessPresenceState;
    using Persistence = component_manager::ManagerCellObserverIncidentPersistenceDTO;
    if (!observer_) {
      return component_manager::ManagerCellObserverObservationDTO{
          core::CellId(config_.cell_id), facts_->source_id, facts_->source_epoch_base,
          observer_start_failure_ ? Presence::kAbsent : Presence::kUnknown};
    }
    const auto exit = observer_->exit_evidence();
    if (!exit) {
      return component_manager::ManagerCellObserverObservationDTO{
          core::CellId(config_.cell_id), facts_->source_id, facts_->source_epoch_base,
          Presence::kPresent};
    }
    if (!observer_incident_observation_) {
      Require(exit->command.cell_id == core::CellId(config_.cell_id) &&
                  exit->command.role == CellLaunchRole::kPoseObserver &&
                  exit->command.observer_source_id == facts_->source_id,
              "cell observer exit belongs to another occurrence");
      const auto detected = WallClock{}.now();
      const auto id = RandomToken("observer-incident-");
      observer_incident_process_ = observer_;
      observer_incident_observation_ =
          component_manager::ManagerCellObserverObservationDTO{
              core::CellId(config_.cell_id), facts_->source_id, facts_->source_epoch_base,
              Presence::kAbsent,
              component_manager::ManagerCellObserverExitDTO{
                  exit->pid,
                  exit->exit_status ? std::optional<std::uint8_t>(*exit->exit_status) : std::nullopt,
                  exit->signal ? std::optional<std::uint8_t>(*exit->signal) : std::nullopt,
                  exit->core_dumped, exit->observed_utc, detected},
              std::nullopt, Persistence::kUnknown};
      try {
      const auto optional_number = [](const std::optional<int> value) {
        return value ? CanonicalValue(static_cast<double>(*value)) : CanonicalValue(nullptr);
      };
      CanonicalValue::Array argv;
      for (const auto &argument : exit->command.argv) argv.emplace_back(argument);
      observer_incident_record_ = core::CellObserverIncidentRecordDTO{
          id, {core::CellId(config_.cell_id), facts_->source_id, facts_->source_epoch_base},
          detected, "cell pose observer exited; observation intervention required",
          CanonicalValue(CanonicalValue::Object{
              {"exit", CanonicalValue(CanonicalValue::Object{
                  {"status", optional_number(exit->exit_status)},
                  {"signal", optional_number(exit->signal)},
                  {"core_dumped", CanonicalValue(exit->core_dumped)},
                  {"observed_utc", CanonicalValue(exit->observed_utc)}})},
              {"context", CanonicalValue(CanonicalValue::Object{
                  {"cell_id", CanonicalValue(config_.cell_id)},
                  {"source_id", CanonicalValue(facts_->source_id)},
                  {"source_epoch_base", CanonicalValue(facts_->source_epoch_base)},
                  {"runtime_unit_id", CanonicalValue(config_.runtime_unit_id)},
                  {"runtime_epoch", CanonicalValue(config_.runtime_epoch)},
                  {"pid", CanonicalValue(static_cast<double>(exit->pid))},
                  {"launch_identity", CanonicalValue(exit->command.identity)},
                  {"request_context", CanonicalValue(exit->request_context)}})},
              {"settings", CanonicalValue(CanonicalValue::Object{
                  {"argv", CanonicalValue(std::move(argv))},
                  {"retained_snapshot_path", CanonicalValue(facts_->retained_snapshot_path)}})},
              {"generated", CanonicalValue(CanonicalValue::Object{})},
              {"output", LaunchIncidentOutput(observer_->output_snapshot())},
              {"core_dump_link", CanonicalValue(nullptr)}})};
      if (!observer_incident_record_->locally_valid()) {
        observer_incident_observation_->incident_id = id;
        observer_incident_observation_->incident_persistence = Persistence::kFailed;
      }
      } catch (const std::bad_alloc &) {
        observer_incident_observation_->incident_persistence = Persistence::kUnknown;
      } catch (...) {
        // A retained real exit remains unavailable even if its payload cannot
        // be qualified. Do not advertise an unconstructed record as pending.
        try {
          observer_incident_observation_->incident_id = id;
          observer_incident_observation_->incident_persistence = Persistence::kFailed;
        } catch (...) {
          observer_incident_observation_->incident_id.reset();
          observer_incident_observation_->incident_persistence = Persistence::kUnknown;
        }
      }
    }
    Require(observer_incident_process_ == observer_,
            "pending observer incident belongs to another current occurrence");
    if (cclc && observer_incident_record_ && observer_incident_record_->locally_valid() &&
        !observer_incident_slot_qualified_) {
      // The managed main channel is the sole observer writer. Lookup and
      // capture are pinned to this exact current occurrence by the same cell
      // transition mutex used for stop/start. An uncertain lookup is never
      // treated as an absent slot or a newly minted durable reference.
      try {
        const auto open = cclc->LoadOpenCellObserverIncident();
        if (open.ok()) {
          if (open.value()) {
            Require(open.value()->target.cell_id == core::CellId(config_.cell_id),
                    "open observer incident belongs to another cell slot");
            observer_incident_observation_->incident_id = open.value()->incident_id;
          } else {
            observer_incident_observation_->incident_id = observer_incident_record_->incident_id;
          }
          observer_incident_observation_->incident_persistence = Persistence::kPending;
          observer_incident_slot_qualified_ = true;
        }
      } catch (...) {
        // Keep the genuine exit unavailable and retain the original canonical
        // payload/time for another ordinary read; no false pending reference.
        observer_incident_observation_->incident_persistence = Persistence::kUnknown;
      }
    }
    return observer_incident_observation_;
  }

  // Called only by the existing serial managed status handler, outside
  // composition/Kernel locks. Inspect itself remains entirely read-only.
  void persist_current_exit(deployment::CCLCRuntimeComposition &cclc) {
    static_cast<void>(observe_current(&cclc));
    std::lock_guard transition_lock(transition_mutex_);
    std::lock_guard lock(state_mutex_);
    using Persistence = component_manager::ManagerCellObserverIncidentPersistenceDTO;
    if (!observer_incident_record_ || !observer_incident_observation_ ||
        !observer_incident_slot_qualified_ ||
        observer_incident_observation_->incident_persistence == Persistence::kPersisted ||
        observer_incident_observation_->incident_persistence == Persistence::kFailed)
      return;
    Require(observer_ && observer_incident_process_ == observer_ && facts_ &&
                observer_incident_record_->target.cell_id == core::CellId(config_.cell_id) &&
                observer_incident_record_->target.source_id == facts_->source_id &&
                observer_incident_record_->target.source_epoch_base == facts_->source_epoch_base &&
                observer_incident_observation_->incident_id.has_value(),
            "observer incident write lost its exact current occurrence");
    try {
      const auto stored = cclc.RecordCellObserverIncident(*observer_incident_record_);
      // The first open record belongs to the cell slot; repeated exit events
      // may have a different source/base without changing that incident ID.
      if (stored.ok() && stored.value().target.cell_id == core::CellId(config_.cell_id)) {
        observer_incident_observation_->incident_id = stored.value().incident_id;
        observer_incident_observation_->incident_persistence = Persistence::kPersisted;
      } else {
        observer_incident_observation_->incident_persistence = Persistence::kUnknown;
      }
    } catch (...) {
      observer_incident_observation_->incident_persistence = Persistence::kUnknown;
    }
  }

  [[nodiscard]] std::optional<CellLaunchExitEvidence> observer_exit_fact() const {
    std::shared_ptr<CellLaunchProcess> process;
    {
      std::lock_guard lock(state_mutex_);
      process = observer_;
    }
    return process ? process->exit_evidence() : std::nullopt;
  }

  [[nodiscard]] std::optional<CellLaunchExitEvidence> retired_observer_fact() const {
    std::shared_ptr<CellLaunchProcess> process;
    {
      std::lock_guard lock(state_mutex_);
      process = retired_observer_;
    }
    return process ? process->exit_evidence() : std::nullopt;
  }

  [[nodiscard]] std::optional<IsaacControllerLaunchInputs>
  prepare(const deployment::RetainedBundle &bundle,
          const std::optional<Facts> &facts, const std::string &context,
          const bool qualify_existing_world = false) {
    std::lock_guard transition_lock(transition_mutex_);
    std::optional<std::string> world;
    std::set<std::string> topics{"/clock"};
    for (const auto &unit : bundle.value().cell_graph.runtime_units) {
      if (unit.runtime_unit_id.value() != config_.runtime_unit_id ||
          unit.runtime_profile_id.value() !=
              deployment::ros2_control_execution_runtime_contract::kRuntimeProfileId) continue;
      // World-free infrastructure needs no robot membership decode. The
      // existing world-free branch below still rejects supplied observer facts.
      if (!unit.configuration.contains("isaac_world")) continue;
      const auto value = deployment::ros2_control_execution_runtime_contract::require_configuration(
          bundle, unit.runtime_unit_id);
      world = value.isaac_world;
      for (const auto &profile : value.controller_profiles) {
        if (!profile.isaac_profile) continue;
        const auto required = retained_isaac_scene_required_topics(*profile.isaac_profile);
        topics.insert(required.begin(), required.end());
      }
    }
    std::optional<IsaacControllerLaunchInputs> inputs;
    std::filesystem::path parameters;
    std::optional<component_manager::IsaacSiteWorld> site_world;
    double startup = 0.0;
    double service = 0.0;
    if (world) {
      Require(config_.isaac_site_settings && config_.isaac_pose_observer_executable &&
                  facts && facts->valid(),
              "named cell world requires supplied site settings and checked observer facts");
      const auto &site = *config_.isaac_site_settings;
      const auto &mapping = site.ResolveWorld(*world);
      site_world = mapping;
      const auto *observer = site.FindPoseObserver(bundle.value().cell_graph.cell_id.value());
      Require(observer != nullptr, "named cell world has no pose observer site mapping");
      startup = site.scene_startup_timeout_s();
      service = site.scene_service_timeout_s();
      Require(std::isfinite(startup) && startup >= 0.1 && startup <= 600.0 &&
                  std::isfinite(service) && service >= 0.1 && service <= 120.0,
              "cell barrier timeouts violate existing barrier ranges");
      Require(!mapping.expected_world_name.empty() && mapping.expected_world_name.size() <= 1024U,
              "cell barrier world name violates its existing byte bound");
      topics.insert(observer->additional_required_topics.begin(),
                    observer->additional_required_topics.end());
      Require(topics.size() <= 64U && std::all_of(topics.begin(), topics.end(),
          [](const auto &topic) { return !topic.empty() && topic.front() == '/' && topic.size() <= 1024U; }),
          "cell barrier required topics violate existing barrier limits");
      const auto barrier = config_.isaac_pose_observer_executable->parent_path() /
                           "isaac_scene_load_barrier";
      Require(::access(barrier.c_str(), X_OK) == 0,
              "installed sibling Isaac scene barrier is not executable");
      inputs = IsaacControllerLaunchInputs{
          mapping.world_uri, mapping.expected_world_name,
          number_text(startup), number_text(service), config_.manager_run_root,
          std::filesystem::path(facts->retained_snapshot_path), {}, true};
    } else {
      Require(!facts, "a world-free cell cannot carry observer launch facts");
    }
    std::optional<std::string> old_world;
    std::optional<Facts> old_facts;
    std::shared_ptr<CellLaunchProcess> old_observer;
    std::optional<IsaacControllerLaunchInputs> old_inputs;
    std::set<std::string> old_topics;
    bool initialized = false;
    {
      std::lock_guard lock(state_mutex_);
      initialized = initialized_;
      old_world = world_;
      old_facts = facts_;
      old_observer = observer_;
      old_inputs = inputs_;
      old_topics = qualified_topics_;
      if (initialized && old_world == world && old_facts == facts) {
        Require(!world || old_observer || observer_start_pending_,
                "cell pose observer is absent; explicit intervention requires a fresh occurrence");
        Require(!old_observer || !old_observer->exit_evidence(),
                "cell pose observer exited; explicit intervention requires a fresh occurrence");
      }
    }
    const bool unchanged_observer = initialized && old_world == world && old_facts == facts;
    if (initialized && old_facts && facts && !unchanged_observer)
      Require(old_facts->source_epoch_base != facts->source_epoch_base,
              "replacement cell observer requires a fresh Manager source epoch base");
    // Cell-only observer metadata is not robot launch identity. Preserve the
    // original robot inputs while the logical world remains unchanged (Q94).
    if (initialized && world && old_world == world) inputs = old_inputs;
    // The caller derives physical Add/Replace/recovery from its immutable
    // plan. Cached topic evidence cannot certify a fresh robot occurrence.
    const bool passive_join = initialized && world && old_world == world &&
        (qualify_existing_world ||
         !std::includes(old_topics.begin(), old_topics.end(), topics.begin(), topics.end()));
    if (passive_join) {
      parameters = barrier_parameters(*facts, *site_world, topics, startup, service, true);
      run_barrier(parameters, *world, *facts, context, startup, service, true);
    }
    if (unchanged_observer) {
      // Keep the current membership's closure, not historical topic evidence:
      // removing then adding the last Isaac robot qualifies its topics again.
      std::lock_guard lock(state_mutex_);
      qualified_topics_ = world ? topics : std::set<std::string>{};
      return old_inputs;
    }
    if (world && old_world != world)
      parameters = barrier_parameters(*facts, *site_world, topics, startup, service, false);
    // Required mappings, checked observer facts and the parameter shape above
    // are validated before retirement. The existing barrier validates its URI.
    // A changed source is started only by finish(), after Manager clears its
    // old source state and activates the already-checked new expectation.
    if (old_observer) {
      old_observer->update_request_context(context);
      Require(host_controller_launch_supervisor().retire_cell_process(old_observer),
              "old cell observer exact leader reap is unknown; replacement not started");
      std::lock_guard lock(state_mutex_);
      retired_observer_ = old_observer;
      observer_.reset();
    }
    if (world && old_world != world)
      run_barrier(parameters, *world, *facts, context, startup, service, false);
    {
      std::lock_guard lock(state_mutex_);
      initialized_ = true;
      world_ = std::move(world);
      facts_ = facts;
      inputs_ = inputs;
      observer_.reset();
      observer_start_pending_ = facts.has_value();
      observer_start_failure_.reset();
      qualified_topics_ = world ? topics : std::set<std::string>{};
    }
    return inputs;
  }

  void finish(const std::optional<Facts> &facts, const std::string &context) {
    std::lock_guard transition_lock(transition_mutex_);
    std::lock_guard lock(state_mutex_);
    Require(initialized_ && facts_ == facts,
            "cell observer Finalize differs from prepared checked facts");
    if (!facts_ || observer_) return;
    Require(observer_start_pending_,
            "cell observer start was already attempted; explicit intervention requires a fresh occurrence" +
                (observer_start_failure_ ? ": " + *observer_start_failure_ : std::string{}));
    const auto scalar = [](const std::string &value) {
      return content_identity::canonical_json(CanonicalValue(value));
    };
    const CellLaunchCommand command{core::CellId(config_.cell_id),
        CellLaunchRole::kPoseObserver, facts_->source_id,
        {config_.isaac_pose_observer_executable->string(), "--ros-args",
         "-p", "site_config_path:=" + scalar(facts_->retained_snapshot_path),
         "-p", "manager_run_root:=" + scalar(config_.manager_run_root.string()),
         "-p", "source_epoch_base:=" + scalar(facts_->source_epoch_base),
         "--disable-external-lib-logs"},
        "cell-observer:" + facts_->source_epoch_base};
    // Consume the one authorized attempt before entering the spawner. Exact
    // Finalize replay must observe failure, never retry an unknown spawn.
    observer_start_pending_ = false;
    try {
      observer_ = host_controller_launch_supervisor().start_cell_process(
          command, config_.execution_run_root, context);
    } catch (const std::exception &error) {
      observer_start_failure_ = error.what();
      throw;
    } catch (...) {
      observer_start_failure_ = "unknown observer spawn failure";
      throw;
    }
    // Do not wait for Open: Manager may be waiting for this host's response.
    // Process spawn alone is not a fresh-pose or Scene-readiness receipt.
  }

  [[nodiscard]] ControllerLaunchSupervisor::HostShutdownSnapshot
  BeginHostShutdown() {
    std::lock_guard lock(state_mutex_);
    return host_controller_launch_supervisor().BeginHostShutdown(
        {observer_, retired_observer_, observer_incident_process_, barrier_});
  }

private:
  static std::string number_text(const double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value;
    auto text = out.str();
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
  }
  void run_barrier(const std::filesystem::path &parameters, const std::string &world,
                   const Facts &facts, const std::string &context,
                   const double startup, const double service, const bool passive) const {
    const auto barrier = config_.isaac_pose_observer_executable->parent_path() /
                         "isaac_scene_load_barrier";
    const CellLaunchCommand command{core::CellId(config_.cell_id),
        CellLaunchRole::kSceneBarrier, std::nullopt,
        {barrier.string(), "--ros-args", "--params-file", parameters.string(),
         "--disable-external-lib-logs"},
        std::string(passive ? "cell-passive-join:" : "cell-world:") + world + ":" +
            facts.source_epoch_base};
    // The normal barrier gives its stopped cleanup two separate calls using
    // the supplied service budget after startup expires. Do not interrupt that
    // existing cleanup with the startup-only bound used by passive joining.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(startup + (passive ? 0.0 : 2.0 * service)));
    std::shared_ptr<CellLaunchProcess> process;
    {
      // Publish the genuine barrier lease before a shutdown cut can complete.
      // No separate cell registry: this is the original in-flight owner.
      std::lock_guard lock(state_mutex_);
      process = host_controller_launch_supervisor().start_cell_process(
          command, config_.execution_run_root, context);
      barrier_ = process;
    }
    while (!process->exit_evidence() && std::chrono::steady_clock::now() < deadline &&
           g_stop_requested == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto exit = process->exit_evidence();
    const bool reaped = host_controller_launch_supervisor().retire_cell_process(process);
    {
      std::lock_guard lock(state_mutex_);
      if (barrier_ == process)
        barrier_.reset();
    }
    Require(exit && exit->exit_status == 0 && !exit->signal && reaped,
            "cell world barrier did not complete with actual exit zero: " +
                (exit ? exit->description : std::string("startup budget elapsed")));
  }
  std::filesystem::path barrier_parameters(
      const Facts &facts, const component_manager::IsaacSiteWorld &world,
      const std::set<std::string> &topics, const double startup, const double service,
      const bool passive) const {
    const auto scalar = [](const std::string &value) {
      return content_identity::canonical_json(CanonicalValue(value));
    };
    CanonicalValue::Array array;
    for (const auto &topic : topics) array.emplace_back(topic);
    const std::string bytes = "manyforge_isaac_scene_load_barrier:\n  ros__parameters:\n"
        "    world_uri: " + scalar(world.world_uri) + "\n    expected_world_name: " +
        scalar(world.expected_world_name) + "\n    passive_join: " + (passive ? "true" : "false") +
        "\n    startup_timeout_s: " + number_text(startup) +
        "\n    service_timeout_s: " + number_text(service) + "\n    required_topics: " +
        content_identity::canonical_json(CanonicalValue(std::move(array))) + "\n";
    // One bounded parameter artifact per checked-input attempt/world config;
    // exact replay reuses these bytes. No polling writer, overwrite or cleanup.
    const auto path = std::filesystem::path(facts.retained_snapshot_path).parent_path() /
        (std::filesystem::path(facts.retained_snapshot_path).filename().string() +
         ".world-" + content_identity::sha256_digest(bytes).substr(7U) + ".yaml");
    FileDescriptor file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.get() < 0 && errno == EEXIST) {
      FileDescriptor existing(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
      if (existing.get() < 0) FailErrno("open retained cell barrier parameters");
      struct stat status {};
      Require(::fstat(existing.get(), &status) == 0 && S_ISREG(status.st_mode) &&
                  status.st_uid == ::geteuid() && (status.st_mode & 0777) == 0600 &&
                  status.st_size >= 0 && static_cast<std::uint64_t>(status.st_size) == bytes.size(),
              "retained cell barrier parameter file differs on exact replay");
      std::string captured(bytes.size(), '\0');
      std::size_t read_offset = 0U;
      while (read_offset < captured.size()) {
        const auto count = ::read(existing.get(), captured.data() + read_offset,
                                  captured.size() - read_offset);
        if (count < 0 && errno == EINTR) continue;
        Require(count > 0, "retained cell barrier parameters are incomplete");
        read_offset += static_cast<std::size_t>(count);
      }
      Require(captured == bytes, "retained cell barrier parameters differ on exact replay");
      return path;
    }
    if (file.get() < 0) FailErrno("create retained cell barrier parameters");
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
      const auto written = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) FailErrno("write retained cell barrier parameters");
      offset += static_cast<std::size_t>(written);
    }
    std::cerr << "manyforge cell barrier parameters: path=" << path
              << " bytes=" << bytes.size() << " snapshot=" << facts.retained_snapshot_path << '\n';
    return path;
  }
  const ManagedRuntimeChildConfig &config_;
  std::mutex transition_mutex_;
  mutable std::mutex state_mutex_;
  bool initialized_{false};
  std::optional<std::string> world_;
  std::optional<Facts> facts_;
  std::optional<IsaacControllerLaunchInputs> inputs_;
  std::shared_ptr<CellLaunchProcess> observer_;
  mutable std::shared_ptr<CellLaunchProcess> barrier_;
  std::shared_ptr<CellLaunchProcess> retired_observer_;
  std::shared_ptr<CellLaunchProcess> observer_incident_process_;
  std::optional<core::CellObserverIncidentRecordDTO> observer_incident_record_;
  std::optional<component_manager::ManagerCellObserverObservationDTO>
      observer_incident_observation_;
  bool observer_incident_slot_qualified_{false};
  bool observer_start_pending_{false};
  std::optional<std::string> observer_start_failure_;
  std::set<std::string> qualified_topics_;
};

[[nodiscard]] int
RunManagedRuntimeChild(const ManagedRuntimeChildConfig &config) {
  using ShutdownPhase = component_manager::detail::NativeShutdownPhase;
  using component_manager::detail::NativeShutdownOwnerPhase;
  component_manager::detail::NativeShutdownProjection shutdown_projection(
      config.runtime_epoch);
  // These outlive every borrowed host owner and the SAME servicing lane.
  std::mutex manager_liveness_failure_mutex;
  std::exception_ptr manager_liveness_failure;
  std::mutex manager_liveness_servicing_mutex;
  bool teardown_only = false; // protected by servicing mutex
  std::atomic<bool> shutdown_status_servicing{false};
  std::jthread manager_liveness_lane;
  NativeShutdownOwnerPhase liveness_lane_destructor_phase{
      shutdown_projection, ShutdownPhase::kServicingLaneDrain,
      manager_liveness_lane};
  const auto offer_digest = SelectedExecutionOfferDigest();
  const std::string ready_packet =
      "manyforge.native-execution-ready.v2 " + config.runtime_unit_id + " " +
      config.runtime_epoch + " " + offer_digest + " " +
      config.product_identity + " " + config.configuration_identity;
  std::uint64_t source_report_sequence = 0U;
  ManagedKernelAdmissionGate admission_gate;
  // Set once to the main-scope cell owner before managed requests are served.
  // Product installations retain this Kernel; the fence callback never looks
  // up a replaceable component composition or calls back into the fence owner.
  core::ResourceAddressedExecutionKernel *stable_kernel_owner{};
  ManagedKernelApplyFenceOwner fence_owner(
      ManagedKernelApplyFenceOwnerConfig{
          config.execution_state_root / "kernel-apply-fence.sqlite3",
          config.manager_state_root,
          config.package_root,
          core::CellId(config.cell_id),
          core::RuntimeUnitId(config.runtime_unit_id),
          core::RuntimeEpoch(config.runtime_epoch),
          core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
          [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
          },
          config.program_artifact_root,
          [&](const core::CellAdmissionRecoveryReceiptQueryDTO &query) {
            if (stable_kernel_owner == nullptr) {
              return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
                  {"selected_kernel_owner_unavailable", "cell Kernel is not installed", {}});
            }
            // The managed owner already holds the admission exclusion. This
            // existing read takes only the claim owner's mutex, preserving
            // fence -> admission -> claim order and the original Kernel life.
            return stable_kernel_owner->GetCellAdmissionRecoveryReceipt(query);
          },
      },
      admission_gate);
  NativeShutdownOwnerPhase fence_destructor_phase{
      shutdown_projection, ShutdownPhase::kFenceOwnerDestruction, fence_owner};
  const ManagedProjectionRegistryConfig projection_config{
      config.package_root,       config.program_artifact_root,
      config.execution_run_root, config.runtime_unit_id,
      config.runtime_epoch,      config.scene_asset_root_fd};
  ManagedProjectionRegistry projections(
      projection_config,
      MakeFilesystemManagedProjectionAssetSource(projection_config),
      [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
      });
  NativeShutdownOwnerPhase projection_destructor_phase{
      shutdown_projection, ShutdownPhase::kProjectionDestruction, projections};
  ManagedIsaacCellProcesses managed_isaac_cell(config);
  NativeShutdownOwnerPhase cell_destructor_phase{
      shutdown_projection, ShutdownPhase::kCellProcessesDestruction,
      managed_isaac_cell};
  const auto prepare_managed_cell_world = [&](const deployment::RetainedBundle &bundle,
      const std::optional<component_manager::ManagerIsaacPoseObserverLaunchFactsDTO> &facts,
      const std::string &context, const bool qualify_existing_world = false) {
    return managed_isaac_cell.prepare(bundle, facts, context, qualify_existing_world);
  };
  const auto finish_managed_cell_observer = [&](const auto &facts, const std::string &context) {
    managed_isaac_cell.finish(facts, context);
  };
  std::mutex managed_composition_mutex;
  // Bounded observation history on this host owner, not Composition pins.
  // Replacing a composition/Apply never depends on reading these receipts.
  struct ComponentProcessStopResultEntry final {
    component_manager::ManagerComponentProcessStopResultDTO result;
    std::uint64_t insertion_order;
  };
  std::map<std::string, ComponentProcessStopResultEntry> component_process_stop_results;
  std::uint64_t component_process_stop_result_order{};
  bool component_process_stop_retention_exhaustion_logged{};
  // A failed fenced installation may already have re-rooted retained
  // participants toward the private candidate. Keep that candidate alive
  // until after the old active/retired views are destroyed; discarding it
  // would strand non-owning root and Stop-reader pointers while the Apply
  // remains command-closed.
  std::unique_ptr<Composition> failed_prepared_composition;
  NativeShutdownOwnerPhase failed_destructor_phase{
      shutdown_projection, ShutdownPhase::kFailedCompositionDestruction,
      failed_prepared_composition};
  std::unique_ptr<Composition> active_managed_composition;
  NativeShutdownOwnerPhase active_destructor_phase{
      shutdown_projection, ShutdownPhase::kActiveCompositionDestruction,
      active_managed_composition};
  std::unique_ptr<Composition> retired_managed_composition;
  NativeShutdownOwnerPhase retired_destructor_phase{
      shutdown_projection, ShutdownPhase::kRetiredCompositionDestruction,
      retired_managed_composition};
  // Snapshot engine identities separately: the runtime registration vector
  // remains authoritative and may be shared with a future active wrapper.
  std::set<core::IResolvedParticipantExecutionEngine *>
      retired_registration_engines;
  std::unique_ptr<Composition> prepared_managed_composition;
  NativeShutdownOwnerPhase prepared_destructor_phase{
      shutdown_projection, ShutdownPhase::kPreparedCompositionDestruction,
      prepared_managed_composition};
  bool prepared_managed_composition_is_empty = false;
  // A Program/Scene-only Apply keeps the exact component owners. Preparation
  // verifies the candidate product and projections without constructing
  // replacement controllers; installation refreshes only their publication
  // views beneath the Manager fence.
  std::optional<CompositionBuildInput> prepared_retained_view;
  std::shared_ptr<WallClock> detached_owner_clock;
  std::shared_ptr<SimpleScheduler> detached_owner_scheduler;
  NativeShutdownOwnerPhase scheduler_destructor_phase{
      shutdown_projection, ShutdownPhase::kSchedulerOwnerRelease,
      detached_owner_scheduler};
  // The cell Kernel's recovery resolver captures these by reference, so they
  // must outlive the cell owner and its Kernel at local-scope teardown.
  std::atomic<RetainedNativeRuntime *> stop_recovery_runtime{nullptr};
  std::atomic<core::ResourceAddressedExecutionKernel *> stop_recovery_kernel{
      nullptr};
  // The stable addressed Kernel borrows this observer from construction until
  // destruction, including intervals with no installed component. Declare
  // it before the Kernel so destruction retires the Kernel first.
  std::shared_ptr<RetainedStopSurveillanceOwner>
      stable_stop_surveillance_owner;
  NativeShutdownOwnerPhase surveillance_destructor_phase{
      shutdown_projection, ShutdownPhase::kStopSurveillanceDestruction,
      stable_stop_surveillance_owner};
  std::unique_ptr<RetainedNativeCellRuntime> stable_cell_runtime_owner;
  NativeShutdownOwnerPhase cell_kernel_destructor_phase{
      shutdown_projection, ShutdownPhase::kKernelDestruction,
      stable_cell_runtime_owner};
  std::shared_ptr<RetainedObservationWake> stable_observation_wake;
  std::unique_ptr<core::SqliteOperationJournal> detached_operation_journal;
  std::unique_ptr<core::ResourceAddressedExecutionKernel>
      detached_addressed_kernel;
  NativeShutdownOwnerPhase detached_kernel_destructor_phase{
      shutdown_projection, ShutdownPhase::kKernelDestruction,
      detached_addressed_kernel};
  // The stable Kernel owns the Stop latch across product reconfiguration.
  // Command dispatch already pins the active composition with its shared
  // lifetime lock, so this atomic selects only that same current product's
  // facts adapter; it is not a second recovery owner.
  core::SqliteOperationJournal *stable_operation_journal{};
  const auto read_current_publication = [&] {
    component_manager::SqliteActiveBundlePublicationStore publication_store(
        config.manager_state_root /
            std::string(
                component_manager::kActiveBundlePublicationStoreFilename),
        core::CellId(config.cell_id));
    const auto integrity = publication_store.VerifyIntegrity();
    Require(integrity.current_generation.nonzero(),
            "managed active retained-bundle publication is absent");
    auto publication = publication_store.ReadActiveBundlePublication(
        integrity.current_generation);
    publication_store.Close();
    Require(publication.ok() && publication.value().valid(),
            "managed active retained-bundle publication cannot be verified");
    return std::move(publication.value());
  };
  auto bootstrap_retained_bundle_id = config.retained_bundle_id;
  auto bootstrap_graph_revision = config.graph_revision;
  if (!config.program_cycle_journal.empty()) {
    const auto retained_run =
        detail::ReadActiveProgramRunProductForReconciliation(
            config.program_cycle_journal);
    if (!retained_run.ok()) {
      Fail(retained_run.error().message);
    }
    if (retained_run.value().has_value()) {
      bootstrap_retained_bundle_id = retained_run.value()->retained_bundle_id;
      bootstrap_graph_revision = retained_run.value()->graph_revision;
    }
  }
  detached_owner_clock = std::make_shared<WallClock>();
  detached_owner_scheduler = std::make_shared<SimpleScheduler>(
      SimpleSchedulerConfig::from_environment());
  // Keep the existing stable scheduling substrate through product exchanges.
  // The first Background-role startup belongs to construction, never status.
  const auto controller_cleanup_scheduler = detached_owner_scheduler;
  const auto controller_cleanup_clock = detached_owner_clock;
  static_cast<void>(controller_cleanup_scheduler->schedule(
      controller_cleanup_clock->now(), [] {},
      core::SchedulerPriority::Background));
  core::ResourceAddressedExecutionKernelConfig bootstrap_kernel_config{
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::CellId(config.cell_id),
      1024U,
      {},
      &admission_gate,
      std::nullopt};
  bootstrap_kernel_config.empty_runtime_retained_bundle_id =
      bootstrap_retained_bundle_id;
  bootstrap_kernel_config.empty_runtime_graph_revision =
      bootstrap_graph_revision;
  bootstrap_kernel_config.prequalified_max_live_stop_participants =
      kNativeMaximumLiveStopParticipants;
  auto bootstrap_stop = LoadManagedBootstrapStopProduct(
      config, bootstrap_retained_bundle_id, bootstrap_graph_revision);
  const auto bootstrap_cell = LoadManagedBundleAtPinnedRoot(config, bootstrap_retained_bundle_id);
  Require(bootstrap_cell && bootstrap_cell->bundle,
          "cell-world bootstrap has no verified retained bundle");
  const std::string cell_bootstrap_context = "Bootstrap runtime=" + config.runtime_unit_id +
                                           " epoch=" + config.runtime_epoch;
  const auto bootstrap_isaac_inputs = prepare_managed_cell_world(
      *bootstrap_cell->bundle, config.isaac_pose_observer_launch_facts, cell_bootstrap_context);
  finish_managed_cell_observer(config.isaac_pose_observer_launch_facts, cell_bootstrap_context);
  const bool bootstrap_requires_physical_composition =
      bootstrap_stop.requires_physical_composition;
  bootstrap_kernel_config.empty_runtime_resource_inventory =
      std::move(bootstrap_stop.resource_inventory);
  bootstrap_kernel_config.stop_fault_recovery_facts_resolver =
      [&](const core::StopStatusDTO &status, const std::int64_t assessed_at_ns)
      -> core::Result<core::StopFaultRecoveryFactsDTO> {
    auto *const runtime = stop_recovery_runtime.load(std::memory_order_acquire);
    auto *const kernel = stop_recovery_kernel.load(std::memory_order_acquire);
    if (runtime == nullptr && stable_cell_runtime_owner && kernel) {
      return build_retained_cell_stop_recovery_facts(*stable_stop_surveillance_owner, *kernel,
          *stable_cell_runtime_owner->clock(), core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
          core::CellId(config.cell_id), &admission_gate, status, assessed_at_ns);
    }
    if (runtime == nullptr || kernel == nullptr) {
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kMissingReference)),
          "managed native Stop recovery owner is unavailable",
          {{"reason", "managed_native_stop_recovery_owner_unavailable"}}});
    }
    return build_retained_native_stop_recovery_facts(
        *runtime, *kernel, &admission_gate, status, assessed_at_ns);
  };
  bootstrap_kernel_config.cell_admission_claim_capacity = 1024U;
  // The managed cell hosts the Headless Alarm Lifecycle Owner: Run/Resume
  // and motion-producing recovery evaluate its published attention. The
  // control plane is single-host: only projections from this host boot are
  // accepted, so owner incarnations are comparable (spec 530 §11).
  bootstrap_kernel_config.operator_alarm_attention =
      core::OperatorAlarmAttentionPolicyDTO{
          core::kOperatorAlarmAttentionMaximumSnapshotAgeNs,
          core::ReadHostBootStamp().boot_id};
  bootstrap_kernel_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.program_cycle_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.logical_scene_database, config.program_cycle_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  RetainedNativeCellRuntimeInput cell_input{
      .clock = detached_owner_clock,
      .scheduler = detached_owner_scheduler,
      .operation_journal_path =
          config.execution_state_root / std::string(core::kOperationJournalFilename),
      .participant_registrations = {},
      .kernel_config = std::move(bootstrap_kernel_config),
      .stop_sources = {}};
  stable_cell_runtime_owner = std::make_unique<RetainedNativeCellRuntime>(std::move(cell_input));
  stable_stop_surveillance_owner = stable_cell_runtime_owner->stop_surveillance_owner();
  stable_observation_wake = stable_cell_runtime_owner->observation_wake();
  stable_kernel_owner = &stable_cell_runtime_owner->addressed_kernel();
  stable_operation_journal = &stable_cell_runtime_owner->operation_journal();
  Require(stable_kernel_owner != nullptr && stable_operation_journal != nullptr,
          "managed native cell owner could not be created");

  if (bootstrap_requires_physical_composition) {
    Require(stable_kernel_owner->SetRouteRecoveryPending(true).ok(),
            "selected physical runtime could not close admission before "
            "composition initialization");
  }
  const auto bind_cell_runtime_to_composition =
      [&](Composition &composition) {
        composition.cell_runtime = stable_cell_runtime_owner.get();
        std::vector<std::shared_ptr<RetainedNativeGripperRuntime>>
            independent_grippers;
        if (composition.gripper_runtime != nullptr) {
          composition.gripper_runtime->BindCellKernel(*stable_kernel_owner);
          independent_grippers.push_back(composition.gripper_runtime);
        }
        for (const auto &participant : composition.participant_compositions) {
          Require(participant != nullptr,
                  "native cell has a missing component composition");
          participant->cell_runtime = stable_cell_runtime_owner.get();
          if (participant->gripper_runtime != nullptr) {
            participant->gripper_runtime->BindCellKernel(*stable_kernel_owner);
            independent_grippers.push_back(participant->gripper_runtime);
          }
          if (participant->runtime != nullptr)
            participant->runtime->cell_runtime_owner =
                stable_cell_runtime_owner.get();
        }
        if (composition.runtime != nullptr) {
          composition.runtime->cell_runtime_owner =
              stable_cell_runtime_owner.get();
          composition.runtime->BindIndependentGripperPeers(
              composition.runtime, std::move(independent_grippers));
        }
      };
  stop_recovery_kernel.store(stable_kernel_owner, std::memory_order_release);
  admission_gate.BindResourceTable(
      stable_kernel_owner->resource_admission_table());
  component_manager::detail::ManagerLivenessCommandAdmission
      manager_liveness_admission;
  component_manager::detail::ChildOwnedManagerLivenessReceiver
      manager_liveness_receiver(std::make_shared<WallClock>(),
                                std::make_unique<SimpleScheduler>(
                                    SimpleSchedulerConfig::from_environment()));
  NativeShutdownOwnerPhase receiver_destructor_phase{
      shutdown_projection, ShutdownPhase::kLivenessReceiverDestruction,
      manager_liveness_receiver};
  std::atomic<bool> manager_liveness_installed{false};
  std::optional<core::ManagerLivenessReceiverConfig>
      manager_liveness_receiver_config;
  std::optional<core::ActiveBundlePublicationGeneration>
      manager_liveness_publication_generation;
  std::optional<core::ContentDigest> manager_liveness_public_pointer_digest;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
  std::set<std::pair<std::string, std::string>> retired_manager_epochs;
  ExecutionCompositionAccess execution_composition;
  execution_composition.addressed_kernel = stable_kernel_owner;
  bool current_composition_building = false;
  bool current_predecessor_composition_pending = false;
  ManagedCellCCLCLifetime managed_cell_cclc;
  // Main-channel ownership; child callbacks retain a copy of this immutable
  // capture rather than reading a replaceable shared continuation variable.
  std::shared_ptr<const ManagedOriginalApplyBringUpCapture>
      original_apply_bring_up_capture;
  std::optional<core::NativeApplyComponentBringUpDTO>
      original_apply_accepted_children;
  std::optional<core::NativeApplyPlanDTO> original_apply_native_plan;
  if (bootstrap_cell->bundle->value().cell_graph.controllable_components.empty()) {
    const auto bootstrap_publication = read_current_publication();
    managed_cell_cclc.CreateEmpty(
        config, bootstrap_cell->bundle, bootstrap_publication,
        *stable_kernel_owner, fence_owner, detached_owner_clock,
        detached_owner_scheduler);
  }
  // The main managed channel serializes its direct reads. Recreation must
  // separately stop all asynchronous ingress before destroying this owner.
  const auto current_cell_cclc = [&]() -> deployment::CCLCRuntimeComposition * {
    if (managed_cell_cclc.facade) return managed_cell_cclc.facade.get();
    auto *const active = active_managed_composition.get();
    if (active && active->cclc_runtime) return active->cclc_runtime.get();
    auto *const retired = retired_managed_composition.get();
    return retired ? retired->cclc_runtime.get() : nullptr;
  };

  ManagedExecutionSurfaceControl execution_surface_control(
      config.execution_run_root, &execution_composition,
      &manager_liveness_admission,
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch));
  std::jthread execution_surface([&](const std::stop_token stop_token) {
    RunManagedExecutionCommandSurface(execution_surface_control, stop_token);
  });
  {
    std::unique_lock lock(execution_surface_control.state_mutex);
    const auto signaled = execution_surface_control.state_changed.wait_for(
        lock, std::chrono::seconds(5), [&] {
          return execution_surface_control.ready ||
                 execution_surface_control.failure != nullptr;
        });
    Require(signaled, "managed execution command surface startup timed out");
    if (execution_surface_control.failure != nullptr) {
      std::rethrow_exception(execution_surface_control.failure);
    }
    Require(execution_surface_control.ready,
            "managed execution command surface did not become ready");
  }
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      preparing_composition_request;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      installed_under_fence_composition_request;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      installed_composition_request;
  std::vector<component_manager::ManagerRuntimeComponentReceiptDTO>
      installed_component_receipts;
  std::atomic<std::shared_ptr<
      const component_manager::ManagerSelectedRuntimeCompositionRequestDTO>>
      installed_liveness_handoff;
  const auto confirm_installed_projection_set = [&](const auto &request) {
    auto bundle = active_managed_composition != nullptr
                      ? active_managed_composition->bundle
                      : std::shared_ptr<const deployment::RetainedBundle>{};
    if (bundle == nullptr ||
        bundle->retained_bundle_id() !=
            request.candidate_projection_revision.retained_bundle_id) {
      const auto selected = LoadManagedBundleAtPinnedRoot(
          config, request.candidate_projection_revision.retained_bundle_id);
      Require(selected.has_value(),
              "installed projection provider set has no verified bundle");
      bundle = selected->bundle;
    }
    Require(bundle->value().cell_graph.graph_revision ==
                request.candidate_projection_revision.graph_revision,
            "installed projection provider set has a different graph");
    projections.ConfirmInstalledProjectionSet(*bundle);
  };
  const auto publish_installed_liveness_handoff = [&](const auto &request) {
    installed_liveness_handoff.store(
        std::make_shared<const component_manager::
                             ManagerSelectedRuntimeCompositionRequestDTO>(
            request),
        std::memory_order_release);
  };
  std::exception_ptr managed_composition_failure;
  std::jthread current_composition_builder;
  bool current_composition_builder_started = false;
  std::jthread managed_composition_builder;
  bool managed_composition_building = false;
  std::exception_ptr selected_composition_failure;

  const auto stable_addressed_kernel = [&]() -> core::ResourceAddressedExecutionKernel* {
    return stable_kernel_owner;
  };

  const auto await_manager_loss_containment = [&] {
    if (!manager_liveness_installed.load(std::memory_order_acquire)) {
      return;
    }
    while (g_stop_requested == 0 &&
           manager_liveness_receiver.get()->status().state !=
               core::ManagerLivenessStateDTO::kExpired) {
      int waited = -1;
      do {
        waited = ::poll(nullptr, 0U, 25);
      } while (waited < 0 && errno == EINTR && g_stop_requested == 0);
      Require(waited >= 0, "Manager-loss containment wait failed");
    }
  };

  const auto snapshot_registration_engines = [](const Composition &composition) {
    std::set<core::IResolvedParticipantExecutionEngine *> engines;
    const auto include = [&](const Composition &component) {
      if (component.runtime != nullptr) {
        for (const auto &registration :
             component.runtime->participant_registrations) {
          Require(registration.engine != nullptr,
                  "composition registration has no execution engine");
          engines.insert(registration.engine);
        }
      }
      if (component.gripper_runtime != nullptr) {
        const auto registration = component.gripper_runtime->registration();
        Require(registration.engine != nullptr,
                "gripper composition has no execution engine");
        engines.insert(registration.engine);
      }
    };
    include(composition);
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "composition has a missing component owner");
      include(*participant);
    }
    Require(!engines.empty(), "physical composition has no execution engines");
    return engines;
  };

  const auto try_release_retired_composition = [&] {
    std::unique_lock lock(managed_composition_mutex);
    auto *const owner = stable_addressed_kernel();
    if (owner == nullptr || installed_under_fence_composition_request.has_value()) {
      return;
    }
    if (retired_managed_composition == nullptr) {
      Require(retired_registration_engines.empty(),
              "retired engine snapshot has no retired composition");
      projections.ReclaimRetiredProjections();
      return;
    }
    std::set<core::IResolvedParticipantExecutionEngine *> active_engines;
    if (active_managed_composition != nullptr) {
      active_engines = snapshot_registration_engines(*active_managed_composition);
    }
    for (auto *engine : retired_registration_engines) {
      if (active_engines.contains(engine)) {
        continue;
      }
      if (owner->participant_engine_in_use(engine)) {
        return;
      }
    }
    const auto *const retiring = retired_managed_composition.get();
    std::vector<std::shared_ptr<ControllerLaunchProcess>> removed_processes;
    std::set<const ControllerLaunchProcess *> active_processes;
    if (active_managed_composition) {
      for (const auto &[id, view] :
           IndexNativeComponentCompositions(*active_managed_composition)) {
        static_cast<void>(id);
        if (view->controller_process)
          active_processes.insert(view->controller_process.get());
      }
    }
    for (const auto &[id, view] :
         IndexNativeComponentCompositions(*retired_managed_composition)) {
      static_cast<void>(id);
      if (view->controller_process &&
          !active_processes.contains(view->controller_process.get()))
        removed_processes.push_back(view->controller_process);
    }
    // The same Manager channel owns composition replacement. Keep its
    // retired Composition, model and engine pins until exact group cleanup
    // completes; operational Stop does not wait on this mutex.
    lock.unlock();
    for (const auto &process : removed_processes)
      static_cast<void>(
          host_controller_launch_supervisor().retire_exact(process));
    lock.lock();
    // Only Manager dispatch mutates the retired owner/fence; the cell owner is stable.
    Require(retired_managed_composition.get() == retiring &&
                stable_addressed_kernel() == owner &&
                !installed_under_fence_composition_request,
            "retired composition changed during exact process cleanup");
    for (auto engine = retired_registration_engines.begin();
         engine != retired_registration_engines.end();) {
      if (active_engines.contains(*engine)) {
        ++engine;
        continue;
      }
      if (!owner->release_retired_engine(*engine).ok()) {
        return;
      }
      engine = retired_registration_engines.erase(engine);
    }
    retired_registration_engines.clear();
    retired_managed_composition.reset();
    projections.ReclaimRetiredProjections();
  };

  const auto rethrow_composition_failure = [&] {
    std::exception_ptr failure;
    {
      std::lock_guard lock(managed_composition_mutex);
      failure = managed_composition_failure;
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
    {
      std::lock_guard lock(execution_surface_control.state_mutex);
      failure = execution_surface_control.failure;
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  };

  const auto try_start_current_composition = [&] {
    {
      std::lock_guard lock(managed_composition_mutex);
      if (active_managed_composition != nullptr ||
          current_composition_builder_started ||
          preparing_composition_request.has_value() ||
          installed_under_fence_composition_request.has_value() ||
          installed_composition_request.has_value()) {
        return;
      }
    }
    bool predecessor_pending = false;
    auto input = TryBuildCurrentManagedCompositionInput(
        config, projections, admission_gate, predecessor_pending);
    {
      std::lock_guard lock(managed_composition_mutex);
      current_predecessor_composition_pending = predecessor_pending;
    }
    if (!input.has_value()) {
      return;
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      if (active_managed_composition != nullptr ||
          current_composition_builder_started ||
          preparing_composition_request.has_value() ||
          installed_under_fence_composition_request.has_value() ||
          installed_composition_request.has_value()) {
        return;
      }
      input->attach_addressed_owner = false;
      input->shared_clock = detached_owner_clock;
      input->shared_scheduler = detached_owner_scheduler;
      input->observation_wake = stable_observation_wake;
      input->managed_isaac_launch_inputs = bootstrap_isaac_inputs;
      current_predecessor_composition_pending = false;
      current_composition_builder_started = true;
      Require(stable_kernel_owner->SetRouteRecoveryPending(true).ok(),
              "initial runtime composition could not close admission");
      current_composition_building = true;
    }
    current_composition_builder =
        std::jthread([&, input = std::move(*input)]() mutable {
          try {
            std::cerr << "native composition phase: materializing runtime\n";
            auto built = BuildProductComposition(std::move(input));
            std::cerr << "native composition phase: runtime materialized\n";
            InstallDirectSceneFrameResolver(projections, *built);
            Composition *installed = nullptr;
            {
              std::lock_guard lock(managed_composition_mutex);
              std::unique_lock execution_lock(execution_composition.mutex);
              Require(stable_kernel_owner != nullptr &&
                          stable_operation_journal != nullptr &&
                          detached_owner_clock != nullptr &&
                          detached_owner_scheduler != nullptr,
                      "current managed composition has no stable cell owner");
              Require((built->runtime || built->gripper_runtime) &&
                          (!built->runtime || (!built->runtime->addressed_kernel &&
                           !built->runtime->operation_journal &&
                           built->runtime->clock == detached_owner_clock &&
                           built->runtime->scheduler == detached_owner_scheduler)),
                      "current managed composition duplicated the stable addressed owner");
              auto current_registrations = NativeCellComponentRegistrations(*built);
              auto current_resolver = NativeCellParticipantResolver(*built, current_registrations);
              auto current_stop_sources = NativeCellStopSources(*built);
              static_cast<void>(LoadManagedBootstrapStopProduct(config, built->bundle->retained_bundle_id(),
                  built->bundle->value().cell_graph.graph_revision));
              auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_product(
                  std::move(current_registrations), std::move(current_resolver), std::move(current_stop_sources));
              if (!reconfigured.ok()) Fail("stable Kernel rejected current runtime participants: " + reconfigured.error().message);
              if (built->runtime) built->runtime->stop_surveillance_owner = stable_stop_surveillance_owner;
              built->controller_launch_adopted = true;
              for (auto &peer : built->participant_compositions)
                peer->controller_launch_adopted = true;
              active_managed_composition = std::move(built);
              if (active_managed_composition->runtime != nullptr)
                active_managed_composition->runtime->BindCellRuntimeOwner(
                    *stable_cell_runtime_owner, config.runtime_epoch, config.cell_id);
              bind_cell_runtime_to_composition(*active_managed_composition);
              execution_composition.replace_current(
                  active_managed_composition.get());
              execution_composition.addressed_kernel =
                  active_managed_composition->active_addressed_kernel();
              stop_recovery_runtime.store(
                  active_managed_composition->runtime.get(),
                  std::memory_order_release);
              installed = active_managed_composition.get();
            }
            std::cerr << "native composition phase: stable Kernel bound\n";
            // Initial CCLC recovery is part of composing a usable runtime.  It
            // runs on the existing builder lane so the ordinary Manager
            // command channel remains responsive and cannot advertise an
            // installed composition before its exact routes are current.
            const auto routes_attached =
                AttachCurrentPublishedCCLCIfReady(*installed, config, fence_owner);
            std::cerr << "native composition phase: control routes "
                      << (routes_attached ? "attached" : "deferred") << '\n';
            const auto attached_head = routes_attached
                ? std::optional(read_current_publication()) : std::nullopt;
            {
              std::lock_guard lock(managed_composition_mutex);
              Require(active_managed_composition.get() == installed,
                      "initial managed composition changed during CCLC "
                      "recovery");
              if (routes_attached) {
                Require(!managed_cell_cclc.facade && attached_head,
                        "initial runtime cannot construct a second cell CCLC owner");
                managed_cell_cclc.TakeAttached(*installed, *attached_head);
              }
              current_composition_building = false;
            }
          } catch (...) {
            std::lock_guard lock(managed_composition_mutex);
            managed_composition_failure = std::current_exception();
            current_composition_building = false;
          }
        });
  };

  // A copy of the fresh capture is retained in each actual accepted runtime.
  // Every callback rereads current authority before entering the existing
  // in-process fence pin. A stored plan cannot reconstruct this closure.
  [[maybe_unused]] const auto make_original_apply_services =
      [&](std::shared_ptr<const ManagedOriginalApplyBringUpCapture> captured,
          const core::ApplyComponentBringUpAuthorityDTO &authority)
      -> core::ComponentLifecycleCoordinatorApplyBringUpServices {
    Require(captured && captured->original_facade && authority.valid() &&
                authority.parent_claim == captured->parent_claim &&
                authority.parent == core::ApplyComponentBringUpReferenceDTO{
                    {captured->sealed_plan.plan.apply_id, captured->sealed_plan.plan.apply_generation},
                    captured->sealed_plan.plan_digest} &&
                managed_cell_cclc.facade.get() == captured->original_facade &&
                managed_cell_cclc.active_publication_reader,
            "original Apply services lack their actual fresh owner capture");
    core::ComponentLifecycleCoordinatorApplyBringUpServices services{
        {}, managed_cell_cclc.active_publication_reader.get(),
        captured->sealed_plan.plan.expected_publication, captured->predecessor_publication};
    services.with_original_live_parent =
        [&, captured = std::move(captured), authority](
            const core::ComponentLifecycleStatusDTO &status,
            const std::function<core::Result<void>()> &action) -> core::Result<void> {
      try {
        Require(action && status.locally_valid() && status.apply_bring_up &&
                    !status.selected_bring_up && !status.reconciliation_required &&
                    status.apply_bring_up->parent_authority == authority &&
                    status.status_owner_epoch == status.accepted_intent.request_context.request_owner_epoch,
                "delegated callback lost its original child authority");
        const auto &plan = captured->sealed_plan.plan;
        const auto child = std::find_if(plan.children.begin(), plan.children.end(),
            [&](const auto &row) { return row.intent == status.accepted_intent; });
        Require(child != plan.children.end() &&
                    child->intent.request_context.request_owner_epoch ==
                        captured->original_facade->coordinator().request_owner_epoch() &&
                    status.apply_bring_up->original_request_payload_digest ==
                        core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                            child->intent, child->candidate_projection_revision,
                            child->candidate_retained_bundle_digest, child->candidate_retained_bundle_bytes,
                            child->runtime_unit_id, child->runtime_epoch, child->manager_runtime_epoch,
                            authority.parent),
                "delegated callback does not name its sealed original child");
        // Journal and claim reads run before the fence/composition locks.
        const auto parent = component_manager::ReadManagerApplyBringUpStatusReadOnly(
            config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
            core::CellId(config.cell_id), captured->parent_claim.intent);
        Require(parent.ok() && parent.value().bring_up_plan == captured->sealed_plan &&
                    !parent.value().terminal_outcome && parent.value().fence_acceptance &&
                    parent.value().affected_scope &&
                    parent.value().affected_scope->affected_scope.scope == authority.held_fence_query.expected_scope,
                "delegated callback lost its exact current immutable parent");
        const auto require_current_manager_and_claim = [&] {
          Require(manager_liveness_installed.load(std::memory_order_acquire) &&
                      manager_liveness_receiver.get(),
                  "delegated callback has no installed Manager observation");
          const auto manager = manager_liveness_receiver.get()->status();
          Require(manager.state == core::ManagerLivenessStateDTO::kCurrent && !manager.command_fenced &&
                      manager.manager_owner_epoch == captured->parent_claim.intent.owner_epoch &&
                      manager.manager_runtime_epoch == child->manager_runtime_epoch &&
                      manager.receiver_runtime_unit_id == child->runtime_unit_id &&
                      manager.receiver_runtime_epoch == child->runtime_epoch,
                  "delegated callback lost its actual current Manager/host occurrence");
          const core::CellAdmissionRecoveryReceiptQueryDTO query{
              captured->parent_claim.intent, captured->parent_claim, std::nullopt};
          const auto held = stable_kernel_owner->GetCellAdmissionRecoveryReceipt(query);
          Require(held.ok() && core::ValidateCellAdmissionRecoveryReceipt(held.value()).ok() &&
                      core::ValidateCellAdmissionRecoveryReceiptQuery(query).ok() &&
                      held.value().matched_claim == std::optional(captured->parent_claim) &&
                      held.value().status == core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact,
                  "delegated callback lost its actual full held parent claim");
        };
        require_current_manager_and_claim();
        const auto head = read_current_publication();
        core::Result<void> result = core::Result<void>::Err(
            {"apply_bring_up_parent_not_invoked", "original parent action was not invoked", {}});
        const auto pinned_action = [&] {
          require_current_manager_and_claim();
          std::lock_guard composition_lock(managed_composition_mutex);
          Require(managed_cell_cclc.facade.get() == captured->original_facade &&
                      managed_cell_cclc.publication == captured->predecessor_publication,
                  "delegated callback lost its actual facade/dependency lifetime");
          if (status.phase != core::ComponentLifecycleTransactionPhaseDTO::kAccepted) {
            Require(active_managed_composition &&
                        active_managed_composition->bundle->canonical_bytes() ==
                            captured->candidate.bundle->canonical_bytes() &&
                        active_managed_composition->projection_revision == child->candidate_projection_revision &&
                        active_managed_composition->cclc_lifecycle_product,
                    "delegated callback lost its actual installed candidate product");
          }
          // This lock pins the original product or the actual installed
          // candidate through every synchronous Core observation/effect.
          // No candidate view retargets the original immutable registry.
          result = action();
        };
        if (head == captured->predecessor_publication) {
          Require(!parent.value().publication_receipt,
                  "delegated callback predecessor conflicts with parent publication");
          fence_owner.RunWithExactHeldPredecessor(authority.held_fence_query,
              head.publication_generation, head.durable_public_pointer_content_digest, pinned_action);
        } else {
          Require(parent.value().publication_receipt &&
                      parent.value().publication_receipt->publication.publication_receipt == head &&
                      head.request == plan.expected_publication,
                  "delegated callback current publication is not its exact parent successor");
          fence_owner.RunWithExactHeldSuccessor(authority.held_fence_query, head, pinned_action);
        }
        return result;
      } catch (const std::exception &error) {
        return core::Result<void>::Err({"apply_bring_up_original_parent_unavailable", error.what(), {}});
      }
    };
    return services;
  };

  // Original managed-channel preparation accepts every sealed child before
  // controller installation. The actual owner repeats W/R/S selection and
  // admission inside its own transaction; these receipts do not permit effects.
  [[maybe_unused]] const auto accept_original_apply_children =
      [&](const component_manager::ManagerSelectedRuntimeCompositionRequestDTO &request)
      -> core::NativeApplyComponentBringUpDTO {
    std::shared_ptr<const ManagedOriginalApplyBringUpCapture> captured;
    {
      std::lock_guard lock(managed_composition_mutex);
      captured = original_apply_bring_up_capture;
      Require(captured && captured->original_facade &&
                  managed_cell_cclc.facade.get() == captured->original_facade &&
                  managed_cell_cclc.publication == captured->predecessor_publication,
              "original child acceptance lost its actual facade capture");
    }
    const auto &plan = captured->sealed_plan.plan;
    const core::ApplyComponentBringUpReferenceDTO parent{
        {plan.apply_id, plan.apply_generation}, captured->sealed_plan.plan_digest};
    const core::ConfigurationFenceStatusQueryDTO query{
        parent.owner, request.configuration_fence_id,
        request.configuration_fence_generation, request.configuration_fence_scope,
        request.manager_runtime_epoch};
    const core::ApplyComponentBringUpAuthorityDTO authority{
        parent, captured->parent_claim, query};
    Require(plan.valid() && request.valid() && authority.valid() &&
                request.phase == component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare &&
                request.apply_id == plan.apply_id && request.apply_generation == plan.apply_generation &&
                request.candidate_projection_revision == plan.expected_publication.candidate_projection_revision &&
                request.retained_bundle_canonical_content_digest == plan.children.front().candidate_retained_bundle_digest &&
                request.retained_bundle_canonical_byte_size == plan.children.front().candidate_retained_bundle_bytes &&
                request.predecessor_projection_revision ==
                    captured->predecessor_projection_revision &&
                request.expected_predecessor_publication_generation ==
                    captured->predecessor_publication.publication_generation &&
                request.expected_predecessor_public_pointer_digest ==
                    captured->predecessor_publication.durable_public_pointer_content_digest,
            "original child acceptance does not match its sealed preparation");
    {
      std::lock_guard lock(managed_composition_mutex);
      if (original_apply_accepted_children) {
        Require(original_apply_accepted_children->parent == parent &&
                    original_apply_accepted_children->children.size() == plan.children.size(),
                "original child receipts belong to another immutable parent");
        return *original_apply_accepted_children;
      }
    }
    // This enabled caller is the reviewed initial empty-cell path. Its actual
    // verified predecessor inventory supplies positive absence; a failed
    // snapshot read alone never supplies that proof.
    Require(captured->predecessor.bundle->value().cell_graph.controllable_components.empty() &&
                captured->predecessor.bundle->value().cell_graph.control_bindings.empty() &&
                captured->predecessor.bundle->value().cell_graph.control_domains.empty(),
            "initial child acceptance lacks its actual empty predecessor inventory");
    core::NativeApplyComponentBringUpDTO accepted{parent, {}};
    accepted.children.reserve(plan.children.size());
    for (const auto &child : plan.children) {
      Require(child.runtime_unit_id.value() == config.runtime_unit_id &&
                  child.runtime_epoch.value() == config.runtime_epoch &&
                  child.manager_runtime_epoch == request.manager_runtime_epoch &&
                  child.intent.request_context.request_owner_epoch ==
                      captured->original_facade->coordinator().request_owner_epoch() &&
                  child.intent.apply_generation_selection &&
                  !child.intent.apply_generation_selection->owner_qualified_predecessor,
              "initial sealed child contains a foreign occurrence or predecessor");
      const auto found = std::find_if(captured->candidate_closure.components.begin(),
          captured->candidate_closure.components.end(), [&](const auto &row) {
            return row.component_id == child.intent.controllable_component_id;
          });
      Require(found != captured->candidate_closure.components.end(),
              "original child lacks its full qualified candidate closure");
      const auto evidence = BuildManagedApplyComponentBringUpEvidence(
          captured->sealed_plan, child, authority, captured->predecessor_publication,
          *found, std::nullopt, std::nullopt, std::nullopt, std::nullopt, false);
      if (!evidence.ok()) Fail("original child evidence is unavailable: " + evidence.error().message);
      auto services = make_original_apply_services(captured, authority);
      const auto result = captured->original_facade->coordinator().AcceptApplyComponentBringUp(
          child.intent, evidence.value(), std::nullopt, found->lifecycle_plan_catalogs,
          found->qualified_candidate_domains, found->candidate_provider_occurrences,
          std::move(services));
      if (!result.ok()) Fail("original child acceptance failed: " + result.error().message);
      const auto receipt = MakeManagedApplyComponentBringUpAcceptedReceipt(
          captured->sealed_plan, child, result.value());
      if (!receipt.ok()) Fail("original child acceptance receipt is invalid: " + receipt.error().message);
      Require(result.value().status &&
                  result.value().status->phase == core::ComponentLifecycleTransactionPhaseDTO::kAccepted &&
                  result.value().status->status_sequence == 1U &&
                  !result.value().status->terminal_result &&
                  std::all_of(result.value().status->domains.begin(), result.value().status->domains.end(),
                      [](const auto &domain) { return domain.primitive_attempts.empty(); }),
              "original child acceptance was not inert and unstarted");
      accepted.children.push_back(receipt.value());
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      Require(original_apply_bring_up_capture == captured &&
                  managed_cell_cclc.facade.get() == captured->original_facade,
              "original child owner changed before receipt retention");
      original_apply_accepted_children = accepted;
    }
    return accepted;
  };

  // Read actual owner-issued lifecycle facts outside composition/Kernel locks.
  // The serial managed caller borrows the stable facade; product exchange must
  // exclude these reads through its accounted current-context pin.
  const auto require_inactive_changed_components =
      [&](const deployment::ApplyComponentDeltaDTO &delta,
          const core::ActiveBundlePublicationReceiptDTO &predecessor) {
    Require(delta.valid() && predecessor.valid(),
            "configuration Apply has invalid delta/predecessor evidence");
    deployment::CCLCRuntimeComposition *facade = nullptr;
    {
      std::lock_guard lock(managed_composition_mutex);
      facade = current_cell_cclc();
      Require(facade != nullptr && !current_composition_building &&
                  !managed_composition_building,
              "configuration Apply has no stable current component owner");
    }
    for (const auto &change : delta.components) {
      if (change.disposition != deployment::ApplyComponentDisposition::kReplace &&
          change.disposition != deployment::ApplyComponentDisposition::kRemove)
        continue;
      const auto fact = facade->ReadCurrentComponentSnapshot(change.controllable_component_id);
      Require(fact.ok() && fact.value().locally_valid() &&
                  fact.value().controllable_component_id == change.controllable_component_id &&
                  fact.value().active_publication_receipt == predecessor,
              "configuration Apply cannot read the exact current component " +
                  change.controllable_component_id.value());
      Require(fact.value().component_lifecycle_state != core::ComponentLifecycleStateDTO::kActive,
              "configuration Apply cannot replace or remove Active component " +
                  change.controllable_component_id.value() + "; deactivate it first");
    }
    Require(read_current_publication() == predecessor,
            "configuration Apply predecessor changed during component inspection");
  };

  const auto install_or_observe_composition =
      [&](const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
              &request)
      -> component_manager::ManagerSelectedRuntimeCompositionResultDTO {
    Require(request.valid() &&
                request.runtime_unit_id.value() == config.runtime_unit_id &&
                request.runtime_epoch.value() == config.runtime_epoch,
            "runtime-composition request names another managed child");
    bool original_configuration_finalize = false;
    // This classifies the exact stored configuration operation's Scene target;
    // effect authority still comes from the existing held-fence checks below.
    // Keep the missing linked child acceptance/install continuation fail-closed.
    // Its existing ordinary native planner is not a substitute generation or
    // effect owner. This guard is removed only with that complete caller join.
    auto configuration_parent_claim =
        stable_addressed_kernel()->ReadHeldCellAdmissionClaim(
            core::CellAdmissionClaimKindDTO::kManagerApply,
            request.apply_id.value());
    if (!configuration_parent_claim) {
      configuration_parent_claim = stable_addressed_kernel()->ReadHeldCellAdmissionClaim(
          core::CellAdmissionClaimKindDTO::kAuthoringMutation);
    }
    if (configuration_parent_claim &&
        (configuration_parent_claim->intent.claim_kind == core::CellAdmissionClaimKindDTO::kAuthoringMutation ||
         configuration_parent_claim->intent.owner_transaction_id == request.apply_id.value())) {
      const auto parent = component_manager::ReadManagerApplyConfigurationStatusReadOnly(
          config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
          core::CellId(config.cell_id), {request.apply_id, request.apply_generation},
          *configuration_parent_claim);
      if (!parent.ok()) {
        std::string detail = "runtime composition cannot verify its original Manager parent";
        if (const auto found = parent.error().details.find("reason"); found != parent.error().details.end())
          detail += "; reason=" + found->second;
        if (const auto found = parent.error().details.find("integrity_detail"); found != parent.error().details.end())
          detail += "; integrity_detail=" + found->second;
        Require(false, std::move(detail));
      }
      Require(!parent.value().bring_up_plan,
              "linked runtime preparation requires its original accepted child continuation");
      if (request.phase == component_manager::ManagerSelectedRuntimeCompositionPhase::kFinalize &&
          request.candidate_projection_revision.scene_revision ==
              request.predecessor_projection_revision.scene_revision) {
        const auto &stored = parent.value();
        const auto &intent = stored.intent.intent;
        Require(stored.fence_attempt && stored.fence_acceptance &&
                    stored.runtime_convergence_intent && stored.latest_runtime_staged_ready &&
                    intent.apply_id == request.apply_id && intent.apply_generation == request.apply_generation &&
                    intent.expected_active_projection_revision == request.predecessor_projection_revision &&
                    intent.candidate_retained_bundle_id == request.candidate_projection_revision.retained_bundle_id &&
                    intent.candidate_graph_revision == request.candidate_projection_revision.graph_revision &&
                    stored.fence_attempt->fence_intent.owner_request_context == intent.request_context &&
                    stored.fence_attempt->fence_intent.owner_transaction ==
                        core::ApplyFenceOwnerDTO{request.apply_id, request.apply_generation} &&
                    stored.fence_acceptance->fence_attempt_digest == stored.fence_attempt->fence_attempt_digest &&
                    stored.fence_acceptance->accepted_fence.matches(stored.fence_attempt->fence_intent) &&
                    stored.fence_acceptance->accepted_fence.configuration_fence_id == request.configuration_fence_id &&
                    stored.fence_acceptance->accepted_fence.configuration_fence_generation == request.configuration_fence_generation &&
                    stored.fence_acceptance->accepted_fence.captured_scope == request.configuration_fence_scope &&
                    stored.runtime_convergence_intent->runtime_convergence_intent.predecessor_publication_generation ==
                        request.expected_predecessor_publication_generation &&
                    stored.runtime_convergence_intent->runtime_convergence_intent.predecessor_public_pointer_digest ==
                        request.expected_predecessor_public_pointer_digest &&
                    std::any_of(stored.latest_runtime_staged_ready->staged_ready.runtime_units.begin(),
                                stored.latest_runtime_staged_ready->staged_ready.runtime_units.end(),
                                [&](const auto &unit) {
                                  return unit.valid() && unit.runtime_unit_id == request.runtime_unit_id &&
                                      unit.runtime_epoch == request.runtime_epoch;
                                }),
                "preserved configuration finalization differs from its exact stored parent/fence/host");
        original_configuration_finalize = true;
      }
    }
    if (request.phase == component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare &&
        !preparing_composition_request && !installed_under_fence_composition_request &&
        (!installed_composition_request ||
         installed_composition_request->apply_id != request.apply_id ||
         installed_composition_request->apply_generation != request.apply_generation)) {
      const auto predecessor = read_current_publication();
      // A published successor is existing observation/replay, not first
      // preparation. Its original predecessor may no longer be current.
      if (predecessor.publication_generation == request.expected_predecessor_publication_generation &&
          predecessor.durable_public_pointer_content_digest == request.expected_predecessor_public_pointer_digest &&
          core::SameActiveDeployment(predecessor.request.candidate_projection_revision,
                                     request.predecessor_projection_revision))
        require_inactive_changed_components(request.component_delta, predecessor);
    }
    const auto read_installed_receipts = [&] {
      const std::array<const Composition *, 1> actual_other_predecessors{
          failed_prepared_composition.get()};
      return ReadNativeInstalledComponentReceipts(
          request, active_managed_composition.get(),
          retired_managed_composition != nullptr
              ? retired_managed_composition.get()
              : (IsExactRetainedNativeViewApply(request)
                     ? active_managed_composition.get()
                     : nullptr),
          actual_other_predecessors);
    };
    const core::ConfigurationFenceStatusQueryDTO fence_query{
        {request.apply_id, request.apply_generation},
        request.configuration_fence_id,
        request.configuration_fence_generation,
        request.configuration_fence_scope,
        request.manager_runtime_epoch};
    if (request.phase ==
        component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare) {
      std::optional<
          component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
          previous_attempt;
      {
        std::lock_guard lock(managed_composition_mutex);
        if (!managed_composition_building && !current_composition_building &&
            (managed_composition_failure || selected_composition_failure))
          previous_attempt = installed_under_fence_composition_request
                                 ? installed_under_fence_composition_request
                                 : preparing_composition_request;
      }
      if (previous_attempt &&
          (previous_attempt->apply_id != request.apply_id ||
           previous_attempt->apply_generation != request.apply_generation)) {
        const auto original_claim =
            stable_addressed_kernel()->ReadHeldCellAdmissionClaim(
                core::CellAdmissionClaimKindDTO::kManagerApply,
                previous_attempt->apply_id.value());
        Require(original_claim.has_value(),
                "actual_installed_state_unknown: failed preparation has no "
                "original held Apply claim; restart the workbench container");
        const std::array original_claims{*original_claim};
        const auto terminal =
            component_manager::ValidateTerminalManagerApplyClaimsReadOnly(
                config.manager_state_root /
                    std::string(
                        component_manager::kManagerApplyJournalFilename),
                core::CellId(config.cell_id), original_claims);
        Require(terminal.ok(),
                "actual_installed_state_unknown: previous preparation remains "
                "live or unknown; restart the workbench container");
        const auto previous =
            component_manager::ReadManagerApplyConfigurationStatusReadOnly(
                config.manager_state_root /
                    std::string(
                        component_manager::kManagerApplyJournalFilename),
                core::CellId(config.cell_id),
                {previous_attempt->apply_id,
                 previous_attempt->apply_generation},
                *original_claim);
        Require(previous.ok() && !previous.value().bring_up_plan &&
                    previous.value().terminal_outcome,
                "actual_installed_state_unknown: previous preparation lacks "
                "its exact immutable terminal");
        // Fresh authority precedes retiring attempt-local markers. Neither the
        // original terminal nor its physical fence/claim is changed here.
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [] {});
        if (managed_composition_builder.joinable())
          managed_composition_builder.join();
        std::optional<CompositionBuildInput> discarded_view;
        {
          std::lock_guard lock(managed_composition_mutex);
          Require(!managed_composition_building &&
                      !current_composition_building &&
                      (installed_under_fence_composition_request ==
                           previous_attempt ||
                       preparing_composition_request == previous_attempt),
                  "actual_installed_state_unknown: previous owner changed "
                  "during finished-builder join");
          Require(!prepared_managed_composition || !failed_prepared_composition,
                  "actual_installed_state_unknown: more than one failed "
                  "candidate owner remains; restart the workbench container");
          if (prepared_managed_composition)
            failed_prepared_composition =
                std::move(prepared_managed_composition);
          discarded_view = std::move(prepared_retained_view);
          prepared_retained_view.reset();
          prepared_managed_composition_is_empty = false;
          preparing_composition_request.reset();
          installed_under_fence_composition_request.reset();
          managed_composition_failure = nullptr;
          selected_composition_failure = nullptr;
        }
      }
    }
    if (selected_composition_failure != nullptr)
      std::rethrow_exception(selected_composition_failure);
    const auto require_installed_candidate_configuration = [&] {
      const auto fence_status = fence_owner.QueryStatus(fence_query);
      Require(fence_status.matches(fence_query) &&
                  fence_status.state == core::ConfigurationFenceState::kHeld &&
                  fence_status.admission_fenced &&
                  fence_status.reconciliation_evidence.execution_host_epoch
                          .runtime_unit_id == request.runtime_unit_id &&
                  fence_status.reconciliation_evidence.execution_host_epoch
                          .runtime_epoch == request.runtime_epoch &&
                  fence_status.reconciliation_evidence
                          .active_publication_generation ==
                      request.expected_predecessor_publication_generation &&
                  fence_status.reconciliation_evidence
                          .active_public_pointer_digest ==
                      request.expected_predecessor_public_pointer_digest,
              "installed runtime composition does not reconcile to its exact "
              "durable held fence and predecessor publication");
    };
    try_release_retired_composition();
    try_start_current_composition();
    rethrow_composition_failure();
    {
      std::lock_guard lock(managed_composition_mutex);
      if (current_predecessor_composition_pending &&
          active_managed_composition == nullptr &&
          !current_composition_builder_started &&
          !installed_under_fence_composition_request.has_value()) {
        // The published predecessor already declares physical components,
        // but their verified Scene projections have not settled yet. Keep
        // the Manager request in preparation; a retain delta may only proceed
        // after the exact predecessor owners exist in this live Kernel.
        return {request,
                component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
                {}};
      }
    }
    if (request.phase == component_manager::
                             ManagerSelectedRuntimeCompositionPhase::kFinalize) {
      std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
          prepared_request;
      {
        std::lock_guard lock(managed_composition_mutex);
        if (installed_under_fence_composition_request.has_value() &&
            installed_under_fence_composition_request->phase ==
                component_manager::ManagerSelectedRuntimeCompositionPhase::
                    kPrepare) {
          prepared_request = installed_under_fence_composition_request;
        }
      }
      if (prepared_request.has_value()) {
        Require(!original_configuration_finalize ||
                    request.authoritative_scene_snapshot_digest ==
                        prepared_request->authoritative_scene_snapshot_digest,
                "preserved configuration finalization changed its prepared Scene snapshot");
        auto expected = request;
        expected.phase = component_manager::
            ManagerSelectedRuntimeCompositionPhase::kPrepare;
        expected.candidate_projection_revision =
            prepared_request->candidate_projection_revision;
        expected.authoritative_scene_snapshot_digest =
            prepared_request->authoritative_scene_snapshot_digest;
        bool original_linked_finalize = false;
        {
          std::lock_guard lock(managed_composition_mutex);
          if (original_apply_bring_up_capture &&
              original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
              original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation) {
            Require(original_apply_native_plan && original_apply_accepted_children &&
                        original_apply_native_plan->component_bring_up == original_apply_accepted_children &&
                        original_apply_native_plan->apply ==
                            core::NamedApplyPublicationTransactionDTO{request.apply_id, request.apply_generation} &&
                        original_apply_native_plan->candidate_retained_bundle_id ==
                            request.candidate_projection_revision.retained_bundle_id &&
                        original_apply_native_plan->candidate_graph_revision ==
                            request.candidate_projection_revision.graph_revision &&
                        managed_cell_cclc.facade.get() ==
                            original_apply_bring_up_capture->original_facade,
                    "original finalization lost its accepted child plan or owner");
            original_linked_finalize = true;
          }
        }
        const bool exact_prepared_apply = expected == *prepared_request &&
                    request.candidate_projection_revision.scene_revision.scene_id ==
                        prepared_request->candidate_projection_revision
                            .scene_revision.scene_id &&
                    ((original_linked_finalize || original_configuration_finalize)
                         ? request.candidate_projection_revision.scene_revision ==
                               prepared_request->candidate_projection_revision.scene_revision
                         : request.candidate_projection_revision.scene_revision.revision_id >
                               prepared_request->candidate_projection_revision.scene_revision.revision_id);
        if (!exact_prepared_apply) {
          // Bounded field names diagnose refusal without disclosing payloads or
          // changing the exact prepared-request or Scene proof predicates.
          std::string mismatches;
          const auto compare = [&](std::string_view field, const auto &actual,
                                   const auto &prepared) {
            if (actual != prepared) {
              if (!mismatches.empty()) mismatches += ",";
              mismatches += field;
            }
          };
          compare("authoritative_scene_snapshot_digest", expected.authoritative_scene_snapshot_digest,
                  prepared_request->authoritative_scene_snapshot_digest);
          compare("apply_id", expected.apply_id,
                  prepared_request->apply_id);
          compare("apply_generation", expected.apply_generation,
                  prepared_request->apply_generation);
          compare("candidate_projection_revision", expected.candidate_projection_revision,
                  prepared_request->candidate_projection_revision);
          compare("expected_predecessor_publication_generation", expected.expected_predecessor_publication_generation,
                  prepared_request->expected_predecessor_publication_generation);
          compare("expected_predecessor_public_pointer_digest", expected.expected_predecessor_public_pointer_digest,
                  prepared_request->expected_predecessor_public_pointer_digest);
          compare("retained_bundle_canonical_content_digest", expected.retained_bundle_canonical_content_digest,
                  prepared_request->retained_bundle_canonical_content_digest);
          compare("retained_bundle_canonical_byte_size", expected.retained_bundle_canonical_byte_size,
                  prepared_request->retained_bundle_canonical_byte_size);
          compare("runtime_unit_id", expected.runtime_unit_id,
                  prepared_request->runtime_unit_id);
          compare("runtime_epoch", expected.runtime_epoch,
                  prepared_request->runtime_epoch);
          compare("manager_runtime_epoch", expected.manager_runtime_epoch,
                  prepared_request->manager_runtime_epoch);
          compare("configuration_fence_id", expected.configuration_fence_id,
                  prepared_request->configuration_fence_id);
          compare("configuration_fence_generation", expected.configuration_fence_generation,
                  prepared_request->configuration_fence_generation);
          compare("configuration_fence_scope", expected.configuration_fence_scope,
                  prepared_request->configuration_fence_scope);
          compare("phase", expected.phase,
                  prepared_request->phase);
          compare("component_delta", expected.component_delta,
                  prepared_request->component_delta);
          compare("predecessor_projection_revision", expected.predecessor_projection_revision,
                  prepared_request->predecessor_projection_revision);
          compare("isaac_pose_observer_launch_facts", expected.isaac_pose_observer_launch_facts,
                  prepared_request->isaac_pose_observer_launch_facts);
          Require(false,
                  "final runtime composition differs from its exact prepared Apply; "
                  "normalized_mismatching_fields=[" + mismatches + "]" +
                      "; original_configuration_finalize=" +
                      (original_configuration_finalize ? "true" : "false") +
                      "; actual_scene_revision=" +
                      std::to_string(request.candidate_projection_revision
                                         .scene_revision.revision_id) +
                      "; prepared_scene_revision=" +
                      std::to_string(prepared_request->candidate_projection_revision
                                         .scene_revision.revision_id));
        }
        require_installed_candidate_configuration();
        const auto selected_bundle =
            LoadExactManagedCandidateBundle(config, request);
        Require(selected_bundle.has_value(),
                "final runtime composition candidate bundle is unavailable");
        const auto &bundle = *selected_bundle->bundle;
        const auto final_projections =
            bundle.value().cell_graph.controllable_components.empty()
                ? std::optional<ManagedComponentProjectionSet>{}
                : SelectManagedComponentProjections(
                      config, bundle, projections,
                      request.candidate_projection_revision, false);
        Require(bundle.value().cell_graph.controllable_components.empty() ||
                    final_projections.has_value(),
                "final runtime composition has no installed Scene projection");
        Composition *current = nullptr;
        {
          std::lock_guard lock(managed_composition_mutex);
          Require(installed_under_fence_composition_request == prepared_request &&
                      !current_composition_building &&
                      !managed_composition_building &&
                      (bundle.value().cell_graph.controllable_components.empty() ||
                       active_managed_composition != nullptr) &&
                      (active_managed_composition == nullptr ||
                       active_managed_composition->projection_revision ==
                           prepared_request->candidate_projection_revision),
                  "prepared runtime changed before final Scene handoff");
          current = active_managed_composition.get();
          installed_under_fence_composition_request = request;
        }
        if (current != nullptr) {
          fence_owner.RunWithExactHeldPredecessor(
              fence_query, request.expected_predecessor_publication_generation,
              request.expected_predecessor_public_pointer_digest, [&] {
                std::unique_lock execution_lock(execution_composition.mutex);
                Require(active_managed_composition.get() == current &&
                            final_projections.has_value(),
                        "native composition changed before final Scene");
                RefreshRetainedNativePublicationView(
                    *current, final_projections->by_component,
                    final_projections->required_projection_pins,
                    request.candidate_projection_revision,
                    current->publication_generation,
                    config.manager_run_root);
              });
          InstallDirectSceneFrameResolver(projections, *current);
        }
      }
    }
    const bool publication_is_successor =
        CurrentPublicationIsInstalledSuccessor(config, request);
    const auto successor_bundle =
        publication_is_successor
            ? LoadExactManagedCandidateBundle(config, request)
            : std::optional<ManagedBundleAtPinnedRoot>{};
    bool reconcile_installed_candidate = false;
    bool published_installed_empty = false;
    Composition *published_installed_composition = nullptr;
    {
      std::lock_guard lock(managed_composition_mutex);
      if (installed_composition_request.has_value()) {
        if (*installed_composition_request == request) {
          if (!publication_is_successor) {
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kInstalled,
                    installed_component_receipts};
          }
          if (current_composition_building) {
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing,
                    {}};
          }
          if (active_managed_composition == nullptr && original_configuration_finalize &&
              successor_bundle && successor_bundle->bundle->value().cell_graph.controllable_components.empty()) {
            // The exact installed empty request retains the original cell
            // owner; its postpublication context still needs reconciliation.
            current_composition_building = true;
            published_installed_empty = true;
          } else
          {
            Require(active_managed_composition != nullptr,
                    "published installed runtime has no active composition");
            current_composition_building = true;
            published_installed_composition = active_managed_composition.get();
          }
        }
      }
      if (published_installed_composition == nullptr && !published_installed_empty &&
          installed_under_fence_composition_request.has_value()) {
        Require(*installed_under_fence_composition_request == request,
                "another selected runtime composition is installed under "
                "the held fence");
        if (request.phase == component_manager::
                                 ManagerSelectedRuntimeCompositionPhase::kPrepare) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kInstalled,
                  {}};
        }
        reconcile_installed_candidate = true;
      }
      if (published_installed_composition == nullptr && !published_installed_empty &&
          !reconcile_installed_candidate) {
        if (current_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
        if (publication_is_successor) {
          const auto expected_generation =
              core::ActiveBundlePublicationGeneration(
                  request.expected_predecessor_publication_generation.value() +
                  1U);
          if (active_managed_composition == nullptr) {
            if (successor_bundle->bundle->value()
                    .cell_graph.controllable_components.empty()) {
              installed_component_receipts = read_installed_receipts();
              installed_composition_request = request;
              confirm_installed_projection_set(request);
              publish_installed_liveness_handoff(request);
              return {request,
                      component_manager::
                          ManagerSelectedRuntimeCompositionState::kInstalled,
                      installed_component_receipts};
            }
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing,
                    {}};
          }
          Require(active_managed_composition->bundle->retained_bundle_id() ==
                          request.candidate_projection_revision
                              .retained_bundle_id &&
                      active_managed_composition->projection_revision ==
                          request.candidate_projection_revision &&
                      active_managed_composition->publication_generation ==
                          expected_generation,
                  "recovered active composition differs from the published "
                  "Apply successor");
          current_composition_building = true;
          published_installed_composition =
              active_managed_composition.get();
        }
        Require(request.phase == component_manager::
                    ManagerSelectedRuntimeCompositionPhase::kPrepare,
                "final runtime composition has no exact prepared candidate");
        if (retired_managed_composition != nullptr) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
      }
    }
    if (published_installed_composition != nullptr || published_installed_empty) {
      bool attached = false;
      try {
        if (original_configuration_finalize) {
          const auto head = read_current_publication();
          auto *const facade = managed_cell_cclc.facade.get();
          const auto *const original_apply = std::get_if<
              core::NamedApplyPublicationTransactionDTO>(&head.request.transaction_ref);
          Require(facade && managed_cell_cclc.publication &&
                      head.valid() && original_apply &&
                      *original_apply == core::NamedApplyPublicationTransactionDTO{
                          request.apply_id, request.apply_generation},
                  "configuration handover lost its actual CCLC owner or successor");
          fence_owner.RunWithExactHeldSuccessor(fence_query, head, [&] {
            if (managed_cell_cclc.publication == head) {
              attached = true;
              return;
            }
            Require(installed_composition_request == request &&
                        managed_cell_cclc.publication->publication_generation ==
                        request.expected_predecessor_publication_generation &&
                        managed_cell_cclc.publication->durable_public_pointer_content_digest ==
                            request.expected_predecessor_public_pointer_digest,
                    "configuration handover changed its original product predecessor");
            if (!managed_cell_cclc.pending_product) {
              // Storage writes finish their operation pins before exchange.
              // Plan/adoption authority remains the original host fence above.
              const auto plan = facade->LoadNativeApplyPlan(
                  {request.apply_id, request.apply_generation});
              Require(plan.ok() && plan.value() && !plan.value()->component_bring_up,
                      "configuration handover has no exact ordinary native plan");
              const auto prepared = [&]() {
                if (published_installed_empty) {
                  Require(successor_bundle && installed_composition_request == request &&
                              plan.value()->apply == core::NamedApplyPublicationTransactionDTO{
                                  request.apply_id, request.apply_generation} &&
                              plan.value()->candidate_retained_bundle_id == head.request.candidate_retained_bundle_id &&
                              plan.value()->candidate_graph_revision == head.request.candidate_graph_revision &&
                              plan.value()->components.empty() && plan.value()->routes.empty() &&
                              plan.value()->retained_components.empty() && plan.value()->retained_domains.empty(),
                          "empty handover has no exact recorded zero-component plan");
                  // Shared preparation verifies the complete empty product and
                  // pinned package before any journal adoption is committed.
                  auto empty = managed_cell_cclc.PrepareEmptyProduct(config, successor_bundle->bundle, head);
                  if (!empty.ok()) return empty;
                  const auto adopted = facade->AdoptNativeApplyFacts({head, {}, {}});
                  if (!adopted.ok())
                    Fail("empty configuration handover could not adopt its recorded facts: " +
                         adopted.error().message);
                  return empty;
                }
                const auto adopted = facade->AdoptNativeApplyFacts(ReadNativeApplyFacts(
                    *published_installed_composition, head, *plan.value()));
                if (!adopted.ok())
                  Fail("configuration handover could not adopt current facts: " + adopted.error().message);
                return managed_cell_cclc.PrepareInstalledProduct(*published_installed_composition, config, head);
              }();
              if (!prepared.ok())
                Fail("configuration handover could not prepare its actual product: " +
                     prepared.error().message);
              // Stage all allocating metadata before installing the one
              // prepared value; a failed capture retains the original context.
              auto exact_request = request;
              auto owner = facade->coordinator().request_owner_epoch();
              managed_cell_cclc.pending_product_request = std::move(exact_request);
              managed_cell_cclc.pending_product_owner = std::move(owner);
              managed_cell_cclc.pending_product = prepared.value();
            }
            Require(managed_cell_cclc.pending_product_request == request &&
                        managed_cell_cclc.pending_product_owner &&
                        facade == managed_cell_cclc.facade.get(),
                    "configuration handover lost its original prepared identity");
            // Copy the allocating successor receipt before the context swap.
            auto successor = head;
            const auto exchanged = facade->TryExchangeProductContext(
                *managed_cell_cclc.pending_product,
                *managed_cell_cclc.pending_product_owner,
                *managed_cell_cclc.publication);
            if (!exchanged.ok())
              Fail("configuration product exchange could not be qualified: " +
                   exchanged.error().message);
            attached = exchanged.value().phase ==
                core::ComponentLifecycleProductExchangePhase::kCompleted;
            if (attached) {
              Require(exchanged.value().current_publication == head &&
                          exchanged.value().owner_epoch ==
                              *managed_cell_cclc.pending_product_owner,
                      "completed configuration product exchange changed identity");
              // NativeAdopt verified the complete installed route inventory;
              // completed exchange now selects that exact product. Clear only
              // this attachment gate, retaining the Manager claim/fence and
              // every independent Stop/fault/admission exclusion.
              if (published_installed_composition != nullptr) {
                Require(published_installed_composition->active_addressed_kernel()
                            ->SetRouteRecoveryPending(false).ok(),
                        "completed configuration exchange could not clear route recovery");
                published_installed_composition->route_recovery_pending.store(
                    false, std::memory_order_release);
                published_installed_composition->cclc_attached.store(true, std::memory_order_release);
              }
              managed_cell_cclc.publication = std::move(successor);
              managed_cell_cclc.pending_product.reset();
              managed_cell_cclc.pending_product_request.reset();
              managed_cell_cclc.pending_product_owner.reset();
            }
          });
        } else
        {
          attached = AttachCurrentPublishedCCLCIfReady(
              *published_installed_composition, config, fence_owner, fence_query);
        }
        std::lock_guard lock(managed_composition_mutex);
        Require(active_managed_composition.get() ==
                    published_installed_composition,
                "published installed runtime changed during route recovery");
        if (attached) {
          if (installed_composition_request == request) {
            // This exact Apply already proved controller retention before
            // retiring its predecessor. Replay that receipt; the released
            // predecessor is no longer a live source of lifetime evidence.
            Require(component_manager::ComponentReceiptsMatchRequest(
                        request, installed_component_receipts),
                    "installed receipt replay differs from its exact Apply");
          } else {
            installed_component_receipts = read_installed_receipts();
          }
          installed_composition_request = request;
          confirm_installed_projection_set(request);
          publish_installed_liveness_handoff(request);
        }
        current_composition_building = false;
      } catch (...) {
        std::lock_guard lock(managed_composition_mutex);
        current_composition_building = false;
        if (original_configuration_finalize)
          selected_composition_failure = std::current_exception();
        throw;
      }
      return {
          request,
          attached
              ? component_manager::ManagerSelectedRuntimeCompositionState::
                    kInstalled
              : component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
          attached ? installed_component_receipts
                   : std::vector<component_manager::
                         ManagerRuntimeComponentReceiptDTO>{}};
    }
    if (reconcile_installed_candidate) {
      require_installed_candidate_configuration();
      // Finalization is still a private parent-Apply effect. Publish and
      // read back every required native route before reporting the selected
      // runtime installed; a later CCLC service attachment is not a route
      // readiness substitute. The Apply fence continues to block commands.
      if (active_managed_composition != nullptr) {
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [&] {
              const auto candidate_package = LoadManagedBundleAtPinnedRoot(
                  config, request.candidate_projection_revision
                              .retained_bundle_id);
              Require(candidate_package.has_value(),
                      "finalized native Apply bundle is unavailable");
              const deployment::FilesystemVerifiedRetainedProductArtifactSource
                  artifacts(candidate_package->package_root);
              const auto representatives =
                  CellControlBindingRepresentatives(*active_managed_composition);
              const auto &candidate = *active_managed_composition->bundle;
              // CCLC records one decision before any candidate route exists.
              // Retained facts are taken from the live Kernel only after the
              // host proves that the exact predecessor controller survived.
              const std::array<const Composition *, 1>
                  actual_other_predecessors{failed_prepared_composition.get()};
              auto plan_request = BuildNativeApplyPlanRequest(
                  request, *active_managed_composition,
                  retired_managed_composition != nullptr
                      ? retired_managed_composition.get()
                      : (IsExactRetainedNativeViewApply(request)
                             ? active_managed_composition.get()
                             : nullptr),
                  representatives, actual_other_predecessors);
              plan_request.changed_component_state =
                  core::ComponentLifecycleStateDTO::kDeclared;
              const auto plan = [&]() -> core::Result<core::NativeApplyPlanDTO> {
                {
                  std::lock_guard lock(managed_composition_mutex);
                  if (original_apply_bring_up_capture &&
                      original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
                      original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation) {
                    Require(original_apply_native_plan && original_apply_accepted_children &&
                                original_apply_native_plan->component_bring_up == original_apply_accepted_children &&
                                original_apply_native_plan->apply == plan_request.apply &&
                                original_apply_native_plan->candidate_retained_bundle_id ==
                                    plan_request.candidate_retained_bundle_id &&
                                original_apply_native_plan->candidate_graph_revision == plan_request.candidate_graph_revision &&
                                managed_cell_cclc.facade.get() ==
                                    original_apply_bring_up_capture->original_facade,
                            "original finalization lost its pre-effect accepted plan");
                    // Holding children have already advanced their genuine facts.
                    // The original pre-effect plan remains the recorded decision;
                    // finalization must not allocate again or reclassify them as Declared.
                    return core::Result<core::NativeApplyPlanDTO>::Ok(*original_apply_native_plan);
                  }
                }
                Require(managed_cell_cclc.facade != nullptr,
                        "configuration Apply lost the existing CCLC journal owner");
                return managed_cell_cclc.facade->PlanNativeApply(plan_request);
              }();
              if (!plan.ok()) {
                Fail("native CCLC could not record the Apply plan: " +
                     plan.error().message);
              }
              auto &installed = *active_managed_composition;
              EnsureGripperOnlyCellPorts(installed);
              Require(installed.runtime != nullptr ||
                          (installed.cclc_cell_route_publication_owner != nullptr &&
                           installed.cclc_cell_command_readiness_port != nullptr),
                      "installed scalar candidate has no complete CCLC route/readiness owners");
              auto &route_owner =
                  installed.cclc_cell_route_publication_owner != nullptr
                      ? *installed.cclc_cell_route_publication_owner
                      : static_cast<deployment::IRouteConfigurationKernelPublicationOwner &>(
                            *installed.runtime);
              auto &readiness_owner = installed.runtime != nullptr
                  ? static_cast<core::ICCLCComponentCommandReadinessPort &>(
                        *installed.runtime)
                  : static_cast<core::ICCLCComponentCommandReadinessPort &>(
                        *installed.cclc_cell_command_readiness_port);
              const auto initialized = InitializeRetainedNativeCandidateControlRoutes(
                  candidate, *installed.active_addressed_kernel(), route_owner,
                  readiness_owner, installed.publication_generation,
                  plan.value(), artifacts, representatives,
                  config.execution_run_root,
                  fence_query.configuration_fence_id);
              if (!initialized.ok()) {
                Fail("native CCLC candidate routes are not ready: " +
                     initialized.error().message);
              }
            });
      }
      {
        std::lock_guard lock(managed_composition_mutex);
        Require(installed_under_fence_composition_request.has_value() &&
                    *installed_under_fence_composition_request == request,
                "installed runtime composition changed during fence "
                "reconciliation");
        installed_component_receipts = read_installed_receipts();
        installed_composition_request = request;
        confirm_installed_projection_set(request);
        publish_installed_liveness_handoff(request);
        installed_under_fence_composition_request.reset();
        preparing_composition_request.reset();
      }
      try_release_retired_composition();
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kInstalled,
              installed_component_receipts};
    }

    std::unique_ptr<Composition> prepared;
    std::optional<CompositionBuildInput> retained_view_input;
    bool prepared_is_empty = false;
    bool start_builder = false;
    std::shared_ptr<WallClock> shared_clock;
    std::shared_ptr<SimpleScheduler> shared_scheduler;
    std::map<core::ControllableComponentId, RetainedNativeComponentSnapshot>
        retained_snapshots;
    std::vector<core::ControllableComponentId> actual_retained_components;
    std::vector<core::ControllableComponentId> actual_removed_components;
    std::vector<std::shared_ptr<ControllerLaunchProcess>> processes_to_retire;
    const auto actual_candidate =
        LoadExactManagedCandidateBundle(config, request);
    Require(
        actual_candidate && actual_candidate->bundle,
        "actual_installed_state_unknown: candidate product is not verified");
    {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (preparing_composition_request.has_value() &&
          *preparing_composition_request != request) {
        Fail("another selected runtime composition is still preparing");
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_retained_view.has_value()) {
        retained_view_input = std::move(prepared_retained_view);
        prepared_retained_view.reset();
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else if (!preparing_composition_request.has_value()) {
        VerifyRetainedNativePredecessors(active_managed_composition.get(),
                                         request);
        std::map<core::ControllableComponentId, const Composition *>
            actual_keepers;
        std::set<const Composition *> included_roots;
        std::set<const ControllerLaunchProcess *> included_processes;
        const std::array<const Composition *, 5> roots{
            active_managed_composition.get(), retired_managed_composition.get(),
            prepared_managed_composition.get(),
            failed_prepared_composition.get(), execution_composition.current};
        std::shared_lock execution_lock(execution_composition.mutex);
        for (const auto *root : roots) {
          if (!root || !included_roots.insert(root).second)
            continue;
          for (const auto &[id, owner] :
               IndexNativeComponentCompositions(*root)) {
            Require(owner->bundle &&
                        (owner->controller_process || IsLocalNoOutputGripperOwner(*owner)) &&
                        owner->ros2_control_runtime_handle &&
                        owner->component_runtime_identity(),
                    "actual_installed_state_unknown: component has no exact "
                    "ROS/process owner");
            const auto wanted = deployment::derive_component_runtime_identity(
                *actual_candidate->bundle, id);
            const auto installed =
                deployment::derive_component_runtime_identity(*owner->bundle,
                                                              id);
            Require(installed.has_value(),
                    "actual_installed_state_unknown: owned component source "
                    "identity is absent");
            if (!wanted)
              actual_removed_components.push_back(id);
            const bool owned_launch_live = owner->controller_process
                ? !owner->controller_process->exited() &&
                      owner->controller_process->cleanup_observation().state ==
                          ControllerLaunchCleanupState::kNotRequested
                : IsLocalNoOutputGripperOwner(*owner);
            if (wanted == installed && owned_launch_live) {
              const auto [existing, inserted] =
                  actual_keepers.emplace(id, owner);
              Require(inserted ||
                          existing->second->component_runtime_identity() ==
                              owner->component_runtime_identity(),
                      "actual_installed_state_unknown: component has multiple "
                      "live target owners");
            }
          }
        }
        for (const auto &[id, owner] : actual_keepers) {
          actual_retained_components.push_back(id);
          retained_snapshots.emplace(
              id, RetainedNativeComponentSnapshot{
                      owner->runtime, owner->gripper_runtime, owner->stage,
                      owner->bundle->value()
                          .compiled_products.planning_collision_model,
                      owner->direct_control_joint_limits,
                      owner->direct_control_motion_descriptor,
                      owner->direct_control_gripper_descriptor,
                      owner->ros2_control_runtime_handle,
                      owner->controller_process});
        }
        std::sort(actual_removed_components.begin(),
                  actual_removed_components.end());
        actual_removed_components.erase(
            std::unique(actual_removed_components.begin(),
                        actual_removed_components.end()),
            actual_removed_components.end());
        for (const auto *root : included_roots)
          for (const auto &[id, owner] :
               IndexNativeComponentCompositions(*root)) {
            const auto kept = actual_keepers.find(id);
            if (kept != actual_keepers.end() &&
                (owner->controller_process
                     ? kept->second->controller_process == owner->controller_process
                     : IsLocalNoOutputGripperOwner(*owner) &&
                           IsLocalNoOutputGripperOwner(*kept->second) &&
                           kept->second->gripper_runtime == owner->gripper_runtime))
              continue;
            Require(std::binary_search(request.configuration_fence_scope
                                           .controllable_component_ids.begin(),
                                       request.configuration_fence_scope
                                           .controllable_component_ids.end(),
                                       id),
                    "actual_installed_state_unknown: actual process retirement "
                    "is outside the accepted fence scope");
            if (owner->controller_process &&
                included_processes.insert(owner->controller_process.get()).second)
              processes_to_retire.push_back(owner->controller_process);
          }
        preparing_composition_request = request;
        managed_composition_building = true;
        start_builder = true;
        if (stable_addressed_kernel() != nullptr) {
          shared_clock = detached_owner_clock;
          shared_scheduler = detached_owner_scheduler;
        }
      }
    }

    if (start_builder) {
      // Preparation creates only closed-gate ROS endpoints. Qualify the exact
      // authority before it starts; executor activation remains fenced below.
      try {
        // Added-domain configuration facts do not exist until installation.
        // Qualify the durable predecessor authority without requiring them.
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [] {});
      } catch (...) {
        std::lock_guard lock(managed_composition_mutex);
        managed_composition_failure = std::current_exception();
        managed_composition_building = false;
        throw;
      }
      managed_composition_builder = std::jthread(
          [&, request, fence_query, shared_clock = std::move(shared_clock),
           shared_scheduler = std::move(shared_scheduler),
           retained_snapshots = std::move(retained_snapshots)
               ,
           actual_retained_components = std::move(actual_retained_components),
           actual_removed_components = std::move(actual_removed_components),
           processes_to_retire = std::move(processes_to_retire)
      ]() mutable {
            try {
              auto input = BuildManagedCompositionInput(
                  config, projections, admission_gate, request,
                  std::move(shared_clock), std::move(shared_scheduler));
              if (input.has_value()) {
                input->retained_component_snapshots =
                    std::move(retained_snapshots);
                input->observation_wake = stable_observation_wake;
              }
              // The outgoing lifecycle reader must see the exact Replace /
              // Remove decision before ensure() retires its controller.
              // These are planned floors, never installed readiness proof.
              std::vector<core::RuntimeParticipantBinding> planned_bindings;
              if (input.has_value()) {
                const deployment::FilesystemVerifiedCatalogArtifactSource
                    catalogs(input->package_root);
                const auto inventory =
                    deployment::compile_direct_control_program_binding_inventory(
                        *input->bundle, input->model_map, catalogs);
                for (const auto &component :
                     input->bundle->value().cell_graph.controllable_components) {
                  auto selected = *input;
                  selected.selected_component_id =
                      component.controllable_component_id;
                  auto physical =
                      ResolveNativePhysicalBindings(selected, inventory);
                  planned_bindings.insert(
                      planned_bindings.end(),
                      std::make_move_iterator(physical.representatives.begin()),
                      std::make_move_iterator(physical.representatives.end()));
                }
              }
              const auto planned_candidate = LoadExactManagedCandidateBundle(
                  config, request);
              const auto planned_predecessor = LoadManagedBundleAtPinnedRoot(
                  config,
                  request.predecessor_projection_revision.retained_bundle_id);
              Require(planned_candidate.has_value() &&
                          planned_predecessor.has_value(),
                      "managed Apply planning lost its verified products");
              std::optional<core::NativeApplyComponentBringUpDTO> linked_children;
              {
                std::lock_guard lock(managed_composition_mutex);
                if (original_apply_bring_up_capture &&
                    original_apply_bring_up_capture->sealed_plan.plan.apply_id == request.apply_id &&
                    original_apply_bring_up_capture->sealed_plan.plan.apply_generation == request.apply_generation)
                  linked_children = core::NativeApplyComponentBringUpDTO{
                      {{request.apply_id, request.apply_generation},
                       original_apply_bring_up_capture->sealed_plan.plan_digest}, {}};
              }
              if (linked_children)
                linked_children = accept_original_apply_children(request);
              fence_owner.RunWithExactHeldPredecessor(
                  fence_query,
                  request.expected_predecessor_publication_generation,
                  request.expected_predecessor_public_pointer_digest, [&] {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(preparing_composition_request == request,
                            "managed Apply planning lost its original request");
                    VerifyRetainedNativePredecessors(
                        active_managed_composition.get(), request);
                    std::vector<core::RuntimeRouteConfigurationReadbackDTO>
                        retained_routes;
                    for (const auto &desired : planned_candidate->bundle
                                                   ->value()
                                                   .desired_control_configurations) {
                      const bool retained = std::any_of(
                          request.component_delta.components.begin(),
                          request.component_delta.components.end(),
                          [&](const auto &component) {
                            return std::binary_search(
                                       actual_retained_components.begin(),
                                       actual_retained_components.end(),
                                       component.controllable_component_id) &&
                                   std::binary_search(
                                       component.candidate_domains.begin(),
                                       component.candidate_domains.end(),
                                       desired.control_domain_id);
                          });
                      if (!retained)
                        continue;
                      const auto route = stable_addressed_kernel()
                                             ->current_runtime_route_configuration(
                                                 desired.control_domain_id);
                      Require(route.ok() && route.value().has_value(),
                              "managed Apply retained route is unavailable before preparation");
                      retained_routes.push_back(*route.value());
                    }
                    auto derived = DeriveRetainedNativeApplyPlanRequest(
                        {request.apply_id, request.apply_generation},
                        *planned_candidate->bundle,
                        planned_predecessor->bundle.get(),
                        request.component_delta, planned_bindings,
                        retained_routes,
                        core::ComponentLifecycleStateDTO::kDeclared,
                        std::span<const core::ControllableComponentId>(
                            actual_retained_components),
                        std::span<const core::ControllableComponentId>(
                            actual_removed_components));
                    if (!derived.ok())
                      Fail("managed Apply plan derivation failed: " +
                           derived.error().message);
                    if (linked_children)
                      derived.value().component_bring_up = *linked_children;
                    Require(managed_cell_cclc.facade != nullptr,
                            "managed Apply planning lost the existing CCLC journal owner");
                    const auto recorded =
                        managed_cell_cclc.facade->PlanNativeApply(derived.value());
                    if (!recorded.ok())
                      Fail("managed CCLC could not record Apply before preparation: " +
                           recorded.error().message);
                    if (linked_children) {
                      Require(recorded.value().component_bring_up == linked_children,
                              "managed Apply plan differs from its actual accepted child receipts");
                      original_apply_native_plan = recorded.value();
                    }
                  });
              for (const auto &process : processes_to_retire) {
                if (process->cleanup_observation().state !=
                    ControllerLaunchCleanupState::kDrained) {
                  deployment::CCLCRuntimeComposition *facade = nullptr;
                  {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(preparing_composition_request == request &&
                                managed_cell_cclc.facade,
                            "actual_installed_state_unknown: retirement lost "
                            "its original component owner");
                    facade = managed_cell_cclc.facade.get();
                  }
                  const core::ControllableComponentId component(
                      process->command().controllable_component_id);
                  const auto predecessor = read_current_publication();
                  const auto snapshot =
                      facade->ReadCurrentComponentSnapshot(component);
                  Require(snapshot.ok() && snapshot.value().locally_valid() &&
                              snapshot.value().controllable_component_id ==
                                  component &&
                              snapshot.value().active_publication_receipt ==
                                  predecessor &&
                              snapshot.value().component_lifecycle_state !=
                                  core::ComponentLifecycleStateDTO::kActive,
                          "actual_installed_state_unknown: retiring component "
                          "lacks exact inactive owner facts; restart the "
                          "workbench container");
                  fence_owner.RunWithExactHeldPredecessor(
                      fence_query,
                      request.expected_predecessor_publication_generation,
                      request.expected_predecessor_public_pointer_digest, [&] {
                        std::lock_guard lock(managed_composition_mutex);
                        std::shared_lock execution_lock(
                            execution_composition.mutex);
                        Require(preparing_composition_request == request &&
                                    managed_cell_cclc.facade.get() == facade &&
                                    read_current_publication() == predecessor,
                                "actual_installed_state_unknown: retirement's "
                                "original owner/publication changed");
                        bool still_owned = false;
                        for (const auto *root :
                             std::array<const Composition *, 5>{
                                 active_managed_composition.get(),
                                 retired_managed_composition.get(),
                                 prepared_managed_composition.get(),
                                 failed_prepared_composition.get(),
                                 execution_composition.current}) {
                          if (!root)
                            continue;
                          const auto owners =
                              IndexNativeComponentCompositions(*root);
                          const auto found = owners.find(component);
                          still_owned =
                              still_owned ||
                              (found != owners.end() &&
                               found->second->controller_process == process);
                        }
                        Require(still_owned,
                                "actual_installed_state_unknown: exact "
                                "retiring process was superseded");
                      });
                  static_cast<void>(
                      host_controller_launch_supervisor().retire_exact(
                          process));
                }
                const auto cleanup = process->cleanup_observation();
                Require(cleanup.state ==
                                ControllerLaunchCleanupState::kDrained &&
                            cleanup.receipt,
                        "actual_installed_state_unknown: old component group "
                        "has not drained; restart the workbench container");
              }
              const bool retain_view =
                  input.has_value() && IsExactRetainedNativeViewApply(request)
                  && actual_retained_components.size() ==
                         input->bundle->value()
                             .cell_graph.controllable_components.size() &&
                  active_managed_composition != nullptr
                  ;
              auto built = input.has_value() && !retain_view
                               ? BuildProductComposition(std::move(*input))
                               : std::unique_ptr<Composition>{};
              std::lock_guard lock(managed_composition_mutex);
              prepared_managed_composition = std::move(built);
              if (retain_view)
                prepared_retained_view = std::move(input);
              prepared_managed_composition_is_empty = !input.has_value();
              managed_composition_building = false;
            } catch (...) {
              std::lock_guard lock(managed_composition_mutex);
              managed_composition_failure = std::current_exception();
              managed_composition_building = false;
            }
          });
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kPreparing,
              {}};
    }
    if (prepared == nullptr && !prepared_is_empty &&
        !retained_view_input.has_value()) {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_retained_view.has_value()) {
        retained_view_input = std::move(prepared_retained_view);
        prepared_retained_view.reset();
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else {
        if (managed_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
        Fail("selected runtime preparation was consumed without an exact "
             "installed result; automatic replacement is forbidden");
      }
    }
    if (managed_composition_builder.joinable()) {
      managed_composition_builder.join();
    }

    try {
      fence_owner.RunWithExactHeldPredecessor(
        fence_query, request.expected_predecessor_publication_generation,
        request.expected_predecessor_public_pointer_digest, [&] {
          std::unique_lock execution_lock(execution_composition.mutex);
          Require(retired_managed_composition == nullptr &&
                      retired_registration_engines.empty(),
                  "a predecessor runtime engine remains lifetime-pinned");
          if (retained_view_input.has_value()) {
            Require(IsExactRetainedNativeViewApply(request) &&
                        active_managed_composition != nullptr &&
                        active_managed_composition->bundle != nullptr &&
                        retained_view_input->bundle != nullptr &&
                        RetainedNativeViewFollowsPredecessor(
                            active_managed_composition->bundle
                                ->retained_bundle_id(),
                            active_managed_composition->projection_revision,
                            retained_view_input->bundle->retained_bundle_id(),
                            request.predecessor_projection_revision),
                    "retained native view crossed its verified predecessor");
            auto &candidate = *active_managed_composition;
            Require(candidate.component_runtime_identity() != nullptr &&
                        candidate.active_addressed_kernel() ==
                            stable_addressed_kernel(),
                    "retained native view lost its stable command owner");
            const auto generation =
                retained_view_input->publication_generation;
            RefreshRetainedNativePublicationView(
                candidate,
                retained_view_input->component_planner_scene_projections,
                retained_view_input->required_projection_pins,
                request.candidate_projection_revision, generation,
                config.manager_run_root);
            candidate.bundle = std::move(retained_view_input->bundle);
            DetachRetainedCCLC(candidate);
            Require(candidate.active_addressed_kernel()
                        ->SetRouteRecoveryPending(true)
                        .ok(),
                    "retained native view could not close route recovery");
            candidate.route_recovery_pending.store(true,
                                                   std::memory_order_release);
            InstallDirectSceneFrameResolver(projections, candidate);
          } else if (prepared_is_empty) {
            Require(prepared == nullptr,
                    "empty selected product has a physical composition");
            if (active_managed_composition) DetachRetainedCCLC(*active_managed_composition);
            auto reconfigured = stable_cell_runtime_owner->reconfigure_runtime_component_set(
                {}, {}, {}, request.candidate_projection_revision.retained_bundle_id,
                request.candidate_projection_revision.graph_revision,
                request.configuration_fence_id, request.configuration_fence_scope, {});
            if (!reconfigured.ok()) Fail("stable Kernel rejected the empty selected runtime: " + reconfigured.error().message);
            if (active_managed_composition) {
              retired_registration_engines = snapshot_registration_engines(*active_managed_composition);
              retired_managed_composition = std::move(active_managed_composition);
            }
            Require(stable_kernel_owner->SetRouteRecoveryPending(false).ok(),
                    "empty selected runtime could not release route recovery");
          } else {
            Require(prepared != nullptr,
                    "physical selected product has no prepared composition");
            auto *const stable_owner = stable_addressed_kernel();
            if (stable_owner == nullptr) {
              attach_retained_native_addressed_owner(
                  prepared->runtime,
                  RetainedNativeAddressedOwnerConfig{
                      core::RequestOwnerEpoch("kernel-owner-" +
                                              config.runtime_epoch),
                      core::CellId(config.cell_id),
                      config.execution_state_root /
                          std::string(core::kOperationJournalFilename),
                      1024U,
                      &admission_gate,
                      prepared->runtime->motion_binding.value().graph_revision,
                      {},
                      BuildOwnerSettlementValidator(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          config.logical_scene_database,
                          config.program_cycle_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      BuildOwnerReconciliationValidator(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          config.program_cycle_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      {}});
              admission_gate.BindResourceTable(
                  prepared->runtime->addressed_kernel
                      ->resource_admission_table());
            } else {
              Require(stable_cell_runtime_owner && detached_owner_clock && detached_owner_scheduler &&
                          (!prepared->runtime || (prepared->runtime->clock == detached_owner_clock &&
                           prepared->runtime->scheduler == detached_owner_scheduler)),
                      "prepared runtime crossed the stable Kernel time capabilities");
              std::optional<std::set<
                  core::IResolvedParticipantExecutionEngine *>>
                  predecessor_engine_snapshot;
              const auto retained_component_ids =
                  RetainedNativeComponentIds(request);
              auto stable_stop_observer = stable_stop_surveillance_owner;
              std::vector<RetainedStopSurveillanceSourceInput>
                  candidate_stop_sources;
              if (active_managed_composition != nullptr) {
                predecessor_engine_snapshot =
                    snapshot_registration_engines(*active_managed_composition);
                DetachRetainedCCLC(*active_managed_composition);
              }
              if (!retained_component_ids.empty()) {
                Require(active_managed_composition != nullptr &&
                            (prepared->runtime == nullptr ||
                             prepared->candidate_participant_resolver_factory),
                        "retained native Apply has no predecessor or candidate "
                        "participant resolver");
                ResealRetainedNativeCandidateOwners(
                    *prepared, *active_managed_composition, request,
                    admission_gate);
                RefreshRetainedNativeCandidateCollision(
                    *prepared, request, admission_gate);
                RefreshRetainedNativeCandidateModels(
                    *prepared, request, admission_gate,
                    prepared->runtime != nullptr
                        ? *prepared->runtime->clock
                        : *stable_cell_runtime_owner->clock());
                if (prepared->runtime != nullptr) {
                  std::vector<std::shared_ptr<RetainedNativeRuntime>> peers;
                  peers.reserve(prepared->participant_compositions.size());
                  for (const auto &component :
                       prepared->participant_compositions) {
                    Require(component != nullptr &&
                                component->component_runtime_identity() != nullptr,
                            "retained native Apply has an incomplete peer");
                    if (component->runtime != nullptr)
                      peers.push_back(component->runtime);
                  }
                  auto rejoined = rejoin_retained_native_runtime_participants(
                      prepared->runtime, std::move(peers),
                      prepared->candidate_participant_resolver_factory,
                      stable_stop_observer, candidate_stop_sources,
                      admission_gate, request.configuration_fence_id,
                      request.configuration_fence_scope);
                  if (!rejoined.ok())
                    Fail("retained native cell participant rejoin failed: " +
                         rejoined.error().message);
                  AppendNativeGripperStopSources(*prepared,
                                                 candidate_stop_sources);
                } else {
                  candidate_stop_sources = NativeCellStopSources(*prepared);
                }
                std::map<core::ControllableComponentId,
                         std::shared_ptr<RetainedPlannerSceneProjection>>
                    selected_projections;
                const auto include_projection = [&](const Composition &view) {
                  if (view.planner_scene_projection == nullptr) {
                    Require(view.runtime == nullptr &&
                                view.gripper_runtime != nullptr,
                            "retained motion candidate has no Scene projection");
                    return;
                  }
                  Require(selected_projections
                              .emplace(view.controllable_component_id,
                                       view.planner_scene_projection)
                              .second,
                          "retained native candidate has a duplicate Scene "
                          "projection");
                };
                include_projection(*prepared);
                for (const auto &component :
                     prepared->participant_compositions)
                  include_projection(*component);
                RefreshRetainedNativePublicationView(
                    *prepared, selected_projections,
                    prepared->required_projection_pins,
                    request.candidate_projection_revision,
                    prepared->publication_generation,
                    config.manager_run_root);
              }
              if (retained_component_ids.empty()) {
                candidate_stop_sources = NativeCellStopSources(*prepared);
              }
              auto candidate_registrations =
                  NativeCellComponentRegistrations(*prepared);
              auto candidate_resolver = NativeCellParticipantResolver(
                  *prepared, candidate_registrations);
              static_cast<void>(LoadManagedBootstrapStopProduct(
                  config, prepared->bundle->retained_bundle_id(),
                  prepared->bundle->value().cell_graph.graph_revision));
              auto reconfigured =
                  stable_cell_runtime_owner->reconfigure_runtime_component_set(
                      std::move(candidate_registrations), std::move(candidate_resolver),
                      std::move(candidate_stop_sources),
                      request.candidate_projection_revision.retained_bundle_id,
                      request.candidate_projection_revision.graph_revision,
                      request.configuration_fence_id,
                      request.configuration_fence_scope, retained_component_ids);
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the selected runtime "
                     "participants: " +
                     reconfigured.error().message);
              }
              const auto adopt_launch = [&](Composition &component) {
                component.controller_launch_adopted = true;
              };
              adopt_launch(*prepared);
              for (auto &peer : prepared->participant_compositions)
                adopt_launch(*peer);
              if (prepared->runtime != nullptr)
                prepared->runtime->stop_surveillance_owner =
                    stable_stop_surveillance_owner;
              if (active_managed_composition) {
                Require(predecessor_engine_snapshot.has_value(), "predecessor engine snapshot is missing");
                retired_registration_engines = std::move(*predecessor_engine_snapshot);
                retired_managed_composition = std::move(active_managed_composition);
              }
            }
            // The stable Kernel and its registered engines must outlive every
            // fallible initialization below. Retain the candidate under the
            // held fence before those calls; failure must not destroy the
            // admission owner still referenced by the command surface.
            active_managed_composition = std::move(prepared);
            if (active_managed_composition->runtime != nullptr)
              active_managed_composition->runtime->BindCellRuntimeOwner(
                    *stable_cell_runtime_owner, config.runtime_epoch, config.cell_id);
            bind_cell_runtime_to_composition(*active_managed_composition);
            execution_composition.replace_current(active_managed_composition.get());
            execution_composition.addressed_kernel = stable_addressed_kernel();
            stop_recovery_runtime.store(active_managed_composition->runtime.get(),
                                        std::memory_order_release);
            auto &candidate = *active_managed_composition;
            Require(
                candidate.active_addressed_kernel()
                    ->SetRouteRecoveryPending(true)
                    .ok(),
                "selected runtime could not close route recovery admission");
            candidate.route_recovery_pending.store(true,
                                                   std::memory_order_release);
            // Program/Scene-only activation has no changed control domains.
            // There are no candidate configuration facts to stage in that case.
            if (!request.configuration_fence_scope.resources.empty()) {
              auto staged =
                candidate.active_addressed_kernel()
                    ->stage_current_runtime_configuration_facts_under_apply_fence(
                        request.configuration_fence_scope);
              if (!staged.ok()) {
                Fail("stable Kernel rejected candidate configuration staging: " +
                   staged.error().message);
              }
            }
            // The candidate, stable Kernel and Stop engines are now retained
            // under this exact Apply fence. Executor start does not wait for a
            // controller or issue a home goal; observed readiness remains NRT.
            for (const auto &[component_id, view] :
                 IndexNativeComponentCompositions(candidate)) {
              static_cast<void>(component_id);
              Require(view->ros2_control_runtime_handle != nullptr,
                      "installed ROS component has no bound runtime handle");
              view->ros2_control_runtime_handle->StartBackend();
            }
            if (original_apply_native_plan &&
                original_apply_native_plan->apply ==
                    core::NamedApplyPublicationTransactionDTO{request.apply_id, request.apply_generation}) {
              Require(original_apply_native_plan->component_bring_up &&
                          original_apply_native_plan->component_bring_up == original_apply_accepted_children &&
                          original_apply_native_plan->candidate_retained_bundle_id ==
                              candidate.bundle->retained_bundle_id(),
                      "installed original Apply lost its accepted native plan");
              const auto package = LoadExactManagedCandidateBundle(config, request);
              Require(package && package->bundle->canonical_bytes() == candidate.bundle->canonical_bytes(),
                      "installed original Apply lost its exact product artifacts");
              EnsureInstalledRetainedRos2ControlLifecycleProduct(candidate, package->package_root);
              const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(package->package_root);
              const auto representatives = CellControlBindingRepresentatives(candidate);
              EnsureGripperOnlyCellPorts(candidate);
              auto& route_owner = candidate.runtime
                  ? static_cast<deployment::IRouteConfigurationKernelPublicationOwner&>(*candidate.runtime)
                  : *candidate.cclc_cell_route_publication_owner;
              auto& readiness_owner = candidate.runtime
                  ? static_cast<core::ICCLCComponentCommandReadinessPort&>(*candidate.runtime)
                  : static_cast<core::ICCLCComponentCommandReadinessPort&>(*candidate.cclc_cell_command_readiness_port);
              const auto routes = InitializeRetainedNativeCandidateControlRoutes(
                  *candidate.bundle, *candidate.active_addressed_kernel(), route_owner,
                  readiness_owner, candidate.publication_generation, *original_apply_native_plan,
                  artifacts, representatives, config.execution_run_root, request.configuration_fence_id);
              if (!routes.ok())
                Fail("installed original Apply routes are unavailable: " + routes.error().message);
            }
            InstallDirectSceneFrameResolver(projections, candidate);
#if !defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
            if (candidate.runtime != nullptr)
              AttachProductPickPlacePlanningOwner(candidate);
#endif
          }
          execution_composition.replace_current(
              active_managed_composition.get());
          execution_composition.addressed_kernel = stable_addressed_kernel();
          stop_recovery_runtime.store(
              active_managed_composition == nullptr
                  ? nullptr
                  : active_managed_composition->runtime.get(),
              std::memory_order_release);
          // Update only the successfully installed occurrence. This includes
          // the exact retained-view path; cached earlier incident facts stay
          // on their original request context inside the process owner.
          if (active_managed_composition) {
            const auto context =
                "Apply=" + request.apply_id.value() + " generation=" +
                std::to_string(request.apply_generation.value());
            const auto update_context = [&](Composition &component) {
              component.controller_request_context = context;
              if (component.controller_process)
                component.controller_process->update_request_context(context);
            };
            update_context(*active_managed_composition);
            for (auto &peer :
                 active_managed_composition->participant_compositions)
              update_context(*peer);
          }
          installed_under_fence_composition_request = request;
          preparing_composition_request.reset();
        });
    } catch (...) {
      // Preparation was consumed. Preserve the first failure rather than
      // attempting another build on a later observation of this Apply.
      selected_composition_failure = std::current_exception();
      if (prepared != nullptr) {
        prepared->discard_unadopted_controller_launches();
        failed_prepared_composition = std::move(prepared);
      }
      throw;
    }
    require_installed_candidate_configuration();
    if (request.phase == component_manager::
                             ManagerSelectedRuntimeCompositionPhase::kPrepare) {
      return {request,
              component_manager::ManagerSelectedRuntimeCompositionState::
                  kInstalled,
              {}};
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      Require(installed_under_fence_composition_request.has_value() &&
                  *installed_under_fence_composition_request == request,
              "installed runtime composition changed before fence "
              "reconciliation completed");
      installed_component_receipts = read_installed_receipts();
      installed_composition_request = request;
      confirm_installed_projection_set(request);
      publish_installed_liveness_handoff(request);
      installed_under_fence_composition_request.reset();
    }
    try_release_retired_composition();
    return {
        request,
        component_manager::ManagerSelectedRuntimeCompositionState::kInstalled,
        installed_component_receipts};
  };

  const auto handle_manager_liveness = [&](const std::string_view packet) {
    const auto decoded =
        component_manager::detail::DecodeManagerLivenessRpcRequest(packet);
    const auto required = std::find_if(
        decoded.required_set.required_receivers.begin(),
        decoded.required_set.required_receivers.end(), [&](const auto &row) {
          return row.runtime_unit_id.value() == config.runtime_unit_id;
        });
    Require(required != decoded.required_set.required_receivers.end() &&
                decoded.required_set.cell_id.value() == config.cell_id &&
                decoded.receiver_runtime_unit_id == required->runtime_unit_id &&
                required->runtime_epoch.value() == config.runtime_epoch,
            "Manager liveness request names a foreign receiver");
    Require(decoded.policy == deployment::execution_runtime_contract::
                                  manager_liveness_policy() &&
                required->runtime_policy_id ==
                    decoded.policy.runtime_policy_id &&
                required->runtime_policy_revision ==
                    decoded.policy.runtime_policy_revision,
            "Manager liveness request differs from retained policy");

    const auto request_matches_publication = [&](const auto &publication) {
      return publication.publication_generation ==
                 decoded.active_publication_generation &&
             publication.request.candidate_retained_bundle_id ==
                 decoded.required_set.active_bundle_id &&
             publication.request.candidate_graph_revision ==
                 decoded.active_graph_revision &&
             publication.durable_public_pointer_content_digest ==
                 decoded.active_public_pointer_digest;
    };
    const auto require_durable_current_publication = [&]() {
      auto publication = read_current_publication();
      if (!request_matches_publication(publication) &&
          decoded.active_publication_generation.value() <
              publication.publication_generation.value()) {
        return std::optional<core::ActiveBundlePublicationReceiptDTO>{};
      }
      Require(request_matches_publication(publication),
              "Manager liveness request has foreign active-publication "
              "identity");
      return std::optional<core::ActiveBundlePublicationReceiptDTO>(
          std::move(publication));
    };

    const core::ManagerLivenessReceiverConfig receiver_config{
        decoded.required_set.cell_id,
        required->runtime_unit_id,
        required->runtime_epoch,
        decoded.required_set.manager_runtime_unit_id,
        decoded.required_set.manager_runtime_epoch,
        decoded.required_set.manager_owner_epoch,
        decoded.policy,
        decoded.required_set.active_bundle_id,
        decoded.required_set.required_runtime_set_digest};
    if (manager_liveness_receiver.get() == nullptr) {
      if (!require_durable_current_publication().has_value()) {
        return component_manager::detail::EncodeManagerLivenessRpcRejection(
            "superseded_active_publication");
      }
      // Initial construction and exact claim settlement own attachment.
      // Their Kernel barrier remains closed through controller readback;
      // this independent lease handler never performs controller I/O.
      const auto installed =
          stable_kernel_owner->ReplaceManagerLivenessRequiredSet(
              decoded.required_set);
      Require(installed.ok(),
              "Kernel rejected the exact Manager-liveness required set");
      manager_liveness_receiver.Install(
          receiver_config,
          [&manager_liveness_admission](
              const core::ManagerLivenessLossIntentDTO &intent) {
            return manager_liveness_admission.CloseAndAttest(intent);
          },
          *stable_kernel_owner, *stable_kernel_owner);
      manager_liveness_installed.store(true, std::memory_order_release);
      manager_liveness_receiver_config = receiver_config;
      manager_liveness_publication_generation =
          decoded.active_publication_generation;
      manager_liveness_public_pointer_digest =
          decoded.active_public_pointer_digest;
      manager_liveness_required_set = decoded.required_set;
    } else {
      Require(manager_liveness_receiver_config.has_value() &&
                  manager_liveness_receiver_config->cell_id ==
                      receiver_config.cell_id &&
                  manager_liveness_receiver_config->receiver_runtime_unit_id ==
                      receiver_config.receiver_runtime_unit_id &&
                  manager_liveness_receiver_config->receiver_runtime_epoch ==
                      receiver_config.receiver_runtime_epoch &&
                  manager_liveness_receiver_config->policy ==
                      receiver_config.policy &&
                  manager_liveness_receiver_config->manager_runtime_unit_id ==
                      receiver_config.manager_runtime_unit_id &&
                  manager_liveness_publication_generation.has_value() &&
                  manager_liveness_public_pointer_digest.has_value(),
              "Manager liveness request changed immutable receiver evidence");
      const bool publication_changed =
          *manager_liveness_publication_generation !=
              decoded.active_publication_generation ||
          *manager_liveness_public_pointer_digest !=
              decoded.active_public_pointer_digest;
      Require(!publication_changed ||
                  (manager_liveness_receiver_config->manager_runtime_epoch ==
                       receiver_config.manager_runtime_epoch &&
                   manager_liveness_receiver_config->manager_owner_epoch ==
                       receiver_config.manager_owner_epoch),
              "Manager liveness publication rebind changed Manager identity");
      if (manager_liveness_receiver_config->manager_runtime_epoch !=
              receiver_config.manager_runtime_epoch ||
          manager_liveness_receiver_config->manager_owner_epoch !=
              receiver_config.manager_owner_epoch) {
        const auto candidate_epoch =
            std::pair{receiver_config.manager_runtime_epoch.value(),
                      receiver_config.manager_owner_epoch.value()};
        Require(!retired_manager_epochs.contains(candidate_epoch),
                "stale Manager epoch cannot replace the current Manager");
        Require(retired_manager_epochs.size() < 64U,
                "Manager epoch replacement history is exhausted");
        retired_manager_epochs.emplace(
            manager_liveness_receiver_config->manager_runtime_epoch.value(),
            manager_liveness_receiver_config->manager_owner_epoch.value());
        const auto replaced =
            manager_liveness_receiver.get()->BeginManagerEpochReplacement(
                receiver_config.manager_runtime_unit_id,
                receiver_config.manager_runtime_epoch,
                receiver_config.manager_owner_epoch);
        Require(replaced.ok(),
                "Manager liveness epoch replacement was rejected");
        manager_liveness_receiver_config->manager_runtime_unit_id =
            receiver_config.manager_runtime_unit_id;
        manager_liveness_receiver_config->manager_runtime_epoch =
            receiver_config.manager_runtime_epoch;
        manager_liveness_receiver_config->manager_owner_epoch =
            receiver_config.manager_owner_epoch;
      }
      if (publication_changed) {
        const auto current_publication = require_durable_current_publication();
        if (!current_publication.has_value()) {
          return component_manager::detail::EncodeManagerLivenessRpcRejection(
              "superseded_active_publication");
        }
        const auto installed_request =
            installed_liveness_handoff.load(std::memory_order_acquire);
        const auto *const named =
            std::get_if<core::NamedApplyPublicationTransactionDTO>(
                &current_publication->request.transaction_ref);
        Require(
            named != nullptr && installed_request != nullptr &&
                installed_request->apply_id == named->apply_id &&
                installed_request->apply_generation ==
                    named->apply_generation &&
                installed_request->candidate_projection_revision ==
                    current_publication->request
                        .candidate_projection_revision &&
                installed_request->runtime_unit_id ==
                    required->runtime_unit_id &&
                installed_request->runtime_epoch == required->runtime_epoch &&
                installed_request->manager_runtime_epoch ==
                    receiver_config.manager_runtime_epoch &&
                installed_request->expected_predecessor_publication_generation
                            .value() +
                        1U ==
                    current_publication->publication_generation.value(),
            "Manager liveness successor lacks exact installed composition "
            "evidence");
        // The exact installed handoff authorizes this lease rebind. The
        // subsequent Apply/authoring claim settlement owns attachment, with
        // ordinary execution closed by the Kernel's route-recovery barrier.
        manager_liveness_admission.CloseForRequiredSetRebind();
        const auto installed =
            stable_kernel_owner->ReplaceManagerLivenessRequiredSet(
                decoded.required_set);
        Require(installed.ok(),
                "Kernel rejected the successor Manager-liveness required set");
        const auto rebound = manager_liveness_receiver.get()->RebindRequiredSet(
            receiver_config.active_bundle_id,
            receiver_config.required_runtime_set_digest);
        Require(rebound.ok(),
                "Manager-liveness receiver rejected the successor required-set "
                "rebind");
        manager_liveness_receiver_config->active_bundle_id =
            receiver_config.active_bundle_id;
        manager_liveness_receiver_config->required_runtime_set_digest =
            receiver_config.required_runtime_set_digest;
        manager_liveness_required_set = decoded.required_set;
        manager_liveness_publication_generation =
            decoded.active_publication_generation;
        manager_liveness_public_pointer_digest =
            decoded.active_public_pointer_digest;
      } else {
        Require(
            manager_liveness_required_set.has_value() &&
                *manager_liveness_required_set == decoded.required_set &&
                manager_liveness_receiver_config->active_bundle_id ==
                    receiver_config.active_bundle_id &&
                manager_liveness_receiver_config->required_runtime_set_digest ==
                    receiver_config.required_runtime_set_digest,
            "Manager liveness request changed required-set identity "
            "without a publication transition");
      }
    }
    const auto admission_generation = manager_liveness_admission.generation();
    const auto renewed =
        manager_liveness_receiver.get()->Renew(decoded.heartbeat);
    Require(renewed.ok(), "Manager heartbeat renewal was rejected");
    if (decoded.reconciliation.has_value() &&
        manager_liveness_receiver.get()->status().command_fenced) {
      const auto reconciled =
          manager_liveness_receiver.get()->CompleteReconciliation(
              *decoded.reconciliation);
      Require(reconciled.ok(), "Manager liveness reconciliation was rejected");
    }
    const auto status = manager_liveness_receiver.get()->status();
    if (status.state == core::ManagerLivenessStateDTO::kCurrent &&
        !status.command_fenced) {
      manager_liveness_admission.OpenIfUnchanged(admission_generation);
    }
    return component_manager::detail::EncodeManagerLivenessRpcResponse(
        core::ManagerLivenessObservationDTO{
            status, decoded.required_set.active_bundle_id,
            decoded.required_set.required_runtime_set_digest,
            core::ComputeManagerLivenessStatusDigest(status)});
  };

  const auto rethrow_manager_liveness_failure = [&] {
    std::lock_guard lock(manager_liveness_failure_mutex);
    if (manager_liveness_failure != nullptr) {
      std::rethrow_exception(manager_liveness_failure);
    }
  };
  manager_liveness_lane = std::jthread([&](const std::stop_token stop_token) {
    try {
      bool ordinary_status_available = true;
      while (!stop_token.stop_requested()) {
        std::array<pollfd, 2> descriptors{
            {{config.manager_liveness_channel_fd, POLLIN | POLLHUP, 0},
             {shutdown_status_servicing.load(std::memory_order_acquire) &&
                      ordinary_status_available
                  ? config.manager_channel_fd
                  : -1,
              POLLIN | POLLHUP, 0}}};
        int ready = -1;
        do {
          ready = ::poll(descriptors.data(), descriptors.size(), 100);
        } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
        Require(ready >= 0, "poll managed runtime liveness channel failed");
        if (ready == 0) {
          continue;
        }
        for (std::size_t index = 0; index < descriptors.size(); ++index) {
          const auto &descriptor = descriptors[index];
          if (descriptor.revents == 0)
            continue;
          if ((descriptor.revents & POLLHUP) != 0 &&
              (descriptor.revents & POLLIN) == 0) {
            if (index == 0U)
              return;
            ordinary_status_available = false;
            continue;
          }
          if (index == 1U) {
            // The sole ordinary reader has exited before this fd is enabled.
            // Reject all effects here. Diagnostic failure disables only status;
            // it cannot stop or block the independent heartbeat servicing path.
            try {
              Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
                      "shutdown progress channel became invalid");
              const auto packet = ReceiveManagedRuntimePacket(descriptor.fd);
              if (!packet) {
                ordinary_status_available = false;
                continue;
              }
              Require(
                  component_manager::detail::IsNativeShutdownRpcPacket(
                      *packet) &&
                      component_manager::detail::DecodeNativeShutdownRpcRequest(
                          *packet) == config.runtime_epoch,
                  "shutdown progress request names a foreign protocol or "
                  "epoch");
              const auto response =
                  component_manager::detail::EncodeNativeShutdownRpcResponse(
                      shutdown_projection.Read());
              // Status is optional observation. Never spend the ordinary 750ms
              // send wait on this same lane while heartbeat renewal is due.
              const auto sent =
                  ::send(descriptor.fd, response.data(), response.size(),
                         MSG_NOSIGNAL | MSG_DONTWAIT);
              if (sent != static_cast<ssize_t>(response.size()))
                ordinary_status_available = false;
            } catch (...) {
              ordinary_status_available = false;
            }
            continue;
          }
          Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
                  "managed runtime liveness channel became invalid");
          const auto packet = ReceiveManagedRuntimePacket(descriptor.fd);
          if (!packet)
            return;
          std::lock_guard servicing_lock(manager_liveness_servicing_mutex);
          if (teardown_only) {
            // No borrowed Kernel/runtime access after the exclusion transition.
            SendManagedRuntimePacket(
                config.manager_liveness_channel_fd,
                component_manager::detail::EncodeManagerLivenessRpcRejection(
                    "runtime_shutting_down"));
            continue;
          }
          if (component_manager::detail::IsManagerLivenessRpcPacket(*packet)) {
            SendManagedRuntimePacket(config.manager_liveness_channel_fd,
                                     handle_manager_liveness(*packet));
            continue;
          }
          Require(
              component_manager::detail::IsKernelCellAdmissionRpcPacket(
                  *packet),
              "dedicated Manager-liveness channel received another protocol");
          const auto decoded =
              component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                  *packet);
          const bool publish_liveness =
              decoded.command == "cell_admission_publish_liveness" &&
              std::holds_alternative<core::ManagerLivenessObservationDTO>(
                  decoded.request);
          const bool update_readiness =
              decoded.command == "cell_admission_update_readiness" &&
              std::holds_alternative<core::CellAdmissionReadinessSnapshotDTO>(
                  decoded.request);
          // Only the ordered fail-closed stream belongs on this channel.  All
          // user/authoring admission commands remain on the ordinary channel.
          Require(publish_liveness || update_readiness,
                  "dedicated Manager-liveness channel received an ordinary "
                  "cell-admission command");
          component_manager::detail::KernelCellAdmissionRpcResponse response;
          try {
            response =
                component_manager::detail::DispatchKernelCellAdmissionRpc(
                    *stable_kernel_owner, decoded);
          } catch (const std::exception &error) {
            response = core::ErrorDTO{
                "cell_admission_request_failed", error.what(), {}};
          } catch (...) {
            response = core::ErrorDTO{"cell_admission_request_failed",
                                      "cell-admission request failed",
                                      {}};
          }
          SendManagedRuntimePacket(
              config.manager_liveness_channel_fd,
              component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                  decoded.command, response));
        }
      }
    } catch (...) {
      {
        std::lock_guard lock(manager_liveness_failure_mutex);
        manager_liveness_failure = std::current_exception();
      }
      static_cast<void>(::shutdown(config.manager_channel_fd, SHUT_RDWR));
    }
  });

  // Keep renewal servicing and captured state alive until every composition
  // builder and execution lane has drained, including exception unwinding.
  // Otherwise reverse declaration order stops renewal before that drain while
  // the receiver's independent watchdog is still armed.
  struct DrainBeforeLivenessShutdown {
    std::array<std::jthread *, 3> workers;
    ManagedProjectionRegistry *projections;
    component_manager::detail::NativeShutdownProjection &progress;
    std::mutex &servicing_mutex;
    bool &teardown_only;
    std::atomic<bool> &status_servicing;
    ManagedIsaacCellProcesses *cells;
    ~DrainBeforeLivenessShutdown() {
      // Keep existing renewals through group/worker/projection drain, as
      // before. Their original receiver can still be armed during those drains.
      progress.Enter(ShutdownPhase::kGroupRetirement);
      // This destructor runs on the sole ordinary reader after that reader's
      // loop/receive has exited, including unwinding. Publish the handoff once.
      status_servicing.store(true, std::memory_order_release);
      HandleStopSignal(0); // same existing stop/wake fact, including barriers
      for (auto *worker : workers)
        worker->request_stop();
      auto snapshot = cells->BeginHostShutdown();
      auto &supervisor = host_controller_launch_supervisor();
      while (true) {
        const auto actual = supervisor.PollHostShutdown(snapshot);
        progress.Groups(actual.total, actual.pending, actual.signalled,
                        actual.escalated, actual.reaped, actual.drained,
                        actual.unknown, actual.pending_pid);
        if (actual.pending == 0U) {
          supervisor.enqueue_host_log(
              "native_shutdown_group_retirement completed total=" +
              std::to_string(actual.total) +
              " signalled=" + std::to_string(actual.signalled) +
              " escalated=" + std::to_string(actual.escalated) +
              " reaped=" + std::to_string(actual.reaped) +
              " drained=" + std::to_string(actual.drained) +
              " unknown=" + std::to_string(actual.unknown));
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      const std::array phases{ShutdownPhase::kExecutionDrain,
                              ShutdownPhase::kCurrentBuilderDrain,
                              ShutdownPhase::kManagedBuilderDrain};
      for (std::size_t i = 0; i < workers.size(); ++i) {
        progress.Enter(phases[i]);
        if (workers[i]->joinable()) {
          workers[i]->join();
          progress.WorkerJoined(); // only an actual completed join
        }
      }
      progress.Enter(ShutdownPhase::kProjectionDrain);
      projections->Drain();
      {
        // Finish the last ordinary private handler before any borrowed owner
        // destructor; all subsequent status uses only the outer projection.
        std::lock_guard lock(servicing_mutex);
        teardown_only = true;
      }
    }
  } drain_before_liveness_shutdown{
      {&execution_surface, &current_composition_builder,
       &managed_composition_builder},
      &projections,
      shutdown_projection,
      manager_liveness_servicing_mutex,
      teardown_only,
      shutdown_status_servicing
      ,
      &managed_isaac_cell
  };

  bool process_inventory_owner_changing_logged = false;
  while (g_stop_requested == 0) {
    rethrow_manager_liveness_failure();
    rethrow_composition_failure();
    try_start_current_composition();
    try_release_retired_composition();
    pollfd descriptor{config.manager_channel_fd, POLLIN | POLLHUP, 0};
    int ready = -1;
    do {
      ready = ::poll(&descriptor, 1U, 1000);
    } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
    if (g_stop_requested != 0) {
      break;
    }
    if (ready < 0) {
      FailErrno("poll managed runtime parent channel");
    }
    if ((descriptor.revents & POLLHUP) != 0 &&
        (descriptor.revents & POLLIN) == 0) {
      await_manager_loss_containment();
      rethrow_manager_liveness_failure();
      return 0;
    }
    Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
            "managed runtime parent channel became invalid");
    if ((descriptor.revents & POLLIN) == 0) {
      continue;
    }
    const auto packet = ReceiveManagedRuntimePacket(config.manager_channel_fd);
    if (!packet.has_value()) {
      rethrow_manager_liveness_failure();
      await_manager_loss_containment();
      return 0;
    }
    const std::string_view request(*packet);
    if (request == "hello") {
      SendManagedRuntimePacket(config.manager_channel_fd, ready_packet);
      continue;
    }
    if (request == "status") {
      struct InstalledControllerOccurrence final {
        std::shared_ptr<ControllerLaunchProcess> process;
        std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
      };
      std::vector<InstalledControllerOccurrence> controller_occurrences;
      {
        std::lock_guard lock(managed_composition_mutex);
        // Every installed occurrence, including inactive/no-target components.
        // A changing composition does not erase the predecessor's actual owner.
        if (active_managed_composition) {
          const auto retain = [&](const Composition &component) {
            if (component.controller_process)
              controller_occurrences.push_back(
                  {component.controller_process,
                   component.ros2_control_runtime_handle});
          };
          retain(*active_managed_composition);
          for (const auto &peer :
               active_managed_composition->participant_compositions)
            retain(*peer);
        }
      }
      // Only cached installed-handle readiness, waitid(WNOHANG), a short
      // evidence lock and an already-started lane enqueue run here. Group
      // waits/scan/reap execute off this 750ms RPC lane.
      for (const auto &occurrence : controller_occurrences)
        static_cast<void>(
            host_controller_launch_supervisor().request_unexpected_exit_cleanup(
                occurrence.process, *controller_cleanup_scheduler,
                controller_cleanup_clock->now(),
                CachedControllerLaunchReadinessSource(occurrence.handle)));
      deployment::CCLCRuntimeComposition *observer_journal_owner = nullptr;
      {
        std::lock_guard lock(managed_composition_mutex);
        if (!current_composition_building && !managed_composition_building &&
            !preparing_composition_request &&
            !installed_under_fence_composition_request)
          observer_journal_owner = current_cell_cclc();
      }
      if (observer_journal_owner) {
        try {
          managed_isaac_cell.persist_current_exit(*observer_journal_owner);
        } catch (...) {
          // Persistence never suppresses the genuine exit or ordinary host
          // status. The read-only observation still reports uncertainty.
        }
      }
      // The ordinary Manager progress poll also correlates the already minted
      // CCLC incident with its existing process evidence. No new polling owner.
      try {
        struct IncidentCorrelation final {
          std::shared_ptr<ControllerLaunchProcess> process;
          std::string incident_id;
          std::string context;
          std::string summary;
          core::ControllableComponentId component_id;
          std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
          std::shared_ptr<const deployment::RetainedBundle> bundle;
          bool needs_snapshot{};
        };
        std::vector<IncidentCorrelation> correlations;
        {
          std::lock_guard lock(managed_composition_mutex);
          auto *cell = active_managed_composition.get();
          auto *const cclc = current_cell_cclc();
          if (cell && cclc) {
            const auto snapshot = [&](Composition &component) {
              if (!component.controller_process ||
                  (component.ros2_control_runtime_handle &&
                   component.ros2_control_runtime_handle
                       ->HasControllerProcessIncidentDiagnostic()))
                return;
              const auto view =
                  cclc->lifecycle_target_driver().ReadTargetView(
                      component.controllable_component_id);
              if (!view.ok() || !view.value().progress ||
                  !view.value().progress->incident)
                return;
              const auto &incident = *view.value().progress->incident;
              const auto context =
                  !component.controller_request_context.empty()
                      ? component.controller_request_context
                      : "Bootstrap runtime=" + config.runtime_unit_id +
                            " epoch=" + config.runtime_epoch;
              correlations.push_back(
                  {component.controller_process, incident.incident_id, context,
                   incident.summary, component.controllable_component_id,
                   component.ros2_control_runtime_handle, component.bundle,
                   !incident.diagnostic_snapshot &&
                       incident.containment_state !=
                           core::ComponentIncidentContainmentStateDTO::
                               kPending});
            };
            snapshot(*cell);
            for (auto &peer : cell->participant_compositions)
              snapshot(*peer);
          }
        }
        // Auxiliary IO is outside composition, CCLC/Kernel and process
        // observation locks. Operational Stop uses its separate control lane.
        for (const auto &correlation : correlations) {
          static_cast<void>(correlation.process->append_incident_correlation(
              correlation.incident_id, correlation.context,
              correlation.summary));
          if (!correlation.needs_snapshot || !correlation.handle ||
              !correlation.bundle ||
              correlation.handle->HasControllerProcessIncidentDiagnostic(
                  correlation.incident_id))
            continue;
          const auto exit = correlation.process->exit_evidence();
          if (!exit)
            continue;
          const auto snapshot = ControllerLaunchIncidentSnapshot(
              *correlation.process, *exit, *correlation.bundle,
              config.runtime_unit_id, config.runtime_epoch);
          std::lock_guard lock(managed_composition_mutex);
          if (!active_managed_composition)
            continue;
          const auto installed =
              IndexNativeComponentCompositions(*active_managed_composition);
          const auto exact = installed.find(correlation.component_id);
          if (exact != installed.end() &&
              exact->second->controller_process == correlation.process &&
              exact->second->ros2_control_runtime_handle == correlation.handle)
            static_cast<void>(
                correlation.handle->CacheControllerProcessIncidentDiagnostic(
                    correlation.incident_id, snapshot));
        }
      } catch (...) {
        // Diagnostic capture/write failure cannot suppress the primary fault
        // or break the Manager's ordinary status/liveness protocol.
      }
      Require(source_report_sequence !=
                  std::numeric_limits<std::uint64_t>::max(),
              "managed runtime source sequence is exhausted");
      ++source_report_sequence;
      auto source_status =
          "manyforge.native-execution-status.v2 " + config.runtime_unit_id +
          " " + config.runtime_epoch + " " +
          std::to_string(source_report_sequence) + " 1 serving healthy " +
          offer_digest + " " + config.product_identity + " " +
          config.configuration_identity + " " +
          std::string(SelectedExecutionRuntimeRole()) +
          " closed_no_active_effect";
      const auto host_counts =
          host_controller_launch_supervisor().host_output_counters();
      source_status += " dropped_host_notice_lines=" +
                       std::to_string(host_counts.dropped_forward_lines) +
                       " dropped_host_notice_bytes=" +
                       std::to_string(host_counts.dropped_forward_bytes);
      SendManagedRuntimePacket(config.manager_channel_fd, source_status);
      continue;
    }
    if (request == "shutdown") {
      SendManagedRuntimePacket(config.manager_channel_fd, "shutdown-accepted");
      return 0;
    }
    if (component_manager::detail::IsManagerLivenessRpcPacket(request)) {
      Fail("Manager-liveness protocol is forbidden on the ordinary managed "
           "runtime channel");
    }
    if (component_manager::detail::IsSelectedRuntimeCompositionRpcPacket(
            request)) {
      component_manager::detail::SelectedRuntimeCompositionRpcResponse
          response =
              component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                  core::ErrorDTO{"selected_runtime_composition_request_failed",
                                 "selected runtime-composition request was not "
                                 "dispatched",
                                 {}}};
      try {
        const auto decoded = component_manager::detail::
            DecodeSelectedRuntimeCompositionRpcRequestVariant(request);
        response = std::visit(
            [&](const auto &value)
                -> component_manager::detail::SelectedRuntimeCompositionRpcResponse {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<
                                T, component_manager::ManagerSelectedRuntimeCompositionRequestDTO>) {
                return install_or_observe_composition(value);
              } else if constexpr (
                  std::is_same_v<
                      T, component_manager::
                             ManagerComponentProcessInventoryRequestDTO>) {
                using Result = component_manager::
                    ManagerComponentProcessInventoryResultDTO;
                const auto failure = [](const std::string &code,
                                        const std::string &message) {
                  return component_manager::detail::
                      SelectedRuntimeCompositionRpcFailure{
                          core::ErrorDTO{code, message, {}}};
                };
                if (!value.valid() ||
                    value.runtime_unit_id.value() != config.runtime_unit_id ||
                    value.runtime_epoch.value() != config.runtime_epoch)
                  return failure("installed_process_inspection_scope_stale",
                                 "installed process read names another actual "
                                 "host occurrence");
                const auto current_manager = [&] {
                  if (!manager_liveness_installed.load(
                          std::memory_order_acquire))
                    return false;
                  const auto observed =
                      manager_liveness_receiver.get()->status();
                  return observed.state ==
                             core::ManagerLivenessStateDTO::kCurrent &&
                         !observed.command_fenced &&
                         observed.manager_runtime_epoch ==
                             value.manager_runtime_epoch &&
                         observed.receiver_runtime_unit_id ==
                             value.runtime_unit_id &&
                         observed.receiver_runtime_epoch == value.runtime_epoch;
                };
                const auto same_publication = [&](const auto &publication) {
                  return publication.publication_generation ==
                             value
                                 .expected_predecessor_publication_generation &&
                         publication.durable_public_pointer_content_digest ==
                             value.expected_predecessor_public_pointer_digest &&
                         publication.request.candidate_projection_revision ==
                             value.predecessor_projection_revision;
                };
                if (!current_manager() ||
                    !same_publication(read_current_publication()))
                  return failure("installed_process_inspection_scope_stale",
                                 "installed process read lost its current "
                                 "Manager or publication scope");
                Result result{value, {}};
                bool changing = false;
                struct CapturedOccurrence final {
                  core::ControllableComponentId component_id;
                  std::shared_ptr<ControllerLaunchProcess> process;
                  std::shared_ptr<const deployment::RetainedBundle> bundle;
                };
                std::vector<CapturedOccurrence> captures;
                std::vector<VerifiedLocalNoneProcessOwner> local_owners;
                std::set<const RetainedNativeGripperRuntime*> included_local_owners;
                // Same acquisition order as the existing candidate inspection:
                // managed composition mutex, then execution-composition read
                // lock.
                const auto roots = [&] {
                  return std::array<const Composition *, 5>{
                      active_managed_composition.get(),
                      retired_managed_composition.get(),
                      prepared_managed_composition.get(),
                      failed_prepared_composition.get(),
                      execution_composition.current};
                };
                const auto owners_changing = [&] {
                  return current_composition_building ||
                         managed_composition_building ||
                         preparing_composition_request ||
                         installed_under_fence_composition_request ||
                         prepared_managed_composition ||
                         prepared_retained_view ||
                         prepared_managed_composition_is_empty ||
                         selected_composition_failure != nullptr ||
                         managed_composition_failure != nullptr;
                };
                std::array<const Composition *, 5> before{};
                {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  before = roots();
                  changing = owners_changing();
                  std::set<const Composition *> included;
                  for (const auto *root : before) {
                    if (!root || !included.insert(root).second)
                      continue;
                    for (const auto &[component, view] :
                         IndexNativeComponentCompositions(*root)) {
                      if (!view->controller_process) {
                        // A local owner has no child process to retain from an
                        // uninstalled/retired view. Only the current installed
                        // composition can supply its exact logical occurrence.
                        const bool installed_root = root == active_managed_composition.get() ||
                            (!active_managed_composition && root == execution_composition.current);
                        if (installed_root && IsLocalNoOutputGripperOwner(*view) &&
                            included_local_owners.insert(view->gripper_runtime.get()).second) {
                          if (local_owners.size() == component_manager::kMaximumComponentProcessInventoryEntries)
                            return failure("installed_process_inventory_exceeds_bound", "installed logical owner union exceeds its bound");
                          local_owners.push_back(ReadVerifiedLocalNoneProcessOwner(*view, value));
                        }
                        continue;
                      }
                      // At most five owner views can refer to the same process.
                      // Bound captures, then enforce the distinct union below.
                      if (captures.size() ==
                          before.size() *
                              component_manager::
                                  kMaximumComponentProcessInventoryEntries)
                        return failure(
                            "installed_process_inventory_exceeds_bound",
                            "installed process union exceeds its bound; no "
                            "truncated inventory returned");
                      captures.push_back(
                          {component, view->controller_process, view->bundle});
                    }
                  }
                }
                std::map<const ControllerLaunchProcess *, std::size_t>
                    included_processes;
                for (const auto &captured : captures) {
                  const auto &process = captured.process;
                  if (process->command().controllable_component_id !=
                      captured.component_id.value())
                    return failure("installed_process_inventory_invalid",
                                   "installed process occurrence has foreign "
                                   "component attribution");
                  const auto identity =
                      captured.bundle
                          ? deployment::derive_component_runtime_identity(
                                *captured.bundle, captured.component_id)
                          : std::nullopt;
                  const auto duplicate = included_processes.find(process.get());
                  if (duplicate != included_processes.end()) {
                    const auto &previous =
                        result.process_occurrences[duplicate->second];
                    if (previous.controllable_component_id !=
                            captured.component_id ||
                        previous.installed_runtime_identity != identity)
                      return failure("installed_process_inventory_invalid",
                                     "shared process has inconsistent verified "
                                     "owner runtime attribution");
                    continue;
                  }
                  const auto cleanup = process->cleanup_observation();
                  // Observe the exact owned leader just as the existing
                  // component BringUp inspection does. These are this host's
                  // known launches, not an exhaustive external process census.
                  const auto presence =
                      process->exited()
                          ? component_manager::ProcessPresenceState::kAbsent
                          : component_manager::ProcessPresenceState::kPresent;
                  if (result.process_occurrences.size() ==
                      component_manager::
                          kMaximumComponentProcessInventoryEntries)
                    return failure("installed_process_inventory_exceeds_bound",
                                   "installed process union exceeds its bound; "
                                   "no truncated inventory returned");
                  included_processes.emplace(process.get(),
                                             result.process_occurrences.size());
                  result.process_occurrences.push_back(
                      {captured.component_id, process->command().identity,
                       process->pid(), presence, cleanup.state, cleanup.receipt,
                       identity});
                }
                {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  changing = changing || owners_changing() || roots() != before;
                  for (const auto& local : local_owners) {
                    auto* const installed_kernel = stable_addressed_kernel();
                    if (!installed_kernel)
                      return failure("installed_process_inspection_scope_stale", "installed logical owner lost its stable Kernel");
                    const auto route = installed_kernel->current_runtime_route_configuration(
                        local.binding.value().control_path->control_domain_id);
                    if (local.runtime->binding().value() != local.binding.value() ||
                        !local.handle->OwnsGripper(local.runtime->backend(), local.binding) ||
                        !route.ok() || !route.value() || *route.value() != local.route)
                      changing = true;
                    if (result.process_occurrences.size() == component_manager::kMaximumComponentProcessInventoryEntries)
                      return failure("installed_process_inventory_exceeds_bound", "complete physical/logical owner union exceeds its bound");
                    result.process_occurrences.push_back(local.occurrence);
                  }
                }
                std::sort(result.process_occurrences.begin(),
                          result.process_occurrences.end(),
                          [](const auto &a, const auto &b) {
                            return std::tie(a.controllable_component_id,
                                            a.launch_identity, a.leader_pid) <
                                   std::tie(b.controllable_component_id,
                                            b.launch_identity, b.leader_pid);
                          });
                if (!current_manager() ||
                    !same_publication(read_current_publication()))
                  return failure("installed_process_inspection_scope_stale",
                                 "installed process read lost its original "
                                 "scope during observation");
                // Close and Apply use this host's known launch union, also
                // after restart. Unobserved external processes are outside this
                // inventory; unsettled current owners still remain unavailable.
                if (changing) {
                  result.error = core::ErrorDTO{
                      "process_inventory_owner_changing",
                      "installed component owners changed or remain unsettled",
                      {{"runtime_unit_id", value.runtime_unit_id.value()},
                       {"runtime_epoch", value.runtime_epoch.value()}}};
                  if (!process_inventory_owner_changing_logged) {
                    std::cerr << "installed component process inventory unknown: "
                                 "reason=process_inventory_owner_changing"
                              << " runtime=" << value.runtime_unit_id.value()
                              << " epoch=" << value.runtime_epoch.value() << '\n';
                    process_inventory_owner_changing_logged = true;
                  }
                }
                if (!result.valid())
                  return failure("installed_process_inventory_invalid",
                                 "installed process union is not a bounded "
                                 "exact occurrence inventory");
                return result;
              } else if constexpr (
                  std::is_same_v<T, component_manager::ManagerComponentProcessStopRequestDTO> ||
                  std::is_same_v<T, component_manager::ManagerComponentProcessStopReadRequestDTO>) {
                constexpr bool read_only = std::is_same_v<T,
                    component_manager::ManagerComponentProcessStopReadRequestDTO>;
                const auto &original = [&]() -> const component_manager::ManagerComponentProcessStopRequestDTO & {
                  if constexpr (read_only) return value.request;
                  else return value;
                }();
                Require(original.valid() && original.runtime_unit_id.value() == config.runtime_unit_id &&
                            original.runtime_epoch.value() == config.runtime_epoch,
                        "component Stop names another host occurrence");
                using StopResult = component_manager::ManagerComponentProcessStopResultDTO;
                const auto unknown = [&](const char *code, const char *message) {
                  return StopResult{original, core::ControllerLaunchCleanupState::kUnknown,
                                    std::nullopt, core::ErrorDTO{code, message, {}}};
                };
                const auto component = original.controllable_component_id.value();
                std::shared_ptr<ControllerLaunchProcess> process;
                std::unique_ptr<core::ICellCloseAdmissionLease> close_join;
                bool retain_result = true;
                {
                  std::lock_guard lock(managed_composition_mutex);
                  const auto cached = component_process_stop_results.find(component);
                  if constexpr (read_only) {
                    if (cached == component_process_stop_results.end() ||
                        cached->second.result.request != original)
                      return unknown("component_process_stop_result_unavailable",
                                     "the exact original Stop result is missing, superseded or evicted");
                    auto result = cached->second.result; // Copy before consuming.
                    component_process_stop_results.erase(cached);
                    return result;
                  } else if (cached != component_process_stop_results.end() &&
                             cached->second.result.request == original) {
                    return cached->second.result; // Exact duplicate never signals again.
                  }
                  Require(manager_liveness_installed.load(std::memory_order_acquire),
                          "component Stop has no current Manager observation");
                  const auto manager = manager_liveness_receiver.get()->status();
                  Require(manager.state == core::ManagerLivenessStateDTO::kCurrent &&
                              !manager.command_fenced &&
                              manager.manager_runtime_epoch == original.manager_runtime_epoch &&
                              manager.receiver_runtime_unit_id == original.runtime_unit_id &&
                              manager.receiver_runtime_epoch == original.runtime_epoch,
                          "component Stop names an unavailable Manager occurrence");
                  const auto publication = read_current_publication();
                  Require(publication.publication_generation == original.expected_predecessor_publication_generation &&
                              publication.durable_public_pointer_content_digest == original.expected_predecessor_public_pointer_digest &&
                              publication.request.candidate_projection_revision == original.predecessor_projection_revision,
                          "component Stop predecessor publication changed");
                  Require(!current_composition_building && !managed_composition_building &&
                              !preparing_composition_request && !installed_under_fence_composition_request,
                          "component Stop cannot cross an in-progress composition effect");
                  Composition *occurrence{};
                  const auto inspect = [&](Composition *root) {
                    if (!root) return;
                    const auto include = [&](Composition &item) {
                      if (item.controllable_component_id == original.controllable_component_id &&
                          item.controller_process && item.controller_process->pid() == original.leader_pid &&
                          item.controller_process->command().identity == original.launch_identity) {
                        Require(!process || process == item.controller_process,
                                "component Stop has contradictory launch occurrence owners");
                        occurrence = &item;
                        process = item.controller_process;
                      }
                    };
                    include(*root);
                    for (auto &peer : root->participant_compositions) {
                      Require(peer != nullptr, "component Stop has a missing physical owner");
                      include(*peer);
                    }
                  };
                  {
                    // Same five-root union and managed -> execution read lock
                    // order as the passive inventory. Keep the actual shared
                    // process lease after releasing the execution read lock.
                    std::shared_lock execution_lock(execution_composition.mutex);
                    inspect(active_managed_composition.get());
                    inspect(retired_managed_composition.get());
                    inspect(prepared_managed_composition.get());
                    inspect(failed_prepared_composition.get());
                    inspect(execution_composition.current);
                  }
                  Require(occurrence && process, "component Stop exact launch owner is unavailable");
                  auto *const kernel = stable_addressed_kernel();
                  Require(kernel, "component Stop has no current Kernel owner");
                  auto acquired_join = kernel->AcquireCellCloseAdmissionLease();
                  if (!acquired_join.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{acquired_join.error()};
                  close_join = std::move(acquired_join.value());
                  const auto prerequisites = close_join->ValidatePrerequisites();
                  if (!prerequisites.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{prerequisites.error()};
                  // DX583: receipt retention never refuses the mandatory exact
                  // Stop. Unresolved entries remain; when all slots are live,
                  // return the immediate actual outcome without retaining it.
                  if (cached == component_process_stop_results.end() &&
                      component_process_stop_results.size() >= core::kMaximumCCLCComponentWatermarks) {
                    auto oldest = component_process_stop_results.end();
                    for (auto it = component_process_stop_results.begin();
                         it != component_process_stop_results.end(); ++it) {
                      if (it->second.result.state == core::ControllerLaunchCleanupState::kDrained &&
                          (oldest == component_process_stop_results.end() ||
                           it->second.insertion_order < oldest->second.insertion_order)) oldest = it;
                    }
                    if (oldest == component_process_stop_results.end()) {
                      retain_result = false;
                      if (!component_process_stop_retention_exhaustion_logged) {
                        component_process_stop_retention_exhaustion_logged = true;
                        std::cerr << "component process Stop receipt retention exhausted; exact Stop proceeds, later result read is unknown\n";
                      }
                    } else component_process_stop_results.erase(oldest);
                  }
                  if (component_process_stop_result_order == std::numeric_limits<std::uint64_t>::max())
                    retain_result = false;
                  if (retain_result) component_process_stop_results.insert_or_assign(component,
                      ComponentProcessStopResultEntry{
                          StopResult{original, core::ControllerLaunchCleanupState::kPending, std::nullopt},
                          ++component_process_stop_result_order});
                }
                StopResult result{original, core::ControllerLaunchCleanupState::kUnknown, std::nullopt};
                try {
                  // Same original retirement lane: no composition/map lock
                  // spans SIGINT/grace/KILL; the shared exact process lease does.
                  const auto before = process->cleanup_observation();
                  const auto before_effect = close_join->ValidatePrerequisites();
                  if (!before_effect.ok()) {
                    result = StopResult{original, core::ControllerLaunchCleanupState::kUnknown, std::nullopt, before_effect.error()};
                  } else {
                  if (before.state != ControllerLaunchCleanupState::kDrained)
                    Require(host_controller_launch_supervisor().retire_exact(process),
                            "component Stop launch occurrence was superseded");
                  const auto observed = process->cleanup_observation();
                  result.state = observed.state;
                  result.receipt = observed.receipt;
                  Require(result.valid(), "component Stop cached drain receipt is foreign");
                  }
                } catch (const std::exception &error) {
                  result = unknown("component_process_stop_outcome_unknown", error.what());
                } catch (...) {
                  result = unknown("component_process_stop_outcome_unknown", "process Stop has no exact terminal result");
                }
                // Physical retirement and exact receipt validation are done.
                // Do not reacquire composition while retaining Kernel
                // admission: builders use composition -> Kernel.
                close_join.reset();
                {
                  std::lock_guard lock(managed_composition_mutex);
                  const auto cached = component_process_stop_results.find(component);
                  if (retain_result) {
                    Require(cached != component_process_stop_results.end() && cached->second.result.request == original,
                            "component Stop result owner changed during retirement");
                    cached->second.result = result;
                  }
                }
                return result;
              } else if constexpr (std::is_same_v<
                                       T, component_manager::ManagerComponentBringUpInspectionRequestDTO>) {
                Require(value.valid() && value.runtime_unit_id.value() == config.runtime_unit_id &&
                            value.runtime_epoch.value() == config.runtime_epoch,
                        "component inspection names another host occurrence");
                Require(manager_liveness_installed.load(std::memory_order_acquire),
                        "component inspection has no current Manager observation");
                const auto manager = manager_liveness_receiver.get()->status();
                Require(manager.state == core::ManagerLivenessStateDTO::kCurrent &&
                            !manager.command_fenced &&
                            manager.manager_runtime_epoch == value.manager_runtime_epoch &&
                            manager.receiver_runtime_unit_id == value.runtime_unit_id &&
                            manager.receiver_runtime_epoch == value.runtime_epoch,
                        "component inspection names an unavailable Manager occurrence");
                const auto require_predecessor = [&](const auto &publication) {
                  Require(publication.publication_generation == value.expected_predecessor_publication_generation &&
                              publication.durable_public_pointer_content_digest == value.expected_predecessor_public_pointer_digest &&
                              publication.request.candidate_projection_revision == value.predecessor_projection_revision,
                          "component inspection predecessor publication changed");
                };
                const auto predecessor = read_current_publication();
                require_predecessor(predecessor);
                const auto selected = LoadManagedBundleAtPinnedRoot(
                    config, value.candidate_projection_revision.retained_bundle_id);
                Require(selected && selected->bundle &&
                            selected->bundle->value().cell_graph.cell_id == core::CellId(config.cell_id) &&
                            selected->bundle->value().cell_graph.graph_revision == value.candidate_projection_revision.graph_revision &&
                            selected->bundle->canonical_bytes().size() == value.candidate_retained_bundle_bytes &&
                            core::ContentDigest(content_identity::sha256_digest(selected->bundle->canonical_bytes())) == value.candidate_retained_bundle_digest,
                        "component inspection candidate is not the exact verified bundle");
                const auto predecessor_bundle = LoadManagedBundleAtPinnedRoot(
                    config, value.predecessor_projection_revision.retained_bundle_id);
                Require(predecessor_bundle && predecessor_bundle->bundle,
                        "component inspection predecessor bundle is unavailable");
                std::optional<ManagedCandidateLifecyclePlanningClosure> candidate_planning;
                if (value.planning_component_id) {
                  Require(value.expected_predecessor_publication_generation.value() !=
                              std::numeric_limits<std::uint64_t>::max(),
                          "component planning publication generation is exhausted");
                  auto compiled = BuildManagedCandidateLifecyclePlanningClosure(
                      config, *selected, value.candidate_projection_revision,
                      core::ActiveBundlePublicationGeneration(
                          value.expected_predecessor_publication_generation.value() + 1U));
                  if (!compiled.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{compiled.error()};
                  candidate_planning = std::move(compiled.value());
                }
                deployment::CCLCRuntimeComposition *planning_owner = nullptr;
                deployment::CCLCRuntimeComposition *observer_read_owner = nullptr;
                bool planning_predecessor_present = false;
                component_manager::ManagerComponentBringUpInspectionResultDTO result{value, {}};
                std::vector<VerifiedLocalNoneProcessOwner> candidate_local_owners;
                const Composition* candidate_local_root = nullptr;
                {
                  // Borrow the existing owners only for synchronous process reads.
                  // No construction, dispatch, shutdown, or epoch mintage occurs.
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  const bool changing = current_composition_building || managed_composition_building ||
                      preparing_composition_request || installed_under_fence_composition_request ||
                      prepared_managed_composition || prepared_retained_view ||
                      prepared_managed_composition_is_empty ||
                      selected_composition_failure != nullptr || managed_composition_failure != nullptr;
                  // Changing component owners are unknown. The independent cell
                  // observer can still report its actual exit while Apply holds
                  // the composition fence; unknown rows authorize no bring-up.
                  if (!changing && active_managed_composition)
                    Require(active_managed_composition->projection_revision == value.predecessor_projection_revision &&
                                active_managed_composition->publication_generation == value.expected_predecessor_publication_generation,
                            "component inspection owner belongs to another publication");
                  // A populated published predecessor with no installed owners
                  // is unknown, not proof that its processes are absent.
                  const auto installed = active_managed_composition
                      ? IndexNativeComponentCompositions(*active_managed_composition)
                      : std::map<core::ControllableComponentId, const Composition *>{};
                  std::set<core::ControllableComponentId> predecessor_components;
                  for (const auto &row : predecessor_bundle->bundle->value().cell_graph.controllable_components)
                    predecessor_components.insert(row.controllable_component_id);
                  for (const auto &row : selected->bundle->value().cell_graph.controllable_components) {
                    Require(std::find(row.runtime_unit_ids.begin(), row.runtime_unit_ids.end(), value.runtime_unit_id) != row.runtime_unit_ids.end(),
                            "component inspection candidate names another runtime unit");
                    auto presence = component_manager::ProcessPresenceState::kUnknown;
                    const auto current = installed.find(row.controllable_component_id);
                    if (changing) {
                      // No installed-component presence inference is valid here.
                    } else if (current == installed.end()) {
                      if (!predecessor_components.contains(row.controllable_component_id))
                        presence = component_manager::ProcessPresenceState::kAbsent;
                    } else {
                      if (current->second->controller_process) {
                        Require(current->second->controller_process->command().controllable_component_id == row.controllable_component_id.value(),
                                "component process observation has foreign attribution");
                        presence = current->second->controller_process->exited()
                            ? component_manager::ProcessPresenceState::kAbsent
                            : component_manager::ProcessPresenceState::kPresent;
                      }
                    }
                    component_manager::ManagerComponentProcessObservationDTO observed{
                        row.controllable_component_id, presence};
                    if (!changing && current != installed.end() &&
                        IsLocalNoOutputGripperOwner(*current->second)) {
                      const component_manager::ManagerComponentProcessInventoryRequestDTO scope{
                          value.predecessor_projection_revision,
                          value.expected_predecessor_publication_generation,
                          value.expected_predecessor_public_pointer_digest,
                          value.runtime_unit_id, value.runtime_epoch, value.manager_runtime_epoch};
                      auto local = ReadVerifiedLocalNoneProcessOwner(*current->second, scope);
                      candidate_local_root = active_managed_composition.get();
                      observed.process_presence = component_manager::ProcessPresenceState::kAbsent;
                      observed.process_owner_kind = local.occurrence.process_owner_kind;
                      observed.pneumatic_none_owner = local.occurrence.pneumatic_none_owner;
                      observed.installed_runtime_identity = local.occurrence.installed_runtime_identity;
                      candidate_local_owners.push_back(std::move(local));
                    }
                    result.configured_components.push_back(std::move(observed));
                  }
                  // The initial builder can transfer ownership concurrently.
                  // Read the pointer under its publication mutex; an established
                  // cell facade is stable through later composition changes.
                  if (managed_cell_cclc.facade)
                    observer_read_owner = managed_cell_cclc.facade.get();
                  else if (!changing)
                    observer_read_owner = current_cell_cclc();
                  if (value.planning_component_id) {
                    Require(!changing && managed_cell_cclc.facade &&
                                managed_cell_cclc.publication == predecessor,
                            "component planning has no stable original cell owner");
                    planning_owner = managed_cell_cclc.facade.get();
                    planning_predecessor_present =
                        predecessor_components.contains(*value.planning_component_id);
                    if (!planning_predecessor_present) {
                      const auto row = std::find_if(result.configured_components.begin(),
                          result.configured_components.end(), [&](const auto &component) {
                            return component.controllable_component_id == *value.planning_component_id;
                          });
                      Require(!installed.contains(*value.planning_component_id) &&
                                  row != result.configured_components.end() &&
                                  row->process_presence == component_manager::ProcessPresenceState::kAbsent,
                              "initial component planning lacks positive predecessor inventory absence");
                    }
                  }
                }
                std::sort(result.configured_components.begin(), result.configured_components.end(),
                          [](const auto &lhs, const auto &rhs) {
                            return lhs.controllable_component_id < rhs.controllable_component_id;
                          });
                if (planning_owner) {
                  // Journal/catalog reads never run under composition or Kernel
                  // locks. The serial managed caller pins this actual facade;
                  // recreation must first exclude and drain all such callers.
                  const auto component = *value.planning_component_id;
                  const auto owner_epoch = planning_owner->coordinator().request_owner_epoch();
                  const auto generation = planning_owner->ReadApplyComponentGenerationSelection(component);
                  if (!generation.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{generation.error()};
                  std::optional<core::ComponentLifecycleCurrentSnapshotDTO> snapshot;
                  if (planning_predecessor_present) {
                    const auto current = planning_owner->ReadCurrentComponentSnapshot(component);
                    if (!current.ok())
                      return component_manager::detail::SelectedRuntimeCompositionRpcFailure{current.error()};
                    Require(generation.value().owner_qualified_predecessor &&
                                generation.value().owner_qualified_predecessor->generation ==
                                    current.value().component_lifecycle_generation &&
                                generation.value().owner_qualified_predecessor->snapshot_digest ==
                                    current.value().snapshot_digest &&
                                current.value().active_publication_receipt == predecessor,
                            "component planning snapshot differs from the owner-selected predecessor");
                    snapshot = current.value();
                  } else {
                    Require(!generation.value().owner_qualified_predecessor,
                            "initial component planning contains a foreign predecessor snapshot");
                  }
                  const auto target_view = planning_owner->lifecycle_target_driver().ReadTargetView(component);
                  if (!target_view.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{target_view.error()};
                  // This is the same absent-target startup choice used by
                  // native adoption. It is not an achieved lifecycle state.
                  const auto target = target_view.value().target
                      ? target_view.value().target->requested_state
                      : core::LifecycleTargetStateDTO::kActive;
                  const auto next = snapshot
                      ? core::NextLifecycleIntentTowardTarget(snapshot->component_lifecycle_state, target)
                      : std::optional<core::ComponentLifecycleIntentKindDTO>{
                            target == core::LifecycleTargetStateDTO::kDisabled
                                ? core::ComponentLifecycleIntentKindDTO::kDisable
                                : core::ComponentLifecycleIntentKindDTO::kPrepare};
                  Require(next && *next != core::ComponentLifecycleIntentKindDTO::kRecover,
                          "component planning has no approved next Apply bring-up action");
                  const auto closure = std::find_if(candidate_planning->components.begin(),
                      candidate_planning->components.end(), [&](const auto &row) {
                        return row.component_id == component;
                      });
                  Require(closure != candidate_planning->components.end(),
                          "component planning lacks its complete verified candidate closure");
                  const auto generation_after = planning_owner->ReadApplyComponentGenerationSelection(component);
                  const auto target_after = planning_owner->lifecycle_target_driver().ReadTargetView(component);
                  Require(generation_after.ok() && generation_after.value() == generation.value() &&
                              target_after.ok() && target_after.value().target == target_view.value().target &&
                              planning_owner->coordinator().request_owner_epoch() == owner_epoch,
                          "component planning owner facts changed during observation");
                  {
                    std::lock_guard composition_lock(managed_composition_mutex);
                    Require(managed_cell_cclc.facade.get() == planning_owner &&
                                managed_cell_cclc.publication == predecessor &&
                                !current_composition_building && !managed_composition_building &&
                                !preparing_composition_request && !installed_under_fence_composition_request,
                            "component planning original owner changed during observation");
                  }
                  result.planning_facts = component_manager::ManagerComponentBringUpPlanningFactsDTO{
                      component, owner_epoch, generation.value(), closure->candidate_expected_domains,
                      target, *next};
                }
                result.cell_observer = managed_isaac_cell.observe_current(observer_read_owner);
                if (!candidate_local_owners.empty()) {
                  std::lock_guard composition_lock(managed_composition_mutex);
                  std::shared_lock execution_lock(execution_composition.mutex);
                  Require(active_managed_composition.get() == candidate_local_root &&
                              !current_composition_building && !managed_composition_building &&
                              !preparing_composition_request && !installed_under_fence_composition_request,
                          "local None candidate owner changed during observation");
                  const auto installed = IndexNativeComponentCompositions(*active_managed_composition);
                  const component_manager::ManagerComponentProcessInventoryRequestDTO scope{
                      value.predecessor_projection_revision,
                      value.expected_predecessor_publication_generation,
                      value.expected_predecessor_public_pointer_digest,
                      value.runtime_unit_id, value.runtime_epoch, value.manager_runtime_epoch};
                  for (const auto& local : candidate_local_owners) {
                    const auto current = installed.find(local.occurrence.controllable_component_id);
                    Require(current != installed.end() && current->second->gripper_runtime == local.runtime &&
                                current->second->ros2_control_runtime_handle == local.handle,
                            "local None candidate lost its exact installed owner");
                    const auto readback = ReadVerifiedLocalNoneProcessOwner(*current->second, scope);
                    Require(readback.binding.value() == local.binding.value() && readback.route == local.route &&
                                readback.occurrence == local.occurrence,
                            "local None candidate binding/route changed during observation");
                  }
                }
                require_predecessor(read_current_publication());
                const auto after = manager_liveness_receiver.get()->status();
                Require(after.state == core::ManagerLivenessStateDTO::kCurrent && !after.command_fenced &&
                            after.manager_runtime_epoch == value.manager_runtime_epoch &&
                            after.receiver_runtime_epoch == value.runtime_epoch && result.valid(),
                        "component process inspection lost its current occurrence");
                return result;
              } else {
                constexpr bool read_only =
                    std::is_same_v<T, component_manager::ManagerSelectedComponentBringUpReadRequestDTO> ||
                    std::is_same_v<T, component_manager::ManagerApplyComponentBringUpReadRequestDTO>;
                const auto &original = [&]() -> const auto & {
                  if constexpr (read_only) return value.request;
                  else return value;
                }();
                using Request = std::decay_t<decltype(original)>;
                constexpr bool delegated = std::is_same_v<
                    Request, component_manager::ManagerApplyComponentBringUpRequestDTO>;
                using Response = std::conditional_t<
                    delegated, component_manager::ManagerApplyComponentBringUpResultDTO,
                    component_manager::ManagerSelectedComponentBringUpResultDTO>;
                if constexpr (delegated && !read_only) {
                  const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                      original.intent, original.candidate_projection_revision,
                      original.candidate_retained_bundle_digest, original.candidate_retained_bundle_bytes,
                      original.runtime_unit_id, original.runtime_epoch, original.manager_runtime_epoch,
                      original.parent_authority.parent);
                  deployment::CCLCRuntimeComposition *result_owner{};
                  std::shared_ptr<const ManagedOriginalApplyBringUpCapture> captured;
                  {
                    std::lock_guard lock(managed_composition_mutex);
                    result_owner = current_cell_cclc();
                    captured = original_apply_bring_up_capture;
                  }
                  Require(result_owner, "original delegated result journal is unavailable");
                  // The serial main channel pins this facade for the read. A
                  // retained result is classified before any current capture
                  // authorization, including after owner recreation or restart.
                  const auto retained = result_owner->coordinator().ReadApplyComponentBringUpRequestResult(
                      original.intent, digest);
                  if (!retained.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{retained.error()};
                  if (!retained.value())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                        core::ErrorDTO{"component_bring_up_live_authority_unavailable",
                                       "new bring-up requires the original accepted continuation", {}}};
                  Require(retained.value()->status.has_value(),
                          "delegated continuation has no actual retained child status");
                  const bool same_original_parent = captured &&
                      captured->sealed_plan.plan.apply_id == original.parent_authority.parent.owner.apply_id &&
                      captured->sealed_plan.plan.apply_generation == original.parent_authority.parent.owner.apply_generation &&
                      captured->sealed_plan.plan_digest == original.parent_authority.parent.plan_digest;
                  if (retained.value()->status->phase != core::ComponentLifecycleTransactionPhaseDTO::kAccepted ||
                      !same_original_parent) {
                    Response result{original, retained.value()};
                    Require(result.valid(), "delegated retained result differs from its request");
                    return result;
                  }
                  {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(original_apply_bring_up_capture == captured &&
                                captured->original_facade && result_owner == captured->original_facade &&
                                managed_cell_cclc.facade.get() == captured->original_facade &&
                                original_apply_accepted_children && original_apply_native_plan &&
                                original_apply_native_plan->component_bring_up == original_apply_accepted_children,
                            "new delegated continuation lacks its actual original accepted runtime");
                  }
                  const auto &plan = captured->sealed_plan.plan;
                  const auto child = std::find_if(plan.children.begin(), plan.children.end(),
                      [&](const auto &row) { return row.intent == original.intent; });
                  Require(child != plan.children.end(),
                          "delegated continuation is not a sealed original child");
                  const component_manager::ManagerApplyComponentBringUpRequestDTO expected{
                      child->intent, child->candidate_projection_revision,
                      child->candidate_retained_bundle_digest, child->candidate_retained_bundle_bytes,
                      child->runtime_unit_id, child->runtime_epoch, child->manager_runtime_epoch,
                      original.parent_authority};
                  Require(expected == original && original.parent_authority.parent_claim == captured->parent_claim &&
                              original.parent_authority.parent == core::ApplyComponentBringUpReferenceDTO{
                                  {plan.apply_id, plan.apply_generation}, captured->sealed_plan.plan_digest},
                          "delegated continuation differs from its full original request");
                  auto &coordinator = captured->original_facade->coordinator();
                  const auto qualified = std::find_if(captured->candidate_closure.components.begin(),
                      captured->candidate_closure.components.end(), [&](const auto &row) {
                        return row.component_id == original.intent.controllable_component_id;
                      });
                  Require(qualified != captured->candidate_closure.components.end(),
                          "delegated continuation lost its qualified candidate closure");
                  std::optional<ManagedInstalledLifecycleComponentClosure> installed;
                  core::ICCLCComponentCommandReadinessPort *readiness{};
                  const auto &query = original.parent_authority.held_fence_query;
                  fence_owner.RunWithExactHeldPredecessor(query,
                      captured->predecessor_publication.publication_generation,
                      captured->predecessor_publication.durable_public_pointer_content_digest, [&] {
                    std::lock_guard lock(managed_composition_mutex);
                    Require(managed_cell_cclc.facade.get() == captured->original_facade &&
                                !current_composition_building && !managed_composition_building &&
                                active_managed_composition && active_managed_composition->runtime &&
                                active_managed_composition->bundle->canonical_bytes() ==
                                    captured->candidate.bundle->canonical_bytes() &&
                                active_managed_composition->projection_revision == original.candidate_projection_revision &&
                                installed_under_fence_composition_request &&
                                installed_under_fence_composition_request->phase ==
                                    component_manager::ManagerSelectedRuntimeCompositionPhase::kPrepare &&
                                installed_under_fence_composition_request->apply_id == plan.apply_id &&
                                installed_under_fence_composition_request->apply_generation == plan.apply_generation,
                            "delegated continuation lacks its exact completed installation");
                    const auto actual = ReadInstalledManagedCandidateLifecycleComponentClosure(
                        *active_managed_composition, *qualified);
                    if (!actual.ok()) Fail("delegated installed closure is unavailable: " + actual.error().message);
                    installed = actual.value();
                    readiness = active_managed_composition->runtime.get();
                  });
                  Require(installed && readiness,
                          "delegated continuation did not retain its actual installed ports");
                  const auto continued = coordinator.ContinueApplyComponentBringUp(original.intent, digest,
                      installed->domains, installed->providers, installed->ports, {}, readiness);
                  if (!continued.ok())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{continued.error()};
                  Response result{original, continued.value()};
                  Require(result.valid(), "delegated continuation result differs from its request");
                  return result;
                }
                // Original membership is consulted before current occurrence
                // authorization. Replayed evidence never enters an effect path.
                std::lock_guard lock(managed_composition_mutex);
                auto *const cclc = current_cell_cclc();
                Require(cclc != nullptr,
                        "original component result journal is unavailable");
                auto &coordinator = cclc->coordinator();
                const auto retained = [&] {
                  if constexpr (delegated) {
                    const auto digest = core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                        original.intent, original.candidate_projection_revision,
                        original.candidate_retained_bundle_digest,
                        original.candidate_retained_bundle_bytes,
                        original.runtime_unit_id, original.runtime_epoch,
                        original.manager_runtime_epoch, original.parent_authority.parent);
                    return coordinator.ReadApplyComponentBringUpRequestResult(original.intent, digest);
                  } else {
                    const auto digest = core::SelectedComponentBringUpSemanticRequestDigest(
                        original.intent, original.candidate_projection_revision,
                        original.candidate_retained_bundle_digest,
                        original.candidate_retained_bundle_bytes,
                        original.runtime_unit_id, original.runtime_epoch,
                        original.manager_runtime_epoch);
                    return coordinator.ReadSelectedComponentBringUpRequestResult(original.intent, digest);
                  }
                }();
                if (!retained.ok())
                  return component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                      retained.error()};
                if constexpr (!read_only) {
                  if (!retained.value())
                    return component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                        core::ErrorDTO{"component_bring_up_live_authority_unavailable",
                                       "new bring-up requires the original live qualified continuation", {}}};
                }
                Response result{original, retained.value()};
                Require(result.valid(), "retained component result does not match its original request");
                return result;
              }
            }, decoded);
      } catch (const std::exception &error) {
        std::cerr << "native selected composition failed: " << error.what() << '\n';
        response =
            component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                core::ErrorDTO{"selected_runtime_composition_request_failed",
                               error.what(),
                               {}}};
      } catch (...) {
        response =
            component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                core::ErrorDTO{"selected_runtime_composition_request_failed",
                               "selected runtime-composition request failed",
                               {}}};
      }
      SendManagedRuntimePacket(
          config.manager_channel_fd,
          component_manager::detail::
              EncodeSelectedRuntimeCompositionRpcResponse(response));
      continue;
    }
    if (component_manager::detail::IsKernelApplyFenceRpcPacket(request)) {
      component_manager::detail::KernelApplyFenceRpcResponse response =
          component_manager::detail::KernelApplyFenceRpcFailure{
              core::ErrorDTO{"kernel_apply_fence_request_failed",
                             "Kernel Apply-fence request was not dispatched",
                             {}}};
      try {
        const auto decoded =
            component_manager::detail::DecodeKernelApplyFenceRpcRequest(
                request);
        const auto selected_result = [](const auto &result)
            -> component_manager::detail::KernelApplyFenceRpcResponse {
          if (!result.ok()) {
            return component_manager::detail::KernelApplyFenceRpcFailure{
                result.error()};
          }
          return result.value();
        };
        response = std::visit(
            [&](const auto &value)
                -> component_manager::detail::KernelApplyFenceRpcResponse {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<
                                T, core::ConfigurationFenceAdmissionQueryDTO>) {
                return fence_owner.ObserveAdmission(value);
              } else if constexpr (
                  std::is_same_v<T, core::ApplyConfigurationFenceIntentDTO>) {
                auto *const kernel = stable_addressed_kernel();
                const auto fresh = kernel->ReadHeldCellAdmissionClaim(
                    core::CellAdmissionClaimKindDTO::kManagerApply,
                    value.owner_transaction.apply_id.value());
                if (fresh) {
                  const auto parent = component_manager::
                      ReadManagerApplyConfigurationStatusReadOnly(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          core::CellId(config.cell_id),
                          {value.owner_transaction.apply_id,
                           value.owner_transaction.apply_generation},
                          *fresh);
                  Require(parent.ok() && !parent.value().bring_up_plan &&
                              !parent.value().terminal_outcome,
                          "configuration Apply fence has no exact live "
                          "ordinary parent");
                  return fence_owner.Acquire(
                      value,
                      [&](const auto &old_intent,
                          const auto &accepted) -> core::Result<void> {
                        const auto old_claim =
                            kernel->ReadHeldCellAdmissionClaim(
                                core::CellAdmissionClaimKindDTO::kManagerApply,
                                old_intent.owner_transaction.apply_id.value());
                        if (!old_claim)
                          return core::Result<void>::Err(
                              {"configuration_apply_original_claim_unavailable",
                               "overlapping barrier has no exact terminal "
                               "NamedApply authority",
                               {}});
                        const std::array old_claims{*old_claim};
                        const auto terminal = component_manager::
                            ValidateTerminalManagerApplyClaimsReadOnly(
                                config.manager_state_root /
                                    std::string(
                                        component_manager::
                                            kManagerApplyJournalFilename),
                                core::CellId(config.cell_id), old_claims);
                        if (!terminal.ok())
                          return terminal;
                        const auto old = component_manager::
                            ReadManagerApplyConfigurationStatusReadOnly(
                                config.manager_state_root /
                                    std::string(
                                        component_manager::
                                            kManagerApplyJournalFilename),
                                core::CellId(config.cell_id),
                                {old_intent.owner_transaction.apply_id,
                                 old_intent.owner_transaction.apply_generation},
                                *old_claim);
                        if (!old.ok())
                          return core::Result<void>::Err(old.error());
                        if (old.value().bring_up_plan ||
                            !old.value().fence_attempt ||
                            !old.value().fence_acceptance ||
                            old.value().fence_attempt->fence_intent !=
                                old_intent ||
                            old.value().fence_acceptance->accepted_fence !=
                                accepted ||
                            old.value()
                                    .fence_acceptance->fence_attempt_digest !=
                                old.value().fence_attempt->fence_attempt_digest)
                          return core::Result<void>::Err(
                              {"configuration_apply_original_fence_mismatch",
                               "overlapping barrier differs from its immutable "
                               "terminal NamedApply fence",
                               {}});
                        return core::Result<void>::Ok();
                      });
                }
                return fence_owner.Acquire(value);
              } else if constexpr (
                  std::is_same_v<T, core::ApplyFenceAcquireAttemptQueryDTO>) {
                return fence_owner.ReconcileAcquireAttempt(value);
              } else if constexpr (std::is_same_v<
                                       T, core::ApplyFenceRehydrateDTO>) {
                return fence_owner.Rehydrate(value);
              } else if constexpr (
                  std::is_same_v<T, core::ConfigurationFenceStatusQueryDTO>) {
                return fence_owner.QueryStatus(value);
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpConfigurationFenceIntentDTO>) {
                return selected_result(
                    fence_owner.AcquireSelectedBringUpFence(value));
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpFenceAcquireAttemptQueryDTO>) {
                return selected_result(
                    fence_owner.ReconcileSelectedBringUpFenceAcquireAttempt(
                        value));
              } else if constexpr (
                  std::is_same_v<
                      T,
                      core::SelectedBringUpConfigurationFenceStatusQueryDTO>) {
                return selected_result(
                    fence_owner.ObserveSelectedBringUpFenceStatus(value));
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpConfigurationFenceReleaseDTO>) {
                return selected_result(
                    fence_owner.ReleaseSelectedBringUpFence(value));
              } else if constexpr (
                  std::is_same_v<
                      T, core::SelectedBringUpFenceReleaseAttemptQueryDTO>) {
                return selected_result(
                    fence_owner.ReconcileSelectedBringUpFenceReleaseAttempt(
                        value));
              } else {
                return fence_owner.Release(value);
              }
            },
            decoded);
      } catch (const std::exception &error) {
        response = component_manager::detail::KernelApplyFenceRpcFailure{
            core::ErrorDTO{
                "kernel_apply_fence_request_failed", error.what(), {}}};
      } catch (...) {
        response = component_manager::detail::KernelApplyFenceRpcFailure{
            core::ErrorDTO{"kernel_apply_fence_request_failed",
                           "Kernel Apply-fence request failed",
                           {}}};
      }
      SendManagedRuntimePacket(
          config.manager_channel_fd,
          component_manager::detail::EncodeKernelApplyFenceRpcResponse(
              response));
      continue;
    }
    if (component_manager::detail::IsKernelCellAdmissionRpcPacket(request)) {
      component_manager::detail::KernelCellAdmissionRpcResponse response =
          core::ErrorDTO{"cell_admission_request_failed",
                         "cell-admission request was not dispatched",
                         {}};
      std::string response_command = "cell_admission_get";
      try {
        const auto decoded =
            component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                request);
        response_command = decoded.command;
        if (PreemptsPlanning(decoded)) {
          PreemptManagedExecutionSurfacePlanning(execution_surface_control);
        }
        core::ResourceAddressedExecutionKernel *owner = nullptr;
        {
          std::lock_guard lock(managed_composition_mutex);
          owner = stable_addressed_kernel();
          Require(owner != nullptr, "stable Kernel is not installed");
        }
        const auto *const acquire_intent =
            std::get_if<core::CellAdmissionClaimIntentDTO>(&decoded.request);
        const auto *const release =
            std::get_if<core::CellAdmissionClaimReleaseDTO>(&decoded.request);
        if (decoded.command == "cell_admission_release" && release &&
            release->owner_role == core::CellAdmissionOwnerRoleDTO::kManager) {
          // Ordinary fresh success settles only authenticated terminal Apply
          // predecessors. No lifecycle claim or linked activation uses this
          // path.
          const auto settle =
              [&]() -> core::Result<core::AuthoringAdmissionDTO> {
            const auto fresh = owner->ReadHeldCellAdmissionClaim(
                core::CellAdmissionClaimKindDTO::kManagerApply,
                release->owner_transaction_id);
            if (!fresh)
              return owner->ReleaseCellAdmissionClaim(*release);
            const auto database =
                config.manager_state_root /
                std::string(component_manager::kManagerApplyJournalFilename);
            const core::CellId cell(config.cell_id);
            const auto parent =
                component_manager::ReadManagerApplyBringUpStatusReadOnly(
                    database, cell, fresh->intent);
            if (!parent.ok())
              return core::Result<core::AuthoringAdmissionDTO>::Err(
                  parent.error());
            if (parent.value().bring_up_plan ||
                !parent.value().terminal_outcome ||
                !parent.value().terminal_outcome->terminal_outcome.succeeded())
              return owner->ReleaseCellAdmissionClaim(*release);
            auto old = owner->ReadHeldCellAdmissionClaims(
                core::CellAdmissionClaimKindDTO::kManagerApply);
            std::erase(old, *fresh);
            if (old.empty())
              return owner->ReleaseCellAdmissionClaim(*release);
            const auto success =
                component_manager::ValidateFreshManagerApplySuccessReadOnly(
                    database, cell, *fresh, *release);
            if (!success.ok())
              return core::Result<core::AuthoringAdmissionDTO>::Err(
                  success.error());
            const auto terminal =
                component_manager::ValidateTerminalManagerApplyClaimsReadOnly(
                    database, cell, old);
            if (!terminal.ok())
              return core::Result<core::AuthoringAdmissionDTO>::Err(
                  terminal.error());
            const auto manager = manager_liveness_receiver.get()->status();
            if (manager.state != core::ManagerLivenessStateDTO::kCurrent ||
                manager.command_fenced ||
                manager.manager_owner_epoch != fresh->intent.owner_epoch)
              return core::Result<core::AuthoringAdmissionDTO>::Err(
                  {"actual_installed_state_unknown",
                   "fresh success lost its current Manager; restart the "
                   "runtime",
                   {}});
            std::vector<core::ConfigurationFenceStatusDTO> fences;
            fences.reserve(old.size());
            for (const auto &claim : old) {
              const auto previous =
                  component_manager::ReadManagerApplyBringUpStatusReadOnly(
                      database, cell, claim.intent);
              if (!previous.ok())
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    previous.error());
              if (previous.value().bring_up_plan ||
                  previous.value().fence_attempt.has_value() !=
                      previous.value().fence_acceptance.has_value())
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    {"configuration_apply_original_fence_unavailable",
                     "old Apply has no complete ordinary fence authority",
                     {}});
              // A pre-effect terminal may legitimately own no physical fence.
              if (!previous.value().fence_attempt)
                continue;
              const auto &attempt = *previous.value().fence_attempt;
              const auto &acceptance = *previous.value().fence_acceptance;
              if (!acceptance.accepted_fence.matches(attempt.fence_intent) ||
                  acceptance.fence_attempt_digest !=
                      attempt.fence_attempt_digest)
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    {"configuration_apply_original_fence_mismatch",
                     "old Apply fence differs from its immutable acceptance",
                     {}});
              const auto &accepted = acceptance.accepted_fence;
              const bool gate_held = admission_gate.exact_fence_is_held(
                  accepted.configuration_fence_id, accepted.captured_scope);
              if (gate_held) {
                const auto observed =
                    admission_gate.Observe(accepted.captured_scope);
                for (const auto &domain : accepted.captured_scope.domains) {
                  if (domain.candidate_control_domain_revision &&
                      std::none_of(
                          observed.active_control_configurations.begin(),
                          observed.active_control_configurations.end(),
                          [&](const auto &configuration) {
                            return configuration.control_domain_id ==
                                   domain.control_domain_id;
                          }))
                    return core::Result<core::AuthoringAdmissionDTO>::Err(
                        {"configuration_apply_old_domain_unavailable",
                         "old fence domain has no current configuration; "
                         "restart "
                         "the runtime",
                         {}});
                }
              }
              const core::ConfigurationFenceStatusQueryDTO query{
                  attempt.fence_intent.owner_transaction,
                  accepted.configuration_fence_id,
                  accepted.configuration_fence_generation,
                  accepted.captured_scope, manager.manager_runtime_epoch};
              const auto status = fence_owner.QueryStatus(query);
              if (!status.matches(query) ||
                  (status.state == core::ConfigurationFenceState::kHeld &&
                   !gate_held) ||
                  status.reconciliation_evidence
                          .affected_queued_operation_count != 0U ||
                  status.reconciliation_evidence
                          .affected_active_operation_count != 0U)
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    {"configuration_apply_old_fence_unsettled",
                     "old fence is not exactly idle and reconciled",
                     {}});
              fences.push_back(status);
            }
            // Complete all reads/allocations and all-domain checks before the
            // first release. The fence owner takes its own mutex then admission
            // exclusion; never call it from the Kernel's claim validator.
            for (const auto &status : fences)
              if (status.state != core::ConfigurationFenceState::kHeld &&
                  status.state != core::ConfigurationFenceState::kReleased)
                return core::Result<core::AuthoringAdmissionDTO>::Err(
                    {"configuration_apply_old_fence_unknown",
                     "old fence outcome is unavailable; restart the workbench "
                     "container",
                     {}});
            for (const auto &status : fences) {
              if (status.state == core::ConfigurationFenceState::kReleased)
                continue;
              static_cast<void>(
                  fence_owner.Release({status.reconciliation_evidence,
                                       manager.manager_runtime_epoch}));
            }
            const auto settled = owner->ReleaseTerminalApplyClaimsAfterSuccess(
                *fresh, *release,
                [&](const auto &actual_fresh, const auto &actual_release,
                    const auto actual_old) -> core::Result<void> {
                  if (actual_old.size() != old.size() ||
                      !std::equal(actual_old.begin(), actual_old.end(),
                                  old.begin()))
                    return core::Result<void>::Err(
                        {"configuration_apply_old_claims_changed",
                         "old Apply claims changed before fresh-success "
                         "settlement",
                         {}});
                  for (const auto &status : fences)
                    if (admission_gate.exact_fence_is_held(
                            status.echoed_query.configuration_fence_id,
                            status.echoed_query.expected_scope))
                      return core::Result<void>::Err(
                          {"configuration_apply_old_fence_held",
                           "old Apply fence remains held",
                           {}});
                  const auto valid_success = component_manager::
                      ValidateFreshManagerApplySuccessReadOnly(
                          database, cell, actual_fresh, actual_release);
                  if (!valid_success.ok())
                    return valid_success;
                  return component_manager::
                      ValidateTerminalManagerApplyClaimsReadOnly(database, cell,
                                                                 actual_old);
                });
            if (!settled.ok())
              return settled;
            return owner->ReleaseCellAdmissionClaim(*release);
          };
          const auto result = settle();
          response =
              result.ok()
                  ? component_manager::detail::KernelCellAdmissionRpcResponse(
                        result.value())
                  : component_manager::detail::KernelCellAdmissionRpcResponse(
                        result.error());
        } else if (decoded.command == "cell_admission_acquire" &&
                   acquire_intent &&
                   acquire_intent->claim_kind ==
                       core::CellAdmissionClaimKindDTO::kManagerApply) {
          // Classify ordinary versus linked only from a successful complete
          // owner snapshot. Read errors never select ordinary permission.
          const auto read = component_manager::ReadManagerApplyBringUpStatusReadOnly(
              config.manager_state_root / std::string(component_manager::kManagerApplyJournalFilename),
              core::CellId(config.cell_id), *acquire_intent);
          if (!read.ok()) {
            response = read.error();
          } else if (!read.value().bring_up_plan) {
            const auto existing_claim = owner->ReadHeldCellAdmissionClaim(
                core::CellAdmissionClaimKindDTO::kManagerApply,
                acquire_intent->owner_transaction_id);
            std::shared_ptr<const deployment::RetainedBundle>
                actual_candidate_product;
            // Exact retained acquisition is observation even after publication
            // or during uncertainty. Classify it before any fresh-product read.
            if (!existing_claim || existing_claim->intent != *acquire_intent) {
              const auto &parent = read.value();
              const auto predecessor = read_current_publication();
              const auto previous = LoadManagedBundleAtPinnedRoot(config,
                  parent.intent.intent.expected_active_projection_revision.retained_bundle_id);
              const auto candidate = LoadManagedBundleAtPinnedRoot(config,
                  parent.intent.intent.candidate_retained_bundle_id);
              Require(previous && previous->bundle && candidate && candidate->bundle &&
                          core::SameActiveDeployment(predecessor.request.candidate_projection_revision,
                              parent.intent.intent.expected_active_projection_revision),
                      "configuration Apply lacks its exact current/candidate products");
              require_inactive_changed_components(
                  deployment::derive_apply_component_delta(*previous->bundle, *candidate->bundle),
                  predecessor);
              actual_candidate_product = candidate->bundle;
            }
            std::lock_guard lock(managed_composition_mutex);
            const auto acquired =
                owner->AcquireConfigurationApplyCellAdmissionClaim(
                    *acquire_intent,
                    [&](const auto &intent, const auto old_claims, const auto &,
                        const auto &manager_epoch) -> core::Result<void> {
                      const auto manager =
                          manager_liveness_receiver.get()->status();
                      if (current_composition_building ||
                          managed_composition_building ||
                          manager.state !=
                              core::ManagerLivenessStateDTO::kCurrent ||
                          manager.command_fenced ||
                          manager.manager_owner_epoch != intent.owner_epoch ||
                          manager.manager_runtime_epoch != manager_epoch ||
                          manager.receiver_runtime_unit_id !=
                              core::RuntimeUnitId(config.runtime_unit_id) ||
                          manager.receiver_runtime_epoch !=
                              core::RuntimeEpoch(config.runtime_epoch))
                        return core::Result<void>::Err(
                            {"actual_installed_state_unknown",
                             "configuration Apply lost its current installed "
                             "owner; restart the workbench container",
                             {}});
                      const auto database =
                          config.manager_state_root /
                          std::string(
                              component_manager::kManagerApplyJournalFilename);
                      const auto terminal = component_manager::
                          ValidateTerminalManagerApplyClaimsReadOnly(
                              database, core::CellId(config.cell_id),
                              old_claims);
                      if (!terminal.ok())
                        return terminal;
                      Require(actual_candidate_product != nullptr,
                              "configuration Apply has no exact fresh "
                              "candidate product");
                      for (const auto &old_claim : old_claims) {
                        const auto old = component_manager::
                            ReadManagerApplyBringUpStatusReadOnly(
                                database, core::CellId(config.cell_id),
                                old_claim.intent);
                        if (!old.ok())
                          return core::Result<void>::Err(old.error());
                        if (!old.value().fence_acceptance)
                          continue;
                        const auto &accepted =
                            old.value().fence_acceptance->accepted_fence;
                        if (!admission_gate.exact_fence_is_held(
                                accepted.configuration_fence_id,
                                accepted.captured_scope))
                          continue;
                        for (const auto &domain :
                             accepted.captured_scope.domains)
                          if (domain.candidate_control_domain_revision &&
                              std::none_of(
                                  actual_candidate_product->value()
                                      .desired_control_configurations.begin(),
                                  actual_candidate_product->value()
                                      .desired_control_configurations.end(),
                                  [&](const auto &wanted) {
                                    return wanted.control_domain_id ==
                                           domain.control_domain_id;
                                  }))
                            return core::Result<void>::Err(
                                {"configuration_apply_old_domain_unavailable",
                                 "old fence domain would be removed without a "
                                 "settlement representation; restart the "
                                 "runtime",
                                 {}});
                      }
                      return core::Result<void>::Ok();
                    });
            response =
                acquired.ok()
                    ? component_manager::detail::KernelCellAdmissionRpcResponse(
                          acquired.value())
                    : component_manager::detail::KernelCellAdmissionRpcResponse(
                          acquired.error());
          } else {
            const auto &parent = read.value();
            const auto &sealed = *parent.bring_up_plan;
            const auto &plan = sealed.plan;
            const auto predecessor = read_current_publication();
            Require(manager_liveness_installed.load(std::memory_order_acquire),
                    "Apply bring-up acquire has no current Manager observation");
            const auto manager = manager_liveness_receiver.get()->status();
            Require(
                        manager.state == core::ManagerLivenessStateDTO::kCurrent && !manager.command_fenced &&
                        manager.manager_owner_epoch == acquire_intent->owner_epoch &&
                        manager.receiver_runtime_unit_id == core::RuntimeUnitId(config.runtime_unit_id) &&
                        manager.receiver_runtime_epoch == core::RuntimeEpoch(config.runtime_epoch) &&
                        !parent.terminal_outcome && !parent.fence_attempt && !parent.fence_acceptance &&
                        !parent.private_materialization && !parent.publication_receipt &&
                        predecessor.request.candidate_projection_revision.retained_bundle_id ==
                            parent.intent.intent.expected_active_projection_revision.retained_bundle_id &&
                        predecessor.request.candidate_projection_revision.graph_revision ==
                            parent.intent.intent.expected_active_projection_revision.graph_revision &&
                        predecessor.request.candidate_projection_revision.scene_revision.scene_id ==
                            parent.intent.intent.expected_active_projection_revision.scene_revision.scene_id &&
                        predecessor.durable_public_pointer_content_digest == plan.predecessor_public_pointer_digest &&
                        predecessor.publication_generation == plan.expected_publication.expected_previous_publication_generation,
                    "Apply bring-up acquire lost its fresh original owner or predecessor");
            const auto candidate = LoadManagedBundleAtPinnedRoot(config, parent.intent.intent.candidate_retained_bundle_id);
            const auto previous = LoadManagedBundleAtPinnedRoot(config,
                parent.intent.intent.expected_active_projection_revision.retained_bundle_id);
            Require(candidate && candidate->bundle && previous && previous->bundle && !plan.children.empty(),
                    "Apply bring-up acquire has no exact verified products");
            std::vector<core::ControllableComponentId> candidate_components;
            for (const auto &component : candidate->bundle->value().cell_graph.controllable_components)
              candidate_components.push_back(component.controllable_component_id);
            std::sort(candidate_components.begin(), candidate_components.end());
            Require(candidate_components == plan.configured_components,
                    "Apply bring-up acquire configured membership changed");
            const auto &first = plan.children.front();
            Require(predecessor.publication_generation.value() != std::numeric_limits<std::uint64_t>::max(),
                    "Apply bring-up publication generation is exhausted");
            const auto compiled = BuildManagedCandidateLifecyclePlanningClosure(config, *candidate,
                first.candidate_projection_revision,
                core::ActiveBundlePublicationGeneration(predecessor.publication_generation.value() + 1U));
            if (!compiled.ok()) Fail(compiled.error().message);
            deployment::CCLCRuntimeComposition *facade = nullptr;
            std::set<core::ControllableComponentId> installed_components;
            {
              std::lock_guard composition_lock(managed_composition_mutex);
              Require(!current_composition_building && !managed_composition_building &&
                          !preparing_composition_request && !installed_under_fence_composition_request &&
                          managed_cell_cclc.facade && managed_cell_cclc.publication == predecessor,
                      "Apply bring-up acquire has no pinned original cell facade");
              facade = managed_cell_cclc.facade.get();
              Require(facade->coordinator().accepting_new_requests() &&
                          !facade->coordinator().admission_fault_closed(),
                      "Apply bring-up acquire original cell owner is closed");
              if (active_managed_composition)
                for (const auto &[id, view] : IndexNativeComponentCompositions(*active_managed_composition)) {
                  static_cast<void>(view);
                  installed_components.insert(id);
                }
            }
            core::ConfigurationFenceScopeDTO scope;
            std::map<core::ControlDomainId, core::ConfigurationFenceDomainScopeDTO> domains;
            std::map<core::ControlBindingId, core::ConfigurationFenceBindingScopeDTO> bindings;
            std::set<core::ResourceId> resources;
            std::vector<core::ApplyBringUpAdmissionComponentDTO> needed;
            for (const auto &child : plan.children) {
              const auto component = child.intent.controllable_component_id;
              const auto closure = std::find_if(compiled.value().components.begin(), compiled.value().components.end(),
                  [&](const auto &row) { return row.component_id == component; });
              const auto selection = facade->ReadApplyComponentGenerationSelection(component);
              Require(child.valid() && child.runtime_unit_id == manager.receiver_runtime_unit_id &&
                          child.runtime_epoch == manager.receiver_runtime_epoch && child.manager_runtime_epoch == manager.manager_runtime_epoch &&
                          child.intent.request_context.request_owner_epoch == facade->coordinator().request_owner_epoch() &&
                          child.candidate_projection_revision == first.candidate_projection_revision &&
                          child.candidate_projection_revision.retained_bundle_id == candidate->bundle->retained_bundle_id() &&
                          child.candidate_retained_bundle_bytes == candidate->bundle->canonical_bytes().size() &&
                          child.candidate_retained_bundle_digest == core::ContentDigest(content_identity::sha256_digest(candidate->bundle->canonical_bytes())) &&
                          closure != compiled.value().components.end() && child.intent.expected_control_domains == closure->candidate_expected_domains &&
                          selection.ok() && child.intent.apply_generation_selection == std::optional(selection.value()),
                      "Apply bring-up acquire changed its sealed owner selection or candidate closure");
              // Positive inventory absence, not a failed snapshot read, proves
              // this initial path. Retained/recovery offers need their genuine
              // containment join; they are never recast as initial here.
              Require(!installed_components.contains(component) &&
                          std::none_of(previous->bundle->value().cell_graph.controllable_components.begin(),
                              previous->bundle->value().cell_graph.controllable_components.end(),
                              [&](const auto &row) { return row.controllable_component_id == component; }) &&
                          !selection.value().owner_qualified_predecessor,
                      "Apply bring-up initial offer has a retained predecessor");
              core::ApplyBringUpAdmissionComponentDTO row{component,
                  core::ApplyBringUpAdmissionComponentKindDTO::kInitial, std::nullopt, {}, std::nullopt};
              scope.controllable_component_ids.push_back(component);
              for (const auto &binding : compiled.value().compiled_physical_bindings) {
                const auto &value = binding.value();
                if (value.controllable_component_id != component) continue;
                row.candidate_participants.push_back(value);
                const auto &path = *value.control_path;
                domains.emplace(path.control_domain_id, core::ConfigurationFenceDomainScopeDTO{
                    path.control_domain_id, std::nullopt, path.control_domain_revision});
                bindings.emplace(path.control_binding_id, core::ConfigurationFenceBindingScopeDTO{
                    path.control_binding_id, std::nullopt, path.control_binding_revision});
                for (const auto &resource : value.resources) resources.insert(resource.resource_id);
              }
              Require(!row.candidate_participants.empty(), "Apply bring-up initial physical closure is absent");
              needed.push_back(std::move(row));
            }
            for (const auto &[id, row] : domains) { static_cast<void>(id); scope.domains.push_back(row); }
            for (const auto &[id, row] : bindings) { static_cast<void>(id); scope.bindings.push_back(row); }
            scope.resources.assign(resources.begin(), resources.end());
            const auto admission = fence_owner.ObserveAdmission({scope, manager.manager_runtime_epoch});
            Require(admission.matches({scope, manager.manager_runtime_epoch}) &&
                        admission.active_projection_revision == predecessor.request.candidate_projection_revision &&
                        admission.active_public_pointer_digest == predecessor.durable_public_pointer_content_digest &&
                        read_current_publication() == predecessor && manager_liveness_receiver.get()->status().manager_owner_epoch == manager.manager_owner_epoch,
                    "Apply bring-up acquire current observation changed");
            const core::ApplyBringUpAdmissionOfferDTO offer{{{plan.apply_id, plan.apply_generation}, sealed.plan_digest},
                admission, plan.configured_components, std::move(needed)};
            const auto acquired = owner->AcquireApplyBringUpCellAdmissionClaim(*acquire_intent, offer);
            if (acquired.ok()) {
              Require(acquired.value().intent == *acquire_intent,
                      "Apply bring-up acquired claim differs from its original request");
              // Copy only actual original-owner facts after successful fresh
              // acquisition. Allocation uncertainty retains the durable claim;
              // it does not create a replacement/replay effect capability.
              original_apply_bring_up_capture =
                  std::make_shared<const ManagedOriginalApplyBringUpCapture>(
                      ManagedOriginalApplyBringUpCapture{
                          sealed, acquired.value(), predecessor,
                          parent.intent.intent.expected_active_projection_revision, *previous,
                          *candidate, compiled.value(), facade});
            }
            response = acquired.ok() ? component_manager::detail::KernelCellAdmissionRpcResponse(acquired.value())
                                     : component_manager::detail::KernelCellAdmissionRpcResponse(acquired.error());
          }
        } else
        {
          std::lock_guard lock(managed_composition_mutex);
          response = component_manager::detail::DispatchKernelCellAdmissionRpc(*owner, decoded);
        }
      } catch (const std::exception &error) {
        response =
            core::ErrorDTO{"cell_admission_request_failed", error.what(), {}};
      } catch (...) {
        response = core::ErrorDTO{"cell_admission_request_failed",
                                  "cell-admission request failed",
                                  {}};
      }
      std::string encoded_response;
      try {
        encoded_response =
            component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                response_command, response);
      } catch (...) {
        encoded_response =
            component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                response_command,
                core::ErrorDTO{
                    "cell_admission_response_oversized",
                    "cell-admission response exceeds the bounded private "
                    "transport",
                    {}});
      }
      SendManagedRuntimePacket(config.manager_channel_fd, encoded_response);
      continue;
    }
    component_manager::detail::NativeProjectionRpcResponse response;
    try {
      response = projections.Dispatch(
          component_manager::detail::DecodeNativeProjectionRpcRequest(request));
    } catch (const std::exception &error) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed", error.what(), {}}};
    } catch (...) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed",
                         "native projection request failed",
                         {}}};
    }
    SendManagedRuntimePacket(
        config.manager_channel_fd,
        component_manager::detail::EncodeNativeProjectionRpcResponse(response));
  }
  return 0;
}



class NativeExecutionHost final {
public:
  explicit NativeExecutionHost(HostConfig config)
      : config_(std::move(config)),
        owner_epoch_(RandomToken("execution-host-")),
        runtime_epoch_(RandomToken("native-runtime-")), admission_gate_(),
        composition_(BuildComposition(config_, owner_epoch_, runtime_epoch_,
                                      &admission_gate_)) {
    // Motion compositions bind this shared admission gate while constructing
    // their Kernel. The standalone gripper-only composition creates its
    // stable cell Kernel above, so bind its resource table explicitly before
    // the durable fence owner can observe or reconcile route publications.
    admission_gate_.BindResourceTable(
        composition_->active_addressed_kernel()->resource_admission_table());
    const auto runtime_units = composition_->host_runtime_unit_epochs();
    Require(runtime_units.size() == 1U,
            "standalone native host has no sole runtime-unit identity");
    fence_owner_ = std::make_unique<ManagedKernelApplyFenceOwner>(
        ManagedKernelApplyFenceOwnerConfig{
            config_.execution_state_root / "kernel-apply-fence.sqlite3",
            config_.manager_state_root, config_.package_root,
            core::CellId(config_.cell_id),
            runtime_units.front().runtime_unit_id, runtime_epoch_, owner_epoch_,
            [] {
              return std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                  .count();
            }},
        admission_gate_);
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
    const auto fixture_readiness =
        composition_->active_addressed_kernel()
            ->UpdateCellAdmissionReadiness(
            core::CellAdmissionReadinessSnapshotDTO{
                true,
                core::ProgramCycleStatusDTO{
                    core::RequestOwnerEpoch(
                        "standalone-admission-fixture.program-runner"),
                    1U, core::ProgramCycleStateDTO::kIdleAfterReset,
                    std::nullopt, std::nullopt, std::nullopt},
                true, 0U, 0U, true, true, 0U, 0U, 0U});
    // Readiness publication is not permission to move. Retained claims and
    // faults must reach real reconciliation/ingress while admission stays
    // closed; do not require authoring eligibility before recovery can run.
    Require(fixture_readiness.ok(),
            "standalone admission fixture could not publish readiness");
#endif
    AttachRetainedNativeCCLC(*composition_, config_, *fence_owner_);
    composition_access_.current = composition_.get();
    composition_access_.addressed_kernel =
        composition_->active_addressed_kernel();
    // This legacy standalone owner has no Manager lease or liveness receiver.
    // Explicitly keep only its private admission seam open; managed children
    // use a separate, initially closed admission instance.
    standalone_command_admission_.OpenIfUnchanged(
        standalone_command_admission_.generation());
    command_surface_ = std::make_unique<ExecutionCommandSurface>(
        config_.execution_run_root, composition_access_,
        standalone_command_admission_, owner_epoch_);
  }

  ~NativeExecutionHost() {
    command_surface_.reset();
  }

  [[nodiscard]] int Run() { return command_surface_->Run(); }

private:
  HostConfig config_;
  core::RequestOwnerEpoch owner_epoch_;
  // The development launcher remains one process incarnation while a5.5
  // qualifies and then retires it in favor of the Manager-owned surface.
  core::RuntimeEpoch runtime_epoch_;
  ManagedKernelAdmissionGate admission_gate_;
  std::unique_ptr<ManagedKernelApplyFenceOwner> fence_owner_;
  std::unique_ptr<Composition> composition_;
  std::unique_ptr<RetainedNativeCellRuntime> gripper_only_cell_runtime_;
  ExecutionCompositionAccess composition_access_;
  component_manager::detail::ManagerLivenessCommandAdmission
      standalone_command_admission_;
  std::unique_ptr<ExecutionCommandSurface> command_surface_;
};

} // namespace
} // namespace manyforge::roboplan_backend

namespace manyforge::roboplan_backend::host_detail {

int RunExecutionHostMain(int argc, char **argv) {
  try {
    manyforge::roboplan_backend::InstallSignalHandlers();
    if (argc > 1 &&
        std::string_view(argv[1]) == "--manyforge-managed-runtime-channel-fd") {
      return manyforge::roboplan_backend::RunManagedRuntimeChild(
          manyforge::roboplan_backend::ParseManagedRuntimeArguments(argc,
                                                                    argv));
    }
    manyforge::roboplan_backend::NativeExecutionHost host(
        manyforge::roboplan_backend::ParseArguments(argc, argv));
    return host.Run();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Native execution host failed with an unknown error\n";
    return 71;
  }
}

} // namespace manyforge::roboplan_backend::host_detail
