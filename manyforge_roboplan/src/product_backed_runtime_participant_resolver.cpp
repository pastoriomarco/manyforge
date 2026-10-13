#include "product_backed_runtime_participant_resolver.hpp"

#include "manyforge_roboplan/retained_collision_world_projector.hpp"
#include "manyforge_roboplan/retained_native_runtime.hpp"

#include <algorithm>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

template <typename Value>
[[nodiscard]] core::Result<Value>
Reject(std::string message, std::string reason,
       core::ErrorCode code = core::ErrorCode::kExecutionRejected) {
  return core::Result<Value>::Err(
      core::ErrorDTO{std::string(core::to_string(code)),
                     std::move(message),
                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::CapabilityId
RequiredPlannerCapability(const core::MotionParticipantIntentDTO &motion) {
  return std::visit(
      [](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, core::JointGoalDTO>) {
          return core::CapabilityId("planning.motion.joint_goal");
        } else if constexpr (std::is_same_v<Value, core::PoseGoalDTO>) {
          return core::CapabilityId("planning.motion.pose_goal");
        } else if constexpr (std::is_same_v<Value, core::NamedTargetDTO>) {
          return core::CapabilityId("planning.motion.named_target");
        } else {
          return core::CapabilityId("planning.motion.cartesian_path");
        }
      },
      motion.motion);
}

template <typename Available, typename Selected, typename Matches>
[[nodiscard]] bool IsSubset(const std::vector<Available> &available,
                            const std::vector<Selected> &selected,
                            Matches matches) {
  return std::all_of(selected.begin(), selected.end(), [&](const auto &value) {
    return std::count_if(available.begin(), available.end(),
                         [&](const auto &candidate) {
                           return matches(candidate, value);
                         }) == 1;
  });
}

[[nodiscard]] bool SupportingPathAvailable(
    const std::vector<core::RuntimeCapabilityProviderPathDTO> &available,
    const core::RuntimeCapabilityProviderPathDTO &selected) {
  return std::count(available.begin(), available.end(), selected) == 1;
}

[[nodiscard]] bool
PhysicallyContainedBy(const core::RuntimeParticipantBinding &physical,
                      const core::RuntimeParticipantBinding &selected) {
  const auto &available = physical.value();
  const auto &value = selected.value();
  return available.retained_bundle_id == value.retained_bundle_id &&
         available.graph_revision == value.graph_revision &&
         available.controllable_component_id ==
             value.controllable_component_id &&
         available.component_type_id == value.component_type_id &&
         available.component_type_revision == value.component_type_revision &&
         available.command_target_id == value.command_target_id &&
         available.capability_id == value.capability_id &&
         available.capability_definition_revision ==
             value.capability_definition_revision &&
         available.command_schema == value.command_schema &&
         IsSubset(available.model_bindings, value.model_bindings,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision;
                  }) &&
         IsSubset(available.model_groups, value.model_groups,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.compiled_group_name ==
                               right.compiled_group_name &&
                           left.expanded_compiled_joint_names ==
                               right.expanded_compiled_joint_names;
                  }) &&
         IsSubset(available.frame_bindings, value.frame_bindings,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision &&
                           left.compiled_frame_name ==
                               right.compiled_frame_name;
                  }) &&
         IsSubset(available.resources, value.resources,
                  [](const auto &left, const auto &right) {
                    return left.resource_id == right.resource_id &&
                           left.source_joint_name == right.source_joint_name &&
                           left.compiled_resource_name ==
                               right.compiled_resource_name &&
                           left.coordinate_kind == right.coordinate_kind;
                  }) &&
         IsSubset(available.collision_body_bindings,
                  value.collision_body_bindings,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision &&
                           left.compiled_collision_body_names ==
                               right.compiled_collision_body_names;
                  }) &&
         available.control_path == value.control_path &&
         available.capability_provider_path == value.capability_provider_path &&
         std::all_of(value.supporting_capability_provider_paths.begin(),
                     value.supporting_capability_provider_paths.end(),
                     [&](const auto &path) {
                       return SupportingPathAvailable(
                           available.supporting_capability_provider_paths,
                           path);
                     }) &&
         available.runtime_unit_epochs == value.runtime_unit_epochs;
}

