#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_roboplan/execution_runtime_qualification.hpp>
#include "retained_native_runtime_internal.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {

RetainedPlanningSceneSynchronizer::RetainedPlanningSceneSynchronizer(
    std::shared_ptr<core::IPlanningSceneSynchronizer> target)
    : target_(std::move(target)) {
  if (target_ == nullptr) {
    throw std::invalid_argument(
        "retained planning-scene synchronizer target is required");
  }
}

void RetainedPlanningSceneSynchronizer::ReplaceTarget(
    std::shared_ptr<core::IPlanningSceneSynchronizer> target) {
  if (target == nullptr) {
    throw std::invalid_argument(
        "retained planning-scene synchronizer target is required");
  }
  std::lock_guard lock(mutex_);
  target_ = std::move(target);
}

RetainedReferencePoseResolverProxy::RetainedReferencePoseResolverProxy(
    std::shared_ptr<core::IReferencePoseResolver> target)
    : target_(std::move(target)) {
  if (target_ == nullptr) {
    throw std::invalid_argument(
        "retained reference-pose resolver target is required");
  }
}

void RetainedReferencePoseResolverProxy::ReplaceTarget(
    std::shared_ptr<core::IReferencePoseResolver> target) {
  if (target == nullptr) {
    throw std::invalid_argument(
        "retained reference-pose resolver target is required");
  }
  std::lock_guard lock(mutex_);
  target_ = std::move(target);
}

std::shared_ptr<core::IReferencePoseResolver>
RetainedReferencePoseResolverProxy::CurrentTarget() const {
  std::lock_guard lock(mutex_);
  return target_;
}

core::Result<core::ProgramFrameResolutionContextDTO>
RetainedReferencePoseResolverProxy::capture_program_frame_resolution_context(
    core::ProgramFrameResolutionCaptureDTO capture) const {
  const auto target = CurrentTarget();
  if (target == nullptr) {
    return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
        core::ErrorDTO{"reference_resolver_target_missing",
                       "retained reference-pose resolver has no active target",
                       {}});
  }
  return target->capture_program_frame_resolution_context(std::move(capture));
}

core::Result<core::Pose3DTO>
RetainedReferencePoseResolverProxy::resolve_reference_pose(
    const core::ReferenceDTO &reference,
    const core::ProgramFrameResolutionContextDTO &context) const {
  const auto target = CurrentTarget();
  if (target == nullptr) {
    return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
        "reference_resolver_target_missing",
        "retained reference-pose resolver has no active target", {}});
  }
  return target->resolve_reference_pose(reference, context);
}

core::Result<core::ResolvedSceneFramePoseDTO>
RetainedReferencePoseResolverProxy::resolve_scene_frame(
    const core::SceneFrameRefDTO &reference,
    const core::SceneFrameResolutionContextDTO &context) const {
  const auto target = CurrentTarget();
  if (target == nullptr) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(core::ErrorDTO{
        "reference_resolver_target_missing",
        "retained reference-pose resolver has no active target", {}});
  }
  return target->resolve_scene_frame(reference, context);
}

core::Result<core::ProjectionRevisionDTO>
RetainedPlanningSceneSynchronizer::SynchronizeBeforePlanning() {
  std::shared_ptr<core::IPlanningSceneSynchronizer> target;
  {
    std::lock_guard lock(mutex_);
    target = target_;
  }
  if (target == nullptr) {
    return core::Result<core::ProjectionRevisionDTO>::Err(core::ErrorDTO{
        "planning_scene_synchronizer_target_missing",
        "retained planning-scene synchronizer has no active target", {}});
  }
  return target->SynchronizeBeforePlanning();
}

RetainedNativeRuntime::~RetainedNativeRuntime() {
  // The cell Kernel holds non-owning event subscriptions to every participant
  // engine, including engines owned by participant_peers. Unsubscribe while
  // all of those engines are still alive; ordinary reverse member destruction
  // would otherwise destroy the peers before the Kernel. Destroying the Kernel
  // is also what settles its ingress, joined workers, accepted Stop-projection
  // work and durable owner-epoch close, so it stays the first retirement.
  auto *const retired_kernel = addressed_kernel.get();
  std::vector<RetainedNativeRuntime *> retained_peers;
  for (const auto &peer : participant_peers) {
    if (peer != nullptr) retained_peers.push_back(peer.get());
  }
  const bool stale_local_root =
      retired_kernel != nullptr && cell_addressed_kernel == retired_kernel;
  addressed_kernel.reset();
  // Cell-wide Stop surveillance observes the same participant engines and
  // channels through Kernel-issued samples. It retires once no Kernel callback
  // can reach it and before the engines it reads disappear.
  stop_surveillance_owner.reset();
  // Only cell-Kernel pointers naming the Kernel retired above are cleared. A
  // process-stable owner installed by the host remains usable, but every peer
  // must stop naming this runtime as its cell root before this object dies.
  for (auto *const peer : retained_peers) {
    if (retired_kernel != nullptr &&
        peer->cell_addressed_kernel == retired_kernel) {
      peer->cell_addressed_kernel = nullptr;
    }
    if (peer->cell_runtime_root == this) {
      peer->cell_runtime_root = nullptr;
    }
  }
  if (stale_local_root) {
    cell_addressed_kernel = nullptr;
  }
  cell_operation_journal = nullptr;
  // Both roots and former peers can retire in different orders across an
  // Apply. Never leave a self- or predecessor-root pointer dangling after
  // this owner is destroyed.
  cell_runtime_root = nullptr;
  // participant_peers intentionally retires with ordinary member destruction,
  // after this body: its engines stayed alive for the Kernel unsubscribe above
  // and for surveillance, and a peer kept alive elsewhere still resolves its
  // live cell root.
}
namespace {

/// Working motion, or an ended execution whose effect settlement is
/// uncertain (an unresolved intervention), blocks generation changes.
[[nodiscard]] bool
motion_participant_unresolved(const core::ExecutionStatus &status) noexcept {
  if (!status.fact.has_value())
    return status.state != core::ExecutionState::kIdle;
  return status.fact->effect ==
         core::ParticipantEffectKnowledge::kEffectUncertain;
}

using manyforge::content_identity::CanonicalValue;

inline constexpr std::string_view kNoActiveGoalEvidenceDomain =
    "native-no-active-goal-evidence-digest";
inline constexpr std::string_view kNoActiveGoalEvidenceApiVersion =
    "manyforge.native-no-active-goal-evidence.v1";

void require_planner_binding(
    const core::RuntimeParticipantBinding &runtime,
    const RoboPlanPlannerBackend &planner,
    const core::ProjectionRevisionDTO &projection_revision) {
  const auto &value = runtime.value();
  const auto &candidate = planner.candidate_binding();
  if (value.model_groups.size() != 1U) {
    throw std::invalid_argument(
        "native planner requires one retained participant model group");
  }
  const auto &model_group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &binding) {
        return binding.model_instance_id == model_group.model_instance_id;
      });
  if (model == value.model_bindings.end() ||
      std::count_if(value.model_bindings.begin(), value.model_bindings.end(),
                    [&](const auto &binding) {
                      return binding.model_instance_id ==
                             model_group.model_instance_id;
                    }) != 1) {
    throw std::invalid_argument(
        "native planner model group has no exact retained model binding");
  }
  std::vector<core::ResolvedFrameDTO> permitted_frames;
  permitted_frames.reserve(value.frame_bindings.size());
  for (const auto &frame : value.frame_bindings) {
    permitted_frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id,
        frame.frame_role_id, frame.compiled_frame_name, frame.model_revision});
  }
  std::vector<std::string> resource_joint_names;
  resource_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resource_joint_names.push_back(resource.compiled_resource_name);
  }
  if (planner.projection_binding().projection_revision != projection_revision ||
      candidate.controllable_component_id != value.controllable_component_id ||
      candidate.command_target_id != value.command_target_id ||
      candidate.model_instance_id != model_group.model_instance_id ||
      candidate.model_revision != model->model_revision ||
      candidate.compiled_group_name != model_group.compiled_group_name ||
      candidate.compiled_joint_names != resource_joint_names ||
      candidate.permitted_frames != permitted_frames) {
    throw std::invalid_argument(
        "native planner crosses its retained participant or projection");
  }
  std::vector<core::ResourceId> resources;
  resources.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
  }
  if (candidate.resources != resources) {
    throw std::invalid_argument(
        "native planner resource order differs from its retained target");
  }
}

[[nodiscard]] RetainedGripperPositionLimits
retained_gripper_limits(const roboplan::Scene &scene,
                        const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
  if (value.resources.size() != 1U) {
    throw std::invalid_argument(
        "native gripper limits require one retained resource");
  }
  const auto &resource = value.resources.front();
  const auto joint = scene.getJointInfo(resource.compiled_resource_name);
  if (!joint.has_value() || joint->num_position_dofs != 1U ||
      joint->limits.min_position.size() != 1 ||
      joint->limits.max_position.size() != 1 ||
      !std::isfinite(joint->limits.min_position[0]) ||
      !std::isfinite(joint->limits.max_position[0]) ||
      joint->limits.min_position[0] > joint->limits.max_position[0]) {
    throw std::invalid_argument(
        "native gripper resource has no exact finite retained position limits");
  }
  const bool matching_joint_kind =
      (resource.coordinate_kind == core::CoordinateKind::kAngular &&
       joint->type == roboplan::JointType::REVOLUTE) ||
      (resource.coordinate_kind == core::CoordinateKind::kLinear &&
       joint->type == roboplan::JointType::PRISMATIC);
  if (!matching_joint_kind) {
    throw std::invalid_argument(
        "native gripper resource kind differs from its retained model joint");
  }
  return RetainedGripperPositionLimits{resource.coordinate_kind,
                                       joint->limits.min_position[0],
                                       joint->limits.max_position[0]};
}

[[nodiscard]] std::shared_ptr<core::ICollisionQueryEngine> collision_engine(
    const collision::ManyForgeCollisionEngine::SharedModelPtr &model,
    const collision::ManyForgeCollisionEngine::SharedSceneCachePtr &cache) {
  return std::make_shared<collision::ManyForgeCollisionEngine>(model, cache);
}

[[nodiscard]] bool same_provider_runtime(
    const core::RuntimeCapabilityProviderPathDTO &left,
    const core::RuntimeCapabilityProviderPathDTO &right) noexcept {
  return left.provider_instance_id == right.provider_instance_id &&
         left.provider_manifest_id == right.provider_manifest_id &&
         left.provider_manifest_revision == right.provider_manifest_revision &&
         left.provider_runtime_unit_id == right.provider_runtime_unit_id &&
         left.provider_runtime_epoch == right.provider_runtime_epoch;
}

[[nodiscard]] const core::JsonValueDTO *
json_member(const core::JsonObjectDTO &object,
            const std::string_view name) noexcept {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] const core::JsonValueDTO::Object &
json_object(const core::JsonValueDTO &value, const std::string_view field) {
  const auto *object = std::get_if<core::JsonValueDTO::Object>(&value.value);
  if (object == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an object");
  }
  return *object;
}

[[nodiscard]] const core::JsonValueDTO::Array &
json_array(const core::JsonValueDTO &value, const std::string_view field) {
  const auto *array = std::get_if<core::JsonValueDTO::Array>(&value.value);
  if (array == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an array");
  }
  return *array;
}

[[nodiscard]] const std::string &json_string(const core::JsonValueDTO &value,
                                             const std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.value);
  if (text == nullptr || text->empty()) {
    throw std::invalid_argument(std::string(field) +
                                " is not a non-empty string");
  }
  return *text;
}

[[nodiscard]] double json_positive_number(const core::JsonValueDTO &value,
                                          const std::string_view field) {
  const auto *number = std::get_if<double>(&value.value);
  if (number == nullptr || !std::isfinite(*number) || *number <= 0.0) {
    throw std::invalid_argument(std::string(field) +
                                " is not a finite positive number");
  }
  return *number;
}

[[nodiscard]] std::vector<core::HardStopResourceLimitDTO>
retained_default_stop_limits(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness) {
  if (!readiness.has_value()) {
    return {};
  }
  const auto &stages =
      readiness->value().resolved_stop_base_plan.escalation_stages;
  if (stages.empty() ||
      !stages.front().default_contract_parameters.has_value()) {
    return {};
  }
  const auto *limits_value = json_member(
      *stages.front().default_contract_parameters, "per_resource_limits");
  if (limits_value == nullptr) {
    return {};
  }
  const auto &limits =
      json_array(*limits_value, "Stop default per_resource_limits");
  if (limits.size() != binding.value().resources.size()) {
    throw std::invalid_argument(
        "Stop default resource limits do not cover the exact binding");
  }

  std::map<core::ResourceId, core::CoordinateKind> expected;
  for (const auto &resource : binding.value().resources) {
    if (!expected.emplace(resource.resource_id, resource.coordinate_kind)
             .second) {
      throw std::invalid_argument(
          "runtime binding repeats a Stop resource identity");
    }
  }
  std::vector<core::HardStopResourceLimitDTO> result;
  result.reserve(limits.size());
  std::set<core::ResourceId> actual;
  for (const auto &value : limits) {
    const auto &row = json_object(value, "Stop default resource limit");
    const auto *resource_value = json_member(row, "resource_id");
    const auto *threshold_value = json_member(row, "settle_velocity_threshold");
    if (resource_value == nullptr || threshold_value == nullptr) {
      throw std::invalid_argument(
          "Stop default resource limit is missing an exact field");
    }
    const core::ResourceId resource_id(
        json_string(*resource_value, "Stop default resource_id"));
    const auto expected_kind = expected.find(resource_id);
    if (expected_kind == expected.end() || !actual.insert(resource_id).second) {
      throw std::invalid_argument(
          "Stop default resource limits are duplicated or out of scope");
    }
    const auto &threshold =
        json_object(*threshold_value, "Stop default settle velocity");
    const auto *kind_value = json_member(threshold, "coordinate_kind");
    if (kind_value == nullptr) {
      throw std::invalid_argument(
          "Stop default settle velocity has no coordinate kind");
    }
    const auto &kind =
        json_string(*kind_value, "Stop default settle coordinate kind");
    if (expected_kind->second == core::CoordinateKind::kAngular) {
      const auto *number = json_member(threshold, "value_rad_s");
      if (kind != "angular_rad_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from an angular resource");
      }
      result.push_back(
          {resource_id, core::AngularVelocityDTO{json_positive_number(
                            *number, "Stop default value_rad_s")}});
    } else {
      const auto *number = json_member(threshold, "value_m_s");
      if (kind != "linear_m_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from a linear resource");
      }
      result.push_back(
          {resource_id, core::LinearVelocityDTO{json_positive_number(
                            *number, "Stop default value_m_s")}});
    }
  }
  std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
              return left.resource_id < right.resource_id;
            });
  return result;
}

struct RetainedControlledStopProfiles final {
  core::MotionInterruptStopProfileDTO operator_profile;
  core::MotionInterruptStopProfileDTO fault_profile;
};

[[nodiscard]] std::optional<RetainedControlledStopProfiles>
retained_controlled_stop_profiles(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness) {
  if (!readiness.has_value()) {
    return std::nullopt;
  }
  const auto &stages =
      readiness->value().resolved_stop_base_plan.escalation_stages;
  const auto controlled =
      std::find_if(stages.begin(), stages.end(), [](const auto &stage) {
        return stage.containment_command.command_kind ==
               core::StopContainmentCommandKind::kMotionControlledStop;
      });
  if (controlled == stages.end()) {
    return std::nullopt;
  }
  if (std::count_if(
          stages.begin(), stages.end(),
          [](const auto &stage) {
            return stage.containment_command.command_kind ==
                   core::StopContainmentCommandKind::kMotionControlledStop;
          }) != 1 ||
      !controlled->default_contract_parameters.has_value()) {
    throw std::invalid_argument(
        "retained motion Stop plan has no single exact controlled profile");
  }
  auto decoded =
      core::map_motion_controlled_stop_contract_parameters_to_profile(
          *controlled->default_contract_parameters, binding.value().resources,
          "operator_soft_on_path");
  if (!decoded.ok()) {
    throw std::invalid_argument(decoded.error().message);
  }
  auto operator_profile = std::move(decoded.value());
  auto fault_profile = operator_profile;
  // Native-only fallback. ROS/Mock composition replaces this with the exact
  // Deployment-authored fault profile below, including spring_back_fast.
  fault_profile.profile_id = "fault_spring_back_hold";
  fault_profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  fault_profile.allow_natural_completion_on_short_path = false;
  fault_profile.fallback_stop_modes.clear();
  if (const auto error =
          core::motion_interrupt_stop_profile_field_error(fault_profile);
      !error.empty()) {
    throw std::invalid_argument("retained fault Stop profile is invalid: " +
                                error);
  }
  // Resolve each configured mode before execution. In particular, the fault
  // Spring preset uses its half-duration, not a copied Soft ramp's preset.
  auto frozen_operator =
      core::resolve_motion_interrupt_stop_jerk_limits(operator_profile);
  auto frozen_fault =
      core::resolve_motion_interrupt_stop_jerk_limits(fault_profile);
  if (!frozen_operator.ok() || !frozen_fault.ok())
    throw std::invalid_argument(!frozen_operator.ok()
                                    ? frozen_operator.error().message
                                    : frozen_fault.error().message);
  return RetainedControlledStopProfiles{std::move(frozen_operator.value()),
                                        std::move(frozen_fault.value())};
}

// This policy belongs to the currently installed cell product, not a retained
// component's bundle or a planner configuration captured before Apply.
[[nodiscard]] std::optional<double> current_cell_takeover_budget(
    const RetainedNativeCellRuntime *cell,
    const core::RuntimeParticipantBinding &binding,
    std::optional<double> core::RuntimeParticipantExecutionRegistration::*
        field = &core::RuntimeParticipantExecutionRegistration::
                    motion_takeover_preparation_budget_s) {
  if (cell == nullptr ||
      binding.value().capability_id.value() != "execution.trajectory.execute") {
    return std::nullopt;
  }
  for (const auto &row : cell->participant_registrations()) {
    const auto &source = row.binding.value();
    if (source.controllable_component_id ==
            binding.value().controllable_component_id &&
        source.command_target_id == binding.value().command_target_id &&
        source.capability_id == binding.value().capability_id) {
      return row.*field;
    }
  }
  return std::nullopt;
}

[[nodiscard]] core::RuntimeParticipantExecutionRegistration
runtime_registration(
    core::RuntimeParticipantBinding binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    core::IResolvedParticipantExecutionEngine *engine,
    core::IControlDomainExecutionPort *stop_execution_port,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &participant_policy,
    std::optional<core::MotionInterruptStopProfileDTO>
        default_operator_stop_profile = std::nullopt,
    std::optional<core::MotionInterruptStopProfileDTO>
        default_fault_stop_profile = std::nullopt,
    std::optional<double> deployment_maximum_tcp_linear_speed_mps =
        std::nullopt,
    const bool fault_stop_profile_from_deployment = false) {
  if (participant_policy.has_value() &&
      (participant_policy->value().controllable_component_id !=
           binding.value().controllable_component_id ||
       participant_policy->value().command_target_id !=
           binding.value().command_target_id)) {
    throw std::invalid_argument(
        "participant motion policy crosses its participant binding");
  }
  if (const auto retained_profiles =
          retained_controlled_stop_profiles(binding, readiness);
      retained_profiles.has_value()) {
    if (default_operator_stop_profile.has_value() &&
        *default_operator_stop_profile != retained_profiles->operator_profile) {
      throw std::invalid_argument(
          "runtime operator Stop profile differs from retained Stop authority");
    }
    default_operator_stop_profile = retained_profiles->operator_profile;
    // Q37: a Deployment-authored fault profile is the authority as given;
    // only the native lane still derives it from the operator profile.
    if (!fault_stop_profile_from_deployment) {
      if (default_fault_stop_profile.has_value() &&
          *default_fault_stop_profile != retained_profiles->fault_profile) {
        throw std::invalid_argument(
            "runtime fault Stop profile differs from retained Stop authority");
      }
      default_fault_stop_profile = retained_profiles->fault_profile;
    }
  }
  auto defaults = retained_default_stop_limits(binding, readiness);
  return core::RuntimeParticipantExecutionRegistration{
      std::move(binding),
      readiness,
      std::move(defaults),
      core::MotionInterruptNodeCategory::kGeneric,
      engine,
      stop_execution_port,
      participant_policy,
      dynamic_cast<core::IResolvedPreparedParticipantExecutionEngine *>(engine),
      std::move(default_operator_stop_profile),
      std::move(default_fault_stop_profile),
      deployment_maximum_tcp_linear_speed_mps};
}

// Rebuilding route/readiness facts must retain the active cell's motion
// policy. Ordinary participant construction still starts without this policy.
template <typename... Args>
[[nodiscard]] core::RuntimeParticipantExecutionRegistration
runtime_registration(const RetainedNativeRuntime &owner, Args &&...args) {
  auto registration = runtime_registration(std::forward<Args>(args)...);
  const auto *cell = owner.cell_runtime_owner;
  if (cell == nullptr && owner.cell_runtime_root != nullptr) {
    cell = owner.cell_runtime_root->cell_runtime_owner;
  }
  registration.motion_takeover_preparation_budget_s =
      current_cell_takeover_budget(cell, registration.binding);
  registration.motion_takeover_planning_budget_s = current_cell_takeover_budget(
      cell, registration.binding,
      &core::RuntimeParticipantExecutionRegistration::
          motion_takeover_planning_budget_s);
  return registration;
}

[[nodiscard]] core::RuntimeParticipantExecutionRegistration
gripper_runtime_registration(
    core::RuntimeParticipantBinding binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    core::IResolvedParticipantExecutionEngine *engine,
    core::IControlDomainExecutionPort *stop_execution_port) {
  return runtime_registration(std::move(binding), readiness, engine,
                              stop_execution_port, std::nullopt);
}

[[nodiscard]] std::optional<core::RuntimeParticipantPolicySelection>
representative_participant_policy(
    const bool primary, const bool motion_owner,
    const std::optional<core::RuntimeParticipantPolicySelection> &policy) {
  if (primary && motion_owner) {
    return policy;
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<core::ResourceId>
stop_observation_resources(const core::RuntimeParticipantBinding &binding,
                           const core::ResolvedStopEscalationStageDTO &stage) {
  if (const auto *resource_scope =
          std::get_if<core::ResolvedStopResourceContainmentScopeDTO>(
              &stage.containment_scope)) {
    return resource_scope->resource_ids;
  }
  std::vector<core::ResourceId> resources;
  resources.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  return resources;
}

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
motion_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                     core::RuntimeParticipantBinding binding);

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
gripper_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                      core::RuntimeParticipantBinding binding);

void append_stop_observation_sources(
    RetainedStopSurveillanceSourceInput &source,
    const core::RuntimeParticipantBinding &binding,
    const core::RuntimeParticipantStopReadiness &readiness) {
  for (const auto &stage :
       readiness.value().resolved_stop_base_plan.escalation_stages) {
    const auto resources = stop_observation_resources(binding, stage);
    for (const auto &requirement : stage.evidence_requirements) {
      if (requirement.requirement.evidence_kind !=
              core::StopEvidenceRequirementKind::kMotionRest ||
          requirement.resolved_source.source_kind !=
              core::StopEvidenceSourceKind::kBindingStateAuthority) {
        continue;
      }
      const RetainedStopSurveillanceObservationSource observation{
          requirement.requirement, requirement.resolved_source, resources};
      if (std::find(source.observation_sources.begin(),
                    source.observation_sources.end(), observation) ==
          source.observation_sources.end()) {
        source.observation_sources.push_back(observation);
      }
    }
  }
}

