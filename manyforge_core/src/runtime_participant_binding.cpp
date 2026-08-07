#include <algorithm>
#include <cmath>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<RuntimeParticipantBinding> invalid(std::string message, std::string reason) {
  return Result<RuntimeParticipantBinding>::Err(
      ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
               std::move(message),
               {{"reason", std::move(reason)}}});
}

template <typename Range, typename Projection>
[[nodiscard]] bool sorted_unique(const Range& values, Projection projection) {
  return std::is_sorted(
             values.begin(), values.end(),
             [&](const auto& lhs, const auto& rhs) { return projection(lhs) < projection(rhs); }) &&
         std::adjacent_find(values.begin(), values.end(), [&](const auto& lhs, const auto& rhs) {
           return projection(lhs) == projection(rhs);
         }) == values.end();
}

template <typename Range, typename Projection>
[[nodiscard]] bool unique_nonempty(const Range& values, Projection projection) {
  std::set<decltype(projection(values.front()))> identities;
  return !values.empty() && std::all_of(values.begin(), values.end(), [&](const auto& value) {
    return identities.insert(projection(value)).second;
  });
}

[[nodiscard]] bool unique_nonempty_strings(const std::vector<std::string>& values) {
  return unique_nonempty(values, [](const auto& value) { return value; }) &&
         std::all_of(values.begin(), values.end(),
                     [](const auto& value) { return !value.empty(); });
}

[[nodiscard]] bool valid_control_path(const RuntimeControlPathDTO& path) noexcept {
  const bool base_valid =
         !path.control_domain_id.empty() && !path.control_domain_revision.empty() &&
         !path.control_binding_id.empty() && !path.control_binding_revision.empty() &&
         !path.control_interface_contract_id.empty() && !path.control_interface_revision.empty() &&
         !path.controller_topology_id.empty() && !path.topology_revision.empty() &&
         !path.control_route_id.empty() && !path.activation_set_id.empty() &&
         !path.active_control_configuration_id.empty() &&
         path.active_control_configuration_generation.nonzero() &&
         !path.provider_instance_id.empty() && !path.provider_manifest_id.empty() &&
         !path.provider_manifest_revision.empty() && !path.provider_runtime_epoch.empty() &&
         !path.state_authority_id.empty() && !path.stop_evidence_policy_id.empty() &&
         !path.stop_evidence_policy_revision.empty();
  if (!base_valid || !path.runtime_scale_qualification.has_value()) {
    return base_valid;
  }
  const auto& scale = *path.runtime_scale_qualification;
  return scale.control_route_id == path.control_route_id &&
         !scale.scaling_domain_id.empty() &&
         !scale.command_endpoint_role_id.empty() &&
         !scale.applied_state_endpoint_role_id.empty() &&
         (!scale.coordinated_barrier_id.has_value() ||
          !scale.coordinated_barrier_id->empty()) &&
         std::isfinite(scale.maximum_slew_per_second) &&
         scale.maximum_slew_per_second > 0.0 &&
         scale.freshness_budget_ns > 0 &&
         scale.freshness_budget_ns <= 9007199254740991LL &&
         scale.acknowledgement_kind ==
             RuntimeScaleAcknowledgementKind::kAppliedState;
}

[[nodiscard]] bool valid_provider_path(const RuntimeCapabilityProviderPathDTO& path) noexcept {
  return !path.capability_id.empty() && !path.capability_definition_revision.empty() &&
         !path.capability_provider_binding_id.empty() &&
         !path.capability_provider_binding_revision.empty() && !path.provider_instance_id.empty() &&
         !path.provider_manifest_id.empty() && !path.provider_manifest_revision.empty() &&
         !path.provider_runtime_epoch.empty();
}

}  // namespace