[[nodiscard]] const core::RuntimeParticipantBinding &RequirePhysicalBinding(
    const deployment::RuntimeProgramTargetBinding &target,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  const core::RuntimeParticipantBinding *result = nullptr;
  for (const auto &binding : physical_bindings) {
    const auto &value = binding.value();
    if (value.controllable_component_id !=
            target.component().controllable_component_id ||
        value.command_target_id != target.target_role().command_target_id ||
        value.capability_id != target.capability().capability_id) {
      continue;
    }
    if (result != nullptr) {
      throw std::runtime_error(
          "Program target resolves to multiple physical runtime owners");
    }
    result = &binding;
  }
  if (result == nullptr) {
    throw std::runtime_error(
        "Program target has no composed physical runtime owner");
  }
  return *result;
}

[[nodiscard]] const core::RuntimeParticipantBinding *
FindExactPrecomposedBinding(
    const core::OperationIntentDTO &operation,
    const core::OperationParticipantIntentDTO &participant,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  const core::RuntimeParticipantBinding *result = nullptr;
  for (const auto &binding : physical_bindings) {
    const auto &value = binding.value();
    if (value.program_id != operation.program_id ||
        value.program_revision != operation.program_revision ||
        value.program_binding_id != operation.program_binding_id ||
        value.component_role_id != participant.component_role_id ||
        value.target_role_id != participant.target_role_id ||
        value.capability_id != participant.required_capability_id) {
      continue;
    }
    if (result != nullptr) {
      throw std::runtime_error(
          "operation resolves to multiple exact precomposed runtime owners");
    }
    result = &binding;
  }
  return result;
}

struct ProviderOccurrence final {
  core::RuntimeUnitId runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;

  friend bool operator==(const ProviderOccurrence &,
                         const ProviderOccurrence &) = default;
};

void InsertProviderOccurrence(
    std::map<core::ProviderInstanceId, ProviderOccurrence> &occurrences,
    const core::ProviderInstanceId &provider,
    const core::RuntimeUnitId &runtime_unit_id,
    const core::RuntimeEpoch &runtime_epoch) {
  const auto [found, inserted] = occurrences.emplace(
      provider, ProviderOccurrence{runtime_unit_id, runtime_epoch});
  if (!inserted &&
      found->second != ProviderOccurrence{runtime_unit_id, runtime_epoch}) {
    throw std::runtime_error(
        "physical runtime advertises conflicting provider occurrences");
  }
}

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO ActiveFacts(
    const deployment::RuntimeProgramTargetBinding &target,
    const std::span<const deployment::RuntimeProgramTargetBinding> supporting,
    const core::RuntimeParticipantBinding &physical) {
  if (!target.desired_route().has_value() ||
      !physical.value().control_path.has_value()) {
    throw std::runtime_error(
        "Program target or physical owner has no active control route");
  }
  const auto &control = *physical.value().control_path;
  if (*target.desired_route() != control.control_route_id ||
      !target.control_domain().has_value() ||
      target.control_domain()->control_domain_id != control.control_domain_id ||
      target.control_domain()->control_domain_revision !=
          control.control_domain_revision) {
    throw std::runtime_error(
        "Program target crosses the physical owner's active control route");
  }

  std::map<core::ProviderInstanceId, ProviderOccurrence> available_occurrences;
  InsertProviderOccurrence(available_occurrences, control.provider_instance_id,
                           control.provider_runtime_unit_id,
                           control.provider_runtime_epoch);
  if (physical.value().capability_provider_path.has_value()) {
    const auto &path = *physical.value().capability_provider_path;
    InsertProviderOccurrence(available_occurrences, path.provider_instance_id,
                             path.provider_runtime_unit_id,
                             path.provider_runtime_epoch);
  }
  for (const auto &path :
       physical.value().supporting_capability_provider_paths) {
    InsertProviderOccurrence(available_occurrences, path.provider_instance_id,
                             path.provider_runtime_unit_id,
                             path.provider_runtime_epoch);
  }

  std::set<core::ProviderInstanceId> required_providers;
  if (target.control_binding().has_value()) {
    required_providers.insert(target.control_binding()->provider_instance_id);
  }
  if (target.capability_provider_binding().has_value()) {
    required_providers.insert(
        target.capability_provider_binding()->provider_instance_id);
  }
  for (const auto &support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      required_providers.insert(
          support.capability_provider_binding()->provider_instance_id);
    }
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(required_providers.size());
  for (const auto &provider : required_providers) {
    const auto found = available_occurrences.find(provider);
    if (found == available_occurrences.end()) {
      throw std::runtime_error(
          "Program target names a provider outside the physical owner");
    }
    provider_epochs.push_back(
        {provider, found->second.runtime_unit_id, found->second.runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      control.control_route_id, control.active_control_configuration_id,
      control.active_control_configuration_generation,
      physical.value().runtime_unit_epochs, std::move(provider_epochs)};
}

} // namespace

core::Result<core::ProgramTargetRoleBindingDTO>
ProductBackedRuntimeParticipantResolver::ResolveOperationTarget(
    const core::ProgramBindingId &program_binding_id,
    const core::TargetRoleId &target_role_id) const {
  try {
    const auto binding = LoadProgramBinding(program_binding_id);
    if (!binding)
      throw std::invalid_argument("no exact retained Program Binding");
    const auto program = LoadProgram(binding->value().program_id,
                                     binding->value().program_revision);
    if (!program ||
        program->value().node_environment != binding->value().node_environment)
      throw std::invalid_argument("no exact canonical Program");
    const auto &targets = binding->value().target_role_bindings;
    const auto target =
        std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
          return row.target_role_id == target_role_id;
        });
    if (target == targets.end())
      throw std::invalid_argument("no declared target role");
    return core::Result<core::ProgramTargetRoleBindingDTO>::Ok(*target);
  } catch (const std::exception &error) {
    return core::Result<core::ProgramTargetRoleBindingDTO>::Err(core::ErrorDTO{
        "execution_operation_target_unavailable", error.what(), {}});
  }
}