[[nodiscard]] std::optional<RetainedStopSurveillanceSourceInput>
stop_surveillance_source(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    const std::shared_ptr<IRetainedObservedStateChannel> &channel,
    RetainedNoActiveGoalEvidenceSource no_active_goal_evidence_source) {
  if (channel == nullptr || !no_active_goal_evidence_source) {
    return std::nullopt;
  }
  RetainedStopSurveillanceSourceInput result{
      binding, channel, {}, std::move(no_active_goal_evidence_source)};
  if (readiness.has_value()) {
    append_stop_observation_sources(result, binding, *readiness);
  }
  return result;
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
stop_surveillance_sources(
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantStopReadiness>
        &motion_readiness,
    const std::shared_ptr<RetainedObservedStateChannel> &motion_channel,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const std::optional<core::RuntimeParticipantStopReadiness>
        &gripper_readiness,
    const std::shared_ptr<RetainedObservedStateChannel> &gripper_channel,
    core::IResolvedParticipantExecutionEngine *engine,
    core::IResolvedParticipantExecutionEngine *gripper_engine,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations) {
  std::vector<RetainedStopSurveillanceSourceInput> result;
  if (auto motion = stop_surveillance_source(
          motion_binding, motion_readiness, motion_channel,
          motion_goal_observer(engine, motion_binding));
      motion.has_value()) {
    result.push_back(std::move(*motion));
  }
  if (gripper_binding.has_value()) {
    if (auto gripper = stop_surveillance_source(
            *gripper_binding, gripper_readiness, gripper_channel,
            gripper_goal_observer(gripper_engine, *gripper_binding));
        gripper.has_value()) {
      result.push_back(std::move(*gripper));
    }
  }
  // Capability representatives share one physical observation channel, but
  // their admitted Stop plans may name distinct exact evidence predicates.
  // Preserve those predicates on that owner's single source row.
  for (auto &source : result) {
    const auto physical = std::find_if(
        registrations.begin(), registrations.end(), [&](const auto &row) {
          return row.binding.value() == source.binding.value();
        });
    if (physical == registrations.end() ||
        !physical->stop_readiness.has_value()) {
      continue;
    }
    for (const auto &registration : registrations) {
      if (!registration.stop_readiness.has_value() ||
          registration.engine != physical->engine ||
          registration.stop_execution_port != physical->stop_execution_port ||
          !(retained_bindings_share_backend_owner(source.binding,
                                                 registration.binding) ||
            core::runtime_participant_binding_physically_contains(
                source.binding, registration.binding))) {
        continue;
      }
      append_stop_observation_sources(source, registration.binding,
                                      *registration.stop_readiness);
    }
  }
  return result;
}

[[nodiscard]] std::optional<core::RuntimeParticipantStopReadiness>
readiness_for(const std::vector<core::RuntimeParticipantExecutionRegistration>
                  &registrations,
              const core::RuntimeParticipantBinding &binding) {
  const auto found = std::find_if(
      registrations.begin(), registrations.end(),
      [&](const auto &row) { return row.binding.value() == binding.value(); });
  return found == registrations.end() ? std::nullopt : found->stop_readiness;
}

[[nodiscard]] bool same_supporting_provider_runtime(
    const std::vector<core::RuntimeCapabilityProviderPathDTO> &primary,
    const std::vector<core::RuntimeCapabilityProviderPathDTO> &alias) noexcept {
  return !primary.empty() &&
         std::all_of(alias.begin(), alias.end(),
                     [&](const auto &candidate) {
                       return std::any_of(primary.begin(), primary.end(),
                                          [&](const auto &selected) {
                                            return same_provider_runtime(
                                                selected, candidate);
                                          });
                     }) &&
         std::all_of(primary.begin(), primary.end(), [&](const auto &selected) {
           return std::any_of(
               alias.begin(), alias.end(), [&](const auto &candidate) {
                 return same_provider_runtime(selected, candidate);
               });
         });
}

[[nodiscard]] bool same_physical_binding(
    const core::RuntimeParticipantBinding &primary,
    const core::RuntimeParticipantBinding &alias,
    const bool permit_supporting_capability_expansion) noexcept {
  const auto &left = primary.value();
  const auto &right = alias.value();
  // Contact policies belong to the verified Program view, not the physical
  // owner. Keep them on the alias while matching every physical identity below.
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision &&
         left.controllable_component_id == right.controllable_component_id &&
         left.component_type_id == right.component_type_id &&
         left.component_type_revision == right.component_type_revision &&
         left.command_target_id == right.command_target_id &&
         left.capability_id == right.capability_id &&
         left.capability_definition_revision ==
             right.capability_definition_revision &&
         left.command_schema == right.command_schema &&
         left.model_bindings == right.model_bindings &&
         left.model_groups == right.model_groups &&
         left.frame_bindings == right.frame_bindings &&
         left.resources == right.resources &&
         left.collision_body_bindings == right.collision_body_bindings &&
         left.io_output_binding == right.io_output_binding &&
         left.control_path == right.control_path &&
         left.capability_provider_path == right.capability_provider_path &&
         (left.supporting_capability_provider_paths ==
              right.supporting_capability_provider_paths ||
          (permit_supporting_capability_expansion &&
           same_supporting_provider_runtime(
               left.supporting_capability_provider_paths,
               right.supporting_capability_provider_paths))) &&
         left.runtime_unit_epochs == right.runtime_unit_epochs;
}

[[nodiscard]] bool represents_binding(
    const core::RuntimeParticipantBinding &candidate,
    const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision) noexcept {
  return candidate.value().control_path.has_value() &&
         candidate.value().control_path->control_binding_id == binding_id &&
         candidate.value().control_path->control_binding_revision ==
             binding_revision;
}

template <typename Value>
[[nodiscard]] core::Result<Value> route_publication_error(
    std::string reason, std::string message,
    core::ErrorCode code = core::ErrorCode::kInvariantViolation) {
  return core::Result<Value>::Err(
      core::ErrorDTO{std::string(core::to_string(code)),
                     std::move(message),
                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::ContentDigest no_active_goal_evidence_digest(
    const core::RuntimeParticipantBinding &binding,
    const std::string_view target_kind, const std::string_view state,
    const std::uint64_t goal_id, const std::uint64_t goal_generation,
    const std::string_view correlation, const std::int64_t observed_at_ns) {
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  const CanonicalValue evidence(CanonicalValue::Object{
      {"command_target_id", CanonicalValue(value.command_target_id.value())},
      {"controllable_component_id",
       CanonicalValue(value.controllable_component_id.value())},
      {"correlation", CanonicalValue(std::string(correlation))},
      {"goal_generation", CanonicalValue(std::to_string(goal_generation))},
      {"goal_id", CanonicalValue(std::to_string(goal_id))},
      {"observed_at_ns", CanonicalValue(std::to_string(observed_at_ns))},
      {"provider_runtime_epoch",
       CanonicalValue(path.provider_runtime_epoch.value())},
      {"provider_runtime_unit_id",
       CanonicalValue(path.provider_runtime_unit_id.value())},
      {"state", CanonicalValue(std::string(state))},
      {"target_kind", CanonicalValue(std::string(target_kind))},
  });
  return core::ContentDigest(content_identity::framed_sha256(
      kNoActiveGoalEvidenceDomain, kNoActiveGoalEvidenceApiVersion, evidence));
}

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
motion_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                     core::RuntimeParticipantBinding binding) {
  if (engine == nullptr || !binding.value().control_path.has_value())
    return {};
  return [engine,
          binding = std::move(binding)](const std::int64_t observed_at_ns)
             -> core::Result<core::ContentDigest> {
    const auto status = engine->get_status();
    // Idle, or an ended execution (any participant fact): no goal is active.
    const bool no_active_goal =
        status.state == core::ExecutionState::kIdle || status.fact.has_value();
    if (!no_active_goal) {
      return route_publication_error<core::ContentDigest>(
          "native_motion_goal_still_active",
          "native motion target still has a nonterminal goal",
          core::ErrorCode::kInvalidStateTransition);
    }
    return core::Result<core::ContentDigest>::Ok(no_active_goal_evidence_digest(
        binding, "motion", core::to_string(status.state), status.execution_id,
        0U, status.active_move_id, observed_at_ns));
  };
}

[[nodiscard]] RetainedNoActiveGoalEvidenceSource
gripper_goal_observer(core::IResolvedParticipantExecutionEngine *engine,
                      core::RuntimeParticipantBinding binding) {
  if (engine == nullptr || !binding.value().control_path.has_value())
    return {};
  return [engine,
          binding = std::move(binding)](const std::int64_t observed_at_ns)
             -> core::Result<core::ContentDigest> {
    const auto target = binding.value().command_target_id.value();
    const auto status = engine->get_gripper_status(target);
    const bool no_active_goal =
        status.gripper_id == target &&
        (status.state == core::GripperCommandState::kIdle ||
         status.state == core::GripperCommandState::kCompleted ||
         status.state == core::GripperCommandState::kFailed);
    if (!no_active_goal) {
      return route_publication_error<core::ContentDigest>(
          "native_gripper_goal_still_active",
          "native gripper target still has a nonterminal goal",
          core::ErrorCode::kInvalidStateTransition);
    }
    return core::Result<core::ContentDigest>::Ok(no_active_goal_evidence_digest(
        binding, "gripper", core::gripper_command_state_name(status.state),
        status.command_id, status.generation, status.gripper_id,
        observed_at_ns));
  };
}

[[nodiscard]] core::Result<core::RuntimeParticipantBinding>
rebase_program_alias(const core::RuntimeParticipantBinding &alias,
                     const core::RuntimeParticipantBinding &old_physical,
                     const core::RuntimeParticipantBinding &new_physical) {
  if (!same_physical_binding(old_physical, alias, true)) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_publication_alias_crosses_physical_owner",
        "runtime Program alias differs from its previous physical owner");
  }
  auto candidate = alias.value();
  candidate.control_path = new_physical.value().control_path;
  candidate.runtime_unit_epochs = new_physical.value().runtime_unit_epochs;
  auto sealed = core::verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_publication_alias_rebind_invalid", sealed.error().message);
  }
  if (!same_physical_binding(new_physical, sealed.value(), true)) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_publication_alias_rebind_mismatch",
        "rebound Program alias differs from its new physical owner");
  }
  return sealed;
}

[[nodiscard]] core::Result<core::RuntimeParticipantBinding>
rebase_runtime_binding_to_predecessor(
    const core::RuntimeParticipantBinding &binding,
    const core::ActiveControlConfigurationDTO &configuration) {
  const auto &value = binding.value();
  if (!configuration.locally_valid() ||
      !configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      !value.control_path.has_value()) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_predecessor_rebind_input_invalid",
        "durable route predecessor rebind has no exact control path",
        core::ErrorCode::kInvalidRequest);
  }
  const auto &path = *value.control_path;
  const auto selected =
      std::find_if(configuration.binding_routes.begin(),
                   configuration.binding_routes.end(), [&](const auto &row) {
                     return row.control_binding_id == path.control_binding_id;
                   });
  if (path.control_domain_id != configuration.control_domain_id ||
      path.control_domain_revision != configuration.control_domain_revision ||
      path.controller_topology_id != configuration.controller_topology_id ||
      path.topology_revision != configuration.topology_revision ||
      path.provider_runtime_unit_id != configuration.source_runtime_unit_id ||
      selected == configuration.binding_routes.end() ||
      selected->control_binding_revision != path.control_binding_revision) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_predecessor_rebind_mismatch",
        "durable route predecessor crosses its retained runtime binding",
        core::ErrorCode::kInvalidRequest);
  }
  auto candidate = value;
  candidate.control_path->control_route_id = selected->control_route_id;
  candidate.control_path->activation_set_id = configuration.activation_set_id;
  candidate.control_path->active_control_configuration_id =
      configuration.active_control_configuration_id;
  candidate.control_path->active_control_configuration_generation =
      configuration.active_control_configuration_generation;
  // Qualification comes from this already verified binding's retained route,
  // not from the durable configuration or its generation. Re-adopting that
  // same route must preserve the exact proof; another route stays unqualified
  // until the retained readiness resolver supplies its own route proof.
  if (candidate.control_path->runtime_scale_qualification.has_value() &&
      candidate.control_path->runtime_scale_qualification->control_route_id !=
          selected->control_route_id) {
    candidate.control_path->runtime_scale_qualification.reset();
  }
  auto sealed = core::verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    return route_publication_error<core::RuntimeParticipantBinding>(
        "route_predecessor_rebind_invalid", sealed.error().message,
        core::ErrorCode::kInvalidRequest);
  }
  return sealed;
}

[[nodiscard]] core::RuntimeParticipantAdmissionResolver
make_kernel_participant_resolver(
    RetainedRuntimeParticipantResolver binding_resolver,
    std::vector<core::RuntimeParticipantExecutionRegistration>
        physical_registrations) {
  if (!binding_resolver) {
    return {};
  }
  return [binding_resolver = std::move(binding_resolver),
          physical_registrations = std::move(physical_registrations)](
             const core::OperationIntentDTO &operation,
             const std::size_t participant_index)
             -> core::Result<core::RuntimeParticipantExecutionRegistration> {
    auto binding = binding_resolver(operation, participant_index);
    if (!binding.ok()) {
      return core::Result<core::RuntimeParticipantExecutionRegistration>::Err(
          binding.error());
    }
    const auto& selected = binding.value().binding;
    const bool exact_output = selected.value().io_output_binding &&
        std::any_of(physical_registrations.begin(), physical_registrations.end(),
                    [&](const auto& row) { return row.binding.value() == selected.value(); });
    const core::RuntimeParticipantExecutionRegistration *physical = nullptr;
    for (const auto &candidate : physical_registrations) {
      if (exact_output
              ? candidate.binding.value() != selected.value()
              : !same_physical_binding(candidate.binding, selected, true)) {
        continue;
      }
      if (physical != nullptr) {
        return route_publication_error<
            core::RuntimeParticipantExecutionRegistration>(
            "runtime_participant_physical_owner_ambiguous",
            "resolved Program participant matches multiple physical owners");
      }
      physical = &candidate;
    }
    if (physical == nullptr) {
      return route_publication_error<
          core::RuntimeParticipantExecutionRegistration>(
          "runtime_participant_stop_port_missing",
          "resolved Program participant has no exact composed physical owner",
          core::ErrorCode::kMissingReference);
    }
    try {
      // Logical outputs already have their exact composed provider engine and
      // prepared-command port. They have no physical Stop defaults to rebuild.
      if (binding.value().binding.value().io_output_binding) {
        auto registration = *physical;
        registration.binding = std::move(binding.value().binding);
        registration.intrinsic_direct_control =
            binding.value().intrinsic_direct_control;
        return core::Result<core::RuntimeParticipantExecutionRegistration>::Ok(
            std::move(registration));
      }
      auto registration = runtime_registration(
          std::move(binding.value().binding), physical->stop_readiness,
          physical->engine, physical->stop_execution_port,
          physical->participant_policy, physical->default_operator_stop_profile,
          physical->default_fault_stop_profile,
          physical->deployment_maximum_tcp_linear_speed_mps,
          // The physical registration already settled the fault profile from
          // the same readiness (derived or Deployment-authored); keep it.
          true);
      registration.intrinsic_direct_control =
          binding.value().intrinsic_direct_control;
      registration.direct_control_joint_limits =
          physical->direct_control_joint_limits;
      registration.motion_takeover_preparation_budget_s =
          physical->motion_takeover_preparation_budget_s;
      registration.motion_takeover_planning_budget_s =
          physical->motion_takeover_planning_budget_s;
      return core::Result<core::RuntimeParticipantExecutionRegistration>::Ok(
          std::move(registration));
    } catch (const std::exception &exception) {
      return route_publication_error<
          core::RuntimeParticipantExecutionRegistration>(
          "runtime_participant_stop_readiness_invalid", exception.what());
    }
  };
}

[[nodiscard]] bool same_readiness_after_product_reseal(
    const std::optional<core::RuntimeParticipantStopReadiness> &predecessor,
    const std::optional<core::RuntimeParticipantStopReadiness> &candidate) {
  if (predecessor.has_value() != candidate.has_value()) return false;
  if (!predecessor.has_value()) return true;
  const auto &before = predecessor->value();
  const auto &after = candidate->value();
  if (before.controllable_component_id != after.controllable_component_id ||
      before.component_lifecycle_generation !=
          after.component_lifecycle_generation ||
      before.expected_controller_domain != after.expected_controller_domain) {
    return false;
  }
  auto prior_plan = before.resolved_stop_base_plan;
  const auto &next_plan = after.resolved_stop_base_plan;
  prior_plan.resolved_stop_base_plan_id =
      next_plan.resolved_stop_base_plan_id;
  prior_plan.retained_bundle_id = next_plan.retained_bundle_id;
  prior_plan.graph_revision = next_plan.graph_revision;
  return prior_plan == next_plan;
}

[[nodiscard]] bool same_registration_after_product_reseal(
    const core::RuntimeParticipantExecutionRegistration &predecessor,
    const core::RuntimeParticipantExecutionRegistration &candidate) {
  const bool policy_equal =
      predecessor.participant_policy.has_value() ==
          candidate.participant_policy.has_value() &&
      (!predecessor.participant_policy.has_value() ||
       predecessor.participant_policy->value() ==
           candidate.participant_policy->value());
  return retained_bindings_preserve_physical_owner_after_product_reseal(
             predecessor.binding, candidate.binding) &&
         same_readiness_after_product_reseal(predecessor.stop_readiness,
                                             candidate.stop_readiness) &&
         predecessor.retained_default_stop_limits ==
             candidate.retained_default_stop_limits &&
         predecessor.stop_node_category == candidate.stop_node_category &&
         predecessor.engine == candidate.engine &&
         predecessor.stop_execution_port == candidate.stop_execution_port &&
         policy_equal &&
         predecessor.prepared_motion_engine ==
             candidate.prepared_motion_engine &&
         predecessor.default_operator_stop_profile ==
             candidate.default_operator_stop_profile &&
         predecessor.default_fault_stop_profile ==
             candidate.default_fault_stop_profile &&
         predecessor.deployment_maximum_tcp_linear_speed_mps ==
             candidate.deployment_maximum_tcp_linear_speed_mps &&
         predecessor.intrinsic_direct_control ==
             candidate.intrinsic_direct_control &&
         predecessor.direct_control_joint_limits ==
             candidate.direct_control_joint_limits &&
         predecessor.motion_takeover_preparation_budget_s ==
             candidate.motion_takeover_preparation_budget_s &&
         predecessor.motion_takeover_planning_budget_s ==
             candidate.motion_takeover_planning_budget_s;
}

} // namespace

void RetainedNativeGripperRoutePublicationOwner::BindMotionRoot(
    const std::shared_ptr<RetainedNativeRuntime>& root) {
  if (!root || root->active_addressed_kernel() != &kernel_ ||
      root->clock.get() != &clock_ || !participant_peers_.empty() ||
      transition_mutex_ != &mutex_)
    throw std::invalid_argument("scalar route owner requires its exact existing motion root");
  transition_owner_ = root;
  transition_is_motion_root_ = true;
  transition_mutex_ = &root->route_publication_mutex_;
}

core::Result<void> RetainedNativeGripperRoutePublicationOwner::ValidateInstalledOwner(
    const std::shared_ptr<void>& transition_owner) const {
  // Caller holds the same transition mutex as installed peer-list writers.
  // The prior lookup released it before delegation, so validate again before
  // any channel/backend rebind; a removed/replaced scalar cannot be revived.
  if (transition_is_motion_root_) {
    const auto root = std::static_pointer_cast<RetainedNativeRuntime>(transition_owner);
    if (!root || std::count(root->independent_gripper_peers.begin(),
        root->independent_gripper_peers.end(), gripper_) != 1)
      return route_publication_error<void>("route_publication_physical_owner_changed",
          "scalar route no longer names the installed owner", core::ErrorCode::kMissingReference);
  }
  if (cell_owner_) {
    const auto actual = gripper_->registrations();
    const auto& rows = cell_owner_->participant_registrations();
    for (const auto& expected : actual) {
      const auto matches = std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.binding.value() == expected.binding.value() && row.engine == expected.engine &&
            row.stop_execution_port == expected.stop_execution_port &&
            row.stop_readiness.has_value() == expected.stop_readiness.has_value() &&
            (!row.stop_readiness || row.stop_readiness->value() == expected.stop_readiness->value());
      });
      if (matches != 1)
        return route_publication_error<void>("route_publication_physical_owner_changed",
            "scalar registration set is no longer the exact installed owner", core::ErrorCode::kMissingReference);
    }
    if (std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
          return row.engine == gripper_->participant_engine() &&
              row.binding.value().controllable_component_id == gripper_->binding().value().controllable_component_id;
        }) != static_cast<std::ptrdiff_t>(actual.size()))
      return route_publication_error<void>("route_publication_physical_owner_changed",
          "installed scalar has unexpected logical registrations", core::ErrorCode::kMissingReference);
  }
  return core::Result<void>::Ok();
}

core::RuntimeParticipantAdmissionResolver
make_retained_cell_kernel_participant_resolver(
    RetainedRuntimeParticipantResolver binding_resolver,
    std::vector<core::RuntimeParticipantExecutionRegistration>
        physical_registrations) {
  return make_kernel_participant_resolver(std::move(binding_resolver),
                                         std::move(physical_registrations));
}

std::optional<RetainedStopSurveillanceSourceInput>
build_retained_gripper_stop_surveillance_source(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    const std::shared_ptr<IRetainedObservedStateChannel> &state_channel,
    core::IResolvedParticipantExecutionEngine *participant_engine) {
  return stop_surveillance_source(
      binding, readiness, state_channel,
      gripper_goal_observer(participant_engine, binding));
}

namespace detail {

RetainedObservedStateChannelConfig select_retained_state_channel_config(
    std::optional<RetainedObservedStateChannelConfig> selected,
    const core::RuntimeParticipantBinding &) {
  if (selected.has_value()) {
    return std::move(*selected);
  }
  throw std::invalid_argument(
      "retained runtime execution adapter set is incomplete or owns a "
      "second state authority");
}

} // namespace detail

