#include "retained_native_runtime_internal.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::roboplan_backend {

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
  // participant_peers intentionally retires with ordinary member destruction,
  // after this body: its engines stayed alive for the Kernel unsubscribe above
  // and for surveillance, and a peer kept alive elsewhere still resolves its
  // live cell root.
}
namespace {

using manyforge::content_identity::CanonicalValue;

inline constexpr std::string_view kNoActiveGoalEvidenceDomain =
    "native-no-active-goal-evidence-digest";
inline constexpr std::string_view kNoActiveGoalEvidenceApiVersion =
    "manyforge.native-no-active-goal-evidence.v1";

[[nodiscard]] RetainedObservedStateChannelConfig
native_state_config(const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
  if (!value.control_path.has_value() ||
      value.control_path->state_authority_class !=
          core::StateAuthorityClass::kPredicted) {
    throw std::invalid_argument(
        "native simulated runtime requires a predicted state authority");
  }
  return RetainedObservedStateChannelConfig{
      core::ObservedStateSourceKind::kPredictedState,
      "roboplan_native/" + value.controllable_component_id.value() + "/" +
          value.command_target_id.value() + "/" +
          value.control_path->control_binding_revision.value(),
      0, 1U};
}

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
  fault_profile.profile_id = "fault_spring_back_hold";
  fault_profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  fault_profile.allow_natural_completion_on_short_path = false;
  fault_profile.recoverable_substitution_mode.reset();
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

[[nodiscard]] core::RuntimeParticipantExecutionRegistration
runtime_registration(
    core::RuntimeParticipantBinding binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    core::IResolvedParticipantExecutionEngine *engine,
    core::IControlDomainExecutionPort *stop_execution_port,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &runtime_policy,
    std::optional<core::MotionInterruptStopProfileDTO>
        default_operator_stop_profile = std::nullopt,
    std::optional<core::MotionInterruptStopProfileDTO>
        default_fault_stop_profile = std::nullopt,
    std::optional<double> deployment_maximum_tcp_linear_speed_mps =
        std::nullopt) {
  if (runtime_policy.has_value() &&
      (runtime_policy->value().controllable_component_id !=
           binding.value().controllable_component_id ||
       runtime_policy->value().command_target_id !=
           binding.value().command_target_id)) {
    throw std::invalid_argument(
        "runtime policy selection crosses its participant binding");
  }
  if (const auto retained_profiles =
          retained_controlled_stop_profiles(binding, readiness);
      retained_profiles.has_value()) {
    if (default_operator_stop_profile.has_value() &&
        *default_operator_stop_profile != retained_profiles->operator_profile) {
      throw std::invalid_argument(
          "runtime operator Stop profile differs from retained Stop authority");
    }
    if (default_fault_stop_profile.has_value() &&
        *default_fault_stop_profile != retained_profiles->fault_profile) {
      throw std::invalid_argument(
          "runtime fault Stop profile differs from retained Stop authority");
    }
    default_operator_stop_profile = retained_profiles->operator_profile;
    default_fault_stop_profile = retained_profiles->fault_profile;
  }
  auto defaults = retained_default_stop_limits(binding, readiness);
  return core::RuntimeParticipantExecutionRegistration{
      std::move(binding),
      readiness,
      std::move(defaults),
      core::MotionInterruptNodeCategory::kGeneric,
      engine,
      stop_execution_port,
      runtime_policy,
      dynamic_cast<core::IResolvedPreparedParticipantExecutionEngine *>(engine),
      std::move(default_operator_stop_profile),
      std::move(default_fault_stop_profile),
      deployment_maximum_tcp_linear_speed_mps};
}

[[nodiscard]] std::optional<core::RuntimeParticipantPolicySelection>
representative_runtime_policy(
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

[[nodiscard]] std::optional<RetainedStopSurveillanceSourceInput>
stop_surveillance_source(
    const core::RuntimeParticipantBinding &binding,
    const std::optional<core::RuntimeParticipantStopReadiness> &readiness,
    const std::shared_ptr<RetainedObservedStateChannel> &channel,
    RetainedNoActiveGoalEvidenceSource no_active_goal_evidence_source) {
  if (channel == nullptr || !no_active_goal_evidence_source) {
    return std::nullopt;
  }
  RetainedStopSurveillanceSourceInput result{
      binding, channel, {}, std::move(no_active_goal_evidence_source)};
  if (readiness.has_value()) {
    const auto &plan = readiness->value().resolved_stop_base_plan;
    for (const auto &stage : plan.escalation_stages) {
      const auto resources = stop_observation_resources(binding, stage);
      for (const auto &requirement : stage.evidence_requirements) {
        if (requirement.requirement.evidence_kind !=
                core::StopEvidenceRequirementKind::kMotionRest ||
            requirement.resolved_source.source_kind !=
                core::StopEvidenceSourceKind::kBindingStateAuthority) {
          continue;
        }
        result.observation_sources.push_back(
            RetainedStopSurveillanceObservationSource{
                requirement.requirement, requirement.resolved_source,
                resources});
      }
    }
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
    core::IResolvedParticipantExecutionEngine *gripper_engine) {
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
  return right.contact_grasp_bindings.empty() &&
         left.retained_bundle_id == right.retained_bundle_id &&
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
    const bool no_active_goal =
        status.state == core::ExecutionState::kIdle ||
        status.state == core::ExecutionState::kStopped ||
        status.state == core::ExecutionState::kCompleted ||
        status.state == core::ExecutionState::kFailed ||
        status.state == core::ExecutionState::kInterventionRequired;
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
  candidate.control_path->runtime_scale_qualification.reset();
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
    const core::RuntimeParticipantExecutionRegistration *physical = nullptr;
    for (const auto &candidate : physical_registrations) {
      if (!same_physical_binding(candidate.binding, binding.value().binding,
                                 true)) {
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
      auto registration = runtime_registration(
          std::move(binding.value().binding), physical->stop_readiness,
          physical->engine, physical->stop_execution_port,
          physical->runtime_policy, physical->default_operator_stop_profile,
          physical->default_fault_stop_profile,
          physical->deployment_maximum_tcp_linear_speed_mps);
      registration.intrinsic_direct_control =
          binding.value().intrinsic_direct_control;
      registration.direct_control_joint_limits =
          physical->direct_control_joint_limits;
      return core::Result<core::RuntimeParticipantExecutionRegistration>::Ok(
          std::move(registration));
    } catch (const std::exception &exception) {
      return route_publication_error<
          core::RuntimeParticipantExecutionRegistration>(
          "runtime_participant_stop_readiness_invalid", exception.what());
    }
  };
}

} // namespace

namespace detail {

RetainedObservedStateChannelConfig select_retained_state_channel_config(
    std::optional<RetainedObservedStateChannelConfig> selected,
    const core::RuntimeParticipantBinding &binding) {
  if (selected.has_value()) {
    return std::move(*selected);
  }
  return native_state_config(binding);
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
  if (!config.cell_stop_deadline_budget.has_value() &&
      input.motion_stop_readiness.has_value()) {
    config.cell_stop_deadline_budget =
        input.motion_stop_readiness->value()
            .resolved_stop_base_plan.deadline_budget;
  }
  const auto controlled_stop_profiles = retained_controlled_stop_profiles(
      input.motion_binding, input.motion_stop_readiness);
  std::optional<core::MotionInterruptStopProfileDTO>
      motion_operator_stop_profile;
  std::optional<core::MotionInterruptStopProfileDTO> motion_fault_stop_profile;
  if (controlled_stop_profiles.has_value()) {
    if (config.kernel.operator_stop_profile &&
        *config.kernel.operator_stop_profile !=
            controlled_stop_profiles->operator_profile) {
      throw std::invalid_argument(
          "Kernel operator Stop profile differs from retained Stop authority");
    }
    if (config.kernel.default_fault_stop_profile &&
        *config.kernel.default_fault_stop_profile !=
            controlled_stop_profiles->fault_profile) {
      throw std::invalid_argument(
          "Kernel fault Stop profile differs from retained Stop authority");
    }
    config.kernel.operator_stop_profile =
        std::make_shared<core::MotionInterruptStopProfileDTO>(
            controlled_stop_profiles->operator_profile);
    config.kernel.default_fault_stop_profile =
        std::make_shared<core::MotionInterruptStopProfileDTO>(
            controlled_stop_profiles->fault_profile);
    motion_operator_stop_profile = controlled_stop_profiles->operator_profile;
    motion_fault_stop_profile = controlled_stop_profiles->fault_profile;
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
            RoboPlanExecutionBackend::
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

  std::optional<core::RuntimeParticipantPolicySelection> motion_runtime_policy;
  if (input.runtime_policy.has_value() !=
      input.runtime_policy_source_artifact.has_value()) {
    throw std::invalid_argument(
        "native runtime policy value and source artifact must be retained "
        "together");
  }
  if (input.runtime_policy.has_value()) {
    auto sealed = core::verify_runtime_participant_policy_selection(
        *input.runtime_policy_source_artifact, *input.runtime_policy,
        input.motion_binding);
    if (!sealed.ok()) {
      throw std::invalid_argument(
          "native runtime policy does not seal against the exact motion "
          "binding: " +
          sealed.error().message);
    }
    motion_runtime_policy = std::move(sealed.value());
  }

  if (!input.deployment_maximum_tcp_linear_speed_mps.has_value() ||
      !std::isfinite(*input.deployment_maximum_tcp_linear_speed_mps) ||
      *input.deployment_maximum_tcp_linear_speed_mps <= 0.0 ||
      *input.deployment_maximum_tcp_linear_speed_mps > 1000.0) {
    throw std::invalid_argument(
        "native motion target requires one finite positive Deployment TCP "
        "linear-speed maximum");
  }

  if (input.ordinary_motion_jerk) {
    if (!motion_runtime_policy ||
        (input.ordinary_motion_jerk->enabled && input.backend_factory))
      throw std::invalid_argument(
          "ordinary jerk profile requires a qualified native execution policy");
  }

  auto runtime = std::make_shared<RetainedNativeRuntime>(input.motion_binding,
                                                         input.gripper_binding);
  runtime->motion_binding_aliases = std::move(input.motion_binding_aliases);
  runtime->gripper_binding_aliases = std::move(input.gripper_binding_aliases);
  runtime->control_binding_representatives =
      std::move(input.control_binding_representatives);
  runtime->motion_operator_stop_profile = motion_operator_stop_profile;
  runtime->motion_fault_stop_profile = motion_fault_stop_profile;
  runtime->motion_runtime_policy = motion_runtime_policy;
  runtime->deployment_maximum_tcp_linear_speed_mps =
      input.deployment_maximum_tcp_linear_speed_mps;
  runtime->direct_control_joint_limits =
      std::move(input.direct_control_joint_limits);
  runtime->participant_resolver_factory =
      std::move(input.participant_resolver_factory);
  runtime->scene = std::move(input.scene);
  runtime->scene_mutex = std::move(input.scene_mutex);
  runtime->state_projector = std::move(input.state_projector);
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
          runtime->motion_binding, runtime->state_projector,
          std::move(motion_state_config), runtime->clock.get(),
          observation_wake);
  const auto initial_timestamp_ns = runtime->clock->now();
  const auto motion_initialized =
      config.initial_motion_positions_rad.empty()
          ? runtime->motion_state_channel->initialize_from_projection(
                initial_timestamp_ns)
          : runtime->motion_state_channel->publish_and_project(
                config.initial_motion_positions_rad,
                std::vector<double>(config.initial_motion_positions_rad.size(),
                                    0.0),
                initial_timestamp_ns, initial_timestamp_ns);
  if (!motion_initialized.applied()) {
    throw std::invalid_argument(
        "native motion state could not be seeded from the retained model");
  }
  runtime->angular_observed_state =
      std::make_unique<core::AddressedAngularObservedRobotStateAdapter>(
          runtime->motion_state_channel->publication());
  runtime->kinematics = std::make_unique<RetainedRoboPlanKinematicsAdapter>(
      runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  if (input.ordinary_motion_jerk) {
    auto limits = runtime->kinematics->joint_limits(
        motion_value.controllable_component_id.value(),
        motion_value.command_target_id.value());
    if (!limits.ok())
      throw std::invalid_argument(limits.error().message);
    auto profile = deployment::native_execution_runtime_contract::
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
        *motion_runtime_policy, runtime->motion_binding,
        std::move(profile.value()));
    if (!selected.ok())
      throw std::invalid_argument(selected.error().message);
    motion_runtime_policy = std::move(selected.value());
    runtime->motion_runtime_policy = motion_runtime_policy;
  }
  runtime->trajectory_validator = RoboPlanTrajectoryValidator::create(
      runtime->motion_binding, runtime->scene, runtime->scene_mutex);
  runtime->collision_trajectory_projector =
      RoboPlanCollisionTrajectoryProjector::create(
          runtime->motion_binding, runtime->scene, runtime->scene_mutex);

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
            model_backed ? runtime->state_projector : nullptr,
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
          retained_gripper_limits(*runtime->scene, *runtime->gripper_binding);
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
  } else {
    auto native_execution = std::make_shared<RoboPlanExecutionBackend>(
        runtime->motion_binding, runtime->motion_state_channel, *runtime->clock,
        *runtime->scheduler, std::move(config.execution));
    runtime->execution = native_execution;
    runtime->rebind_motion_route =
        [native_execution](
            core::RuntimeParticipantBinding binding,
            std::vector<core::RuntimeParticipantBinding> program_aliases) {
          return native_execution->RebindRoute(std::move(binding),
                                               std::move(program_aliases));
        };
    if (runtime->gripper_binding.has_value()) {
      auto native_gripper = std::make_shared<RetainedGripperCommandBackend>(
          *runtime->gripper_binding, runtime->gripper_state_channel,
          runtime->gripper_position_limits, *runtime->clock,
          *runtime->scheduler, config.gripper_update_period_ns);
      runtime->gripper = native_gripper;
      runtime->gripper_stop_execution_port = native_gripper.get();
      runtime->rebind_gripper_route =
          [native_gripper](
              core::RuntimeParticipantBinding binding,
              std::shared_ptr<IRetainedObservedStateChannel> state_channel) {
            return native_gripper->RebindRoute(std::move(binding),
                                               std::move(state_channel));
          };
    }
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
        runtime->gripper_stop_execution_port != gripper_owned_stop) {
      throw std::invalid_argument(
          "retained runtime gripper Stop port is not owned by its adapter "
          "set");
    }
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

  core::ExecutionKernelPorts ports{*runtime->angular_observed_state,
                                   *runtime->kinematics,
                                   *runtime->reference_resolver,
                                   *runtime->trajectory_validator,
                                   *runtime->collision_trajectory_projector,
                                   nullptr,
                                   nullptr,
                                   input.planning_scene_synchronizer.get()};
  runtime->planning_scene_synchronizer =
      std::move(input.planning_scene_synchronizer);
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
    registrations.push_back(runtime_registration(
        representative,
        primary ? (motion_owner ? input.motion_stop_readiness
                                : input.gripper_stop_readiness)
                : std::nullopt,
        runtime->participant_engine_for(motion_owner),
        motion_owner ? static_cast<core::IControlDomainExecutionPort *>(
                           runtime->execution.get())
                     : runtime->gripper_stop_execution_port,
        representative_runtime_policy(primary, motion_owner,
                                      motion_runtime_policy),
        motion_owner ? motion_operator_stop_profile
                     : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? motion_fault_stop_profile
                     : std::optional<core::MotionInterruptStopProfileDTO>{},
        motion_owner ? input.deployment_maximum_tcp_linear_speed_mps
                     : std::optional<double>{}));
  }
  for (const auto &alias : runtime->motion_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.motion_stop_readiness, runtime->participant_engine.get(),
        runtime->execution.get(), motion_runtime_policy,
        motion_operator_stop_profile, motion_fault_stop_profile,
        input.deployment_maximum_tcp_linear_speed_mps));
  }
  for (const auto &alias : runtime->gripper_binding_aliases) {
    registrations.push_back(runtime_registration(
        alias, input.gripper_stop_readiness,
        runtime->gripper_participant_engine.get(),
        runtime->gripper_stop_execution_port, std::nullopt));
  }
  std::vector<core::RuntimeParticipantExecutionRegistration>
      physical_registrations;
  physical_registrations.push_back(runtime_registration(
      runtime->motion_binding, input.motion_stop_readiness,
      runtime->participant_engine.get(), runtime->execution.get(),
      motion_runtime_policy, motion_operator_stop_profile,
      motion_fault_stop_profile,
      input.deployment_maximum_tcp_linear_speed_mps));
  physical_registrations.back().direct_control_joint_limits =
      runtime->direct_control_joint_limits;
  if (runtime->gripper_binding.has_value()) {
    physical_registrations.push_back(runtime_registration(
        *runtime->gripper_binding, input.gripper_stop_readiness,
        runtime->gripper_participant_engine.get(),
        runtime->gripper_stop_execution_port, std::nullopt));
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
  runtime->cell_stop_deadline_budget = config.cell_stop_deadline_budget;
  runtime->stop_surveillance_policy = config.stop_surveillance_policy;
  runtime->participant_registrations = std::move(registrations);
  runtime->participant_resolver = std::move(participant_resolver);
  runtime->stop_surveillance_owner =
      std::make_unique<RetainedStopSurveillanceOwner>(
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
              runtime->gripper_participant_engine.get()),
          runtime->clock.get());
  return runtime;
}

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
    if (peer->cell_stop_deadline_budget.has_value() !=
            primary->cell_stop_deadline_budget.has_value() ||
        (peer->cell_stop_deadline_budget.has_value() &&
         (peer->cell_stop_deadline_budget->outer_duration_ns !=
              primary->cell_stop_deadline_budget->outer_duration_ns ||
          peer->cell_stop_deadline_budget->kernel_completion_reserve_ns !=
              primary->cell_stop_deadline_budget
                  ->kernel_completion_reserve_ns))) {
      throw std::invalid_argument(
          "native participant join has inconsistent cell Stop envelopes");
    }
    runtimes.push_back(peer);
  }

  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  std::vector<core::RuntimeParticipantExecutionRegistration> physical;
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  std::vector<RetainedStopSurveillanceSourceInput> surveillance;
  for (const auto &runtime : runtimes) {
    registrations.insert(registrations.end(), runtime->participant_registrations.begin(),
                         runtime->participant_registrations.end());
    const auto append_physical = [&](const core::RuntimeParticipantBinding &binding) {
      physical_bindings.push_back(binding);
      const auto found = std::find_if(
          runtime->participant_registrations.begin(),
          runtime->participant_registrations.end(), [&](const auto &registration) {
            return registration.binding.value() == binding.value();
          });
      if (found == runtime->participant_registrations.end()) {
        throw std::invalid_argument("native participant join has no physical registration");
      }
      physical.push_back(*found);
    };
    append_physical(runtime->motion_binding);
    if (runtime->gripper_binding.has_value()) append_physical(*runtime->gripper_binding);
    auto sources = stop_surveillance_sources(
        runtime->motion_binding,
        readiness_for(runtime->participant_registrations, runtime->motion_binding),
        runtime->motion_state_channel, runtime->gripper_binding,
        runtime->gripper_binding.has_value()
            ? readiness_for(runtime->participant_registrations, *runtime->gripper_binding)
            : std::nullopt,
        runtime->gripper_state_channel, runtime->participant_engine.get(),
        runtime->gripper_participant_engine.get());
    surveillance.insert(surveillance.end(),
                        std::make_move_iterator(sources.begin()),
                        std::make_move_iterator(sources.end()));
  }
  if (registrations.size() > core::kMaxProgramExecutionMemberships ||
      physical.size() > kNativeMaximumLiveStopParticipants) {
    throw std::invalid_argument("native participant join exceeds the cell bound");
  }
  auto binding_resolver = participant_resolver_factory(physical_bindings);
  if (!binding_resolver) {
    throw std::invalid_argument("native participant join has no resolver");
  }
  primary->participant_registrations = std::move(registrations);
  primary->participant_resolver = make_kernel_participant_resolver(
      std::move(binding_resolver), std::move(physical));
  primary->participant_resolver_factory = std::move(participant_resolver_factory);
  primary->stop_surveillance_owner =
      std::make_unique<RetainedStopSurveillanceOwner>(std::move(surveillance),
                                                       primary->clock.get());
  primary->cell_runtime_root = primary.get();
  for (const auto &peer : peers) peer->cell_runtime_root = primary.get();
  primary->participant_peers = std::move(peers);
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
  return result;
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
compose_cell_surveillance_sources(
    RetainedNativeRuntime &changed,
    const core::RuntimeParticipantBinding &changed_motion,
    const std::optional<core::RuntimeParticipantStopReadiness> &changed_motion_readiness,
    const std::optional<core::RuntimeParticipantBinding> &changed_gripper,
    const std::optional<core::RuntimeParticipantStopReadiness> &changed_gripper_readiness) {
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
        runtime.participant_engine.get(), runtime.gripper_participant_engine.get());
    result.insert(result.end(), std::make_move_iterator(rows.begin()),
                  std::make_move_iterator(rows.end()));
  });
  return result;
}

} // namespace