core::RuntimeUnitId retained_provider_runtime_unit(
    const core::CapabilityProviderBindingDTO &binding) {
  std::set<core::RuntimeUnitId> selected;
  for (const auto &endpoint : binding.endpoint_bindings) {
    if (endpoint.source_kind !=
        core::CapabilityEndpointSourceKind::kRuntimeUnit)
      continue;
    const auto *runtime_unit =
        std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
    if (!runtime_unit)
      throw std::invalid_argument(
          "capability-provider endpoint source kind and value differ");
    selected.insert(*runtime_unit);
  }
  if (selected.size() != 1U)
    throw std::invalid_argument(
        "capability provider has no single exact runtime-unit owner");
  return *selected.begin();
}

bool target_observation_source_is_current(
    const core::RuntimeParticipantBinding &read_binding,
    const core::ObservedStateSnapshotDTO &sample,
    const core::ActiveControlConfigurationDTO &configuration) {
  const auto &read = read_binding.value();
  if (!read.control_path)
    return false;
  const auto &path = *read.control_path;
  if (configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.control_domain_id != path.control_domain_id ||
      configuration.control_domain_revision != path.control_domain_revision ||
      configuration.active_control_configuration_id !=
          path.active_control_configuration_id ||
      configuration.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      configuration.controller_topology_id != path.controller_topology_id ||
      configuration.topology_revision != path.topology_revision ||
      configuration.activation_set_id != path.activation_set_id ||
      sample.controllable_component_id != read.controllable_component_id ||
      sample.command_target_id != read.command_target_id ||
      sample.control_domain_id != path.control_domain_id ||
      sample.control_domain_revision != path.control_domain_revision ||
      sample.active_control_configuration_id !=
          path.active_control_configuration_id ||
      sample.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      sample.controller_topology_id != path.controller_topology_id ||
      sample.topology_revision != path.topology_revision ||
      sample.control_route_id != path.control_route_id ||
      sample.provider_instance_id != path.provider_instance_id ||
      sample.provider_manifest_id != path.provider_manifest_id ||
      sample.provider_manifest_revision != path.provider_manifest_revision ||
      sample.source_epoch != path.provider_runtime_epoch ||
      sample.state_authority_id != path.state_authority_id ||
      sample.state_authority_class != path.state_authority_class)
    return false;
  const auto has_route = [&](const auto &id, const auto &revision) {
    return std::count(configuration.binding_routes.begin(),
                      configuration.binding_routes.end(),
                      core::ActiveControlConfigurationBindingRouteDTO{
                          id, revision, path.control_route_id}) == 1;
  };
  if (!has_route(path.control_binding_id, path.control_binding_revision) ||
      !has_route(sample.control_binding_id, sample.control_binding_revision))
    return false;
  return std::all_of(
      read.resources.begin(), read.resources.end(), [&](const auto &resource) {
        return std::count_if(
                   configuration.kinematic_resource_source_projections.begin(),
                   configuration.kinematic_resource_source_projections.end(),
                   [&](const auto &group) {
                     const auto contains = [&](const auto &id,
                                               const auto &revision) {
                       return std::count(
                                  group.contributing_control_bindings.begin(),
                                  group.contributing_control_bindings.end(),
                                  core::KinematicControlBindingSourceDTO{
                                      id, revision}) == 1;
                     };
                     return group.resource_id == resource.resource_id &&
                            group.provider_instance_id ==
                                sample.provider_instance_id &&
                            group.provider_manifest_id ==
                                sample.provider_manifest_id &&
                            group.provider_manifest_revision ==
                                sample.provider_manifest_revision &&
                            group.state_authority_id ==
                                sample.state_authority_id &&
                            group.state_authority_class ==
                                sample.state_authority_class &&
                            group.publication_source == sample.source_kind &&
                            contains(path.control_binding_id,
                                     path.control_binding_revision) &&
                            contains(sample.control_binding_id,
                                     sample.control_binding_revision);
                   }) == 1;
      });
}