std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime_participants(
    RetainedNativeRuntimeInput input, RetainedNativeRuntimeConfig config,
    std::shared_ptr<WallClock> shared_clock,
    std::shared_ptr<SimpleScheduler> shared_scheduler) {
  if ((shared_clock == nullptr) != (shared_scheduler == nullptr)) {
    throw std::invalid_argument("native runtime clock and scheduler "
                                "capabilities must be supplied together");
  }
  if (!input.scene || !input.scene_mutex || !input.state_projector ||
      !input.collision_publisher || !input.planner ||
      !input.collision_shared_model || !input.reference_resolver_factory ||
      config.kernel.validation_collision_workers < 1 ||
      config.kernel.validation_collision_workers > 64 ||
      config.kernel.runtime_collision_lookahead_workers < 1 ||
      config.kernel.runtime_collision_lookahead_workers > 64 ||
      config.gripper_update_period_ns <= 0 ||
      (!config.initial_motion_positions_rad.empty() &&
       (config.initial_motion_positions_rad.size() !=
            input.motion_binding.value().resources.size() ||
        std::any_of(
            config.initial_motion_positions_rad.begin(),
            config.initial_motion_positions_rad.end(),
            [](const double value) { return !std::isfinite(value); })))) {
    throw std::invalid_argument(
        "native runtime composition input is incomplete or unbounded");
  }
  const auto controlled_stop_profiles = retained_controlled_stop_profiles(
      input.motion_binding, input.motion_stop_readiness);
  std::optional<core::MotionInterruptStopProfileDTO>
      motion_operator_stop_profile;
  std::optional<core::MotionInterruptStopProfileDTO> motion_fault_stop_profile;
  if (controlled_stop_profiles.has_value()) {
    motion_operator_stop_profile = controlled_stop_profiles->operator_profile;
    motion_fault_stop_profile = controlled_stop_profiles->fault_profile;
  }
  // Q37 (as main): the Deployment-authored fault profile is read directly,
  // with its defaults resolved once at load (item 24); nothing is derived.
  if (input.fault_stop_profile_from_deployment) {
    motion_fault_stop_profile.reset();
    if (input.deployment_fault_stop_profile.has_value()) {
      auto resolved = core::resolve_motion_interrupt_stop_jerk_limits(
          *input.deployment_fault_stop_profile);
      if (!resolved.ok()) {
        throw std::invalid_argument(
            "Deployment fault Stop profile is invalid: " +
            resolved.error().message);
      }
      // The component-authored map may also name gripper/mimic joints.
      // Like the operator profile's retained resource projection, select only
      // this participant's actual source joints, keeping its own fault values.
      // Missing arm limits remain absent for the existing strict Prepare check.
      const auto &resources = input.motion_binding.value().resources;
      const auto outside_participant = [&](const auto &limit) {
        return std::none_of(resources.begin(), resources.end(), [&](const auto &resource) {
          return resource.source_joint_name.has_value() &&
                 *resource.source_joint_name == limit.first;
        });
      };
      std::erase_if(resolved.value().per_joint_max_deceleration, outside_participant);
      std::erase_if(resolved.value().per_joint_max_jerk, outside_participant);
      if (resolved.value().per_joint_settle_velocity_threshold.has_value()) {
        std::erase_if(*resolved.value().per_joint_settle_velocity_threshold,
                      outside_participant);
        if (resolved.value().per_joint_settle_velocity_threshold->empty())
          resolved.value().per_joint_settle_velocity_threshold.reset();
      }
      motion_fault_stop_profile = std::move(resolved.value());
    }
  }
  const auto current_inventory =
      input.collision_publisher->current_scene_snapshot_lease_inventory();
  if (!current_inventory.has_value() ||
      !current_inventory->projection_binding.valid() ||
      !current_inventory->bundle.world ||
      !current_inventory->bundle.policy.has_value()) {
    throw std::invalid_argument(
        "native runtime requires one initialized retained collision world");
  }
  const auto &projection_revision =
      current_inventory->projection_binding.projection_revision;
  const auto &motion_value = input.motion_binding.value();
  if (input.control_binding_representatives.empty()) {
    input.control_binding_representatives.push_back(input.motion_binding);
    if (input.gripper_binding.has_value()) {
      input.control_binding_representatives.push_back(*input.gripper_binding);
    }
  }
  if (std::any_of(input.control_binding_representatives.begin(),
                  input.control_binding_representatives.end(),
                  [](const auto &candidate) {
                    return !candidate.value().control_path.has_value();
                  })) {
    throw std::invalid_argument(
        "native control inventory contains an unaddressed representative");
  }
  std::sort(input.control_binding_representatives.begin(),
            input.control_binding_representatives.end(),
            [](const auto &left, const auto &right) {
              return left.value().control_path->control_binding_id <
                     right.value().control_path->control_binding_id;
            });
  const std::size_t registration_count =
      input.control_binding_representatives.size() +
      input.motion_binding_aliases.size() +
      input.gripper_binding_aliases.size();
  const auto representative_owner_count = [&](const auto &candidate) {
    return static_cast<std::size_t>(retained_bindings_share_backend_owner(
               input.motion_binding, candidate)) +
           static_cast<std::size_t>(input.gripper_binding.has_value() &&
                                    retained_bindings_share_backend_owner(
                                        *input.gripper_binding, candidate));
  };
  const auto contains_exact_representative = [&](const auto &binding) {
    return std::count_if(input.control_binding_representatives.begin(),
                         input.control_binding_representatives.end(),
                         [&](const auto &candidate) {
                           return candidate.value() == binding.value();
                         }) == 1;
  };
  if (registration_count > core::kMaxOperationParticipants ||
      input.control_binding_representatives.size() >
          core::kMaxRouteReadinessPublicationRows ||
      std::any_of(input.control_binding_representatives.begin(),
                  input.control_binding_representatives.end(),
                  [&](const auto &candidate) {
                    return !candidate.value().control_path.has_value() ||
                           representative_owner_count(candidate) != 1U;
                  }) ||
      std::adjacent_find(
          input.control_binding_representatives.begin(),
          input.control_binding_representatives.end(),
          [](const auto &left, const auto &right) {
            return left.value().control_path->control_binding_id ==
                   right.value().control_path->control_binding_id;
          }) != input.control_binding_representatives.end() ||
      !contains_exact_representative(input.motion_binding) ||
      (input.gripper_binding.has_value() &&
       !contains_exact_representative(*input.gripper_binding)) ||
      std::any_of(input.motion_binding_aliases.begin(),
                  input.motion_binding_aliases.end(), [&](const auto &alias) {
                    return !same_physical_binding(input.motion_binding, alias,
                                                  true);
                  })) {
    throw std::invalid_argument(
        "native control inventory is incomplete, duplicated, or crosses its "
        "physical backend owner");
  }
  if ((!input.gripper_binding.has_value() &&
       !input.gripper_binding_aliases.empty()) ||
      (input.gripper_binding.has_value() &&
       std::any_of(input.gripper_binding_aliases.begin(),
                   input.gripper_binding_aliases.end(), [&](const auto &alias) {
                     return !same_physical_binding(*input.gripper_binding,
                                                   alias, false);
                   }))) {
    throw std::invalid_argument(
        "native gripper alias crosses or widens its physical runtime binding");
  }
  if (motion_value.control_path.has_value() &&
      motion_value.control_path->runtime_scale_qualification.has_value()) {
    const auto &runtime_scale =
        *motion_value.control_path->runtime_scale_qualification;
    if (runtime_scale.acknowledgement_kind !=
            core::RuntimeScaleAcknowledgementKind::kAppliedState ||
        runtime_scale.coordinated_barrier_id.has_value() ||
        runtime_scale.maximum_slew_per_second !=
            qualified_runtime_scale_slew_per_second()) {
      throw std::invalid_argument("native motion runtime-scale qualification "
                                  "differs from the selected mechanism");
    }
  }
  if (projection_revision.retained_bundle_id !=
          motion_value.retained_bundle_id ||
      projection_revision.graph_revision != motion_value.graph_revision) {
    throw std::invalid_argument(
        "native runtime motion binding crosses its collision projection");
  }
  require_planner_binding(input.motion_binding, *input.planner,
                          projection_revision);

  std::optional<core::RuntimeParticipantPolicySelection>
      motion_participant_policy =
          core::select_runtime_participant_policy(input.motion_binding);

  if (!input.deployment_maximum_tcp_linear_speed_mps.has_value() ||
      !std::isfinite(*input.deployment_maximum_tcp_linear_speed_mps) ||
      *input.deployment_maximum_tcp_linear_speed_mps <= 0.0 ||
      *input.deployment_maximum_tcp_linear_speed_mps > 1000.0) {
    throw std::invalid_argument(
        "native motion target requires one finite positive Deployment TCP "
        "linear-speed maximum");
  }

  if (input.ordinary_motion_jerk && input.ordinary_motion_jerk->enabled &&
      input.backend_factory) {
    throw std::invalid_argument(
        "ordinary jerk profile requires the native execution backend");
  }

  auto runtime = std::make_shared<RetainedNativeRuntime>(input.motion_binding,
                                                         input.gripper_binding);
  runtime->motion_binding_aliases = std::move(input.motion_binding_aliases);
  runtime->gripper_binding_aliases = std::move(input.gripper_binding_aliases);
  runtime->control_binding_representatives =
      std::move(input.control_binding_representatives);
  runtime->motion_operator_stop_profile = motion_operator_stop_profile;
  runtime->motion_fault_stop_profile = motion_fault_stop_profile;
  runtime->motion_fault_stop_profile_from_deployment =
      input.fault_stop_profile_from_deployment;
  runtime->motion_participant_policy = motion_participant_policy;
  runtime->deployment_maximum_tcp_linear_speed_mps =
      input.deployment_maximum_tcp_linear_speed_mps;
  runtime->direct_control_joint_limits =
      std::move(input.direct_control_joint_limits);
  runtime->participant_resolver_factory =
      std::move(input.participant_resolver_factory);
  runtime->prepare_measured_sources = std::move(input.prepare_measured_sources);
  runtime->kinematic_model = std::make_shared<RetainedKinematicModelProxy>(
      make_retained_motion_model_generation(
          runtime->motion_binding, std::move(input.scene),
          std::move(input.scene_mutex), std::move(input.state_projector)));
  const auto model_generation = runtime->kinematic_model->current_generation();
  runtime->collision_publisher = std::move(input.collision_publisher);

  runtime->clock = shared_clock != nullptr ? std::move(shared_clock)
                                           : std::make_shared<WallClock>();
  runtime->scheduler =
      shared_scheduler != nullptr
          ? std::move(shared_scheduler)
          : std::make_shared<SimpleScheduler>(std::move(config.scheduler));
  runtime->cache = std::make_unique<core::TrajectoryCache>(config.cache);
  runtime->planner = std::move(input.planner);

  auto motion_state_config = detail::select_retained_state_channel_config(
      std::move(input.motion_state_channel_config), runtime->motion_binding);
  auto observation_wake = input.observation_wake != nullptr
                              ? std::move(input.observation_wake)
                              : std::make_shared<RetainedObservationWake>();
  runtime->motion_state_channel =
      std::make_shared<RetainedObservedStateChannel>(
          runtime->motion_binding, model_generation->state_projector,
          std::move(motion_state_config), runtime->clock.get(),
          observation_wake);
  const auto initial_timestamp_ns = runtime->clock->now();
  const auto motion_initialized =
      config.initial_motion_positions_rad.empty()
          ? runtime->motion_state_channel->initialize_from_projection(
                initial_timestamp_ns)
          : runtime->motion_state_channel->source_kind() == core::ObservedStateSourceKind::kCommandedState
                ? runtime->motion_state_channel->publish_commanded_initialization_and_project(
                      config.initial_motion_positions_rad,
                      std::vector<double>(config.initial_motion_positions_rad.size(), 0.0),
                      initial_timestamp_ns)
                : runtime->motion_state_channel->publish_and_project(
                      config.initial_motion_positions_rad,
                      std::vector<double>(config.initial_motion_positions_rad.size(), 0.0),
                      initial_timestamp_ns, initial_timestamp_ns);
  if (!motion_initialized.applied()) {
    throw std::invalid_argument(
        "native motion state could not be seeded from the retained model");
  }
  runtime->angular_observed_state =
      std::make_unique<core::AddressedAngularObservedRobotStateAdapter>(
          runtime->motion_state_channel->publication());
  if (input.ordinary_motion_jerk && input.ordinary_motion_jerk->enabled) {
    auto limits = runtime->kinematic_model->joint_limits(
        motion_value.controllable_component_id.value(),
        motion_value.command_target_id.value());
    if (!limits.ok())
      throw std::invalid_argument(limits.error().message);
    auto profile = deployment::execution_runtime_contract::
        resolve_ordinary_motion_jerk_policy(
            *input.ordinary_motion_jerk,
            runtime->motion_binding.value().resources, limits.value());
    if (!profile.ok())
      throw std::invalid_argument(profile.error().message);
    for (const auto &[joint, cap] : runtime->direct_control_joint_limits) {
      if (!cap.max_jerk_rad_s3)
        continue;
      const auto found = profile.value().limits.find(joint);
      if (!profile.value().enabled || found == profile.value().limits.end() ||
          !std::isfinite(*cap.max_jerk_rad_s3) || *cap.max_jerk_rad_s3 <= 0.0 ||
          *cap.max_jerk_rad_s3 >
              std::get<core::AngularJerkDTO>(found->second).value_rad_s3)
        throw std::invalid_argument(
            "manual jerk cap conflicts with the ordinary-motion policy for " +
            joint);
    }
    auto selected = core::apply_deployment_motion_jerk_policy(
        *motion_participant_policy, runtime->motion_binding,
        std::move(profile.value()));
    if (!selected.ok())
      throw std::invalid_argument(selected.error().message);
    motion_participant_policy = std::move(selected.value());
    runtime->motion_participant_policy = motion_participant_policy;
  }

  if (runtime->gripper_binding.has_value()) {
    const auto &gripper_value = runtime->gripper_binding->value();
    if (gripper_value.retained_bundle_id != motion_value.retained_bundle_id ||
        gripper_value.graph_revision != motion_value.graph_revision ||
        gripper_value.controllable_component_id !=
            motion_value.controllable_component_id) {
      throw std::invalid_argument(
          "native aggregate gripper crosses its motion component generation");
    }
    if (gripper_value.resources.size() != 1U) {
      throw std::invalid_argument(
          "native gripper binding has no exact scalar resource");
    }
    const bool model_backed =
        gripper_value.resources.front().source_joint_name.has_value();
    runtime->gripper_state_channel =
        std::make_shared<RetainedObservedStateChannel>(
            *runtime->gripper_binding,
            model_backed ? model_generation->state_projector : nullptr,
            detail::select_retained_state_channel_config(
                std::move(input.gripper_state_channel_config),
                *runtime->gripper_binding),
            runtime->clock.get(), observation_wake);
    if (model_backed) {
      const auto gripper_initialized =
          runtime->gripper_state_channel->initialize_from_projection(
              runtime->clock->now());
      if (!gripper_initialized.applied()) {
        throw std::invalid_argument(
            "native gripper state could not be seeded from the retained model");
      }
      runtime->gripper_position_limits =
          retained_gripper_limits(*model_generation->scene,
                                  *runtime->gripper_binding);
    }
  }

  if (input.backend_factory) {
    auto selected = input.backend_factory(
        runtime->motion_binding, runtime->gripper_binding,
        runtime->motion_state_channel, runtime->gripper_state_channel,
        *runtime->clock, *runtime->scheduler);
    runtime->execution = std::move(selected.execution);
    runtime->gripper = std::move(selected.gripper);
    runtime->gripper_stop_execution_port = selected.gripper_stop_execution_port;
    runtime->rebind_motion_route = std::move(selected.rebind_motion_route);
    runtime->rebind_gripper_route = std::move(selected.rebind_gripper_route);
  }
  if (!runtime->execution || !runtime->rebind_motion_route ||
      runtime->execution->observed_robot_state_publication() !=
          &runtime->motion_state_channel->publication().numeric_publication() ||
      runtime->gripper_binding.has_value() !=
          static_cast<bool>(runtime->gripper) ||
      (runtime->gripper_binding.has_value() &&
       !runtime->rebind_gripper_route)) {
    throw std::invalid_argument(
        "retained runtime execution adapter set is incomplete or owns a "
        "second state authority");
  }
  if (const auto rebound = runtime->rebind_motion_route(
          runtime->motion_binding, runtime->motion_binding_aliases);
      !rebound.ok()) {
    throw std::invalid_argument(rebound.error().message);
  }

  std::vector<std::shared_ptr<RetainedObservedStateChannel>> state_channels{
      runtime->motion_state_channel};
  if (runtime->gripper_state_channel) {
    state_channels.push_back(runtime->gripper_state_channel);
  }
  runtime->reference_resolver =
      input.reference_resolver_factory(state_channels);
  if (!runtime->reference_resolver) {
    throw std::invalid_argument(
        "native runtime reference resolver factory returned no resolver");
  }
  runtime->reference_resolver_proxy =
      std::make_shared<RetainedReferencePoseResolverProxy>(
          runtime->reference_resolver);

  const auto &shared_model = input.collision_shared_model;
  if (!shared_model) {
    throw std::invalid_argument(
        "native collision publisher has no retained shared model");
  }
  runtime->runtime_collision_engine = collision_engine(
      shared_model,
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model));
  const auto validation_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model);
  runtime->validation_collision_engines.reserve(
      static_cast<std::size_t>(config.kernel.validation_collision_workers));
  for (std::size_t index = 0U;
       index <
       static_cast<std::size_t>(config.kernel.validation_collision_workers);
       ++index) {
    runtime->validation_collision_engines.push_back(
        collision_engine(shared_model, validation_cache));
  }
  const auto lookahead_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          shared_model);
  runtime->lookahead_collision_engines.reserve(static_cast<std::size_t>(
      config.kernel.runtime_collision_lookahead_workers));
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(
                   config.kernel.runtime_collision_lookahead_workers);
       ++index) {
    runtime->lookahead_collision_engines.push_back(
        collision_engine(shared_model, lookahead_cache));
  }

  if (input.planning_scene_synchronizer != nullptr) {
    runtime->planning_scene_synchronizer =
        std::make_shared<RetainedPlanningSceneSynchronizer>(
            std::move(input.planning_scene_synchronizer));
  }
  core::ExecutionKernelPorts ports{*runtime->angular_observed_state,
                                   *runtime->kinematic_model,
                                   *runtime->reference_resolver_proxy,
                                   *runtime->kinematic_model,
                                   *runtime->kinematic_model,
                                   nullptr,
                                   runtime->planning_scene_synchronizer.get()};
  runtime->participant_engine = std::make_shared<core::ExecutionKernel>(
      *runtime->planner, ports, *runtime->execution, *runtime->clock,
      *runtime->scheduler, *runtime->cache, std::move(config.kernel));
  runtime->participant_engine->set_collision_resources(
      *runtime->collision_publisher, runtime->runtime_collision_engine,
      runtime->validation_collision_engines,
      runtime->lookahead_collision_engines);
  runtime->participant_engine->set_collision_validation_policy_snapshot(
      runtime->collision_publisher->collision_policy_snapshot());
  if (runtime->gripper) {
    runtime->gripper_participant_engine =
        std::make_shared<core::GripperParticipantEngine>(*runtime->gripper,
                                                         *runtime->clock);
    if (runtime->gripper_stop_execution_port == nullptr)
      runtime->gripper_stop_execution_port = runtime->gripper_participant_engine.get();
  }
  if (runtime->gripper_binding.has_value() &&
      runtime->gripper_stop_execution_port == nullptr) {
    throw std::invalid_argument(
        "retained runtime gripper adapter has no Stop execution port");
  }
  if (runtime->gripper_binding.has_value()) {
    const auto *execution_stop =
        static_cast<core::IControlDomainExecutionPort *>(
            runtime->execution.get());
    const auto *gripper_owned_stop =
        dynamic_cast<core::IControlDomainExecutionPort *>(
            runtime->gripper.get());
    if (runtime->gripper_stop_execution_port != execution_stop &&
        runtime->gripper_stop_execution_port != gripper_owned_stop &&
        runtime->gripper_stop_execution_port != runtime->gripper_participant_engine.get()) {
      throw std::invalid_argument(
          "retained runtime gripper Stop port is not owned by its adapter "
          "set");
    }
  }

  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  registrations.reserve(runtime->control_binding_representatives.size() +
                        runtime->motion_binding_aliases.size() +
                        runtime->gripper_binding_aliases.size());
  for (const auto &representative : runtime->control_binding_representatives) {
    const bool motion_owner = retained_bindings_share_backend_owner(
        runtime->motion_binding, representative);
    const bool primary = representative.value() ==
                         (motion_owner ? runtime->motion_binding.value()
                                       : runtime->gripper_binding->value());
    if (motion_owner) {
      registrations.push_back(runtime_registration(
          representative,
          primary ? input.motion_stop_readiness : std::nullopt,
          runtime->participant_engine_for(true),
          static_cast<core::IControlDomainExecutionPort *>(
              runtime->execution.get()),
          representative_participant_policy(primary, true,
                                            motion_participant_policy),
          motion_operator_stop_profile, motion_fault_stop_profile,
          input.deployment_maximum_tcp_linear_speed_mps,
          input.fault_stop_profile_from_deployment));
    } else {
      registrations.push_back(gripper_runtime_registration(
          representative,
          primary ? input.gripper_stop_readiness : std::nullopt,
          runtime->participant_engine_for(false),
          runtime->gripper_stop_execution_port));
    }
  }
  for (const auto &alias : runtime->motion_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.motion_stop_readiness, runtime->participant_engine.get(),
        runtime->execution.get(), motion_participant_policy,
        motion_operator_stop_profile, motion_fault_stop_profile,
        input.deployment_maximum_tcp_linear_speed_mps,
        input.fault_stop_profile_from_deployment));
  }
  for (const auto &alias : runtime->gripper_binding_aliases) {
    registrations.push_back(gripper_runtime_registration(
        alias, input.gripper_stop_readiness,
        runtime->gripper_participant_engine.get(),
        runtime->gripper_stop_execution_port));
  }
  std::vector<core::RuntimeParticipantExecutionRegistration>
      physical_registrations;
  physical_registrations.push_back(runtime_registration(
      runtime->motion_binding, input.motion_stop_readiness,
      runtime->participant_engine.get(), runtime->execution.get(),
      motion_participant_policy, motion_operator_stop_profile,
      motion_fault_stop_profile,
      input.deployment_maximum_tcp_linear_speed_mps,
      input.fault_stop_profile_from_deployment));
  physical_registrations.back().direct_control_joint_limits =
      runtime->direct_control_joint_limits;
  if (runtime->gripper_binding.has_value()) {
    physical_registrations.push_back(gripper_runtime_registration(
        *runtime->gripper_binding, input.gripper_stop_readiness,
        runtime->gripper_participant_engine.get(),
        runtime->gripper_stop_execution_port));
  }
  auto binding_resolver = std::move(input.participant_resolver);
  if (runtime->participant_resolver_factory) {
    std::vector<core::RuntimeParticipantBinding> physical_bindings{
        runtime->motion_binding};
    if (runtime->gripper_binding.has_value()) {
      physical_bindings.push_back(*runtime->gripper_binding);
    }
    binding_resolver =
        runtime->participant_resolver_factory(std::move(physical_bindings));
  }
  auto participant_resolver = make_kernel_participant_resolver(
      std::move(binding_resolver), std::move(physical_registrations));
  runtime->stop_surveillance_policy = config.stop_surveillance_policy;
  runtime->participant_registrations = std::move(registrations);
  runtime->participant_resolver = std::move(participant_resolver);
  runtime->stop_surveillance_owner =
      std::make_shared<RetainedStopSurveillanceOwner>(
          stop_surveillance_sources(
              runtime->motion_binding,
              readiness_for(runtime->participant_registrations,
                            runtime->motion_binding),
              runtime->motion_state_channel, runtime->gripper_binding,
              runtime->gripper_binding.has_value()
                  ? readiness_for(runtime->participant_registrations,
                                  *runtime->gripper_binding)
                  : std::nullopt,
              runtime->gripper_state_channel, runtime->participant_engine.get(),
              runtime->gripper_participant_engine.get(),
              runtime->participant_registrations),
          runtime->clock.get());
  return runtime;
}

namespace {

struct CollectedNativeRuntimeParticipants final {
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  std::vector<core::RuntimeParticipantExecutionRegistration> physical;
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  std::vector<RetainedStopSurveillanceSourceInput> surveillance;
};

[[nodiscard]] CollectedNativeRuntimeParticipants
collect_native_runtime_participants(
    const std::vector<std::shared_ptr<RetainedNativeRuntime>> &runtimes) {
  CollectedNativeRuntimeParticipants collected;
  for (const auto &runtime : runtimes) {
    collected.registrations.insert(collected.registrations.end(),
                                   runtime->participant_registrations.begin(),
                                   runtime->participant_registrations.end());
    const auto append_physical =
        [&](const core::RuntimeParticipantBinding &binding) {
          collected.physical_bindings.push_back(binding);
          const auto found = std::find_if(
              runtime->participant_registrations.begin(),
              runtime->participant_registrations.end(),
              [&](const auto &registration) {
                return registration.binding.value() == binding.value();
              });
          if (found == runtime->participant_registrations.end()) {
            throw std::invalid_argument(
                "native participant join has no physical registration");
          }
          collected.physical.push_back(*found);
        };
    append_physical(runtime->motion_binding);
    if (runtime->gripper_binding.has_value()) {
      append_physical(*runtime->gripper_binding);
    }
    auto sources = stop_surveillance_sources(
        runtime->motion_binding,
        readiness_for(runtime->participant_registrations,
                      runtime->motion_binding),
        runtime->motion_state_channel, runtime->gripper_binding,
        runtime->gripper_binding.has_value()
            ? readiness_for(runtime->participant_registrations,
                            *runtime->gripper_binding)
            : std::nullopt,
        runtime->gripper_state_channel, runtime->participant_engine.get(),
        runtime->gripper_participant_engine.get(),
        runtime->participant_registrations);
    collected.surveillance.insert(
        collected.surveillance.end(),
        std::make_move_iterator(sources.begin()),
        std::make_move_iterator(sources.end()));
  }
  return collected;
}

} // namespace

void join_retained_native_runtime_participants(
    const std::shared_ptr<RetainedNativeRuntime> &primary,
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers,
    RetainedRuntimeParticipantResolverFactory participant_resolver_factory) {
  if (primary == nullptr || primary->addressed_kernel != nullptr ||
      primary->operation_journal != nullptr || primary->clock == nullptr ||
      primary->scheduler == nullptr || !participant_resolver_factory ||
      peers.empty() || !primary->participant_peers.empty()) {
    throw std::invalid_argument("native participant join is incomplete or duplicated");
  }
  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtimes{primary};
  for (auto &peer : peers) {
    if (peer == nullptr || peer.get() == primary.get() ||
        peer->addressed_kernel != nullptr || peer->operation_journal != nullptr ||
        peer->clock != primary->clock || peer->scheduler != primary->scheduler ||
        !peer->participant_peers.empty()) {
      throw std::invalid_argument("native participant join crosses runtime ownership");
    }
    runtimes.push_back(peer);
  }

  auto collected = collect_native_runtime_participants(runtimes);
  if (collected.registrations.size() >
          core::kMaxProgramExecutionMemberships ||
      collected.physical.size() > kNativeMaximumLiveStopParticipants) {
    throw std::invalid_argument("native participant join exceeds the cell bound");
  }
  auto binding_resolver =
      participant_resolver_factory(collected.physical_bindings);
  if (!binding_resolver) {
    throw std::invalid_argument("native participant join has no resolver");
  }
  primary->participant_registrations = std::move(collected.registrations);
  primary->participant_resolver = make_kernel_participant_resolver(
      std::move(binding_resolver), std::move(collected.physical));
  primary->participant_resolver_factory = std::move(participant_resolver_factory);
  primary->stop_surveillance_owner =
      std::make_shared<RetainedStopSurveillanceOwner>(
          std::move(collected.surveillance), primary->clock.get());
  primary->cell_runtime_root = primary.get();
  for (const auto &peer : peers) peer->cell_runtime_root = primary.get();
  primary->participant_peers = std::move(peers);
}