core::IResolvedParticipantExecutionEngine *
RetainedNativeRuntime::participant_engine_for(const bool motion) const {
  if (motion)
    return participant_engine.get();
  return gripper_participant_engine.get();
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

bool RetainedNativeRuntime::GripperSourceIsCurrent(
    const core::RuntimeParticipantBinding &source) const {
  if (!gripper || !source.value().control_path.has_value() ||
      (source.value().capability_id.value() !=
           "tool.gripper.confirmation.operator" &&
       source.value().capability_id.value() != "tool.gripper.feedback"))
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

core::Result<core::GripperGraspEvidenceDTO>
RetainedNativeRuntime::RecordGraspObservation(
    const core::RuntimeParticipantBinding &source,
    const core::GripperGraspObservationDTO &observation,
    const core::Duration freshness_budget_ns,
    const core::TimePoint admission_deadline_ns) {
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
  if (!GripperSourceIsCurrent(source) ||
      source.value().capability_id.value() !=
          "tool.gripper.confirmation.operator")
    return core::Result<core::GripperGraspEvidenceDTO>::Err(core::ErrorDTO{
        "grasp_confirmation_source_stale",
        "Confirmation source has no exact current native realization",
        {}});
  // An unknown route can be published without rebinding the backend. Keep
  // this final check and write within the same existing publication lock;
  // never install evidence and subsequently call that a no-effect rejection.
  return gripper->record_grasp_observation(
      source, observation, freshness_budget_ns, admission_deadline_ns);
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
      return (*peer)->PublishAndReadBackRouteConfiguration(configuration,
                                                           std::move(participants));
  }
  auto *const cell_root = cell_runtime_root != nullptr ? cell_runtime_root : this;
  std::lock_guard lock(cell_root->route_publication_mutex_);
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
        motion_runtime_policy;
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
      motion_runtime_policy = current->runtime_policy;
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
          representative, readiness_for_binding(representative),
          participant_engine_for(motion_owner),
          motion_owner ? static_cast<core::IControlDomainExecutionPort *>(
                             execution.get())
                       : gripper_stop_execution_port,
          representative_runtime_policy(primary, motion_owner,
                                        motion_runtime_policy),
          motion_owner ? motion_operator_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? motion_fault_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? deployment_maximum_tcp_linear_speed_mps
                       : std::optional<double>{}));
    }
    for (const auto &alias : next_motion_aliases) {
      registrations.push_back(runtime_registration(
          alias, motion_readiness, participant_engine.get(), execution.get(),
          motion_runtime_policy, motion_operator_stop_profile,
          motion_fault_stop_profile,
          deployment_maximum_tcp_linear_speed_mps));
    }
    for (const auto &alias : next_gripper_aliases) {
      registrations.push_back(runtime_registration(
          alias, gripper_readiness, gripper_participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
    }

    const auto local_registrations = registrations;
    auto cell_participants = compose_cell_participants(
        *this, registrations, next_motion, next_gripper);
    registrations = std::move(cell_participants.registrations);
    std::vector<core::RuntimeParticipantExecutionRegistration>
        cell_physical_registrations;
    cell_physical_registrations.reserve(cell_participants.physical_bindings.size());
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
    RetainedRuntimeParticipantResolver binding_resolver;
    if (cell_root->participant_resolver_factory) {
      binding_resolver = cell_root->participant_resolver_factory(
          std::move(cell_participants.physical_bindings));
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

    const auto transition_time_ns = clock->now();
    const auto old_motion = motion_binding;
    const auto old_gripper = gripper_binding;
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
        *this, next_motion, motion_readiness, next_gripper, gripper_readiness);
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
      participant_registrations = registrations;
    } else {
      participant_registrations = local_registrations;
      cell_root->participant_registrations = registrations;
    }
    participant_resolver = std::move(kernel_resolver);
    if (cell_root != this) {
      cell_root->participant_resolver = participant_resolver;
    }
    if (motion_readiness.has_value()) {
      cell_stop_deadline_budget =
          motion_readiness->value().resolved_stop_base_plan.deadline_budget;
    } else if (gripper_readiness.has_value()) {
      cell_stop_deadline_budget =
          gripper_readiness->value().resolved_stop_base_plan.deadline_budget;
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
        motion_runtime_policy;
    if (const auto *current = current_registration(motion_binding);
        current != nullptr) {
      motion_runtime_policy = current->runtime_policy;
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
          representative, readiness_for_binding(representative),
          participant_engine_for(motion_owner),
          motion_owner ? static_cast<core::IControlDomainExecutionPort *>(
                             execution.get())
                       : gripper_stop_execution_port,
          representative_runtime_policy(primary, motion_owner,
                                        motion_runtime_policy),
          motion_owner ? motion_operator_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? motion_fault_stop_profile
                       : std::optional<core::MotionInterruptStopProfileDTO>{},
          motion_owner ? deployment_maximum_tcp_linear_speed_mps
                       : std::optional<double>{}));
    }
    for (const auto &alias : next_motion_aliases) {
      registrations.push_back(runtime_registration(
          alias, motion_readiness, participant_engine.get(), execution.get(),
          motion_runtime_policy, motion_operator_stop_profile,
          motion_fault_stop_profile,
          deployment_maximum_tcp_linear_speed_mps));
    }
    for (const auto &alias : next_gripper_aliases) {
      registrations.push_back(runtime_registration(
          alias, gripper_readiness, gripper_participant_engine.get(),
          gripper_stop_execution_port, std::nullopt));
    }

    const auto local_registrations = registrations;
    auto cell_participants = compose_cell_participants(
        *this, registrations, next_motion, next_gripper);
    registrations = std::move(cell_participants.registrations);
    std::vector<core::RuntimeParticipantExecutionRegistration>
        cell_physical_registrations;
    cell_physical_registrations.reserve(cell_participants.physical_bindings.size());
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
    RetainedRuntimeParticipantResolver binding_resolver;
    if (cell_root->participant_resolver_factory) {
      binding_resolver = cell_root->participant_resolver_factory(
          std::move(cell_participants.physical_bindings));
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
          *this, next_motion, motion_readiness, next_gripper, gripper_readiness);
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
      participant_registrations = registrations;
    } else {
      participant_registrations = local_registrations;
      cell_root->participant_registrations = registrations;
    }
    participant_resolver = std::move(kernel_resolver);
    if (cell_root != this) {
      cell_root->participant_resolver = participant_resolver;
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

core::Result<core::StopFaultRecoveryFactsDTO>
build_retained_native_stop_recovery_facts(
    RetainedNativeRuntime &runtime,
    core::ResourceAddressedExecutionKernel &addressed_kernel,
    core::IResourceOperationAdmissionGate *operation_admission_gate,
    const core::StopStatusDTO &status, const std::int64_t assessed_at_ns) {
  if (runtime.stop_surveillance_owner == nullptr ||
      !status.fault_latch.has_value()) {
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kMissingReference)),
        "native Stop recovery owner is unavailable",
        {{"reason", "native_stop_recovery_owner_unavailable"}}});
  }
  auto facts = runtime.stop_surveillance_owner->BuildRecoveryFacts(
      status, assessed_at_ns);
  if (!facts.ok())
    return facts;
  const auto &latch = *status.fault_latch;
  // Binding identity is not proof that a replacement host has reconciled its
  // controllers. Keep configuration evidence separate from physical rest.
  //
  // A cell may contain several independently-built native runtimes.  The
  // addressed Kernel is shared, but each runtime owns the observation channel
  // for its own control domain.  Looking only at the primary runtime's
  // channels makes a Stop captured on a peer (for example ur5e in a
  // dual-robot cell) appear to wait forever even though its route is already
  // reconciled.  Resolve the source by domain before checking the exact route.
  std::vector<RetainedNativeRuntime *> cell_runtimes;
  for_each_cell_runtime(runtime, [&](RetainedNativeRuntime &candidate) {
    cell_runtimes.push_back(&candidate);
  });
  for (auto &fact : facts.value().configuration_facts) {
    if (runtime.stop_recovery_configuration_source) {
      auto proof =
          runtime.stop_recovery_configuration_source(fact.configuration);
      fact.reconciled = proof.ok();
      fact.evidence_digest =
          proof.ok() ? std::optional(proof.value()) : std::nullopt;
      continue;
    }
    bool source_reconciled = false;
    for (const auto *candidate : cell_runtimes) {
      const auto current = candidate->ReadCurrentPublishedRouteConfiguration(
          fact.configuration.control_domain_id);
      if (!current.ok() || !current.value()) continue;
      const auto matches_source = [&](const auto &channel) {
        if (!channel) return false;
        const auto context = channel->query_context();
        const auto &configuration = *current.value();
        return context.control_domain_id == fact.configuration.control_domain_id &&
               configuration.state ==
                   core::ActiveControlConfigurationStateDTO::kReconciled &&
               configuration.control_domain_id == context.control_domain_id &&
               configuration.active_control_configuration_id ==
                   context.active_control_configuration_id &&
               configuration.active_control_configuration_generation ==
                   context.active_control_configuration_generation &&
               configuration.source_runtime_epoch == context.runtime_epoch;
      };
      source_reconciled = matches_source(candidate->motion_state_channel) ||
                          matches_source(candidate->gripper_state_channel);
      if (source_reconciled) break;
    }
    if (!source_reconciled) {
      fact.reconciled = false;
      fact.evidence_digest.reset();
    }
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
    auto reconciled = reconcile_observed_orphans(
        runtime, slots.operation_resource_generation, orphans);
    if (!reconciled.ok())
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
          reconciled.error());
  }
  for (const auto &slot : addressed_kernel.resource_snapshot()) {
    if (!std::binary_search(latch.captured_resources.begin(),
                            latch.captured_resources.end(), slot.resource_id)) {
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
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
        "native Stop recovery facts failed Kernel blocker join",
        {{"reason", "native_stop_recovery_blocker_join_invalid"}}});
  }
  return facts;
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
        std::move(config.cell_stop_deadline_budget),
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
                               RetainedNativeRuntimeConfig config) {
  config.kernel.runtime_collision_stop_owned_by_addressed_kernel = true;
  auto stop_budget = config.cell_stop_deadline_budget;
  if (!stop_budget.has_value() && input.motion_stop_readiness.has_value()) {
    stop_budget = input.motion_stop_readiness->value()
                      .resolved_stop_base_plan.deadline_budget;
  }
  config.cell_stop_deadline_budget = stop_budget;
  RetainedNativeAddressedOwnerConfig owner_config{
      std::move(config.request_owner_epoch),
      std::move(config.cell_id),
      std::move(config.operation_journal_path),
      config.request_replay_capacity,
      input.operation_admission_gate,
      input.motion_binding.value().graph_revision,
      std::move(stop_budget),
      std::move(config.cell_admission_intervention_validator),
      std::move(config.cell_admission_settlement_validator),
      std::move(config.cell_admission_reconcile_validator),
      std::move(config.manager_liveness_required_set)};
  auto runtime = create_retained_native_runtime_participants(std::move(input),
                                                             std::move(config));
  attach_retained_native_addressed_owner(runtime, std::move(owner_config));
  return runtime;
}

} // namespace manyforge::roboplan_backend