ProductBackedRuntimeParticipantResolver::
    ProductBackedRuntimeParticipantResolver(
        std::filesystem::path package_root,
        std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
        deployment::VerifiedRetainedModelMap retained_model_map,
        std::vector<core::RuntimeParticipantBinding> physical_bindings,
        std::vector<core::ProgramBindingId> intrinsic_direct_bindings,
        std::filesystem::path program_artifact_root)
    : programs_(package_root), bindings_(std::move(package_root)),
      artifact_programs_(
          program_artifact_root.empty()
              ? nullptr
              : std::make_unique<deployment::FilesystemVerifiedProgramSource>(
                    program_artifact_root)),
      artifact_bindings_(
          program_artifact_root.empty()
              ? nullptr
              : std::make_unique<
                    deployment::FilesystemVerifiedProgramBindingSource>(
                    std::move(program_artifact_root))),
      retained_bundle_(std::move(retained_bundle)),
      retained_model_map_(std::move(retained_model_map)),
      physical_bindings_(std::move(physical_bindings)),
      intrinsic_direct_bindings_(std::move(intrinsic_direct_bindings)) {
  if (retained_bundle_ == nullptr || physical_bindings_.empty() ||
      retained_model_map_.query().retained_bundle_id !=
          retained_bundle_->retained_bundle_id() ||
      std::any_of(
          physical_bindings_.begin(), physical_bindings_.end(),
          [&](const auto &binding) {
            return binding.value().retained_bundle_id !=
                       retained_bundle_->retained_bundle_id() ||
                   binding.value().graph_revision !=
                       retained_bundle_->value().cell_graph.graph_revision;
          })) {
    throw std::invalid_argument(
        "product-backed participant resolver inputs do not share one active "
        "runtime product");
  }
  std::sort(intrinsic_direct_bindings_.begin(),
            intrinsic_direct_bindings_.end());
  if (std::adjacent_find(intrinsic_direct_bindings_.begin(),
                         intrinsic_direct_bindings_.end()) !=
          intrinsic_direct_bindings_.end() ||
      std::any_of(intrinsic_direct_bindings_.begin(),
                  intrinsic_direct_bindings_.end(), [&](const auto &id) {
                    return std::none_of(
                        physical_bindings_.begin(), physical_bindings_.end(),
                        [&](const auto &binding) {
                          return binding.value().program_binding_id == id;
                        });
                  })) {
    throw std::invalid_argument(
        "compiler-direct bindings must identify exact composed participants");
  }

  std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
      semantic_bodies;
  for (const auto &role : retained_model_map_.model_collision_bodies()) {
    if (!role.owner_component_id.has_value()) {
      throw std::invalid_argument(
          "passive-model collision geometry has no runtime body alias");
    }
    for (std::size_t index = 0U; index < role.compiled_link_names.size();
         ++index) {
      semantic_bodies.push_back({
          *role.owner_component_id,
          role.model_instance_id,
          role.model_revision,
          role.collision_body_role_id,
          index,
          role.compiled_link_names[index],
      });
    }
  }
  const auto canonical =
      canonicalize_retained_component_bodies(semantic_bodies);
  collision_body_aliases_.reserve(canonical.aliases.size());
  for (const auto &[semantic, physical] : canonical.aliases) {
    collision_body_aliases_.push_back({semantic, physical});
  }
}