core::Result<void> rejoin_retained_native_runtime_participants(
    const std::shared_ptr<RetainedNativeRuntime> &primary,
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers,
    RetainedRuntimeParticipantResolverFactory participant_resolver_factory,
    std::shared_ptr<RetainedStopSurveillanceOwner> stable_stop_observer,
    std::vector<RetainedStopSurveillanceSourceInput>
        &candidate_surveillance_sources,
    core::IResourceOperationAdmissionGate &admission_gate,
    const core::ConfigurationFenceId &fence_id,
    const core::ConfigurationFenceScopeDTO &scope) {
  const auto fail = [](std::string reason, std::string message) {
    return route_publication_error<void>(std::move(reason),
                                         std::move(message));
  };
  if (primary == nullptr || !participant_resolver_factory ||
      stable_stop_observer == nullptr || fence_id.empty() || !scope.valid() ||
      !admission_gate.exact_fence_is_held(fence_id, scope)) {
    return fail("native_participant_rejoin_fence_or_owner_invalid",
                "native participant rejoin requires a complete candidate and exact held Apply fence");
  }

  std::vector<std::shared_ptr<RetainedNativeRuntime>> runtimes{primary};
  runtimes.insert(runtimes.end(), peers.begin(), peers.end());
  std::set<RetainedNativeRuntime *> unique_runtimes;
  std::vector<RetainedNativeRuntime *> lock_order;
  std::set<core::ControllableComponentId> component_ids;
  std::map<core::ControlDomainId, core::ControllableComponentId> domain_owners;
  std::map<core::ResourceId, core::ControllableComponentId> resource_owners;
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  std::vector<core::RuntimeParticipantExecutionRegistration> physical;
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  std::vector<RetainedStopSurveillanceSourceInput> surveillance_sources;
  std::size_t addressed_kernel_owners = 0U;
  std::size_t journal_owners = 0U;
  std::optional<core::StopSurveillancePolicyDTO> surveillance_policy;
  std::set<RetainedNativeRuntime *> predecessor_roots;

  for (const auto &runtime : runtimes) {
    if (runtime == nullptr || !unique_runtimes.insert(runtime.get()).second ||
        runtime->clock == nullptr || runtime->scheduler == nullptr ||
        runtime->motion_binding.value().controllable_component_id.empty() ||
        runtime->participant_engine == nullptr ||
        runtime->motion_state_channel == nullptr ||
        runtime->stop_surveillance_owner == nullptr) {
      return fail("native_participant_rejoin_owner_invalid",
                  "candidate cell has a null, duplicated, or incomplete component owner");
    }
    lock_order.push_back(runtime.get());
    if (!component_ids
             .insert(runtime->motion_binding.value().controllable_component_id)
             .second) {
      return fail("native_participant_rejoin_component_duplicated",
                  "candidate cell repeats a controllable component owner");
    }
    if (runtime->addressed_kernel != nullptr) ++addressed_kernel_owners;
    if (runtime->operation_journal != nullptr) ++journal_owners;
    if (runtime->addressed_kernel != nullptr &&
        runtime->operation_journal == nullptr) {
      return fail("native_participant_rejoin_owner_pair_incomplete",
                  "candidate stable Kernel has no matching durable journal");
    }
    if (runtime->cell_runtime_root != nullptr) {
      predecessor_roots.insert(runtime->cell_runtime_root);
      if ((runtime->cell_runtime_root->addressed_kernel != nullptr ||
           runtime->cell_runtime_root->cell_addressed_kernel != nullptr) &&
          runtime->cell_runtime_root->stop_surveillance_owner !=
              stable_stop_observer) {
        return fail("native_participant_rejoin_stable_observer_mismatch",
                    "candidate does not preserve the observer referenced by its stable Kernel");
      }
    }
    if (runtime->clock != primary->clock ||
        runtime->scheduler != primary->scheduler) {
      return fail("native_participant_rejoin_time_owner_mismatch",
                  "candidate component engines do not share the cell clock and scheduler");
    }
    if (runtime->stop_surveillance_policy.has_value() !=
            primary->stop_surveillance_policy.has_value() ||
        (runtime->stop_surveillance_policy.has_value() &&
         runtime->stop_surveillance_policy !=
             primary->stop_surveillance_policy)) {
      return fail("native_participant_rejoin_stop_policy_mismatch",
                  "candidate component owners disagree on the cell Stop policy");
    }
    if (!surveillance_policy.has_value())
      surveillance_policy = runtime->stop_surveillance_policy;
    for (const auto &binding : runtime->control_binding_representatives) {
      if (!binding.value().control_path.has_value()) {
        return fail("native_participant_rejoin_binding_route_missing",
                    "candidate component representative has no control route");
      }
      const auto &component = binding.value().controllable_component_id;
      const auto &domain = binding.value().control_path->control_domain_id;
      const auto [domain_at, domain_inserted] =
          domain_owners.emplace(domain, component);
      if (!domain_inserted && domain_at->second != component) {
        return fail("native_participant_rejoin_shared_domain_unsupported",
                    "native control domain is shared by distinct component owners");
      }
    }
    const auto add_resources = [&](const core::RuntimeParticipantBinding &binding) {
      for (const auto &resource : binding.value().resources) {
        const auto [at, inserted] = resource_owners.emplace(
            resource.resource_id,
            binding.value().controllable_component_id);
        if (!inserted) return false;
      }
      return true;
    };
    if (!add_resources(runtime->motion_binding) ||
        (runtime->gripper_binding.has_value() &&
         !add_resources(*runtime->gripper_binding))) {
      return fail("native_participant_rejoin_resource_overlap",
                  "candidate component owners share a native physical resource");
    }
  }
  if (addressed_kernel_owners > 1U || journal_owners > 1U ||
      addressed_kernel_owners != journal_owners) {
    return fail("native_participant_rejoin_multiple_stable_owners",
                "candidate cell does not have exactly one consistent addressed Kernel owner");
  }
  if (predecessor_roots.size() > 1U) {
    return fail("native_participant_rejoin_predecessor_root_ambiguous",
                "candidate components came from multiple native cell roots");
  }

  std::sort(lock_order.begin(), lock_order.end());
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(lock_order.size());
  for (auto *runtime : lock_order)
    locks.emplace_back(runtime->route_publication_mutex_);
  if (!admission_gate.exact_fence_is_held(fence_id, scope)) {
    return fail("native_participant_rejoin_fence_changed",
                "candidate cell lost its exact Apply fence during validation");
  }

  for (const auto &runtime : runtimes) {
    std::vector<core::RuntimeParticipantExecutionRegistration> local;
    for (const auto &registration : runtime->participant_registrations) {
      if (registration.binding.value().controllable_component_id ==
          runtime->motion_binding.value().controllable_component_id) {
        local.push_back(registration);
      }
    }
    if (local.empty()) {
      return fail("native_participant_rejoin_registration_missing",
                  "candidate component has no local execution registrations");
    }
    const auto collect_physical = [&](
        const core::RuntimeParticipantBinding &binding) -> bool {
      const auto selected = std::find_if(
          local.begin(), local.end(), [&](const auto &registration) {
            return registration.binding.value() == binding.value();
          });
      if (selected == local.end()) return false;
      physical_bindings.push_back(binding);
      physical.push_back(*selected);
      return true;
    };
    if (!collect_physical(runtime->motion_binding) ||
        (runtime->gripper_binding.has_value() &&
         !collect_physical(*runtime->gripper_binding))) {
      return fail("native_participant_rejoin_physical_registration_missing",
                  "candidate component lacks exact physical registrations and Stop readiness");
    }
    const auto expected_count =
        runtime->control_binding_representatives.size() +
        runtime->motion_binding_aliases.size() +
        runtime->gripper_binding_aliases.size();
    std::vector<core::RuntimeParticipantBinding> expected_bindings;
    expected_bindings.reserve(expected_count);
    expected_bindings.insert(expected_bindings.end(),
                             runtime->control_binding_representatives.begin(),
                             runtime->control_binding_representatives.end());
    expected_bindings.insert(expected_bindings.end(),
                             runtime->motion_binding_aliases.begin(),
                             runtime->motion_binding_aliases.end());
    expected_bindings.insert(expected_bindings.end(),
                             runtime->gripper_binding_aliases.begin(),
                             runtime->gripper_binding_aliases.end());
    std::set<core::ControlBindingId> local_ids;
    if (local.size() != expected_count ||
        std::any_of(local.begin(), local.end(), [&](const auto &row) {
          return !row.binding.value().control_path.has_value() ||
                 !local_ids.insert(row.binding.value()
                                       .control_path->control_binding_id)
                      .second ||
                 std::none_of(expected_bindings.begin(),
                              expected_bindings.end(), [&](const auto &expected) {
                                return expected.value() ==
                                       row.binding.value();
                              });
        }) ||
        local_ids.size() != expected_bindings.size()) {
      return fail("native_participant_rejoin_registration_count_mismatch",
                  "candidate component registrations do not exactly match its selected bindings");
    }
    registrations.insert(registrations.end(), local.begin(), local.end());
    auto local_sources = stop_surveillance_sources(
        runtime->motion_binding,
        readiness_for(local, runtime->motion_binding),
        runtime->motion_state_channel, runtime->gripper_binding,
        runtime->gripper_binding.has_value()
            ? readiness_for(local, *runtime->gripper_binding)
            : std::nullopt,
        runtime->gripper_state_channel, runtime->participant_engine.get(),
        runtime->gripper_participant_engine.get(), local);
    surveillance_sources.insert(
        surveillance_sources.end(),
        std::make_move_iterator(local_sources.begin()),
        std::make_move_iterator(local_sources.end()));
  }
  if (registrations.size() > core::kMaxProgramExecutionMemberships ||
      physical.size() > kNativeMaximumLiveStopParticipants ||
      surveillance_sources.empty()) {
    return fail("native_participant_rejoin_cell_bound_exceeded",
                "candidate cell exceeds a qualified limit or has no Stop sources");
  }

  auto sources_valid =
      RetainedStopSurveillanceOwner::ValidateSources(surveillance_sources);
  if (!sources_valid.ok()) {
    return fail("native_participant_rejoin_surveillance_invalid",
                sources_valid.error().message);
  }
  RetainedRuntimeParticipantResolver binding_resolver;
  try {
    binding_resolver = participant_resolver_factory(physical_bindings);
  } catch (const std::exception &exception) {
    return fail("native_participant_rejoin_resolver_factory_failed",
                exception.what());
  } catch (...) {
    return fail("native_participant_rejoin_resolver_factory_failed",
                "candidate native resolver factory threw an unknown exception");
  }
  if (!binding_resolver) {
    return fail("native_participant_rejoin_resolver_missing",
                "candidate native resolver factory returned no resolver");
  }
  auto aggregate_resolver = make_kernel_participant_resolver(
      std::move(binding_resolver), physical);
  if (!aggregate_resolver) {
    return fail("native_participant_rejoin_resolver_aggregate_failed",
                "candidate native resolver could not be aggregated");
  }
  if (!admission_gate.exact_fence_is_held(fence_id, scope)) {
    return fail("native_participant_rejoin_fence_changed",
                "candidate cell lost its exact Apply fence before publication");
  }

  // Return the fully validated source table for the host's atomic Kernel
  // participant commit. Replacing sources here would expose the new observer
  // view before that Kernel commit succeeds.
  candidate_surveillance_sources = std::move(surveillance_sources);

  // All candidate validation is complete. Move the prepared aggregate into
  // the new root while preserving the exact observer object already
  // referenced non-owningly by the stable Kernel; the observer's source table
  // remains unchanged until the host's commit hook publishes it.
  primary->participant_registrations = std::move(registrations);
  primary->participant_resolver = std::move(aggregate_resolver);
  primary->participant_resolver_factory =
      std::move(participant_resolver_factory);
  primary->stop_surveillance_policy = surveillance_policy;
  primary->stop_surveillance_owner = std::move(stable_stop_observer);
  for (const auto &runtime : runtimes) {
    if (runtime.get() != primary.get()) {
      runtime->participant_peers.clear();
      runtime->cell_runtime_root = primary.get();
    }
  }
  primary->cell_runtime_root = primary.get();
  primary->participant_peers = std::move(peers);
  return core::Result<void>::Ok();
}

core::Result<void> reseal_retained_native_runtime_for_apply(
    RetainedNativeRuntime &runtime, RetainedNativeRuntimeResealInput input) {
  const auto fail = [](std::string reason, std::string message) {
    return route_publication_error<void>(std::move(reason),
                                         std::move(message));
  };
  if (input.apply_fence_id.empty() || !input.apply_scope.valid() ||
      !input.admission_gate.exact_fence_is_held(input.apply_fence_id,
                                                input.apply_scope)) {
    return fail("native_product_reseal_fence_not_held",
                "retained native owner re-seal requires the exact held Apply fence");
  }

  // Every reader and writer of a participant's routes (GripperSourceIsCurrent,
  // ReadGripperStatus, route publication) serializes
  // on its cell root's mutex, so the re-seal must hold that same mutex: a peer
  // participant's own mutex would leave those readers running against the
  // bindings and registrations replaced below.
  auto *const cell_root = runtime.cell_runtime_root != nullptr
                              ? runtime.cell_runtime_root
                              : &runtime;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  if (!input.admission_gate.exact_fence_is_held(input.apply_fence_id,
                                                input.apply_scope)) {
    return fail("native_product_reseal_fence_changed",
                "retained native owner lost its exact Apply fence");
  }
  if (runtime.clock == nullptr || runtime.motion_state_channel == nullptr ||
      runtime.execution == nullptr || !runtime.rebind_motion_route ||
      runtime.participant_engine == nullptr ||
      runtime.reference_resolver_proxy == nullptr ||
      input.reference_resolver == nullptr ||
      input.motion_binding.value().control_path == std::nullopt ||
      input.motion_binding.value().controllable_component_id !=
          runtime.motion_binding.value().controllable_component_id ||
      (input.gripper_binding.has_value() &&
       (input.gripper_binding->value().retained_bundle_id !=
            input.motion_binding.value().retained_bundle_id ||
        input.gripper_binding->value().graph_revision !=
            input.motion_binding.value().graph_revision)) ||
      !retained_bindings_preserve_physical_owner_after_product_reseal(
          runtime.motion_binding, input.motion_binding) ||
      input.gripper_binding.has_value() !=
          runtime.gripper_binding.has_value() ||
      (input.gripper_binding.has_value() &&
       (runtime.gripper_state_channel == nullptr ||
        runtime.gripper == nullptr || !runtime.rebind_gripper_route ||
        !retained_bindings_preserve_physical_owner_after_product_reseal(
            *runtime.gripper_binding, *input.gripper_binding)))) {
    return fail("native_product_reseal_physical_identity_changed",
                "candidate product changes the retained component's physical controller identity");
  }

  const auto motion_status = runtime.participant_engine->get_status();
  if (motion_participant_unresolved(motion_status)) {
    return fail("native_product_reseal_motion_not_idle",
                "retained native controller has active or unresolved motion work");
  }
  if (input.gripper_binding.has_value()) {
    const auto gripper_status = runtime.gripper->get_gripper_status(
        input.gripper_binding->value().command_target_id.value());
    if (gripper_status.gripper_id !=
            input.gripper_binding->value().command_target_id.value() ||
        gripper_status.state == core::GripperCommandState::kExecuting) {
      return fail("native_product_reseal_gripper_not_idle",
                  "retained native gripper has active or unresolved command work");
    }
  }

  std::vector<core::RuntimeParticipantBinding> expected_bindings;
  expected_bindings.reserve(input.control_binding_representatives.size() +
                            input.motion_binding_aliases.size() +
                            input.gripper_binding_aliases.size());
  expected_bindings.insert(expected_bindings.end(),
                           input.control_binding_representatives.begin(),
                           input.control_binding_representatives.end());
  expected_bindings.insert(expected_bindings.end(),
                           input.motion_binding_aliases.begin(),
                           input.motion_binding_aliases.end());
  expected_bindings.insert(expected_bindings.end(),
                           input.gripper_binding_aliases.begin(),
                           input.gripper_binding_aliases.end());
  if (expected_bindings.empty() ||
      input.participant_registrations.size() != expected_bindings.size()) {
    return fail("native_product_reseal_registration_set_incomplete",
                "candidate registrations do not exactly cover its control bindings");
  }
  std::set<core::ControlBindingId> expected_ids;
  for (const auto &binding : expected_bindings) {
    if (!binding.value().control_path.has_value() ||
        !expected_ids
             .insert(binding.value().control_path->control_binding_id)
             .second) {
      return fail("native_product_reseal_binding_set_ambiguous",
                  "candidate component repeats or omits a control binding identity");
    }
  }
  std::set<core::ControlBindingId> registration_ids;
  for (const auto &candidate : input.participant_registrations) {
    const auto &binding = candidate.binding;
    if (!binding.value().control_path.has_value() ||
        !registration_ids
             .insert(binding.value().control_path->control_binding_id)
             .second ||
        std::none_of(expected_bindings.begin(), expected_bindings.end(),
                     [&](const auto &expected) {
                       return expected.value() == binding.value();
                     })) {
      return fail("native_product_reseal_registration_set_mismatch",
                  "candidate registration does not exactly match one unique component binding");
    }
    const auto predecessor = std::find_if(
        runtime.participant_registrations.begin(),
        runtime.participant_registrations.end(), [&](const auto &prior) {
          return prior.binding.value().control_path.has_value() &&
                 prior.binding.value().control_path->control_binding_id ==
                     binding.value().control_path->control_binding_id &&
                 prior.binding.value().controllable_component_id ==
                     binding.value().controllable_component_id;
        });
    if (predecessor == runtime.participant_registrations.end() ||
        std::count_if(runtime.participant_registrations.begin(),
                      runtime.participant_registrations.end(),
                      [&](const auto &prior) {
                        return prior.binding.value().control_path.has_value() &&
                               prior.binding.value().control_path->control_binding_id ==
                                   binding.value().control_path->control_binding_id &&
                               prior.binding.value().controllable_component_id ==
                                   binding.value().controllable_component_id;
                      }) != 1 ||
        !same_registration_after_product_reseal(*predecessor, candidate)) {
      return fail("native_product_reseal_registration_identity_changed",
                  "candidate registration changes a retained engine, route, policy, or Stop fact");
    }
    const bool motion_owner =
        retained_bindings_share_backend_owner(input.motion_binding, binding);
    const bool gripper_owner = input.gripper_binding.has_value() &&
        retained_bindings_share_backend_owner(*input.gripper_binding, binding);
    if (motion_owner == gripper_owner ||
        candidate.engine != (motion_owner
                                 ? runtime.participant_engine_for(true)
                                 : runtime.participant_engine_for(false))) {
      return fail("native_product_reseal_registration_owner_mismatch",
                  "candidate binding does not resolve to exactly one retained engine");
    }
  }
  if (registration_ids != expected_ids) {
    return fail("native_product_reseal_registration_set_mismatch",
                "candidate component registrations omit a control binding");
  }
  if (std::any_of(input.motion_binding_aliases.begin(),
                  input.motion_binding_aliases.end(), [&](const auto &alias) {
                    return !same_physical_binding(input.motion_binding, alias,
                                                  true);
                  }) ||
      std::any_of(input.gripper_binding_aliases.begin(),
                  input.gripper_binding_aliases.end(), [&](const auto &alias) {
                    return !input.gripper_binding.has_value() ||
                           !same_physical_binding(*input.gripper_binding, alias,
                                                  true);
                  })) {
    return fail("native_product_reseal_alias_identity_changed",
                "candidate Program alias widens or changes the retained physical owner");
  }

  const auto old_motion = runtime.motion_binding;
  const auto old_gripper = runtime.gripper_binding;
  const auto old_motion_aliases = runtime.motion_binding_aliases;
  const auto transition_time_ns = runtime.clock->now();
  bool motion_channel_attempted = false;
  bool motion_backend_attempted = false;
  bool gripper_channel_attempted = false;
  bool gripper_backend_attempted = false;
  const auto rollback = [&]() noexcept {
    bool complete = true;
    try {
      if (gripper_backend_attempted && old_gripper.has_value() &&
          runtime.rebind_gripper_route) {
        if (gripper_channel_attempted &&
            runtime.gripper_state_channel != nullptr) {
          const bool channel_restored =
              runtime.gripper_state_channel
                  ->RebindRoute(*old_gripper, runtime.clock->now())
                  .ok();
          complete = channel_restored && complete;
          if (channel_restored) {
            complete = runtime.rebind_gripper_route(
                           *old_gripper, runtime.gripper_state_channel)
                           .ok() &&
                       complete;
          }
        } else {
          complete = false;
        }
      } else if (gripper_channel_attempted && old_gripper.has_value() &&
                 runtime.gripper_state_channel != nullptr) {
        complete = runtime.gripper_state_channel
                       ->RebindRoute(*old_gripper, runtime.clock->now())
                       .ok() &&
                   complete;
      }
      if (motion_channel_attempted && runtime.motion_state_channel != nullptr) {
        const bool channel_restored =
            runtime.motion_state_channel
                ->RebindRoute(old_motion, runtime.clock->now())
                .ok();
        complete = channel_restored && complete;
        if (motion_backend_attempted && runtime.rebind_motion_route &&
            channel_restored) {
          complete = runtime.rebind_motion_route(old_motion,
                                                 old_motion_aliases)
                         .ok() &&
                     complete;
        } else if (motion_backend_attempted) {
          complete = false;
        }
      } else if (motion_backend_attempted) {
        complete = false;
      }
    } catch (...) {
      complete = false;
    }
    return complete;
  };

  motion_channel_attempted = true;
  auto rebound = runtime.motion_state_channel->RebindRoute(
      input.motion_binding, transition_time_ns);
  if (!rebound.ok()) {
    if (!rollback()) {
      return fail("native_product_reseal_rollback_failed",
                  "motion state rebind failed and its predecessor could not be restored");
    }
    return core::Result<void>::Err(rebound.error());
  }
  if (input.gripper_binding.has_value()) {
    gripper_channel_attempted = true;
    rebound = runtime.gripper_state_channel->RebindRoute(
        *input.gripper_binding, transition_time_ns);
    if (!rebound.ok()) {
      if (!rollback()) {
        return fail("native_product_reseal_rollback_failed",
                    "gripper state rebind failed and its predecessor could not be restored");
      }
      return core::Result<void>::Err(rebound.error());
    }
  }
  auto validated_surveillance = RetainedStopSurveillanceOwner::ValidateSources(
      stop_surveillance_sources(
          input.motion_binding,
          readiness_for(input.participant_registrations,
                        input.motion_binding),
          runtime.motion_state_channel, input.gripper_binding,
          input.gripper_binding.has_value()
              ? readiness_for(input.participant_registrations,
                              *input.gripper_binding)
              : std::nullopt,
          runtime.gripper_state_channel, runtime.participant_engine.get(),
          runtime.gripper_participant_engine.get(),
          input.participant_registrations));
  if (!validated_surveillance.ok()) {
    if (!rollback()) {
      return fail("native_product_reseal_rollback_failed",
                  "candidate Stop sources were invalid and predecessor routes could not be restored");
    }
    return fail("native_product_reseal_surveillance_invalid",
                validated_surveillance.error().message);
  }
  motion_backend_attempted = true;
  rebound = runtime.rebind_motion_route(input.motion_binding,
                                       input.motion_binding_aliases);
  if (!rebound.ok()) {
    if (!rollback()) {
      return fail("native_product_reseal_rollback_failed",
                  "motion backend rebind failed and its predecessor could not be restored");
    }
    return core::Result<void>::Err(rebound.error());
  }
  if (input.gripper_binding.has_value()) {
    gripper_backend_attempted = true;
    rebound = runtime.rebind_gripper_route(
        *input.gripper_binding, runtime.gripper_state_channel);
    if (!rebound.ok()) {
      if (!rollback()) {
        return fail("native_product_reseal_rollback_failed",
                    "gripper backend rebind failed and its predecessor could not be restored");
      }
      return core::Result<void>::Err(rebound.error());
    }
  }

  runtime.motion_binding = std::move(input.motion_binding);
  runtime.gripper_binding = std::move(input.gripper_binding);
  runtime.motion_binding_aliases = std::move(input.motion_binding_aliases);
  runtime.gripper_binding_aliases = std::move(input.gripper_binding_aliases);
  runtime.control_binding_representatives =
      std::move(input.control_binding_representatives);
  runtime.participant_registrations = std::move(input.participant_registrations);
  runtime.reference_resolver = std::move(input.reference_resolver);
  runtime.reference_resolver_proxy->ReplaceTarget(runtime.reference_resolver);
  if (const auto primary = readiness_for(runtime.participant_registrations,
                                         runtime.motion_binding);
      primary.has_value()) {
    const auto registration = std::find_if(
        runtime.participant_registrations.begin(),
        runtime.participant_registrations.end(), [&](const auto &row) {
          return row.binding.value() == runtime.motion_binding.value();
        });
    if (registration != runtime.participant_registrations.end()) {
      runtime.motion_participant_policy = registration->participant_policy;
      runtime.deployment_maximum_tcp_linear_speed_mps =
          registration->deployment_maximum_tcp_linear_speed_mps;
      runtime.direct_control_joint_limits =
          registration->direct_control_joint_limits;
    }
  }
  return core::Result<void>::Ok();
}

core::Result<void> refresh_retained_native_runtime_model_for_apply(
    RetainedNativeRuntime &runtime,
    RetainedNativeRuntimeModelRefreshInput input) {
  const auto fail = [](std::string reason, std::string message) {
    return route_publication_error<void>(std::move(reason),
                                         std::move(message));
  };
  if (input.apply_fence_id.empty() || !input.apply_scope.valid() ||
      !input.admission_gate.exact_fence_is_held(input.apply_fence_id,
                                                input.apply_scope)) {
    return fail("native_model_refresh_fence_not_held",
                "retained model refresh requires the exact held Apply fence");
  }
  // Route readers and route publication serialize on the cell root; model
  // consumers are replaced under the same order as the re-seal.
  auto *const cell_root = runtime.cell_runtime_root != nullptr
                              ? runtime.cell_runtime_root
                              : &runtime;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  if (!input.admission_gate.exact_fence_is_held(input.apply_fence_id,
                                                input.apply_scope)) {
    return fail("native_model_refresh_fence_changed",
                "retained model refresh lost its exact Apply fence");
  }
  const auto &candidate = input.candidate;
  if (candidate == nullptr || candidate->scene == nullptr ||
      candidate->state_projector == nullptr ||
      candidate->kinematics == nullptr ||
      candidate->trajectory_validator == nullptr ||
      candidate->collision_trajectory_projector == nullptr ||
      runtime.kinematic_model == nullptr || runtime.clock == nullptr ||
      runtime.motion_state_channel == nullptr ||
      runtime.participant_engine == nullptr ||
      runtime.gripper_binding.has_value() !=
          (runtime.gripper_state_channel != nullptr)) {
    return fail("native_model_refresh_incomplete",
                "retained model refresh has no complete candidate generation");
  }

  // Drain: no planning, validation, execution or gripper command may span
  // the generation change. Admission is already closed by the fence.
  const auto motion_status = runtime.participant_engine->get_status();
  if (motion_participant_unresolved(motion_status)) {
    return fail("native_model_refresh_motion_not_idle",
                "retained controller has active or unresolved motion work");
  }
  if (runtime.gripper_binding.has_value() && runtime.gripper != nullptr) {
    const auto gripper_status = runtime.gripper->get_gripper_status(
        runtime.gripper_binding->value().command_target_id.value());
    if (gripper_status.state == core::GripperCommandState::kExecuting) {
      return fail("native_model_refresh_gripper_not_idle",
                  "retained gripper has active command work");
    }
  }

  // Retaining the controller is valid only while its own model facts are
  // unchanged; only their placement in the whole cell may differ.
  const auto &motion = runtime.motion_binding.value();
  const auto &component = motion.controllable_component_id.value();
  const auto &target = motion.command_target_id.value();
  const auto current_limits =
      runtime.kinematic_model->joint_limits(component, target);
  const auto candidate_limits =
      candidate->kinematics->joint_limits(component, target);
  const auto current_names = runtime.kinematic_model->group_joint_names(target);
  const auto candidate_names = candidate->kinematics->group_joint_names(target);
  if (!current_limits.ok() || !candidate_limits.ok() || !current_names.ok() ||
      !candidate_names.ok() ||
      current_names.value() != candidate_names.value() ||
      current_limits.value().joint_names !=
          candidate_limits.value().joint_names ||
      current_limits.value().position_lower_rad !=
          candidate_limits.value().position_lower_rad ||
      current_limits.value().position_upper_rad !=
          candidate_limits.value().position_upper_rad ||
      current_limits.value().velocity_max_rad_s !=
          candidate_limits.value().velocity_max_rad_s ||
      current_limits.value().acceleration_max_rad_s2 !=
          candidate_limits.value().acceleration_max_rad_s2) {
    return fail("native_model_refresh_controller_model_changed",
                "candidate cell model changes the retained controller's own "
                "kinematic target");
  }
  const bool gripper_model_backed =
      runtime.gripper_state_channel != nullptr &&
      runtime.gripper_state_channel->kinematic_projector() != nullptr;
  if (gripper_model_backed) {
    std::optional<RetainedGripperPositionLimits> candidate_gripper_limits;
    try {
      candidate_gripper_limits =
          retained_gripper_limits(*candidate->scene, *runtime.gripper_binding);
    } catch (const std::exception &) {
    }
    if (!runtime.gripper_position_limits.has_value() ||
        !candidate_gripper_limits.has_value() ||
        candidate_gripper_limits->coordinate_kind !=
            runtime.gripper_position_limits->coordinate_kind ||
        candidate_gripper_limits->lower_position !=
            runtime.gripper_position_limits->lower_position ||
        candidate_gripper_limits->upper_position !=
            runtime.gripper_position_limits->upper_position) {
      return fail("native_model_refresh_controller_model_changed",
                  "candidate cell model changes the retained gripper limits");
    }
  }

  // Install. The proxy waits for in-flight host/Kernel model calls; each
  // channel waits for its in-flight projection, then projects its latest
  // owner sample into the candidate scene.
  runtime.kinematic_model->ReplaceGeneration(candidate);
  const auto now_ns = runtime.clock->now();
  if (auto replaced = runtime.motion_state_channel->ReplaceKinematicProjector(
          candidate->state_projector, now_ns);
      !replaced.ok()) {
    return core::Result<void>::Err(replaced.error());
  }
  if (gripper_model_backed) {
    if (auto replaced =
            runtime.gripper_state_channel->ReplaceKinematicProjector(
                candidate->state_projector, now_ns);
        !replaced.ok()) {
      return core::Result<void>::Err(replaced.error());
    }
  }
  if (runtime.kinematic_model->current_generation() != candidate ||
      runtime.motion_state_channel->kinematic_projector() !=
          candidate->state_projector ||
      (gripper_model_backed &&
       runtime.gripper_state_channel->kinematic_projector() !=
           candidate->state_projector)) {
    return fail("native_model_refresh_readback_mismatch",
                "retained model consumers do not read back one candidate "
                "generation");
  }
  return core::Result<void>::Ok();
}

