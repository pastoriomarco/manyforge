#include "product_backed_runtime_participant_resolver.hpp"

#include "manyforge_roboplan/retained_collision_world_projector.hpp"

#include <algorithm>
#include <map>
#include <manyforge_deployment/runtime_program_binding.hpp>
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
[[nodiscard]] core::Result<Value> Reject(std::string message,
                                         std::string reason,
                                         core::ErrorCode code =
                                             core::ErrorCode::kExecutionRejected) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(code)), std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::CapabilityId RequiredPlannerCapability(
    const core::MotionParticipantIntentDTO& motion) {
  return std::visit(
      [](const auto& value) {
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
[[nodiscard]] bool IsSubset(const std::vector<Available>& available,
                            const std::vector<Selected>& selected,
                            Matches matches) {
  return std::all_of(selected.begin(), selected.end(), [&](const auto& value) {
    return std::count_if(available.begin(), available.end(),
                         [&](const auto& candidate) {
                           return matches(candidate, value);
                         }) == 1;
  });
}

[[nodiscard]] bool SupportingPathAvailable(
    const std::vector<core::RuntimeCapabilityProviderPathDTO>& available,
    const core::RuntimeCapabilityProviderPathDTO& selected) {
  return std::count(available.begin(), available.end(), selected) == 1;
}

[[nodiscard]] bool PhysicallyContainedBy(
    const core::RuntimeParticipantBinding& physical,
    const core::RuntimeParticipantBinding& selected) {
  const auto& available = physical.value();
  const auto& value = selected.value();
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
                  [](const auto& left, const auto& right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision;
                  }) &&
         IsSubset(available.model_groups, value.model_groups,
                  [](const auto& left, const auto& right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.compiled_group_name ==
                               right.compiled_group_name &&
                           left.expanded_compiled_joint_names ==
                               right.expanded_compiled_joint_names;
                  }) &&
         IsSubset(available.frame_bindings, value.frame_bindings,
                  [](const auto& left, const auto& right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision &&
                           left.compiled_frame_name ==
                               right.compiled_frame_name;
                  }) &&
         IsSubset(available.resources, value.resources,
                  [](const auto& left, const auto& right) {
                    return left.resource_id == right.resource_id &&
                           left.source_joint_name == right.source_joint_name &&
                           left.compiled_resource_name ==
                               right.compiled_resource_name &&
                           left.coordinate_kind == right.coordinate_kind;
                  }) &&
         IsSubset(available.collision_body_bindings,
                  value.collision_body_bindings,
                  [](const auto& left, const auto& right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision &&
                           left.compiled_collision_body_names ==
                               right.compiled_collision_body_names;
                  }) &&
         available.control_path == value.control_path &&
         available.capability_provider_path == value.capability_provider_path &&
         std::all_of(value.supporting_capability_provider_paths.begin(),
                     value.supporting_capability_provider_paths.end(),
                     [&](const auto& path) {
                       return SupportingPathAvailable(
                           available.supporting_capability_provider_paths,
                           path);
                     }) &&
         available.runtime_unit_epochs == value.runtime_unit_epochs;
}

[[nodiscard]] const core::RuntimeParticipantBinding& RequirePhysicalBinding(
    const deployment::RuntimeProgramTargetBinding& target,
    const std::vector<core::RuntimeParticipantBinding>& physical_bindings) {
  const core::RuntimeParticipantBinding* result = nullptr;
  for (const auto& binding : physical_bindings) {
    const auto& value = binding.value();
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

[[nodiscard]] const core::RuntimeParticipantBinding* FindExactPrecomposedBinding(
    const core::OperationIntentDTO& operation,
    const core::OperationParticipantIntentDTO& participant,
    const std::vector<core::RuntimeParticipantBinding>& physical_bindings) {
  const core::RuntimeParticipantBinding* result = nullptr;
  for (const auto& binding : physical_bindings) {
    const auto& value = binding.value();
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

  friend bool operator==(const ProviderOccurrence&,
                         const ProviderOccurrence&) = default;
};

void InsertProviderOccurrence(
    std::map<core::ProviderInstanceId, ProviderOccurrence>& occurrences,
    const core::ProviderInstanceId& provider,
    const core::RuntimeUnitId& runtime_unit_id,
    const core::RuntimeEpoch& runtime_epoch) {
  const auto [found, inserted] = occurrences.emplace(
      provider, ProviderOccurrence{runtime_unit_id, runtime_epoch});
  if (!inserted &&
      found->second != ProviderOccurrence{runtime_unit_id, runtime_epoch}) {
    throw std::runtime_error(
        "physical runtime advertises conflicting provider occurrences");
  }
}

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO ActiveFacts(
    const deployment::RuntimeProgramTargetBinding& target,
    const std::span<const deployment::RuntimeProgramTargetBinding> supporting,
    const core::RuntimeParticipantBinding& physical) {
  if (!target.desired_route().has_value() ||
      !physical.value().control_path.has_value()) {
    throw std::runtime_error(
        "Program target or physical owner has no active control route");
  }
  const auto& control = *physical.value().control_path;
  if (*target.desired_route() != control.control_route_id ||
      !target.control_domain().has_value() ||
      target.control_domain()->control_domain_id != control.control_domain_id ||
      target.control_domain()->control_domain_revision !=
          control.control_domain_revision) {
    throw std::runtime_error(
        "Program target crosses the physical owner's active control route");
  }

  std::map<core::ProviderInstanceId, ProviderOccurrence>
      available_occurrences;
  InsertProviderOccurrence(available_occurrences,
                           control.provider_instance_id,
                           control.provider_runtime_unit_id,
                           control.provider_runtime_epoch);
  if (physical.value().capability_provider_path.has_value()) {
    const auto& path = *physical.value().capability_provider_path;
    InsertProviderOccurrence(available_occurrences,
                             path.provider_instance_id,
                             path.provider_runtime_unit_id,
                             path.provider_runtime_epoch);
  }
  for (const auto& path :
       physical.value().supporting_capability_provider_paths) {
    InsertProviderOccurrence(available_occurrences,
                             path.provider_instance_id,
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
  for (const auto& support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      required_providers.insert(
          support.capability_provider_binding()->provider_instance_id);
    }
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(required_providers.size());
  for (const auto& provider : required_providers) {
    const auto found = available_occurrences.find(provider);
    if (found == available_occurrences.end()) {
      throw std::runtime_error(
          "Program target names a provider outside the physical owner");
    }
    provider_epochs.push_back(
        {provider, found->second.runtime_unit_id,
         found->second.runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      control.control_route_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation,
      physical.value().runtime_unit_epochs,
      std::move(provider_epochs)};
}

}  // namespace

ProductBackedRuntimeParticipantResolver::
    ProductBackedRuntimeParticipantResolver(
        std::filesystem::path package_root,
        std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
        deployment::VerifiedRetainedModelMap retained_model_map,
        std::vector<core::RuntimeParticipantBinding> physical_bindings)
    : programs_(package_root),
      bindings_(std::move(package_root)),
      retained_bundle_(std::move(retained_bundle)),
      retained_model_map_(std::move(retained_model_map)),
      physical_bindings_(std::move(physical_bindings)) {
  if (retained_bundle_ == nullptr || physical_bindings_.empty() ||
      retained_model_map_.query().retained_bundle_id !=
          retained_bundle_->retained_bundle_id() ||
      std::any_of(physical_bindings_.begin(), physical_bindings_.end(),
                  [&](const auto& binding) {
                    return binding.value().retained_bundle_id !=
                               retained_bundle_->retained_bundle_id() ||
                           binding.value().graph_revision !=
                               retained_bundle_->value()
                                   .cell_graph.graph_revision;
                  })) {
    throw std::invalid_argument(
        "product-backed participant resolver inputs do not share one active "
        "runtime product");
  }

  std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
      semantic_bodies;
  for (const auto& role : retained_model_map_.model_collision_bodies()) {
    if (!role.owner_component_id.has_value()) {
      throw std::invalid_argument(
          "passive-model collision geometry has no runtime body alias");
    }
    for (std::size_t index = 0U;
         index < role.compiled_link_names.size(); ++index) {
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
  for (const auto& [semantic, physical] : canonical.aliases) {
    collision_body_aliases_.push_back({semantic, physical});
  }
}

core::Result<core::RuntimeParticipantBinding>
ProductBackedRuntimeParticipantResolver::Resolve(
    const core::OperationIntentDTO& operation,
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
    const auto& participant = operation.participants[participant_index];
    if (const auto* precomposed = FindExactPrecomposedBinding(
            operation, participant, physical_bindings_);
        precomposed != nullptr) {
      return core::bind_runtime_participant_collision_body_aliases(
          *precomposed, collision_body_aliases_);
    }

    const auto binding = bindings_.get_verified(
        operation.program_binding_id, *retained_bundle_, retained_model_map_);
    if (binding == nullptr ||
        binding->value().program_id != operation.program_id ||
        binding->value().program_revision != operation.program_revision) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation has no exact generated Program Binding in the active "
          "product",
          "runtime_program_binding_unavailable",
          core::ErrorCode::kMissingReference);
    }
    const auto program = programs_.get_verified(
        operation.program_id, operation.program_revision, *retained_bundle_);
    if (program == nullptr ||
        program->value().node_environment !=
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
    if (const auto* motion =
            std::get_if<core::MotionParticipantIntentDTO>(
                &participant.payload)) {
      supporting.push_back(deployment::resolve_runtime_program_target(
          *binding, participant.target_role_id,
          RequiredPlannerCapability(*motion), *retained_bundle_,
          retained_model_map_));
    }
    const auto& physical = RequirePhysicalBinding(target, physical_bindings_);
    auto candidate = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            target,
            ActiveFacts(target,
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
  } catch (const std::exception& exception) {
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

}  // namespace manyforge::roboplan_backend