std::shared_ptr<const deployment::ProgramBinding>
ProductBackedRuntimeParticipantResolver::LoadProgramBinding(
    const core::ProgramBindingId &program_binding_id) const {
  if (artifact_bindings_) {
    auto binding = artifact_bindings_->get_verified(
        program_binding_id, *retained_bundle_, retained_model_map_);
    if (binding)
      return binding;
  }
  return bindings_.get_verified(program_binding_id, *retained_bundle_,
                                retained_model_map_);
}

std::shared_ptr<const deployment::ProgramBindingContract>
ProductBackedRuntimeParticipantResolver::LoadProgram(
    const core::ProgramId &program_id,
    const core::ProgramRevision &program_revision) const {
  if (artifact_programs_) {
    auto program = artifact_programs_->get_verified(
        program_id, program_revision, *retained_bundle_);
    if (program)
      return program;
  }
  return programs_.get_verified(program_id, program_revision,
                                *retained_bundle_);
}

bool ProductBackedRuntimeParticipantResolver::FrameObservationCovered(
    const core::ProgramFrameRoleBindingDTO &frame,
    const core::RuntimeParticipantBinding &observed_binding) const {
  const auto &binding = observed_binding.value();
  const auto *target = retained_model_map_.find_target(
      binding.controllable_component_id, binding.command_target_id);
  if (!target)
    return false;
  const auto selected = std::find_if(
      target->frames.begin(), target->frames.end(), [&](const auto &row) {
        return row.frame_role_id == frame.frame_role_id;
      });
  if (selected == target->frames.end())
    return false;
  const auto *terminal =
      std::get_if<deployment::CompositionComponentModelFrame>(&selected->frame);
  if (!terminal || terminal->model_instance_id != frame.model_instance_id ||
      terminal->model_revision != frame.model_revision ||
      terminal->compiled_frame_name != frame.compiled_frame_name)
    return false;
  const auto *path = retained_model_map_.find_kinematic_frame_path(
      core::ResolvedComponentModelFrameDTO{
          terminal->controllable_component_id, terminal->model_instance_id,
          terminal->frame_role_id, terminal->compiled_frame_name,
          terminal->model_revision});
  if (path == nullptr || !binding.control_path.has_value())
    return false;
  for (const auto &segment : path->segments) {
    const auto *joint =
        std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
    if (joint == nullptr ||
        joint->coordinate_kind == core::KinematicJointCoordinateKind::kFixed)
      continue;
    const auto &control = *binding.control_path;
    if (std::none_of(joint->eligible_resource_dependencies.begin(),
                     joint->eligible_resource_dependencies.end(),
                     [&](const auto &dependency) {
                       return dependency.control_domain_id ==
                                  control.control_domain_id &&
                              dependency.control_domain_revision ==
                                  control.control_domain_revision &&
                              dependency.control_binding_id ==
                                  control.control_binding_id &&
                              dependency.control_binding_revision ==
                                  control.control_binding_revision &&
                              std::any_of(
                                  binding.resources.begin(),
                                  binding.resources.end(),
                                  [&](const auto &resource) {
                                    return resource.resource_id ==
                                               dependency.resource_id &&
                                           resource.coordinate_kind ==
                                               dependency.coordinate_kind;
                                  });
                     }))
      return false;
  }
  return true;
}