namespace {

[[nodiscard]] bool runtime_owns_domain(
    const RetainedNativeRuntime &runtime,
    const core::ControlDomainId &domain_id) {
  return std::any_of(
      runtime.control_binding_representatives.begin(),
      runtime.control_binding_representatives.end(), [&](const auto &binding) {
        return binding.value().control_path.has_value() &&
               binding.value().control_path->control_domain_id == domain_id;
      });
}

template <typename Visitor>
void for_each_cell_runtime(RetainedNativeRuntime &runtime, Visitor visitor) {
  auto *root = runtime.cell_runtime_root != nullptr ? runtime.cell_runtime_root
                                                    : &runtime;
  visitor(*root);
  for (const auto &peer : root->participant_peers) visitor(*peer);
}

[[nodiscard]] std::vector<core::RuntimeParticipantBinding> composed_bindings(
    const std::vector<core::RuntimeParticipantExecutionRegistration>& rows) {
  std::vector<core::RuntimeParticipantBinding> result;
  result.reserve(rows.size());
  for (const auto& row : rows) result.push_back(row.binding);
  return result;
}

[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
physical_registration_cache(
    std::vector<core::RuntimeParticipantExecutionRegistration> rows) {
  std::erase_if(rows, [](const auto& row) {
    return row.binding.value().io_output_binding.has_value();
  });
  return rows;
}

void append_io_output_registrations(
    std::vector<core::RuntimeParticipantExecutionRegistration>& owners,
    const std::vector<core::RuntimeParticipantExecutionRegistration>& rows) {
  for (const auto& row : rows)
    if (row.binding.value().io_output_binding) owners.push_back(row);
}

struct CellParticipantSet final {
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
};

[[nodiscard]] CellParticipantSet compose_cell_participants(
    RetainedNativeRuntime &changed,
    const std::vector<core::RuntimeParticipantExecutionRegistration> &changed_registrations,
    const core::RuntimeParticipantBinding &changed_motion,
    const std::optional<core::RuntimeParticipantBinding> &changed_gripper) {
  CellParticipantSet result;
  for_each_cell_runtime(changed, [&](RetainedNativeRuntime &runtime) {
    if (&runtime == &changed) {
      result.registrations.insert(result.registrations.end(),
                                  changed_registrations.begin(),
                                  changed_registrations.end());
      result.physical_bindings.push_back(changed_motion);
      if (changed_gripper) result.physical_bindings.push_back(*changed_gripper);
      return;
    }
    for (const auto &registration : runtime.participant_registrations) {
      const auto &binding = registration.binding.value();
      if (binding.control_path &&
          runtime_owns_domain(runtime, binding.control_path->control_domain_id))
        result.registrations.push_back(registration);
    }
    result.physical_bindings.push_back(runtime.motion_binding);
    if (runtime.gripper_binding)
      result.physical_bindings.push_back(*runtime.gripper_binding);
  });
  auto *const root = changed.cell_runtime_root != nullptr
                         ? changed.cell_runtime_root
                         : &changed;
  for (const auto &gripper : root->independent_gripper_peers) {
    if (gripper == nullptr)
      throw std::logic_error("cell route has a missing independent gripper");
    const auto aliases = gripper->registrations();
    result.registrations.insert(result.registrations.end(), aliases.begin(), aliases.end());
    result.physical_bindings.push_back(gripper->binding());
  }
  // Provider outputs belong to the stable cell, not a subordinate arm cache.
  // Route adoption replaces physical rows only; retain these exact installed
  // rows, including their actual engine, occurrence and lifecycle generation.
  if (root->cell_runtime_owner) {
    for (const auto& row : root->cell_runtime_owner->participant_registrations())
      if (row.binding.value().io_output_binding)
        result.registrations.push_back(row);
  }
  return result;
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
compose_cell_surveillance_sources(
    RetainedNativeRuntime &changed,
    const core::RuntimeParticipantBinding &changed_motion,
    const std::optional<core::RuntimeParticipantStopReadiness> &changed_motion_readiness,
    const std::optional<core::RuntimeParticipantBinding> &changed_gripper,
    const std::optional<core::RuntimeParticipantStopReadiness> &changed_gripper_readiness,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &changed_registrations) {
  std::vector<RetainedStopSurveillanceSourceInput> result;
  for_each_cell_runtime(changed, [&](RetainedNativeRuntime &runtime) {
    const bool selected = &runtime == &changed;
    const auto &motion = selected ? changed_motion : runtime.motion_binding;
    const auto &gripper = selected ? changed_gripper : runtime.gripper_binding;
    const auto motion_readiness =
        selected ? changed_motion_readiness
                 : readiness_for(runtime.participant_registrations, motion);
    const auto gripper_readiness =
        selected ? changed_gripper_readiness
                 : (gripper ? readiness_for(runtime.participant_registrations, *gripper)
                            : std::nullopt);
    auto rows = stop_surveillance_sources(
        motion, motion_readiness, runtime.motion_state_channel, gripper,
        gripper_readiness, runtime.gripper_state_channel,
        runtime.participant_engine.get(), runtime.gripper_participant_engine.get(),
        selected ? changed_registrations : runtime.participant_registrations);
    result.insert(result.end(), std::make_move_iterator(rows.begin()),
                  std::make_move_iterator(rows.end()));
  });
  auto *const root = changed.cell_runtime_root != nullptr
                         ? changed.cell_runtime_root
                         : &changed;
  for (const auto &gripper : root->independent_gripper_peers) {
    if (gripper == nullptr)
      throw std::logic_error("cell Stop has a missing independent gripper");
    auto source = build_retained_gripper_stop_surveillance_source(
        gripper->binding(), gripper->stop_readiness(),
        gripper->state_channel(), gripper->participant_engine());
    if (source.has_value()) result.push_back(std::move(*source));
  }
  return result;
}

} // namespace

core::IResolvedParticipantExecutionEngine *
RetainedNativeRuntime::participant_engine_for(const bool motion) const {
  if (motion)
    return participant_engine.get();
  return gripper_participant_engine.get();
}

void RetainedNativeRuntime::BindCellRuntimeOwner(
    RetainedNativeCellRuntime& cell, const std::string& /*runtime_epoch*/,
    const std::string& cell_id) {
  auto& runtime = *this;
  auto* const stable_kernel_owner = &cell.addressed_kernel();
  auto* const stable_operation_journal = &cell.operation_journal();
  runtime.cell_addressed_kernel = stable_kernel_owner;
  runtime.cell_operation_journal = stable_operation_journal;
  runtime.addressed_request_owner_epoch =
      stable_kernel_owner->request_owner_epoch();
  runtime.addressed_cell_id = core::CellId(cell_id);
  const auto seed_current_policy = [&](RetainedNativeRuntime &component) {
    for (auto &row : component.participant_registrations) {
      row.motion_takeover_preparation_budget_s =
          current_cell_takeover_budget(&cell, row.binding);
      row.motion_takeover_planning_budget_s = current_cell_takeover_budget(
          &cell, row.binding,
          &core::RuntimeParticipantExecutionRegistration::
              motion_takeover_planning_budget_s);
    }
  };
  seed_current_policy(runtime);
  for (const auto &peer : runtime.participant_peers) {
    peer->cell_addressed_kernel = stable_kernel_owner;
    peer->cell_operation_journal = stable_operation_journal;
    peer->addressed_request_owner_epoch =
        runtime.addressed_request_owner_epoch;
    peer->addressed_cell_id = runtime.addressed_cell_id;
    seed_current_policy(*peer);
  }
}

void RetainedNativeRuntime::AdoptVerifiedParticipantResolverFactory(
    RetainedRuntimeParticipantResolverFactory &&factory) {
  auto *root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(root->route_publication_mutex_);
  root->participant_resolver_factory.swap(factory);
}

void RetainedNativeRuntime::BindIndependentGripperPeers(
    const std::shared_ptr<RetainedNativeRuntime>& root,
    std::vector<std::shared_ptr<RetainedNativeGripperRuntime>> peers) {
  if (root.get() != this)
    throw std::invalid_argument("scalar topology must bind its exact retained root");
  std::lock_guard lock(route_publication_mutex_);
  independent_gripper_peers.swap(peers);
  installed_self_ = root;
}

core::Result<std::shared_ptr<RetainedNativeGripperRoutePublicationOwner>>
RetainedNativeRuntime::CurrentIndependentGripperRouteOwner(const core::ControlDomainId& domain) const {
  using Result = core::Result<std::shared_ptr<RetainedNativeGripperRoutePublicationOwner>>;
  std::lock_guard lock(route_publication_mutex_);
  std::shared_ptr<RetainedNativeGripperRuntime> selected;
  for (const auto& gripper : independent_gripper_peers) {
    if (!gripper) return Result::Err({"missing_reference", "installed scalar peer is absent", {}});
    const auto binding = gripper->binding();
    if (!binding.value().control_path || binding.value().control_path->control_domain_id != domain) continue;
    if (selected) return Result::Err({"invalid_request", "scalar domain has multiple actual owners", {}});
    selected = gripper;
  }
  if (!selected) return Result::Ok(nullptr);
  bool arm_owns_domain = false;
  const auto note_arm_domains = [&](const RetainedNativeRuntime& runtime) {
    for (const auto& binding : runtime.control_binding_representatives)
      if (binding.value().control_path && binding.value().control_path->control_domain_id == domain)
        arm_owns_domain = true;
  };
  note_arm_domains(*this);
  for (const auto& peer : participant_peers) note_arm_domains(*peer);
  if (arm_owns_domain) return Result::Err(
      {"invalid_request", "scalar domain aliases an existing arm-component owner", {}});
  auto root = installed_self_.lock();
  if (!root || root.get() != this || !root->cell_runtime_owner || !root->clock ||
      root->active_addressed_kernel() != &root->cell_runtime_owner->addressed_kernel())
    return Result::Err({"missing_reference", "scalar route lost its exact installed root", {}});
  const std::weak_ptr<RetainedNativeRuntime> weak_root = root;
  auto candidate = [weak_root, selected](const auto&, const auto& replacement)
      -> core::Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate> {
    using CandidateResult = core::Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>;
    // The route owner has reacquired this same mutex and verified the selected
    // installed owner before any physical rebind. Re-read current data here;
    // no captured predecessor bundle/model/factory survives an Apply.
    auto current = weak_root.lock();
    if (!current || !current->cell_runtime_owner || !current->participant_resolver_factory ||
        std::count(current->independent_gripper_peers.begin(), current->independent_gripper_peers.end(), selected) != 1)
      return CandidateResult::Err({"missing_reference", "scalar candidate lost its installed owner or factory", {}});
    auto rows = current->cell_runtime_owner->participant_registrations();
    if (replacement.size() != selected->registrations().size())
      return CandidateResult::Err({"missing_reference", "scalar candidate omits installed aliases", {}});
    for (const auto& next : replacement) {
      const auto old = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.engine == selected->participant_engine() &&
               row.binding.value().controllable_component_id == next.binding.value().controllable_component_id &&
               row.binding.value().command_target_id == next.binding.value().command_target_id &&
               row.binding.value().control_path && next.binding.value().control_path &&
               row.binding.value().control_path->control_binding_id == next.binding.value().control_path->control_binding_id &&
               row.binding.value().capability_id == next.binding.value().capability_id;
      });
      if (old == rows.end()) return CandidateResult::Err(
          {"missing_reference", "scalar candidate has no actual installed registration", {}});
      *old = next;
    }
    const auto primary = std::find_if(replacement.begin(), replacement.end(), [&](const auto& row) {
      return row.binding.value().capability_id == selected->binding().value().capability_id;
    });
    if (primary == replacement.end()) return CandidateResult::Err(
        {"missing_reference", "scalar candidate has no primary command registration", {}});
    auto binding_resolver = current->participant_resolver_factory(composed_bindings(rows));
    if (!binding_resolver) return CandidateResult::Err(
        {"missing_reference", "current scalar participant resolver factory returned no resolver", {}});
    auto resolver = make_retained_cell_kernel_participant_resolver(std::move(binding_resolver), rows);
    auto cache = physical_registration_cache(rows);
    auto cached_resolver = resolver;
    std::function<void()> adopt = [current = std::move(current), cache = std::move(cache),
        cached_resolver = std::move(cached_resolver)]() mutable noexcept {
      current->participant_registrations.swap(cache);
      current->participant_resolver.swap(cached_resolver);
    };
    return CandidateResult::Ok({std::move(rows), std::move(resolver), std::move(adopt)});
  };
  auto route = std::make_shared<RetainedNativeGripperRoutePublicationOwner>(
      selected, *root->clock, *root->active_addressed_kernel(), std::move(candidate), root->cell_runtime_owner);
  route->BindMotionRoot(root);
  return Result::Ok(std::move(route));
}

core::Result<std::vector<core::RuntimeParticipantBinding>>
RetainedNativeRuntime::ReadCurrentPhysicalParticipants(
    const core::ActiveControlConfigurationDTO &configuration) const {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled) {
    return route_publication_error<
        std::vector<core::RuntimeParticipantBinding>>(
        "route_publication_configuration_invalid",
        "native route publication requested an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  const auto* root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  auto scalar = root->CurrentIndependentGripperRouteOwner(configuration.control_domain_id);
  if (!scalar.ok()) return core::Result<std::vector<core::RuntimeParticipantBinding>>::Err(scalar.error());
  if (scalar.value()) return scalar.value()->ReadCurrentPhysicalParticipants(configuration);

  const auto owns_domain = [&](const auto &runtime) {
    return std::any_of(
        runtime.control_binding_representatives.begin(),
        runtime.control_binding_representatives.end(), [&](const auto &binding) {
          return binding.value().control_path.has_value() &&
                 binding.value().control_path->control_domain_id ==
                     configuration.control_domain_id;
        });
  };
  if (!owns_domain(*this)) {
    const auto peer = std::find_if(
        participant_peers.begin(), participant_peers.end(),
        [&](const auto &candidate) { return owns_domain(*candidate); });
    if (peer != participant_peers.end())
      return (*peer)->ReadCurrentPhysicalParticipants(configuration);
  }
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root
                                                       : const_cast<RetainedNativeRuntime *>(this);
  std::lock_guard lock(cell_root->route_publication_mutex_);
  std::vector<core::RuntimeParticipantBinding> result;
  result.reserve(configuration.binding_routes.size());
  for (const auto &selected : configuration.binding_routes) {
    const core::RuntimeParticipantBinding *physical = nullptr;
    for (const auto &candidate : control_binding_representatives) {
      if (!candidate.value().control_path.has_value()) {
        continue;
      }
      const auto &path = *candidate.value().control_path;
      if (path.control_domain_id != configuration.control_domain_id ||
          path.control_binding_id != selected.control_binding_id ||
          path.control_binding_revision != selected.control_binding_revision) {
        continue;
      }
      if (physical != nullptr) {
        return route_publication_error<
            std::vector<core::RuntimeParticipantBinding>>(
            "route_publication_physical_binding_ambiguous",
            "native runtime has multiple physical owners for one binding");
      }
      physical = &candidate;
    }
    if (physical == nullptr) {
      return route_publication_error<
          std::vector<core::RuntimeParticipantBinding>>(
          "route_publication_physical_binding_missing",
          "native runtime omits a selected physical binding",
          core::ErrorCode::kMissingReference);
    }
    result.push_back(*physical);
  }
  return core::Result<std::vector<core::RuntimeParticipantBinding>>::Ok(
      std::move(result));
}

core::Result<std::optional<core::ActiveControlConfigurationDTO>>
RetainedNativeRuntime::ReadCurrentPublishedRouteConfiguration(
    const core::ControlDomainId &control_domain_id) const {
  auto *const kernel = addressed_kernel != nullptr ? addressed_kernel.get()
                                                    : cell_addressed_kernel;
  if (kernel == nullptr) {
    return route_publication_error<
        std::optional<core::ActiveControlConfigurationDTO>>(
        "route_publication_kernel_missing",
        "native route current-fact read has no stable Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  const auto readback =
      kernel->current_runtime_route_configuration(control_domain_id);
  if (!readback.ok()) {
    return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::
        Err(readback.error());
  }
  if (!readback.value().has_value()) {
    return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
        std::nullopt);
  }
  return core::Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
      readback.value()->active_configuration);
}

namespace {

template <typename Value>
core::Result<Value> measured_source_unavailable(std::string reason,
                                                std::string message) {
  return core::Result<Value>::Err({"runtime_measured_source_unavailable",
                                   std::move(message),
                                   {{"reason", std::move(reason)}}});
}

// Caller holds the existing cell route-publication exclusion. Include every
// registered domain, not just the operation's selected motion participant.
using MeasuredRouteReadbacks = std::vector<
    std::pair<core::ControlDomainId,
              std::optional<core::RuntimeRouteConfigurationReadbackDTO>>>;

core::Result<MeasuredRouteReadbacks>
measured_route_readbacks(const RetainedNativeRuntime &runtime) {
  using Result = core::Result<MeasuredRouteReadbacks>;
  const auto *root = runtime.cell_runtime_root != nullptr
                         ? runtime.cell_runtime_root
                         : &runtime;
  auto *kernel = root->active_addressed_kernel();
  if (kernel == nullptr)
    return measured_source_unavailable<MeasuredRouteReadbacks>(
        "measured_kernel_missing", "Measured routes have no current Kernel");
  std::set<core::ControlDomainId> domains;
  const auto add = [&](const auto &binding) {
    if (binding.value().control_path)
      domains.insert(binding.value().control_path->control_domain_id);
  };
  for (const auto &row : root->participant_registrations)
    add(row.binding);
  for (const auto &row : runtime.control_binding_representatives)
    add(row);
  add(runtime.motion_binding);
  if (runtime.gripper_binding)
    add(*runtime.gripper_binding);
  MeasuredRouteReadbacks result;
  result.reserve(domains.size());
  for (const auto &domain : domains) {
    auto current = kernel->current_runtime_route_configuration(domain);
    if (!current.ok())
      return Result::Err(current.error());
    // Missing route truth remains part of the authenticated closure. It does
    // not make an unrelated component's existing route publication fail.
    result.emplace_back(domain, std::move(current.value()));
  }
  return Result::Ok(std::move(result));
}

std::vector<core::ActiveControlConfigurationDTO>
measured_configurations(const MeasuredRouteReadbacks &readbacks) {
  std::vector<core::ActiveControlConfigurationDTO> result;
  result.reserve(readbacks.size());
  for (const auto &[domain, row] : readbacks)
    if (row)
      result.push_back(row->active_configuration);
  return result;
}

bool measured_physical_scope_matches(
    const RetainedNativeRuntime &runtime,
    const core::RuntimeParticipantBinding &motion,
    const std::optional<core::RuntimeParticipantBinding> &gripper,
    const std::vector<core::RuntimeParticipantBinding> &representatives) {
  if (runtime.motion_binding.value() != motion.value() ||
      runtime.gripper_binding.has_value() != gripper.has_value() ||
      (gripper && runtime.gripper_binding->value() != gripper->value()) ||
      runtime.control_binding_representatives.size() != representatives.size())
    return false;
  for (std::size_t i = 0; i < representatives.size(); ++i)
    if (runtime.control_binding_representatives[i].value() !=
        representatives[i].value())
      return false;
  return true;
}

core::Result<void> validate_prepared_measured_sources(
    const RetainedPreparedRobotMeasuredSources &prepared,
    const core::RuntimeParticipantBinding &motion,
    const std::optional<core::RuntimeParticipantBinding> &gripper,
    const std::vector<core::ActiveControlConfigurationDTO> &configurations) {
  std::vector<core::RuntimeParticipantBinding> physical{motion};
  if (gripper) physical.push_back(*gripper);
  return ValidateRetainedPreparedMeasuredSources(
      prepared, motion.value().controllable_component_id, physical, configurations);
}

void enable_prepared_measured_sources(
    const RetainedPreparedRobotMeasuredSources &prepared) noexcept {
  // Composition supplies the actual no-throw gate. A throwing callback violates
  // that construction contract and cannot become a partial association commit.
  prepared.enable_publication();
}

} // namespace

core::Result<void> ValidateRetainedPreparedMeasuredSources(
    const RetainedPreparedRobotMeasuredSources &prepared,
    const core::ControllableComponentId &component,
    std::span<const core::RuntimeParticipantBinding> physical_bindings,
    const std::vector<core::ActiveControlConfigurationDTO> &configurations) {
  const auto unavailable = [] {
    return measured_source_unavailable<void>(
        "measured_prepared_scope_mismatch",
        "Prepared sources cross their physical route");
  };
  if (!prepared.enable_publication ||
      prepared.source_view.component_id !=
          component)
    return unavailable();
  for (const auto &source : prepared.source_view.sources) {
    auto valid = source.descriptor.validate();
    if (!valid.ok())
      return valid;
    const auto &query = source.descriptor.publication.query_context;
    const auto matches = [&](const core::RuntimeParticipantBinding &binding) {
      const auto &value = binding.value();
      if (!value.control_path)
        return false;
      const auto &path = *value.control_path;
      if (query.controllable_component_id != value.controllable_component_id ||
          query.command_target_id != value.command_target_id ||
          query.capability_id != value.capability_id ||
          query.control_domain_id != path.control_domain_id ||
          query.control_domain_revision != path.control_domain_revision ||
          query.control_binding_id != path.control_binding_id ||
          query.control_binding_revision != path.control_binding_revision ||
          query.controller_topology_id != path.controller_topology_id ||
          query.topology_revision != path.topology_revision ||
          query.control_route_id != path.control_route_id ||
          query.control_interface_contract_id !=
              path.control_interface_contract_id ||
          query.control_interface_revision != path.control_interface_revision ||
          query.active_control_configuration_id !=
              path.active_control_configuration_id ||
          query.active_control_configuration_generation !=
              path.active_control_configuration_generation ||
          query.provider_instance_id != path.provider_instance_id ||
          query.provider_manifest_id != path.provider_manifest_id ||
          query.provider_manifest_revision != path.provider_manifest_revision ||
          query.runtime_epoch != path.provider_runtime_epoch ||
          query.resources.size() != value.resources.size())
        return false;
      for (std::size_t i = 0; i < value.resources.size(); ++i) {
        const auto &resource = value.resources[i];
        if (query.resources[i] != resource.resource_id ||
            query.joint_names[i] != resource.compiled_resource_name ||
            query.coordinate_kinds[i] != resource.coordinate_kind ||
            !resource.source_joint_name ||
            source.descriptor.source_joint_names[i] !=
                *resource.source_joint_name)
          return false;
      }
      return std::any_of(
          configurations.begin(), configurations.end(),
          [&](const auto &configuration) {
            return configuration.control_domain_id == path.control_domain_id &&
                   configuration.control_domain_revision ==
                       path.control_domain_revision &&
                   configuration.active_control_configuration_id ==
                       path.active_control_configuration_id &&
                   configuration.active_control_configuration_generation ==
                       path.active_control_configuration_generation &&
                   configuration.controller_topology_id ==
                       path.controller_topology_id &&
                   configuration.topology_revision == path.topology_revision &&
                   configuration.activation_set_id == path.activation_set_id &&
                   configuration.source_runtime_unit_id ==
                       path.provider_runtime_unit_id &&
                   configuration.source_runtime_epoch ==
                       path.provider_runtime_epoch &&
                   configuration.state ==
                       core::ActiveControlConfigurationStateDTO::kReconciled &&
                   std::any_of(configuration.binding_routes.begin(),
                               configuration.binding_routes.end(),
                               [&](const auto &route) {
                                 return route.control_binding_id ==
                                            path.control_binding_id &&
                                        route.control_binding_revision ==
                                            path.control_binding_revision &&
                                        route.control_route_id ==
                                            path.control_route_id;
                               });
          });
    };
    if (std::none_of(physical_bindings.begin(), physical_bindings.end(), matches))
      return unavailable();
  }
  return core::Result<void>::Ok();
}

core::Result<void> ValidateRetainedMeasuredReadinessSnapshot(
    const core::ControllableComponentId& component,
    std::span<const core::RuntimeParticipantBinding> physical_bindings,
    const core::ComponentLifecycleCurrentSnapshotDTO &snapshot,
    const MeasuredRouteReadbacks &readbacks) {
  const auto unavailable = [] {
    return measured_source_unavailable<void>(
        "measured_snapshot_association_mismatch",
        "Readiness snapshot has no exact current physical route association");
  };
  if (!snapshot.locally_valid() ||
      snapshot.controllable_component_id !=
          component)
    return unavailable();
  const auto &publication = snapshot.active_publication_receipt.request;
  std::set<core::ControlDomainId> domains;
  for (const auto &binding : physical_bindings) {
    const auto &value = binding.value();
    if (value.controllable_component_id != snapshot.controllable_component_id ||
        value.retained_bundle_id != publication.candidate_retained_bundle_id ||
        value.graph_revision != publication.candidate_graph_revision ||
        !value.control_path)
      return unavailable();
    domains.insert(value.control_path->control_domain_id);
  }
  const auto controller_domains = std::count_if(
      snapshot.domains.begin(), snapshot.domains.end(),
      [](const auto &scope) { return scope.provider() == nullptr; });
  if (domains.size() != static_cast<std::size_t>(controller_domains))
    return unavailable();
  for (const auto &scope : snapshot.domains) {
    // Generic provider membership is checked by the host's actual driver
    // capture. This helper owns only physical route/coordinate association.
    if (scope.provider())
      continue;
    if (!domains.contains(scope.control_domain_id))
      return unavailable();
    const auto current =
        std::find_if(readbacks.begin(), readbacks.end(), [&](const auto &row) {
          return row.first == scope.control_domain_id;
        });
    if (current == readbacks.end() || !current->second)
      return unavailable();
    const auto &readback = *current->second;
    if (scope.provider() || !scope.controller().active_control_configuration ||
        *scope.controller().active_control_configuration !=
            readback.active_configuration ||
        scope.control_domain_revision !=
            readback.active_configuration.control_domain_revision ||
        scope.controller().controller_topology_id !=
            readback.active_configuration.controller_topology_id ||
        scope.controller().topology_revision !=
            readback.active_configuration.topology_revision ||
        readback.active_configuration.state !=
            core::ActiveControlConfigurationStateDTO::kReconciled ||
        !readback.operation_admission_open)
      return unavailable();
    std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
    std::vector<core::ResourceId> resources;
    for (const auto &representative : physical_bindings) {
      const auto &value = representative.value();
      const auto &path = *value.control_path;
      if (path.control_domain_id != scope.control_domain_id)
        continue;
      bindings.push_back({path.control_binding_id,
                          path.control_binding_revision,
                          path.control_route_id});
      for (const auto &resource : value.resources)
        resources.push_back(resource.resource_id);
      const auto provider = std::find_if(
          snapshot.provider_occurrences.begin(),
          snapshot.provider_occurrences.end(), [&](const auto &row) {
            return row.control_domain_id == path.control_domain_id &&
                   row.control_domain_revision ==
                       path.control_domain_revision &&
                   row.provider_instance_id == path.provider_instance_id &&
                   row.provider_manifest_id == path.provider_manifest_id &&
                   row.provider_manifest_revision ==
                       path.provider_manifest_revision &&
                   row.provider_runtime_unit_id ==
                       path.provider_runtime_unit_id &&
                   row.provider_runtime_epoch == path.provider_runtime_epoch;
          });
      if (provider == snapshot.provider_occurrences.end())
        return unavailable();
      const auto readiness = std::find_if(
          readback.readiness_rows.begin(), readback.readiness_rows.end(),
          [&](const auto &row) {
            return row.controllable_component_id ==
                       value.controllable_component_id &&
                   row.component_lifecycle_generation ==
                       snapshot.component_lifecycle_generation &&
                   row.control_binding_id == path.control_binding_id &&
                   row.control_binding_revision ==
                       path.control_binding_revision;
          });
      if (readiness == readback.readiness_rows.end())
        return unavailable();
    }
    std::sort(bindings.begin(), bindings.end(),
              [](const auto &a, const auto &b) {
                return a.control_binding_id < b.control_binding_id;
              });
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()),
                    resources.end());
    if (bindings != scope.control_bindings() ||
        resources != scope.physical_resources())
      return unavailable();
  }
  return core::Result<void>::Ok();
}