Result<RuntimeParticipantBinding> verify_runtime_participant_binding(
    RuntimeParticipantBindingDTO value) {
  if (value.program_id.empty() || value.program_revision.empty() ||
      value.program_binding_id.empty() || value.retained_bundle_id.empty() ||
      value.graph_revision.empty() || value.component_role_id.empty() ||
      value.target_role_id.empty() || value.controllable_component_id.empty() ||
      value.component_type_id.empty() || value.component_type_revision.empty() ||
      value.command_target_id.empty() || value.capability_id.empty() ||
      value.capability_definition_revision.empty()) {
    return invalid("runtime participant has an empty required identity",
                   "runtime_participant_identity_empty");
  }
  if (!unique_nonempty(value.model_bindings, [](const auto& item) { return item.model_role_id; }) ||
      std::any_of(value.model_bindings.begin(), value.model_bindings.end(), [](const auto& item) {
        return item.model_role_id.empty() || item.model_instance_id.empty() ||
               item.model_revision.empty();
      })) {
    return invalid("runtime participant model bindings are incomplete or duplicate",
                   "runtime_model_binding_invalid");
  }
  if (!unique_nonempty(value.model_groups,
                       [](const auto& item) {
                         return std::pair(item.model_group_role_id, item.model_instance_id);
                       }) ||
      std::any_of(value.model_groups.begin(), value.model_groups.end(), [&](const auto& item) {
        return item.model_group_role_id.empty() || item.model_instance_id.empty() ||
               item.compiled_group_name.empty() ||
               !unique_nonempty_strings(item.expanded_compiled_joint_names) ||
               std::count_if(value.model_bindings.begin(), value.model_bindings.end(),
                             [&](const auto& model) {
                               return model.model_instance_id == item.model_instance_id;
                             }) != 1;
      })) {
    return invalid("runtime participant model groups are incomplete or duplicate",
                   "runtime_model_group_invalid");
  }
  if (!unique_nonempty(value.frame_bindings, [](const auto& item) { return item.frame_role_id; }) ||
      std::any_of(value.frame_bindings.begin(), value.frame_bindings.end(), [](const auto& item) {
        return item.frame_role_id.empty() || item.model_instance_id.empty() ||
               item.model_revision.empty() || item.compiled_frame_name.empty();
      })) {
    return invalid("runtime participant frame bindings are incomplete or duplicate",
                   "runtime_frame_binding_invalid");
  }
  std::set<std::string, std::less<>> source_joint_names;
  if (!unique_nonempty(value.resources, [](const auto& item) { return item.resource_id; }) ||
      !unique_nonempty(value.resources,
                       [](const auto& item) { return item.compiled_resource_name; }) ||
      std::any_of(value.resources.begin(), value.resources.end(), [&](const auto& item) {
        return item.resource_role_id.empty() || item.resource_id.empty() ||
               item.compiled_resource_name.empty() ||
               (item.source_joint_name.has_value() &&
                (item.source_joint_name->empty() ||
                 !source_joint_names.insert(*item.source_joint_name).second));
      })) {
    return invalid("runtime participant resources are incomplete or duplicate",
                   "runtime_resource_binding_invalid");
  }
  if (value.control_path.has_value() && !valid_control_path(*value.control_path)) {
    return invalid("runtime participant control path is incomplete",
                   "runtime_control_path_invalid");
  }
  if (value.capability_provider_path.has_value()) {
    const auto& provider = *value.capability_provider_path;
    if (!valid_provider_path(provider) || provider.capability_id != value.capability_id ||
        provider.capability_definition_revision != value.capability_definition_revision) {
      return invalid("runtime participant provider path is incomplete",
                     "runtime_provider_path_invalid");
    }
  }
  if (!sorted_unique(value.supporting_capability_provider_paths,
                     [](const auto& item) {
                       return std::tie(item.capability_id, item.capability_definition_revision);
                     }) ||
      std::any_of(value.supporting_capability_provider_paths.begin(),
                  value.supporting_capability_provider_paths.end(),
                  [&](const auto& provider) {
                    return !valid_provider_path(provider) ||
                           (provider.capability_id == value.capability_id &&
                            provider.capability_definition_revision ==
                                value.capability_definition_revision);
                  }) ||
      !std::all_of(value.supporting_capability_provider_paths.begin(),
                   value.supporting_capability_provider_paths.end(), [&](const auto& provider) {
                     return std::count_if(value.supporting_capability_provider_paths.begin(),
                                          value.supporting_capability_provider_paths.end(),
                                          [&](const auto& candidate) {
                                            return candidate.capability_provider_binding_id ==
                                                   provider.capability_provider_binding_id;
                                          }) == 1;
                   })) {
    return invalid(
        "runtime supporting capability providers are incomplete, unsorted, "
        "or duplicate",
        "runtime_supporting_provider_path_invalid");
  }
  if (!value.control_path.has_value() && !value.capability_provider_path.has_value()) {
    return invalid("runtime participant has no control or provider path",
                   "runtime_participant_path_missing");
  }
  if (!sorted_unique(value.runtime_unit_epochs,
                     [](const auto& item) { return item.runtime_unit_id; }) ||
      std::any_of(value.runtime_unit_epochs.begin(), value.runtime_unit_epochs.end(),
                  [](const auto& item) {
                    return item.runtime_unit_id.empty() || item.runtime_epoch.empty();
                  })) {
    return invalid("runtime participant epochs are incomplete, unsorted, or duplicate",
                   "runtime_epoch_binding_invalid");
  }
  if (value.command_schema.has_value() && (value.command_schema->command_schema_id.empty() ||
                                           value.command_schema->command_schema_revision.empty())) {
    return invalid("runtime participant command schema is incomplete",
                   "runtime_command_schema_invalid");
  }
  return Result<RuntimeParticipantBinding>::Ok(RuntimeParticipantBinding(std::move(value)));
}

Result<ObservedStateQueryContextDTO> make_observed_state_query_context(
    const RuntimeParticipantBinding& binding, std::int64_t freshness_budget_ns) {
  const auto& value = binding.value();
  if (!value.control_path.has_value()) {
    return Result<ObservedStateQueryContextDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "runtime participant has no observed-state control path",
                 {{"reason", "observed_state_control_path_missing"}}});
  }
  const auto& path = *value.control_path;
  std::vector<ResourceId> resources;
  std::vector<std::string> joint_names;
  std::vector<CoordinateKind> coordinate_kinds;
  resources.reserve(value.resources.size());
  joint_names.reserve(value.resources.size());
  coordinate_kinds.reserve(value.resources.size());
  for (const auto& resource : value.resources) {
    resources.push_back(resource.resource_id);
    joint_names.push_back(resource.compiled_resource_name);
    coordinate_kinds.push_back(resource.coordinate_kind);
  }
  return Result<ObservedStateQueryContextDTO>::Ok(
      ObservedStateQueryContextDTO{value.controllable_component_id,
                                   value.command_target_id,
                                   value.capability_id,
                                   path.control_domain_id,
                                   path.control_domain_revision,
                                   path.control_binding_id,
                                   path.control_binding_revision,
                                   path.controller_topology_id,
                                   path.topology_revision,
                                   path.control_route_id,
                                   std::move(resources),
                                   std::move(joint_names),
                                   std::move(coordinate_kinds),
                                   path.control_interface_contract_id,
                                   path.control_interface_revision,
                                   path.active_control_configuration_id,
                                   path.active_control_configuration_generation,
                                   path.provider_instance_id,
                                   path.provider_manifest_id,
                                   path.provider_manifest_revision,
                                   path.provider_runtime_epoch,
                                   path.state_authority_id,
                                   path.state_authority_class,
                                   freshness_budget_ns});
}

}  // namespace manyforge::core