core::Result<ResolvedTargetObservation>
ProductBackedRuntimeParticipantResolver::ResolveObservationTarget(
    const core::ProgramBindingId &program_binding_id,
    const core::TargetRoleId &target_role_id,
    const core::CapabilityId &capability_id,
    const core::RuntimeParticipantBinding &physical,
    const std::span<const core::RuntimeParticipantBinding>
        current_representatives) const {
  using Target = ResolvedTargetObservation;
  try {
    if (capability_id.value() != "model.kinematics.fk" &&
        capability_id.value() != "tool.gripper.feedback") {
      throw std::invalid_argument("unsupported observation capability");
    }
    const bool frame_query = capability_id.value() == "model.kinematics.fk";
    auto selection = observation_selection_.load();
    if (!selection ||
        selection->target.program_binding_id() != program_binding_id ||
        selection->target.target_role().target_role_id != target_role_id ||
        selection->target.capability().capability_id != capability_id) {
      auto binding = selection && selection->binding->program_binding_id() ==
                                      program_binding_id
                         ? selection->binding
                         : nullptr;
      if (!binding) {
        binding = LoadProgramBinding(program_binding_id);
        if (!binding) {
          throw std::invalid_argument("no exact retained Program Binding");
        }
        const auto program = LoadProgram(binding->value().program_id,
                                         binding->value().program_revision);
        if (!program || program->value().node_environment !=
                            binding->value().node_environment) {
          throw std::invalid_argument("no exact canonical Program");
        }
      }
      auto target = deployment::resolve_runtime_program_target(
          *binding, target_role_id, capability_id, *retained_bundle_,
          retained_model_map_);
      auto state_target =
          frame_query
              ? std::optional{deployment::resolve_runtime_program_target(
                    *binding, target_role_id,
                    core::CapabilityId("state.observed.current"),
                    *retained_bundle_, retained_model_map_)}
              : std::nullopt;
      selection = std::make_shared<const ObservationSelection>(
          std::move(binding), std::move(target), std::move(state_target));
      observation_selection_.store(selection);
    }
    const auto &target = selection->target;
    const auto &read_target =
        selection->frame_state_target ? *selection->frame_state_target : target;
    if (!read_target.control_binding())
      throw std::invalid_argument("observation has no exact ControlBinding");
    const auto &selected = *read_target.control_binding();
    const auto representative = std::find_if(
        current_representatives.begin(), current_representatives.end(),
        [&](const auto &row) {
          const auto &path = row.value().control_path;
          return path &&
                 path->control_binding_id == selected.control_binding_id &&
                 path->control_binding_revision ==
                     selected.control_binding_revision &&
                 row.value().capability_id == selected.capability_id &&
                 row.value().capability_definition_revision ==
                     selected.capability_definition_revision;
        });
    if (representative == current_representatives.end() ||
        !retained_bindings_share_backend_owner(physical, *representative))
      throw std::invalid_argument(
          "selected observation has no current native realization");
    // The sealed read capability selects this exact retained CB. Its existing
    // native representative already owns the full coordinate realization.
    // A read-only Program need not declare execution resource roles or build
    // an operation participant merely to query its declared frame/feedback.
    if (frame_query) {
      // The native FK adapter is realized by the same exact provider occurrence
      // as this runtime's retained planning adapter, not any target-wide claim.
      const auto &provider = target.capability_provider_binding();
      if (!provider)
        throw std::invalid_argument("selected FK has no retained provider");
      const auto runtime_unit = retained_provider_runtime_unit(*provider);
      const auto *anchor =
          std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
              &provider->target);
      if (!anchor ||
          std::none_of(
              current_representatives.begin(), current_representatives.end(),
              [&](const auto &row) {
                const auto &path = row.value().control_path;
                return path &&
                       path->control_binding_id == anchor->control_binding_id &&
                       path->control_binding_revision ==
                           anchor->control_binding_revision &&
                       retained_bindings_share_backend_owner(physical, row);
              }) ||
          std::none_of(
              physical.value().supporting_capability_provider_paths.begin(),
              physical.value().supporting_capability_provider_paths.end(),
              [&](const auto &path) {
                return path.provider_instance_id ==
                           provider->provider_instance_id &&
                       path.provider_manifest_id ==
                           provider->provider_manifest_id &&
                       path.provider_manifest_revision ==
                           provider->provider_manifest_revision &&
                       path.provider_runtime_unit_id == runtime_unit &&
                       std::any_of(physical.value().runtime_unit_epochs.begin(),
                                   physical.value().runtime_unit_epochs.end(),
                                   [&](const auto &epoch) {
                                     return epoch.runtime_unit_id ==
                                                path.provider_runtime_unit_id &&
                                            epoch.runtime_epoch ==
                                                path.provider_runtime_epoch;
                                   });
              }))
        throw std::invalid_argument(
            "selected FK provider has no current native realization");
    }
    return core::Result<Target>::Ok({target, *representative});
  } catch (const std::exception &exception) {
    return core::Result<Target>::Err(core::ErrorDTO{
        "execution_target_binding_unavailable", exception.what(), {}});
  }
}