namespace {
core::Result<void> validate_measured_readiness_snapshot(
    const RetainedNativeRuntime& runtime,
    const core::ComponentLifecycleCurrentSnapshotDTO& snapshot,
    const MeasuredRouteReadbacks& readbacks) {
  return ValidateRetainedMeasuredReadinessSnapshot(runtime.motion_binding.value().controllable_component_id,
      runtime.control_binding_representatives, snapshot, readbacks);
}

} // namespace

core::Result<core::RobotMeasuredStateSourceView>
RetainedNativeRuntime::PrepareCurrentMeasuredSources() {
  using Result = core::Result<core::RobotMeasuredStateSourceView>;
  // Owning holders precede the exclusion so every return/exception releases
  // subscriptions and their shared clocks only after the route lock retires.
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources> previous;
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources> candidate;
  RetainedRobotMeasuredSourcePreparer preparer;
  auto *root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::unique_lock lock(root->route_publication_mutex_);
  if (!prepare_measured_sources)
    return measured_source_unavailable<core::RobotMeasuredStateSourceView>(
        "measured_preparer_missing",
        "The runtime has no measured producer preparer");
  try {
    preparer = prepare_measured_sources;
    previous = prepared_measured_sources_;
    const auto motion = motion_binding;
    const auto gripper = gripper_binding;
    const auto representatives = control_binding_representatives;
    auto readbacks = measured_route_readbacks(*this);
    if (!readbacks.ok())
      return Result::Err(readbacks.error());
    auto configurations = measured_configurations(readbacks.value());
    lock.unlock();
    auto staged = preparer(motion, gripper, configurations);
    if (!staged.ok())
      return Result::Err(staged.error());
    candidate = std::move(staged.value());
    std::optional<core::RobotMeasuredStateSourceView> source_view;
    if (candidate) {
      auto valid = validate_prepared_measured_sources(*candidate, motion,
                                                      gripper, configurations);
      if (!valid.ok())
        return Result::Err(valid.error());
      source_view = candidate->source_view;
    }
    if ((cell_runtime_root != nullptr ? cell_runtime_root : this) != root)
      return measured_source_unavailable<core::RobotMeasuredStateSourceView>(
          "measured_source_association_changed",
          "Measured cell root changed while preparing sources");
    lock.lock();
    auto current = measured_route_readbacks(*this);
    if (!current.ok())
      return Result::Err(current.error());
    if (prepared_measured_sources_ != previous ||
        !measured_physical_scope_matches(*this, motion, gripper,
                                         representatives) ||
        current.value() != readbacks.value())
      return measured_source_unavailable<core::RobotMeasuredStateSourceView>(
          "measured_source_association_changed",
          "Measured route changed while preparing sources");
    prepared_measured_sources_.swap(candidate);
    if (prepared_measured_sources_)
      enable_prepared_measured_sources(*prepared_measured_sources_);
    lock.unlock();
    if (!source_view)
      return measured_source_unavailable<core::RobotMeasuredStateSourceView>(
          "measured_prepared_sources_missing",
          "Current measured sources are unavailable");
    return Result::Ok(std::move(*source_view));
  } catch (const std::exception &exception) {
    return measured_source_unavailable<core::RobotMeasuredStateSourceView>(
        "measured_source_preparation_failed", exception.what());
  } catch (...) {
    return measured_source_unavailable<core::RobotMeasuredStateSourceView>(
        "measured_source_preparation_failed",
        "Measured source preparation failed");
  }
}

core::Result<core::ComponentLifecycleReadinessSourceView>
RetainedNativeRuntime::CaptureReadinessSources(
    const core::ComponentLifecycleCurrentSnapshotDTO &snapshot,
    const std::shared_ptr<RetainedNativeRuntime> &owning_runtime) const {
  using Result = core::Result<core::ComponentLifecycleReadinessSourceView>;
  if (!owning_runtime || owning_runtime.get() != this)
    return measured_source_unavailable<
        core::ComponentLifecycleReadinessSourceView>(
        "measured_runtime_owner_mismatch",
        "Readiness needs this exact owning runtime");
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources> prepared;
  auto *root =
      cell_runtime_root != nullptr ? cell_runtime_root : owning_runtime.get();
  std::lock_guard lock(root->route_publication_mutex_);
  try {
    prepared = prepared_measured_sources_;
    if (!prepared)
      return measured_source_unavailable<
          core::ComponentLifecycleReadinessSourceView>(
          "measured_current_sources_missing",
          "Current measured sources are unavailable");
    auto readbacks = measured_route_readbacks(*this);
    if (!readbacks.ok())
      return Result::Err(readbacks.error());
    auto valid = validate_measured_readiness_snapshot(*this, snapshot,
                                                      readbacks.value());
    if (!valid.ok())
      return Result::Err(valid.error());
    valid = validate_prepared_measured_sources(
        *prepared, motion_binding, gripper_binding,
        measured_configurations(readbacks.value()));
    if (!valid.ok())
      return Result::Err(valid.error());
    core::ComponentLifecycleReadinessSourceView result{
        prepared->source_view,
        [owning_runtime, prepared, root, motion = motion_binding,
         gripper = gripper_binding,
         representatives = control_binding_representatives,
         expected_readbacks = std::move(readbacks.value()),
         snapshot]() -> core::Result<void> {
          // The existing product context pins the actual cell owner as well as
          // this runtime. Never lock an association retired to another root.
          if ((owning_runtime->cell_runtime_root != nullptr
                   ? owning_runtime->cell_runtime_root
                   : owning_runtime.get()) != root)
            return measured_source_unavailable<void>(
                "measured_source_association_changed",
                "Measured cell root is no longer current");
          std::lock_guard current_lock(root->route_publication_mutex_);
          if (owning_runtime->prepared_measured_sources_ != prepared ||
              !measured_physical_scope_matches(*owning_runtime, motion, gripper,
                                               representatives))
            return measured_source_unavailable<void>(
                "measured_source_association_changed",
                "Captured measured sources are no longer current");
          auto current = measured_route_readbacks(*owning_runtime);
          if (!current.ok())
            return core::Result<void>::Err(current.error());
          if (current.value() != expected_readbacks)
            return measured_source_unavailable<void>(
                "measured_source_association_changed",
                "Captured Kernel route readback changed");
          return validate_measured_readiness_snapshot(*owning_runtime, snapshot,
                                                      current.value());
        }};
    return Result::Ok(std::move(result));
  } catch (const std::exception &exception) {
    return measured_source_unavailable<
        core::ComponentLifecycleReadinessSourceView>(
        "measured_source_capture_failed", exception.what());
  } catch (...) {
    return measured_source_unavailable<
        core::ComponentLifecycleReadinessSourceView>(
        "measured_source_capture_failed", "Measured source capture failed");
  }
}

bool RetainedNativeRuntime::GripperSourceIsCurrent(
    const core::RuntimeParticipantBinding &source) const {
  if (!gripper || !source.value().control_path.has_value() ||
      source.value().capability_id.value() != "tool.gripper.feedback")
    return false;
  const auto &path = *source.value().control_path;
  const auto current =
      ReadCurrentPublishedRouteConfiguration(path.control_domain_id);
  if (!current.ok() || !current.value().has_value())
    return false;
  const auto &configuration = *current.value();
  if (configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.active_control_configuration_id !=
          path.active_control_configuration_id ||
      configuration.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      configuration.control_domain_revision != path.control_domain_revision ||
      configuration.controller_topology_id != path.controller_topology_id ||
      configuration.topology_revision != path.topology_revision ||
      configuration.activation_set_id != path.activation_set_id ||
      std::none_of(configuration.binding_routes.begin(),
                   configuration.binding_routes.end(),
                   [&](const auto &row) {
                     return row.control_binding_id == path.control_binding_id &&
                            row.control_binding_revision ==
                                path.control_binding_revision &&
                            row.control_route_id == path.control_route_id;
                   }) ||
      std::none_of(control_binding_representatives.begin(),
                   control_binding_representatives.end(), [&](const auto &row) {
                     return row.value() == source.value();
                   }))
    return false;
  return true;
}

core::Result<core::GripperCommandStatusDTO>
RetainedNativeRuntime::ReadGripperStatus(
    const core::RuntimeParticipantBinding &source) const {
  auto *const cell_root = cell_runtime_root != nullptr
                              ? cell_runtime_root
                              : const_cast<RetainedNativeRuntime *>(this);
  std::lock_guard lock(cell_root->route_publication_mutex_);
  if (!GripperSourceIsCurrent(source))
    return core::Result<core::GripperCommandStatusDTO>::Err(
        core::ErrorDTO{"grasp_confirmation_source_stale",
                       "Gripper source has no exact current native realization",
                       {}});
  return core::Result<core::GripperCommandStatusDTO>::Ok(
      gripper->get_gripper_status(source.value().command_target_id.value()));
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeRuntime::PublishAndReadBackRouteConfiguration(
    const core::ActiveControlConfigurationDTO &configuration,
    std::vector<deployment::VerifiedRouteReadinessParticipant> participants) {
  if (!configuration.locally_valid()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_configuration_invalid",
        "native route publication has an invalid configuration",
        core::ErrorCode::kInvalidRequest);
  }
  const auto* root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  auto scalar = root->CurrentIndependentGripperRouteOwner(configuration.control_domain_id);
  if (!scalar.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(scalar.error());
  if (scalar.value()) return scalar.value()->PublishAndReadBackRouteConfiguration(configuration, std::move(participants));

  const auto owns_domain = [&](const auto &runtime) {
    return std::any_of(
        runtime.control_binding_representatives.begin(),
        runtime.control_binding_representatives.end(), [&](const auto &binding) {
          return binding.value().control_path.has_value() &&
                 binding.value().control_path->control_domain_id ==
                     configuration.control_domain_id;
        });
  };
  if (!owns_domain(*this)) {
    const auto peer = std::find_if(
        participant_peers.begin(), participant_peers.end(),
        [&](const auto &candidate) { return owns_domain(*candidate); });
    if (peer != participant_peers.end())
      return (*peer)->PublishAndReadBackRouteConfiguration(
          configuration, std::move(participants));
  }
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources>
      candidate_measured_sources;
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources>
      previous_measured_sources;
  RetainedRobotMeasuredSourcePreparer measured_preparer;
  core::RuntimeParticipantAdmissionResolver staged_root_resolver;
  core::RuntimeParticipantAdmissionResolver displaced_local_resolver;
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::unique_lock lock(cell_root->route_publication_mutex_);
  auto *const kernel = addressed_kernel != nullptr ? addressed_kernel.get()
                                                    : cell_addressed_kernel;
  if (kernel == nullptr || participant_engine == nullptr) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_kernel_missing",
        "native route publication has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }

  if (configuration.state == core::ActiveControlConfigurationStateDTO::
                                 kExternalOrUnknownUnreconciled) {
    if (!participants.empty()) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_unreconciled_participants_present",
          "unreconciled route publication contains readiness participants",
          core::ErrorCode::kInvalidRequest);
    }
    auto published =
        kernel->publish_runtime_route_configuration(configuration);
    if (!published.ok()) {
      return published;
    }
    if (gripper && gripper_binding.has_value() &&
        gripper_binding->value().control_path.has_value() &&
        gripper_binding->value().control_path->control_domain_id ==
            configuration.control_domain_id) {
      // Publication has already retired the source, even if a subsequent
      // readback fails. Do not let reconciliation resurrect its evidence.
      gripper->invalidate_grasp_evidence();
    }
    return kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
  }
  if (configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      participants.size() != configuration.binding_routes.size()) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_participant_set_invalid",
        "reconciled route publication has an incomplete participant set",
        core::ErrorCode::kInvalidRequest);
  }

  try {
    auto next_motion = motion_binding;
    auto next_gripper = gripper_binding;
    auto next_representatives = control_binding_representatives;
    std::map<core::ControlBindingId,
             std::optional<core::RuntimeParticipantStopReadiness>>
        readiness_by_binding;
    std::optional<core::RuntimeParticipantPolicySelection>
        motion_participant_policy;
    const auto current_registration =
        [&](const core::RuntimeParticipantBinding &binding)
        -> const core::RuntimeParticipantExecutionRegistration * {
      const auto found = std::find_if(
          participant_registrations.begin(), participant_registrations.end(),
          [&](const auto &candidate) {
            return candidate.binding.value() == binding.value();
          });
      return found == participant_registrations.end() ? nullptr : &*found;
    };
    for (const auto &representative : control_binding_representatives) {
      const auto *current = current_registration(representative);
      if (current == nullptr ||
          !representative.value().control_path.has_value()) {
        throw std::logic_error(
            "native control representative has no current registration");
      }
      readiness_by_binding.emplace(
          representative.value().control_path->control_binding_id,
          current->stop_readiness);
    }
    if (const auto *current = current_registration(motion_binding);
        current != nullptr) {
      motion_participant_policy = current->participant_policy;
    }

    std::set<core::ControlBindingId> supplied_bindings;
    for (auto &participant : participants) {
      const auto &value = participant.participant_binding.value();
      if (!value.control_path.has_value() ||
          value.control_path->control_domain_id !=
              configuration.control_domain_id ||
          participant.readiness_row.control_binding_id !=
              value.control_path->control_binding_id ||
          participant.readiness_row.control_binding_revision !=
              value.control_path->control_binding_revision ||
          participant.readiness_row.controllable_component_id !=
              value.controllable_component_id ||
          participant.readiness_row.resolved_stop_base_plan_id !=
              participant.stop_readiness.value()
                  .resolved_stop_base_plan.resolved_stop_base_plan_id ||
          !supplied_bindings.insert(value.control_path->control_binding_id)
               .second) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_participant_referent_mismatch",
            "route readiness participant differs from its publication row");
      }
      const auto current = std::find_if(
          control_binding_representatives.begin(),
          control_binding_representatives.end(), [&](const auto &candidate) {
            return represents_binding(
                candidate, value.control_path->control_binding_id,
                value.control_path->control_binding_revision);
          });
      if (current == control_binding_representatives.end() ||
          std::find_if(std::next(current),
                       control_binding_representatives.end(),
                       [&](const auto &candidate) {
                         return represents_binding(
                             candidate, value.control_path->control_binding_id,
                             value.control_path->control_binding_revision);
                       }) != control_binding_representatives.end()) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_physical_binding_mismatch",
            "route readiness participant has no sole control representative");
      }
      const bool primary_motion = current->value() == motion_binding.value();
      const bool primary_gripper = gripper_binding.has_value() &&
                                   current->value() == gripper_binding->value();
      const auto index = static_cast<std::size_t>(
          std::distance(control_binding_representatives.begin(), current));
      if (primary_motion) {
        next_motion = participant.participant_binding;
      } else if (primary_gripper) {
        next_gripper = participant.participant_binding;
      }
      readiness_by_binding.at(value.control_path->control_binding_id) =
          std::move(participant.stop_readiness);
      next_representatives[index] = std::move(participant.participant_binding);
    }
    if (supplied_bindings.size() != configuration.binding_routes.size() ||
        std::any_of(configuration.binding_routes.begin(),
                    configuration.binding_routes.end(), [&](const auto &row) {
                      return !supplied_bindings.contains(
                          row.control_binding_id);
                    })) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_selected_binding_missing",
          "route readiness omits one selected physical binding",
          core::ErrorCode::kMissingReference);
    }

    const auto readiness_for_binding = [&](const auto &binding) {
      return readiness_by_binding.at(
          binding.value().control_path->control_binding_id);
    };
    const auto motion_readiness = readiness_for_binding(next_motion);
    const auto gripper_readiness =
        next_gripper.has_value()
            ? readiness_for_binding(*next_gripper)
            : std::optional<core::RuntimeParticipantStopReadiness>{};

    std::vector<core::RuntimeParticipantBinding> next_motion_aliases;
    next_motion_aliases.reserve(motion_binding_aliases.size());
    for (const auto &alias : motion_binding_aliases) {
      auto rebound = rebase_program_alias(alias, motion_binding, next_motion);
      if (!rebound.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      next_motion_aliases.push_back(std::move(rebound.value()));
    }
    std::vector<core::RuntimeParticipantBinding> next_gripper_aliases;
    next_gripper_aliases.reserve(gripper_binding_aliases.size());
    if (gripper_binding.has_value() && next_gripper.has_value()) {
      for (const auto &alias : gripper_binding_aliases) {
        auto rebound =
            rebase_program_alias(alias, *gripper_binding, *next_gripper);
        if (!rebound.ok()) {
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
              rebound.error());
        }
        next_gripper_aliases.push_back(std::move(rebound.value()));
      }
    }

    std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
    registrations.reserve(next_representatives.size() +
                          next_motion_aliases.size() +
                          next_gripper_aliases.size());
    for (const auto &representative : next_representatives) {
      const bool motion_owner =
          retained_bindings_share_backend_owner(next_motion, representative);
      const bool gripper_owner =
          next_gripper.has_value() &&
          retained_bindings_share_backend_owner(*next_gripper, representative);
      if (motion_owner == gripper_owner) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_backend_owner_mismatch",
            "rebound control representative has no sole backend owner");
      }
      const bool primary =
          representative.value() ==
          (motion_owner ? next_motion.value() : next_gripper->value());
      registrations.push_back(runtime_registration(
          *this, representative, readiness_for_binding(representative),
          participant_engine_for(motion_owner),
          motion_owner ? static_cast<core::IControlDomainExecutionPort *>(
                             execution.get())
                       : gripper_stop_execution_port,
          representative_participant_policy(primary, motion_owner,
                                            motion_participant_policy),
          motion_owner ? motion_operator_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? motion_fault_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? deployment_maximum_tcp_linear_speed_mps
                       : std::optional<double>{},
          motion_owner && motion_fault_stop_profile_from_deployment));
    }
    for (const auto &alias : next_motion_aliases) {
      registrations.push_back(runtime_registration(
          *this, alias, motion_readiness, participant_engine.get(),
          execution.get(), motion_participant_policy,
          motion_operator_stop_profile, motion_fault_stop_profile,
          deployment_maximum_tcp_linear_speed_mps,
          motion_fault_stop_profile_from_deployment));
    }
    for (const auto &alias : next_gripper_aliases) {
      registrations.push_back(runtime_registration(
          *this, alias, gripper_readiness, gripper_participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
    }

    auto local_registrations = registrations;
    auto cell_participants = compose_cell_participants(
        *this, registrations, next_motion, next_gripper);
    registrations = std::move(cell_participants.registrations);
    std::vector<core::RuntimeParticipantExecutionRegistration>
        cell_physical_registrations;
    for (const auto &physical_binding : cell_participants.physical_bindings) {
      const auto registration = std::find_if(
          registrations.begin(), registrations.end(), [&](const auto &candidate) {
            return candidate.binding.value() == physical_binding.value();
          });
      if (registration == registrations.end()) {
        return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_physical_binding_missing",
            "cell route publication has no physical participant registration");
      }
      cell_physical_registrations.push_back(*registration);
    }
    append_io_output_registrations(cell_physical_registrations, registrations);
    RetainedRuntimeParticipantResolver binding_resolver;
    if (cell_root->participant_resolver_factory) {
      binding_resolver = cell_root->participant_resolver_factory(
          composed_bindings(registrations));
      if (!binding_resolver) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_resolver_factory_failed",
            "native participant resolver factory returned no resolver");
      }
    } else if (cell_root->participant_resolver) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_resolver_factory_missing",
          "dynamic native participant resolution cannot retain stale physical "
          "facts");
    }
    auto kernel_resolver = make_kernel_participant_resolver(
        std::move(binding_resolver), std::move(cell_physical_registrations));
    if (cell_root != this)
      staged_root_resolver = kernel_resolver;

    const auto old_motion = motion_binding;
    const auto old_gripper = gripper_binding;
    const auto old_representatives = control_binding_representatives;
    if (prepare_measured_sources) {
      measured_preparer = prepare_measured_sources;
      previous_measured_sources = prepared_measured_sources_;
      auto before = measured_route_readbacks(*this);
      if (!before.ok())
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            before.error());
      auto configurations = measured_configurations(before.value());
      const auto selected = std::find_if(
          configurations.begin(), configurations.end(), [&](const auto &row) {
            return row.control_domain_id == configuration.control_domain_id;
          });
      if (selected == configurations.end())
        configurations.push_back(configuration);
      else
        *selected = configuration;
      lock.unlock();
      auto staged =
          measured_preparer(next_motion, next_gripper, configurations);
      if (!staged.ok())
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            staged.error());
      candidate_measured_sources = std::move(staged.value());
      if (candidate_measured_sources) {
        auto valid = validate_prepared_measured_sources(
            *candidate_measured_sources, next_motion, next_gripper,
            configurations);
        if (!valid.ok())
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
              valid.error());
      }
      if ((cell_runtime_root != nullptr ? cell_runtime_root : this) !=
          cell_root)
        return measured_source_unavailable<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "measured_source_association_changed",
            "Measured cell root changed while staging a successor");
      lock.lock();
      auto current = measured_route_readbacks(*this);
      if (!current.ok())
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            current.error());
      if (prepared_measured_sources_ != previous_measured_sources ||
          !measured_physical_scope_matches(*this, old_motion, old_gripper,
                                           old_representatives) ||
          current.value() != before.value())
        return measured_source_unavailable<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "measured_source_association_changed",
            "Measured route changed while staging a successor");
    }
    const auto transition_time_ns = clock->now();
    bool motion_rebound = false;
    bool gripper_rebound = false;
    const auto rollback_physical_route = [&]() noexcept {
      try {
        if (gripper_rebound && old_gripper.has_value() && gripper != nullptr) {
          const auto now_ns = clock->now();
          if (gripper_state_channel->RebindRoute(*old_gripper, now_ns).ok()) {
            static_cast<void>(
                rebind_gripper_route(*old_gripper, gripper_state_channel));
          }
        }
        if (motion_rebound && execution != nullptr) {
          const auto now_ns = clock->now();
          if (motion_state_channel->RebindRoute(old_motion, now_ns).ok()) {
            static_cast<void>(
                rebind_motion_route(old_motion, motion_binding_aliases));
          }
        }
      } catch (...) {
      }
    };
    if (next_motion.value() != motion_binding.value()) {
      auto rebound =
          motion_state_channel->RebindRoute(next_motion, transition_time_ns);
      if (!rebound.ok()) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_motion_state_rebind_failed",
            rebound.error().message);
      }
      motion_rebound = true;
      rebound = rebind_motion_route(next_motion, next_motion_aliases);
      if (!rebound.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_motion_backend_rebind_failed",
            rebound.error().message);
      }
    }
    const bool gripper_binding_changed =
        next_gripper.has_value() != gripper_binding.has_value() ||
        (next_gripper.has_value() && gripper_binding.has_value() &&
         next_gripper->value() != gripper_binding->value());
    if (gripper_binding_changed) {
      if (!next_gripper.has_value() || !gripper_binding.has_value() ||
          gripper_state_channel == nullptr || gripper == nullptr) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_gripper_owner_missing",
            "native route publication cannot rebind a missing gripper owner");
      }
      auto rebound =
          gripper_state_channel->RebindRoute(*next_gripper, transition_time_ns);
      if (!rebound.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_gripper_state_rebind_failed",
            rebound.error().message);
      }
      gripper_rebound = true;
      rebound = rebind_gripper_route(*next_gripper, gripper_state_channel);
      if (!rebound.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_gripper_backend_rebind_failed",
            rebound.error().message);
      }
    }
    auto next_surveillance_sources = compose_cell_surveillance_sources(
        *this, next_motion, motion_readiness, next_gripper, gripper_readiness,
        local_registrations);
    if (cell_root->stop_surveillance_owner != nullptr) {
      auto valid = RetainedStopSurveillanceOwner::ValidateSources(
          next_surveillance_sources);
      if (!valid.ok()) {
        rollback_physical_route();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_publication_surveillance_source_invalid",
            valid.error().message);
      }
    }
    // Stage the stable cell-owner snapshot before changing Kernel state.
    // Its later adoption is a no-throw swap under the same route fence.
    std::vector<core::RuntimeParticipantExecutionRegistration>
        stable_cell_registrations;
    core::RuntimeParticipantAdmissionResolver stable_cell_resolver;
    if (cell_root->cell_runtime_owner != nullptr) {
      stable_cell_registrations = registrations;
      stable_cell_resolver = kernel_resolver;
    }
    auto root_physical_cache = physical_registration_cache(registrations);
    auto published = kernel->publish_runtime_route_configuration(
        configuration, registrations, kernel_resolver);
    if (!published.ok()) {
      rollback_physical_route();
      return published;
    }
    auto readback = kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok() || readback.value() != published.value()) {
      rollback_physical_route();
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_publication_independent_readback_mismatch",
          "native Kernel readback differs from its route publication");
    }

    if (cell_root->stop_surveillance_owner != nullptr) {
      auto replaced = cell_root->stop_surveillance_owner->ReplaceSources(
          std::move(next_surveillance_sources));
      if (!replaced.ok()) {
        throw std::logic_error(
            "prevalidated native surveillance source replacement failed");
      }
    }

    motion_binding = std::move(next_motion);
    gripper_binding = std::move(next_gripper);
    motion_binding_aliases = std::move(next_motion_aliases);
    gripper_binding_aliases = std::move(next_gripper_aliases);
    control_binding_representatives = std::move(next_representatives);
    if (cell_root == this) {
      participant_registrations.swap(root_physical_cache);
    } else {
      participant_registrations.swap(local_registrations);
      cell_root->participant_registrations.swap(root_physical_cache);
    }
    participant_resolver.swap(kernel_resolver);
    displaced_local_resolver.swap(kernel_resolver);
    if (cell_root != this) {
      cell_root->participant_resolver.swap(staged_root_resolver);
    }
    if (cell_root->cell_runtime_owner != nullptr) {
      cell_root->cell_runtime_owner->AdoptVerifiedComponentReadinessSnapshot(
          std::move(stable_cell_registrations),
          std::move(stable_cell_resolver));
    }
    if (measured_preparer) {
      prepared_measured_sources_.swap(candidate_measured_sources);
      if (prepared_measured_sources_)
        enable_prepared_measured_sources(*prepared_measured_sources_);
    }

    return readback;
  } catch (const std::exception &exception) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_native_composition_failed", exception.what());
  } catch (...) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_publication_native_composition_failed",
        "native route publication failed");
  }
}