core::Result<core::ProgramRunExecutionCaptureDTO>
ProductBackedRuntimeParticipantResolver::ResolveProgramCapture(
    core::ProgramRunExecutionCaptureDTO capture) const {
  try {
    if (capture.table != nullptr || capture.binding != nullptr ||
        capture.retained_bundle_id != retained_bundle_->retained_bundle_id() ||
        capture.graph_revision !=
            retained_bundle_->value().cell_graph.graph_revision) {
      return Reject<core::ProgramRunExecutionCaptureDTO>(
          "Program capture must name the active product and carry no table",
          "program_capture_product_mismatch", core::ErrorCode::kInvalidRequest);
    }
    const auto binding = LoadProgramBinding(capture.program_binding_id);
    if (binding == nullptr ||
        binding->value().program_id != capture.program_id ||
        binding->value().program_revision != capture.program_revision) {
      return Reject<core::ProgramRunExecutionCaptureDTO>(
          "Program capture has no exact generated Binding",
          "program_capture_binding_unavailable",
          core::ErrorCode::kMissingReference);
    }
    const auto program =
        LoadProgram(capture.program_id, capture.program_revision);
    if (program == nullptr || program->value().node_environment !=
                                  binding->value().node_environment) {
      return Reject<core::ProgramRunExecutionCaptureDTO>(
          "Program capture has no exact canonical Program",
          "program_capture_source_unavailable",
          core::ErrorCode::kMissingReference);
    }
    capture.table = std::shared_ptr<const core::ProgramExecutionTableDTO>(
        program, &program->execution_table());
    capture.binding = std::shared_ptr<const core::ProgramBindingDTO>(
        binding, &binding->value());
    return core::Result<core::ProgramRunExecutionCaptureDTO>::Ok(
        std::move(capture));
  } catch (const std::exception &exception) {
    return Reject<core::ProgramRunExecutionCaptureDTO>(
        std::string("Program capture product resolution failed: ") +
            exception.what(),
        "program_capture_resolution_failed");
  }
}

core::Result<core::RuntimeParticipantBinding>
ProductBackedRuntimeParticipantResolver::Resolve(
    const core::OperationIntentDTO &operation,
    const std::size_t participant_index) const {
  try {
    if (participant_index >= operation.participants.size() ||
        operation.expected_retained_bundle_id !=
            retained_bundle_->retained_bundle_id() ||
        operation.expected_graph_revision !=
            retained_bundle_->value().cell_graph.graph_revision) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation does not name one participant in the active retained "
          "product",
          "runtime_participant_product_mismatch",
          core::ErrorCode::kMissingReference);
    }
    const auto &participant = operation.participants[participant_index];
    if (std::binary_search(intrinsic_direct_bindings_.begin(),
                           intrinsic_direct_bindings_.end(),
                           operation.program_binding_id)) {
      const auto *precomposed = FindExactPrecomposedBinding(
          operation, participant, physical_bindings_);
      if (precomposed == nullptr) {
        return Reject<core::RuntimeParticipantBinding>(
            "direct-control operation differs from its compiled product",
            "direct_control_product_mismatch",
            core::ErrorCode::kInvalidRequest);
      }
      return core::bind_runtime_participant_collision_body_aliases(
          *precomposed, collision_body_aliases_);
    }

    const auto binding = LoadProgramBinding(operation.program_binding_id);
    if (binding == nullptr ||
        binding->value().program_id != operation.program_id ||
        binding->value().program_revision != operation.program_revision) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation has no exact generated Program Binding in the active "
          "product",
          "runtime_program_binding_unavailable",
          core::ErrorCode::kMissingReference);
    }
    const auto program =
        LoadProgram(operation.program_id, operation.program_revision);
    if (program == nullptr || program->value().node_environment !=
                                  binding->value().node_environment) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation has no exact canonical Program in the active product",
          "runtime_program_unavailable", core::ErrorCode::kMissingReference);
    }

    auto target = deployment::resolve_runtime_program_target(
        *binding, participant.target_role_id,
        participant.required_capability_id, *retained_bundle_,
        retained_model_map_);
    std::vector<deployment::RuntimeProgramTargetBinding> supporting;
    if (const auto *motion = std::get_if<core::MotionParticipantIntentDTO>(
            &participant.payload)) {
      supporting.push_back(deployment::resolve_runtime_program_target(
          *binding, participant.target_role_id,
          RequiredPlannerCapability(*motion), *retained_bundle_,
          retained_model_map_));
    }
    const auto &physical = RequirePhysicalBinding(target, physical_bindings_);
    auto candidate = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            target,
            ActiveFacts(
                target,
                std::span<const deployment::RuntimeProgramTargetBinding>(
                    supporting),
                physical),
            std::span<const deployment::RuntimeProgramTargetBinding>(
                supporting)));
    if (!candidate.ok()) {
      return candidate;
    }
    candidate = core::bind_runtime_participant_collision_body_aliases(
        std::move(candidate.value()), collision_body_aliases_);
    if (!candidate.ok()) {
      return candidate;
    }
    if (!PhysicallyContainedBy(physical, candidate.value())) {
      return Reject<core::RuntimeParticipantBinding>(
          "resolved Program participant widens or crosses its composed "
          "physical runtime owner",
          "runtime_program_binding_crosses_physical_owner",
          core::ErrorCode::kInvariantViolation);
    }
    return candidate;
  } catch (const std::exception &exception) {
    return Reject<core::RuntimeParticipantBinding>(
        std::string("exact product-backed participant resolution failed: ") +
            exception.what(),
        "runtime_participant_product_resolution_failed");
  } catch (...) {
    return Reject<core::RuntimeParticipantBinding>(
        "exact product-backed participant resolution failed",
        "runtime_participant_product_resolution_failed");
  }
}

core::Result<void>
ProductBackedRuntimeParticipantResolver::ValidateExecutionOrigin(
    const core::OperationIntentDTO &operation) const {
  const bool direct = std::binary_search(intrinsic_direct_bindings_.begin(),
                                         intrinsic_direct_bindings_.end(),
                                         operation.program_binding_id);
  for (const auto &participant : operation.participants) {
    if (direct) {
      if (participant.execution_scope.has_value() ||
          FindExactPrecomposedBinding(operation, participant,
                                      physical_bindings_) == nullptr) {
        return Reject<void>(
            "direct-control operation differs from its compiled product",
            "direct_control_product_mismatch",
            core::ErrorCode::kInvalidRequest);
      }
    } else if (!participant.execution_scope.has_value()) {
      return Reject<void>(
          "Program effects require private captured node origin",
          "program_execution_scope_missing", core::ErrorCode::kInvalidRequest);
    }
  }
  return core::Result<void>::Ok();
}

core::Result<RetainedResolvedParticipant>
ProductBackedRuntimeParticipantResolver::ResolveExecutionParticipant(
    const core::OperationIntentDTO &operation,
    const std::size_t participant_index) const {
  auto binding = Resolve(operation, participant_index);
  if (!binding.ok())
    return core::Result<RetainedResolvedParticipant>::Err(binding.error());
  return core::Result<RetainedResolvedParticipant>::Ok({
      std::move(binding.value()),
      std::binary_search(intrinsic_direct_bindings_.begin(),
                         intrinsic_direct_bindings_.end(),
                         operation.program_binding_id),
  });
}

} // namespace manyforge::roboplan_backend