namespace {

/// The same deterministic re-seal the Kernel applies to its registrations:
/// every participant of the component moves to the published generation.
[[nodiscard]] core::Result<
    std::vector<core::RuntimeParticipantExecutionRegistration>>
rebind_component_registrations(
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations,
    const core::ControllableComponentId &component,
    const core::ComponentLifecycleGeneration generation) {
  for (auto &registration : registrations) {
    if (registration.binding.value().controllable_component_id != component)
      continue;
    if (registration.stop_readiness.has_value() &&
        registration.stop_readiness->value().component_lifecycle_generation != generation) {
      auto refreshed = core::rebind_runtime_participant_stop_readiness_generation(
          *registration.stop_readiness, generation);
      if (!refreshed.ok())
        return core::Result<std::vector<core::RuntimeParticipantExecutionRegistration>>::Err(
            refreshed.error());
      registration.stop_readiness = std::move(refreshed.value());
    }
    if (registration.binding.value().io_output_binding &&
        registration.binding.value().io_output_binding->component_lifecycle_generation != generation) {
      auto value = registration.binding.value();
      value.io_output_binding->component_lifecycle_generation = generation;
      auto refreshed = core::verify_runtime_participant_binding(std::move(value));
      if (!refreshed.ok())
        return core::Result<std::vector<core::RuntimeParticipantExecutionRegistration>>::Err(
            refreshed.error());
      registration.binding = std::move(refreshed.value());
    }
  }
  return core::Result<
      std::vector<core::RuntimeParticipantExecutionRegistration>>::Ok(
      std::move(registrations));
}

} // namespace

core::Result<core::ComponentCommandReadinessDTO>
RetainedNativeRuntime::CloseComponentCommandEligibility(
    const core::LifecycleTransactionId &lifecycle_transaction_id,
    const core::ControllableComponentId &controllable_component_id) {
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  auto *const kernel = cell_root->addressed_kernel != nullptr
                           ? cell_root->addressed_kernel.get()
                           : cell_root->cell_addressed_kernel;
  if (kernel == nullptr) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_kernel_missing",
        "native component readiness has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  return kernel->close_component_command_eligibility(lifecycle_transaction_id,
                                                     controllable_component_id);
}

core::Result<core::ComponentCommandReadinessDTO>
RetainedNativeRuntime::CloseComponentCommandEligibility(
    const core::CellAdmissionClaimDTO &selected_claim,
    const core::SelectedComponentBringUpOfferDTO &offer) {
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  auto *const kernel = cell_root->addressed_kernel != nullptr
                           ? cell_root->addressed_kernel.get()
                           : cell_root->cell_addressed_kernel;
  if (kernel == nullptr) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_kernel_missing",
        "selected component readiness has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  return kernel->close_component_command_eligibility(selected_claim, offer);
}

core::Result<core::ComponentCommandReadinessDTO>
RetainedNativeRuntime::ReadComponentCommandReadiness(
    const core::ControllableComponentId &controllable_component_id) const {
  const auto *const cell_root =
      cell_runtime_root != nullptr ? cell_runtime_root : this;
  const auto *const kernel = cell_root->addressed_kernel != nullptr
                                 ? cell_root->addressed_kernel.get()
                                 : cell_root->cell_addressed_kernel;
  if (kernel == nullptr) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_kernel_missing",
        "native component readiness has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  return kernel->component_command_readiness(controllable_component_id);
}

core::Result<core::ComponentCommandReadinessDTO>
RetainedNativeRuntime::PublishComponentCommandReadiness(
    const core::ComponentCommandReadinessPublicationDTO &publication) {
  if (!publication.locally_valid()) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_publication_invalid",
        "native component readiness publication is invalid",
        core::ErrorCode::kInvalidRequest);
  }
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  auto *const kernel = cell_root->addressed_kernel != nullptr
                           ? cell_root->addressed_kernel.get()
                           : cell_root->cell_addressed_kernel;
  if (kernel == nullptr) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_kernel_missing",
        "native component readiness has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  try {
    // Stage subordinate physical caches separately from the authoritative
    // complete stable-cell inventory; neither is mutated before readback.
    std::vector<std::pair<RetainedNativeRuntime *,
                          std::vector<core::RuntimeParticipantExecutionRegistration>>>
        staged;
    std::vector<core::RuntimeParticipantBinding> physical_bindings;
    std::vector<std::tuple<std::shared_ptr<RetainedNativeGripperRuntime>,
        std::optional<core::RuntimeParticipantStopReadiness>,
        std::vector<core::RuntimeParticipantExecutionRegistration>>> staged_grippers;
    std::optional<core::Result<void>> failure;
    for_each_cell_runtime(*this, [&](RetainedNativeRuntime &runtime) {
      if (failure.has_value()) return;
      auto rebound = rebind_component_registrations(
          physical_registration_cache(runtime.participant_registrations),
          publication.controllable_component_id,
          publication.component_lifecycle_generation);
      if (!rebound.ok()) {
        failure = core::Result<void>::Err(rebound.error());
        return;
      }
      staged.emplace_back(&runtime, std::move(rebound.value()));
      physical_bindings.push_back(runtime.motion_binding);
      if (runtime.gripper_binding) {
        physical_bindings.push_back(*runtime.gripper_binding);
      }
    });
    if (failure.has_value()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          failure->error());
    }
    // Existing independent scalar owners participate in the same complete
    // cell snapshot. Stage their caches before publishing, just like arms.
    for (const auto& gripper : cell_root->independent_gripper_peers) {
      if (!gripper)
        return route_publication_error<core::ComponentCommandReadinessDTO>(
            "component_readiness_physical_binding_missing", "cell readiness lost a scalar owner");
      const auto registration = gripper->registration();
      auto rebound = rebind_component_registrations(
          gripper->registrations(), publication.controllable_component_id,
          publication.component_lifecycle_generation);
      if (!rebound.ok()) return core::Result<core::ComponentCommandReadinessDTO>::Err(rebound.error());
      auto& complete = staged.front().second;
      std::optional<core::RuntimeParticipantStopReadiness> primary_readiness;
      for (const auto& candidate : rebound.value()) {
        const auto previous = std::find_if(complete.begin(), complete.end(), [&](const auto& row) {
          return row.binding.value() == candidate.binding.value();
        });
        if (previous != complete.end()) *previous = candidate;
        else complete.push_back(candidate);
        if (candidate.binding.value() == registration.binding.value()) primary_readiness = candidate.stop_readiness;
      }
      physical_bindings.push_back(registration.binding);
      if (registration.binding.value().controllable_component_id == publication.controllable_component_id)
        staged_grippers.emplace_back(gripper, std::move(primary_readiness), std::move(rebound.value()));
    }
    auto complete = rebind_component_registrations(
        cell_root->cell_runtime_owner
            ? cell_root->cell_runtime_owner->participant_registrations()
            : staged.front().second,
        publication.controllable_component_id,
        publication.component_lifecycle_generation);
    if (!complete.ok())
      return core::Result<core::ComponentCommandReadinessDTO>::Err(complete.error());
    const auto &cell_registrations = complete.value();
    std::vector<core::RuntimeParticipantExecutionRegistration>
        cell_physical_registrations;
    for (const auto &physical_binding : physical_bindings) {
      const auto registration = std::find_if(
          cell_registrations.begin(), cell_registrations.end(),
          [&](const auto &candidate) {
            return candidate.binding.value() == physical_binding.value();
          });
      if (registration == cell_registrations.end()) {
        return route_publication_error<core::ComponentCommandReadinessDTO>(
            "component_readiness_physical_binding_missing",
            "cell readiness publication has no physical participant "
            "registration");
      }
      cell_physical_registrations.push_back(*registration);
    }
    append_io_output_registrations(cell_physical_registrations, cell_registrations);
    RetainedRuntimeParticipantResolver binding_resolver;
    if (cell_root->participant_resolver_factory) {
      binding_resolver =
          cell_root->participant_resolver_factory(composed_bindings(cell_registrations));
      if (!binding_resolver) {
        return route_publication_error<core::ComponentCommandReadinessDTO>(
            "component_readiness_resolver_factory_failed",
            "native participant resolver factory returned no resolver");
      }
    } else if (cell_root->participant_resolver) {
      return route_publication_error<core::ComponentCommandReadinessDTO>(
          "component_readiness_resolver_factory_missing",
          "dynamic native participant resolution cannot retain stale "
          "readiness");
    }
    auto kernel_resolver = make_kernel_participant_resolver(
        std::move(binding_resolver), std::move(cell_physical_registrations));
    auto staged_root_resolver = kernel_resolver;
    std::vector<core::RuntimeParticipantExecutionRegistration> stable_registrations;
    core::RuntimeParticipantAdmissionResolver stable_resolver;
    if (cell_root->cell_runtime_owner) {
      stable_registrations = cell_registrations;
      stable_resolver = kernel_resolver;
    }
    auto published =
        kernel->publish_component_command_readiness(publication, kernel_resolver);
    if (!published.ok()) {
      return published;
    }
    auto readback = kernel->component_command_readiness(
        publication.controllable_component_id);
    if (!readback.ok() || readback.value() != published.value() ||
        readback.value().command_eligible != publication.command_eligible ||
        readback.value().component_lifecycle_generation !=
            publication.component_lifecycle_generation) {
      return route_publication_error<core::ComponentCommandReadinessDTO>(
          "component_readiness_readback_mismatch",
          "native Kernel component readiness readback differs from its "
          "publication",
          core::ErrorCode::kInvariantViolation);
    }
    for (auto &[runtime, registrations] : staged) {
      runtime->participant_registrations = std::move(registrations);
    }
    cell_root->participant_resolver.swap(staged_root_resolver);
    if (cell_root != this) participant_resolver.swap(kernel_resolver);
    if (cell_root->cell_runtime_owner)
      cell_root->cell_runtime_owner->AdoptVerifiedComponentReadinessSnapshot(
          std::move(stable_registrations), std::move(stable_resolver));
    for (auto& [gripper, readiness, registrations] : staged_grippers) {
      gripper->AdoptVerifiedReadiness(std::move(readiness));
      gripper->AdoptVerifiedRegistrations(std::move(registrations));
    }
    return readback;
  } catch (const std::exception &exception) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_native_composition_failed", exception.what());
  }
}

core::Result<core::ComponentCommandReadinessDTO>
RetainedNativeRuntime::PublishComponentCommandReadiness(
    const core::ComponentCommandReadinessPublicationDTO &publication,
    const core::ComponentLifecycleCurrentSnapshotDTO &actual_snapshot,
    const std::vector<core::BrandLifecycleObservationDTO>
        &actual_observations) {
  const bool has_provider = std::any_of(
      actual_snapshot.domains.begin(), actual_snapshot.domains.end(),
      [](const auto &domain) { return domain.provider() != nullptr; });
  if (has_provider) {
    auto *const root = cell_runtime_root != nullptr ? cell_runtime_root : this;
    auto *const kernel = root->addressed_kernel != nullptr
                             ? root->addressed_kernel.get()
                             : root->cell_addressed_kernel;
    if (kernel == nullptr) {
      return route_publication_error<core::ComponentCommandReadinessDTO>(
          "component_readiness_kernel_missing",
          "native component readiness has no stable addressed Kernel owner",
          core::ErrorCode::kMissingReference);
    }
    const auto provider_publication =
        kernel->publish_runtime_provider_configuration(
            publication, actual_snapshot, actual_observations);
    if (!provider_publication.ok()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          provider_publication.error());
    }
  }
  // This owns route exclusion and the complete component reseal; provider-map
  // publication alone never establishes whole-component command eligibility.
  return PublishComponentCommandReadiness(publication);
}

core::Result<core::ComponentCommandReadinessDTO>
RetainedNativeRuntime::PublishComponentCommandReadiness(
    const core::CellAdmissionClaimDTO &claim,
    const core::SelectedComponentBringUpOfferDTO &offer,
    const core::ComponentLifecycleStatusDTO &status,
    const core::SelectedControllerOccurrenceDTO &occurrence,
    const std::vector<core::RuntimeParticipantExecutionRegistration> &installed,
    const std::vector<core::ActiveControlConfigurationDTO> &configurations,
    bool command_eligible) {
  auto *const root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(root->route_publication_mutex_);
  auto *const kernel = root->addressed_kernel ? root->addressed_kernel.get()
                                            : root->cell_addressed_kernel;
  if (kernel == nullptr) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_kernel_missing",
        "selected successor readiness has no stable addressed Kernel owner",
        core::ErrorCode::kMissingReference);
  }
  const auto &component = offer.predecessor.controllable_component_id;
  const auto exact = [](const auto &left, const auto &right) {
    return left.binding.value() == right.binding.value() &&
           left.stop_readiness.has_value() == right.stop_readiness.has_value() &&
           (!left.stop_readiness || left.stop_readiness->value() == right.stop_readiness->value()) &&
           same_registration_after_product_reseal(left, right);
  };
  const auto& complete = root->cell_runtime_owner
      ? root->cell_runtime_owner->participant_registrations() : root->participant_registrations;
  const auto actual_count = std::count_if(complete.begin(),
      complete.end(), [&](const auto &row) {
        return row.binding.value().controllable_component_id == component;
      });
  if (static_cast<std::size_t>(actual_count) != installed.size() ||
      std::any_of(installed.begin(), installed.end(), [&](const auto &row) {
        return row.binding.value().controllable_component_id != component ||
               std::none_of(complete.begin(), complete.end(),
                   [&](const auto &current) { return exact(current, row); });
      })) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "selected_successor_runtime_inventory_changed",
        "selected successor differs from the actual installed cell cache");
  }
  // Read only the existing component caches under the same publication pin.
  // No cache reseal, resolver replacement or participant installation occurs.
  bool cache_exact = true;
  for_each_cell_runtime(*root, [&](RetainedNativeRuntime &runtime) {
    for (const auto &row : runtime.participant_registrations) {
      if (row.binding.value().controllable_component_id == component &&
          std::none_of(installed.begin(), installed.end(),
                       [&](const auto &current) { return exact(current, row); })) {
        cache_exact = false;
      }
    }
  });
  for (const auto& gripper : root->independent_gripper_peers) {
    if (!gripper) { cache_exact = false; continue; }
    for (const auto& row : gripper->registrations()) {
      if (row.binding.value().controllable_component_id == component &&
          std::none_of(installed.begin(), installed.end(),
                       [&](const auto& current) { return exact(current, row); }))
        cache_exact = false;
    }
  }
  if (!cache_exact) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "selected_successor_runtime_inventory_changed",
        "selected successor component caches are not the installed occurrence");
  }
  auto published = kernel->publish_component_command_readiness(
      claim, offer, status, occurrence, installed, configurations, command_eligible);
  if (!published.ok()) return published;
  auto readback = kernel->component_command_readiness(component);
  if (!readback.ok() || readback.value() != published.value() ||
      readback.value().command_eligible ||
      readback.value().component_lifecycle_generation != status.acceptance.component_lifecycle_generation) {
    return route_publication_error<core::ComponentCommandReadinessDTO>(
        "component_readiness_readback_mismatch",
        "selected successor Kernel readiness is not the exact closed accepted generation",
        core::ErrorCode::kInvariantViolation);
  }
  return readback;
}

namespace {
core::Result<void>
reconcile_observed_orphans(RetainedNativeRuntime &runtime,
                           const std::uint64_t resource_generation,
                           const std::vector<core::ResourceId> &resources) {
  if (resources.empty())
    return core::Result<void>::Ok();
  auto *const kernel = runtime.addressed_kernel != nullptr
                           ? runtime.addressed_kernel.get()
                           : runtime.cell_addressed_kernel;
  if (!runtime.stop_surveillance_owner ||
      !runtime.addressed_request_owner_epoch || !runtime.addressed_cell_id ||
      kernel == nullptr || !runtime.clock) {
    return core::Result<void>::Err(core::ErrorDTO{
        "native_orphan_reconciliation_owner_missing",
        "Native command-free orphan reconciliation owner is unavailable",
        {}});
  }
  auto facts =
      runtime.stop_surveillance_owner->BuildOrphanResourceReconciliationFacts(
          *runtime.addressed_request_owner_epoch, *runtime.addressed_cell_id,
          resource_generation, resources, runtime.clock->now());
  if (!facts.ok())
    return core::Result<void>::Err(facts.error());
  auto reconciled = kernel->reconcile_orphaned_resources(facts.value());
  if (!reconciled.ok())
    return core::Result<void>::Err(reconciled.error());
  return core::Result<void>::Ok();
}
} // namespace

core::Result<void> RetainedNativeRuntime::ReconcileCommandFreeOrphans() {
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  auto *const kernel = cell_root->addressed_kernel != nullptr
                           ? cell_root->addressed_kernel.get()
                           : cell_root->cell_addressed_kernel;
  if (kernel == nullptr) {
    return core::Result<void>::Err(core::ErrorDTO{
        "native_orphan_reconciliation_kernel_missing",
        "Native command-free orphan reconciliation has no addressed Kernel",
        {}});
  }
  const auto slots = kernel->resource_table_snapshot();
  std::vector<core::ResourceId> resources;
  for (const auto &slot : slots.slots) {
    if (slot.state ==
        core::ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
      resources.push_back(slot.resource_id);
    }
  }
  std::cerr << "native command-free orphan reconciliation: resources="
            << resources.size() << '\n';
  return reconcile_observed_orphans(
      *cell_root, slots.operation_resource_generation, resources);
}

core::Result<core::RuntimeRouteConfigurationReadbackDTO>
RetainedNativeRuntime::AdoptAndReadBackRouteConfigurationPredecessor(
    const core::ActiveControlConfigurationDTO &configuration) {
  if (!configuration.locally_valid() ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_configuration_invalid",
        "native restart predecessor is not one reconciled configuration",
        core::ErrorCode::kInvalidRequest);
  }
  const auto* root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  auto scalar = root->CurrentIndependentGripperRouteOwner(configuration.control_domain_id);
  if (!scalar.ok()) return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(scalar.error());
  if (scalar.value()) return scalar.value()->AdoptAndReadBackRouteConfigurationPredecessor(configuration);

  const auto owns_domain = [&](const auto &runtime) {
    return std::any_of(
        runtime.control_binding_representatives.begin(),
        runtime.control_binding_representatives.end(), [&](const auto &binding) {
          return binding.value().control_path.has_value() &&
                 binding.value().control_path->control_domain_id ==
                     configuration.control_domain_id;
        });
  };
  if (!owns_domain(*this)) {
    const auto peer = std::find_if(
        participant_peers.begin(), participant_peers.end(),
        [&](const auto &candidate) { return owns_domain(*candidate); });
    if (peer != participant_peers.end())
      return (*peer)->AdoptAndReadBackRouteConfigurationPredecessor(configuration);
  }
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  auto *const kernel = addressed_kernel != nullptr ? addressed_kernel.get()
                                                    : cell_addressed_kernel;
  if (kernel == nullptr || participant_engine == nullptr ||
      execution == nullptr || motion_state_channel == nullptr ||
      clock == nullptr) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_runtime_incomplete",
        "native restart predecessor has no stable runtime owner",
        core::ErrorCode::kMissingReference);
  }

  try {
    auto next_motion = motion_binding;
    auto next_gripper = gripper_binding;
    auto next_representatives = control_binding_representatives;
    const auto rebase_if_selected =
        [&](const core::RuntimeParticipantBinding &binding)
        -> core::Result<core::RuntimeParticipantBinding> {
      if (!binding.value().control_path.has_value() ||
          binding.value().control_path->control_domain_id !=
              configuration.control_domain_id) {
        return core::Result<core::RuntimeParticipantBinding>::Ok(binding);
      }
      return rebase_runtime_binding_to_predecessor(binding, configuration);
    };
    // Rebase each backend owner from its own retained participant binding.
    // Control-binding representatives may be distinct capability/Program
    // identities for the same physical owner; using one of those identities
    // as the owner would leave the state channel and orphan facts on the
    // successor route while Kernel registrations use the predecessor route.
    auto rebound_motion = rebase_if_selected(motion_binding);
    if (!rebound_motion.ok()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          rebound_motion.error());
    }
    next_motion = std::move(rebound_motion.value());
    if (gripper_binding.has_value()) {
      auto rebound_gripper = rebase_if_selected(*gripper_binding);
      if (!rebound_gripper.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound_gripper.error());
      }
      next_gripper = std::move(rebound_gripper.value());
    }
    const auto current_registration =
        [&](const core::RuntimeParticipantBinding &binding)
        -> const core::RuntimeParticipantExecutionRegistration * {
      const auto found = std::find_if(
          participant_registrations.begin(), participant_registrations.end(),
          [&](const auto &candidate) {
            return candidate.binding.value() == binding.value();
          });
      return found == participant_registrations.end() ? nullptr : &*found;
    };
    std::map<core::ControlBindingId,
             std::optional<core::RuntimeParticipantStopReadiness>>
        readiness_by_binding;
    std::optional<core::RuntimeParticipantPolicySelection>
        motion_participant_policy;
    if (const auto *current = current_registration(motion_binding);
        current != nullptr) {
      motion_participant_policy = current->participant_policy;
    }
    for (std::size_t index = 0U; index < control_binding_representatives.size();
         ++index) {
      const auto &representative = control_binding_representatives[index];
      const auto *current = current_registration(representative);
      if (current == nullptr ||
          !representative.value().control_path.has_value()) {
        throw std::logic_error(
            "native predecessor representative has no registration");
      }
      auto readiness = current->stop_readiness;
      auto rebound = rebase_if_selected(representative);
      if (!rebound.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      if (representative.value().control_path->control_domain_id ==
          configuration.control_domain_id) {
        readiness.reset();
      }
      readiness_by_binding.emplace(
          rebound.value().value().control_path->control_binding_id,
          std::move(readiness));
      next_representatives[index] = std::move(rebound.value());
    }
    const auto readiness_for_binding = [&](const auto &binding) {
      return readiness_by_binding.at(
          binding.value().control_path->control_binding_id);
    };
    const auto motion_readiness = readiness_for_binding(next_motion);
    const auto gripper_readiness =
        next_gripper.has_value()
            ? readiness_for_binding(*next_gripper)
            : std::optional<core::RuntimeParticipantStopReadiness>{};

    std::vector<core::RuntimeParticipantBinding> next_motion_aliases;
    next_motion_aliases.reserve(motion_binding_aliases.size());
    for (const auto &alias : motion_binding_aliases) {
      auto rebound = rebase_program_alias(alias, motion_binding, next_motion);
      if (!rebound.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      next_motion_aliases.push_back(std::move(rebound.value()));
    }
    std::vector<core::RuntimeParticipantBinding> next_gripper_aliases;
    next_gripper_aliases.reserve(gripper_binding_aliases.size());
    if (gripper_binding.has_value() && next_gripper.has_value()) {
      for (const auto &alias : gripper_binding_aliases) {
        auto rebound =
            rebase_program_alias(alias, *gripper_binding, *next_gripper);
        if (!rebound.ok()) {
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
              rebound.error());
        }
        next_gripper_aliases.push_back(std::move(rebound.value()));
      }
    }

    std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
    registrations.reserve(next_representatives.size() +
                          next_motion_aliases.size() +
                          next_gripper_aliases.size());
    for (const auto &representative : next_representatives) {
      const bool motion_owner =
          retained_bindings_share_backend_owner(next_motion, representative);
      const bool gripper_owner =
          next_gripper.has_value() &&
          retained_bindings_share_backend_owner(*next_gripper, representative);
      if (motion_owner == gripper_owner) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_predecessor_backend_owner_mismatch",
            "predecessor control representative has no sole backend owner");
      }
      const bool primary =
          representative.value() ==
          (motion_owner ? next_motion.value() : next_gripper->value());
      registrations.push_back(runtime_registration(
          *this, representative, readiness_for_binding(representative),
          participant_engine_for(motion_owner),
          motion_owner ? static_cast<core::IControlDomainExecutionPort *>(
                             execution.get())
                       : gripper_stop_execution_port,
          representative_participant_policy(primary, motion_owner,
                                            motion_participant_policy),
          motion_owner ? motion_operator_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? motion_fault_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? deployment_maximum_tcp_linear_speed_mps
                       : std::optional<double>{},
          motion_owner && motion_fault_stop_profile_from_deployment));
    }
    for (const auto &alias : next_motion_aliases) {
      registrations.push_back(runtime_registration(
          *this, alias, motion_readiness, participant_engine.get(),
          execution.get(), motion_participant_policy,
          motion_operator_stop_profile, motion_fault_stop_profile,
          deployment_maximum_tcp_linear_speed_mps,
          motion_fault_stop_profile_from_deployment));
    }
    for (const auto &alias : next_gripper_aliases) {
      registrations.push_back(runtime_registration(
          *this, alias, gripper_readiness, gripper_participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
    }

    auto local_registrations = registrations;
    auto cell_participants = compose_cell_participants(
        *this, registrations, next_motion, next_gripper);
    registrations = std::move(cell_participants.registrations);
    std::vector<core::RuntimeParticipantExecutionRegistration>
        cell_physical_registrations;
    for (const auto &physical_binding : cell_participants.physical_bindings) {
      const auto registration = std::find_if(
          registrations.begin(), registrations.end(), [&](const auto &candidate) {
            return candidate.binding.value() == physical_binding.value();
          });
      if (registration == registrations.end()) {
        return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
            "route_predecessor_physical_binding_missing",
            "cell predecessor has no physical participant registration");
      }
      cell_physical_registrations.push_back(*registration);
    }
    append_io_output_registrations(cell_physical_registrations, registrations);
    RetainedRuntimeParticipantResolver binding_resolver;
    if (cell_root->participant_resolver_factory) {
      binding_resolver = cell_root->participant_resolver_factory(
          composed_bindings(registrations));
      if (!binding_resolver) {
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_predecessor_resolver_factory_failed",
            "native predecessor resolver factory returned no resolver");
      }
    } else if (cell_root->participant_resolver) {
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_predecessor_resolver_factory_missing",
          "native predecessor adoption cannot retain a stale resolver");
    }
    auto kernel_resolver = make_kernel_participant_resolver(
        std::move(binding_resolver), std::move(cell_physical_registrations));

    const auto transition_time_ns = clock->now();
    const auto old_motion = motion_binding;
    const auto old_gripper = gripper_binding;
    bool motion_rebound = false;
    bool gripper_rebound = false;
    const auto rollback = [&]() noexcept {
      try {
        if (gripper_rebound && old_gripper.has_value() && gripper != nullptr &&
            gripper_state_channel != nullptr) {
          const auto now_ns = clock->now();
          if (gripper_state_channel->RebindRoute(*old_gripper, now_ns).ok()) {
            static_cast<void>(
                rebind_gripper_route(*old_gripper, gripper_state_channel));
          }
        }
        if (motion_rebound) {
          const auto now_ns = clock->now();
          if (motion_state_channel->RebindRoute(old_motion, now_ns).ok()) {
            static_cast<void>(
                rebind_motion_route(old_motion, motion_binding_aliases));
          }
        }
      } catch (...) {
      }
    };
    if (next_motion.value() != motion_binding.value()) {
      auto rebound =
          motion_state_channel->RebindRoute(next_motion, transition_time_ns);
      if (!rebound.ok()) {
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      motion_rebound = true;
      rebound = rebind_motion_route(next_motion, next_motion_aliases);
      if (!rebound.ok()) {
        rollback();
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
    }
    if (next_gripper.has_value() != gripper_binding.has_value() ||
        (next_gripper.has_value() && gripper_binding.has_value() &&
         next_gripper->value() != gripper_binding->value())) {
      if (!next_gripper.has_value() || !gripper_binding.has_value() ||
          gripper_state_channel == nullptr || gripper == nullptr) {
        rollback();
        return route_publication_error<
            core::RuntimeRouteConfigurationReadbackDTO>(
            "route_predecessor_gripper_owner_missing",
            "native predecessor rebind has no gripper owner");
      }
      auto rebound =
          gripper_state_channel->RebindRoute(*next_gripper, transition_time_ns);
      if (!rebound.ok()) {
        rollback();
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
      gripper_rebound = true;
      rebound = rebind_gripper_route(*next_gripper, gripper_state_channel);
      if (!rebound.ok()) {
        rollback();
        return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
            rebound.error());
      }
    }

    std::vector<core::RuntimeParticipantExecutionRegistration>
        stable_cell_registrations;
    core::RuntimeParticipantAdmissionResolver stable_cell_resolver;
    if (cell_root->cell_runtime_owner != nullptr) {
      stable_cell_registrations = registrations;
      stable_cell_resolver = kernel_resolver;
    }
    auto root_physical_cache = physical_registration_cache(registrations);
    auto published =
        kernel->adopt_runtime_route_configuration_predecessor(
            configuration, registrations, kernel_resolver);
    if (!published.ok()) {
      rollback();
      return published;
    }
    auto readback = kernel->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok() || readback.value() != published.value() ||
        readback.value().operation_admission_open ||
        !readback.value().readiness_rows.empty()) {
      rollback();
      return route_publication_error<
          core::RuntimeRouteConfigurationReadbackDTO>(
          "route_predecessor_readback_mismatch",
          "native predecessor adoption failed independent closed readback");
    }

    if (cell_root->stop_surveillance_owner != nullptr) {
      auto next_sources = compose_cell_surveillance_sources(
          *this, next_motion, motion_readiness, next_gripper, gripper_readiness,
          local_registrations);
      auto valid = RetainedStopSurveillanceOwner::ValidateSources(next_sources);
      if (!valid.ok()) {
        throw std::logic_error(
            "predecessor surveillance source validation failed");
      }
      auto replaced =
          cell_root->stop_surveillance_owner->ReplaceSources(std::move(next_sources));
      if (!replaced.ok()) {
        throw std::logic_error(
            "predecessor surveillance source replacement failed");
      }
    }

    motion_binding = std::move(next_motion);
    gripper_binding = std::move(next_gripper);
    motion_binding_aliases = std::move(next_motion_aliases);
    gripper_binding_aliases = std::move(next_gripper_aliases);
    control_binding_representatives = std::move(next_representatives);
    if (cell_root == this) {
      participant_registrations.swap(root_physical_cache);
    } else {
      participant_registrations.swap(local_registrations);
      cell_root->participant_registrations.swap(root_physical_cache);
    }
    participant_resolver = std::move(kernel_resolver);
    if (cell_root != this) {
      cell_root->participant_resolver = participant_resolver;
    }
    if (cell_root->cell_runtime_owner != nullptr) {
      cell_root->cell_runtime_owner->AdoptVerifiedComponentReadinessSnapshot(
          std::move(stable_cell_registrations),
          std::move(stable_cell_resolver));
    }

    return readback;
  } catch (const std::exception &exception) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_adoption_failed", exception.what());
  } catch (...) {
    return route_publication_error<core::RuntimeRouteConfigurationReadbackDTO>(
        "route_predecessor_adoption_failed",
        "native predecessor adoption raised an unknown exception");
  }
}

namespace {

[[nodiscard]] bool retained_stop_source_matches_current_route(
    const RetainedStopSurveillanceSourceInput &source,
    const core::ActiveControlConfigurationDTO &configuration,
    const core::StopFaultRecoveryConfigurationExpectationDTO &expectation) {
  if (source.state_channel == nullptr ||
      !source.binding.value().control_path.has_value()) {
    return false;
  }
  const auto &path = *source.binding.value().control_path;
  const auto context = source.state_channel->query_context();
  const auto derived_context = core::make_observed_state_query_context(
      source.binding, context.freshness_budget_ns);
  if (!derived_context.ok() || derived_context.value() != context ||
      configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.control_domain_id != expectation.control_domain_id ||
      configuration.active_control_configuration_id !=
          expectation.active_control_configuration_id ||
      configuration.active_control_configuration_generation !=
          expectation.active_control_configuration_generation ||
      configuration.control_domain_id != path.control_domain_id ||
      configuration.control_domain_revision != path.control_domain_revision ||
      configuration.activation_set_id != path.activation_set_id ||
      configuration.controller_topology_id != path.controller_topology_id ||
      configuration.topology_revision != path.topology_revision ||
      configuration.source_runtime_unit_id != path.provider_runtime_unit_id ||
      configuration.source_runtime_epoch != path.provider_runtime_epoch ||
      context.active_control_configuration_id !=
          expectation.active_control_configuration_id ||
      context.active_control_configuration_generation !=
          expectation.active_control_configuration_generation ||
      context.runtime_epoch != configuration.source_runtime_epoch) {
    return false;
  }
  return std::any_of(
      configuration.binding_routes.begin(), configuration.binding_routes.end(),
      [&](const auto &route) {
        return route.control_binding_id == path.control_binding_id &&
               route.control_binding_revision == path.control_binding_revision &&
               route.control_route_id == path.control_route_id;
      });
}

[[nodiscard]] core::ErrorDTO native_stop_recovery_error(
    const core::ErrorCode code, std::string message, std::string reason) {
  return core::ErrorDTO{std::string(core::to_string(code)), std::move(message),
                        {{"reason", std::move(reason)}}};
}

}  // namespace

core::Result<core::StopFaultRecoveryFactsDTO>
build_retained_cell_stop_recovery_facts(
    RetainedStopSurveillanceOwner &stop_surveillance_owner,
    core::ResourceAddressedExecutionKernel &addressed_kernel,
    core::IClock &clock, const core::RequestOwnerEpoch &request_owner_epoch,
    const core::CellId &cell_id,
    core::IResourceOperationAdmissionGate *operation_admission_gate,
    const core::StopStatusDTO &status, const std::int64_t assessed_at_ns,
    std::function<core::Result<core::ContentDigest>(
        const core::StopFaultRecoveryConfigurationExpectationDTO &)>
        configuration_source_override) {
  if (!status.fault_latch.has_value() || request_owner_epoch.empty() ||
      cell_id.empty()) {
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
        native_stop_recovery_error(core::ErrorCode::kMissingReference,
                                   "native Stop recovery owner is unavailable",
                                   "native_stop_recovery_owner_unavailable"));
  }
  try {
    auto facts = stop_surveillance_owner.BuildRecoveryFacts(status, assessed_at_ns);
    if (!facts.ok()) {
      return facts;
    }
    const auto &latch = *status.fault_latch;
    const auto sources = stop_surveillance_owner.SnapshotSources();
    // Report why a configuration row is unreconciled, once per change of
    // reason per domain, so a waiting recovery names its failing check.
    const auto report_unreconciled = [](const std::string &domain,
                                        const std::string &reason) {
      static std::mutex mutex;
      static std::map<std::string, std::string> last;
      std::lock_guard lock(mutex);
      auto &previous = last[domain];
      if (previous == reason) {
        return;
      }
      previous = reason;
      if (!reason.empty()) {
        std::cerr << "native Stop recovery configuration unreconciled: domain="
                  << domain << " reason=" << reason << std::endl;
      }
    };
    for (auto &fact : facts.value().configuration_facts) {
      const auto domain = fact.configuration.control_domain_id.value();
      if (configuration_source_override) {
        auto proof = configuration_source_override(fact.configuration);
        fact.reconciled = proof.ok();
        fact.evidence_digest =
            proof.ok() ? std::optional(proof.value()) : std::nullopt;
        report_unreconciled(
            domain, proof.ok() ? std::string()
                               : proof.error().code + ": " + proof.error().message);
        continue;
      }

      const auto current = addressed_kernel.current_runtime_route_configuration(
          fact.configuration.control_domain_id);
      bool source_reconciled = false;
      if (current.ok() && current.value().has_value()) {
        source_reconciled = std::any_of(
            sources.begin(), sources.end(), [&](const auto &source) {
              return source.binding.value().control_path.has_value() &&
                     source.binding.value().control_path->control_domain_id ==
                         fact.configuration.control_domain_id &&
                     retained_stop_source_matches_current_route(
                         source, current.value()->active_configuration,
                         fact.configuration);
            });
      }
      if (!source_reconciled) {
        fact.reconciled = false;
        fact.evidence_digest.reset();
      }
      report_unreconciled(
          domain,
          fact.reconciled ? std::string()
          : !current.ok() ? "route_configuration_unreadable: " + current.error().message
          : !current.value().has_value() ? std::string("route_configuration_unpublished")
                                         : std::string("source_binding_mismatch"));
    }

    if (std::all_of(facts.value().configuration_facts.begin(),
                    facts.value().configuration_facts.end(),
                    [](const auto &fact) { return fact.reconciled; })) {
      const auto slots = addressed_kernel.resource_table_snapshot();
      std::vector<core::ResourceId> orphans;
      for (const auto &slot : slots.slots) {
        if (slot.state ==
                core::ResourceSlotState::kUnreconciledExternalOrOrphanGoal &&
            std::binary_search(latch.captured_resources.begin(),
                               latch.captured_resources.end(),
                               slot.resource_id)) {
          orphans.push_back(slot.resource_id);
        }
      }
      if (!orphans.empty()) {
        auto orphan_facts =
            stop_surveillance_owner.BuildOrphanResourceReconciliationFacts(
                request_owner_epoch, cell_id,
                slots.operation_resource_generation, orphans, clock.now());
        if (!orphan_facts.ok()) {
          return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
              orphan_facts.error());
        }
        auto reconciled =
            addressed_kernel.reconcile_orphaned_resources(orphan_facts.value());
        if (!reconciled.ok()) {
          return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
              reconciled.error());
        }
      }
    }

    for (const auto &slot : addressed_kernel.resource_snapshot()) {
      if (!std::binary_search(latch.captured_resources.begin(),
                              latch.captured_resources.end(),
                              slot.resource_id)) {
        continue;
      }
      const bool unresolved =
          slot.state ==
              core::ResourceSlotState::kUnreconciledExternalOrOrphanGoal ||
          slot.state == core::ResourceSlotState::kReserved ||
          slot.state == core::ResourceSlotState::kDispatched ||
          slot.state == core::ResourceSlotState::kStopping ||
          (slot.state == core::ResourceSlotState::kTerminal &&
           slot.stop_generation > latch.stop_generation);
      if (unresolved) {
        facts.value().fenced_or_orphan_resources.push_back(slot.resource_id);
      }
    }
    if (operation_admission_gate != nullptr) {
      auto fenced = operation_admission_gate->current_fenced_resources(
          latch.captured_resources);
      facts.value().fenced_or_orphan_resources.insert(
          facts.value().fenced_or_orphan_resources.end(), fenced.begin(),
          fenced.end());
    }
    auto &blocked = facts.value().fenced_or_orphan_resources;
    std::sort(blocked.begin(), blocked.end());
    blocked.erase(std::unique(blocked.begin(), blocked.end()), blocked.end());
    if (core::stop_fault_recovery_facts_field_error(facts.value()).has_value()) {
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
          native_stop_recovery_error(
              core::ErrorCode::kInvariantViolation,
              "native Stop recovery facts failed Kernel blocker join",
              "native_stop_recovery_blocker_join_invalid"));
    }
    return facts;
  } catch (const std::exception &exception) {
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
        native_stop_recovery_error(
            core::ErrorCode::kInvariantViolation,
            "native Stop recovery facts could not be joined: " +
                std::string(exception.what()),
            "native_stop_recovery_fact_join_failed"));
  } catch (...) {
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
        native_stop_recovery_error(
            core::ErrorCode::kInvariantViolation,
            "native Stop recovery facts could not be joined",
            "native_stop_recovery_fact_join_failed"));
  }
}

core::Result<core::StopFaultRecoveryFactsDTO>
build_retained_native_stop_recovery_facts(
    RetainedNativeRuntime &runtime,
    core::ResourceAddressedExecutionKernel &addressed_kernel,
    core::IResourceOperationAdmissionGate *operation_admission_gate,
    const core::StopStatusDTO &status, const std::int64_t assessed_at_ns) {
  if (runtime.stop_surveillance_owner == nullptr || runtime.clock == nullptr ||
      !runtime.addressed_request_owner_epoch || !runtime.addressed_cell_id) {
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
        native_stop_recovery_error(core::ErrorCode::kMissingReference,
                                   "native Stop recovery owner is unavailable",
                                   "native_stop_recovery_owner_unavailable"));
  }
  return build_retained_cell_stop_recovery_facts(
      *runtime.stop_surveillance_owner, addressed_kernel, *runtime.clock,
      *runtime.addressed_request_owner_epoch, *runtime.addressed_cell_id,
      operation_admission_gate, status, assessed_at_ns,
      runtime.stop_recovery_configuration_source);
}

void attach_retained_native_addressed_owner(
    const std::shared_ptr<RetainedNativeRuntime> &runtime,
    RetainedNativeAddressedOwnerConfig config) {
  if (runtime == nullptr || runtime->clock == nullptr ||
      runtime->scheduler == nullptr ||
      runtime->participant_registrations.empty() ||
      runtime->participant_engine == nullptr ||
      runtime->stop_surveillance_owner == nullptr ||
      runtime->operation_journal != nullptr ||
      runtime->addressed_kernel != nullptr ||
      config.request_owner_epoch.empty() || config.cell_id.empty() ||
      config.operation_journal_path.empty() || config.graph_revision.empty() ||
      config.request_replay_capacity == 0U ||
      config.request_replay_capacity > core::kMaxI6OperationReplayEntries) {
    throw std::invalid_argument(
        "native addressed-owner attachment is incomplete or duplicated");
  }
  const core::RequestOwnerEpoch kernel_request_owner_epoch =
      config.request_owner_epoch;
  const core::CellId kernel_cell_id = config.cell_id;
  runtime->operation_journal = std::make_unique<core::SqliteOperationJournal>(
      std::move(config.operation_journal_path), std::move(config.cell_id));
  try {
    std::optional<core::StopSurveillanceExecutionConfig>
        stop_surveillance_config;
    if (runtime->stop_surveillance_policy.has_value()) {
      stop_surveillance_config = core::StopSurveillanceExecutionConfig{
          runtime->stop_surveillance_owner.get(),
          *runtime->stop_surveillance_policy};
    }
    auto *const runtime_owner = runtime.get();
    auto *const admission_gate = config.operation_admission_gate;
    core::StopFaultRecoveryFactsResolver recovery_facts_resolver =
        [runtime_owner, admission_gate](const core::StopStatusDTO &status,
                                        const std::int64_t assessed_at_ns)
        -> core::Result<core::StopFaultRecoveryFactsDTO> {
      if (runtime_owner->addressed_kernel == nullptr) {
        return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
            core::ErrorDTO{
                std::string(
                    core::to_string(core::ErrorCode::kMissingReference)),
                "native Stop recovery owner is unavailable",
                {{"reason", "native_stop_recovery_owner_unavailable"}}});
      }
      return build_retained_native_stop_recovery_facts(
          *runtime_owner, *runtime_owner->addressed_kernel, admission_gate,
          status, assessed_at_ns);
    };
    core::ResourceAddressedExecutionKernelConfig addressed_config{
        std::move(config.request_owner_epoch),
        kernel_cell_id,
        config.request_replay_capacity,
        std::move(runtime->participant_resolver),
        config.operation_admission_gate,
        std::move(config.graph_revision),
        core::kMaximumEmergencyStopCorrelationEntries,
        std::min(kNativeMaximumLiveStopParticipants,
                 runtime->participant_registrations.size()),
        0U,
        std::move(stop_surveillance_config),
        std::move(recovery_facts_resolver)};
    addressed_config.cell_admission_claim_capacity = 1024U;
    addressed_config.cell_admission_intervention_validator =
        std::move(config.cell_admission_intervention_validator);
    addressed_config.cell_admission_settlement_validator =
        std::move(config.cell_admission_settlement_validator);
    addressed_config.cell_admission_reconcile_validator =
        std::move(config.cell_admission_reconcile_validator);
    addressed_config.manager_liveness_required_set =
        std::move(config.manager_liveness_required_set);
    addressed_config.cell_admission_program_reset_status_source =
        std::move(config.cell_admission_program_reset_status_source);
    runtime->addressed_kernel =
        std::make_unique<core::ResourceAddressedExecutionKernel>(
            runtime->participant_registrations, *runtime->clock,
            *runtime->scheduler, *runtime->operation_journal,
            std::move(addressed_config));
    runtime->cell_addressed_kernel = runtime->addressed_kernel.get();
    for (const auto &peer : runtime->participant_peers) {
      peer->cell_addressed_kernel = runtime->addressed_kernel.get();
      peer->addressed_request_owner_epoch = kernel_request_owner_epoch;
      peer->addressed_cell_id = kernel_cell_id;
    }
    runtime->addressed_request_owner_epoch = kernel_request_owner_epoch;
    runtime->addressed_cell_id = kernel_cell_id;
  } catch (...) {
    runtime->operation_journal.reset();
    throw;
  }
}

std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime(RetainedNativeRuntimeInput input,
                               RetainedNativeRuntimeConfig config,
                               std::shared_ptr<WallClock> shared_clock,
                               std::shared_ptr<SimpleScheduler> shared_scheduler) {
  RetainedNativeAddressedOwnerConfig owner_config{
      std::move(config.request_owner_epoch),
      std::move(config.cell_id),
      std::move(config.operation_journal_path),
      config.request_replay_capacity,
      input.operation_admission_gate,
      input.motion_binding.value().graph_revision,
      std::move(config.cell_admission_intervention_validator),
      std::move(config.cell_admission_settlement_validator),
      std::move(config.cell_admission_reconcile_validator),
      std::move(config.manager_liveness_required_set),
      std::move(config.cell_admission_program_reset_status_source)};
  auto runtime = create_retained_native_runtime_participants(
      std::move(input), std::move(config), std::move(shared_clock),
      std::move(shared_scheduler));
  attach_retained_native_addressed_owner(runtime, std::move(owner_config));
  return runtime;
}

} // namespace manyforge::roboplan_backend
