#include <algorithm>
#include <cmath>
#include <manyforge_core/program_frame_identity.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <map>
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
      !path.provider_manifest_revision.empty() && !path.provider_runtime_unit_id.empty() &&
      !path.provider_runtime_epoch.empty() && !path.state_authority_id.empty() &&
      !path.stop_evidence_policy_id.empty() && !path.stop_evidence_policy_revision.empty();
  if (!base_valid || !path.runtime_scale_qualification.has_value()) {
    return base_valid;
  }
  const auto& scale = *path.runtime_scale_qualification;
  return scale.control_route_id == path.control_route_id && !scale.scaling_domain_id.empty() &&
         !scale.command_endpoint_role_id.empty() && !scale.applied_state_endpoint_role_id.empty() &&
         (!scale.coordinated_barrier_id.has_value() || !scale.coordinated_barrier_id->empty()) &&
         std::isfinite(scale.maximum_slew_per_second) && scale.maximum_slew_per_second > 0.0 &&
         scale.freshness_budget_ns > 0 && scale.freshness_budget_ns <= 9007199254740991LL &&
         scale.acknowledgement_kind == RuntimeScaleAcknowledgementKind::kAppliedState;
}

[[nodiscard]] bool valid_provider_path(const RuntimeCapabilityProviderPathDTO& path) noexcept {
  return !path.capability_id.empty() && !path.capability_definition_revision.empty() &&
         !path.capability_provider_binding_id.empty() &&
         !path.capability_provider_binding_revision.empty() && !path.provider_instance_id.empty() &&
         !path.provider_manifest_id.empty() && !path.provider_manifest_revision.empty() &&
         !path.provider_runtime_unit_id.empty() && !path.provider_runtime_epoch.empty();
}

template <typename Available, typename Selected, typename Matches>
[[nodiscard]] bool is_unambiguous_subset(
    const std::vector<Available>& available,
    const std::vector<Selected>& selected,
    Matches matches) noexcept {
  return std::all_of(selected.begin(), selected.end(), [&](const auto& value) {
    return std::count_if(available.begin(), available.end(),
                         [&](const auto& candidate) {
                           return matches(candidate, value);
                         }) == 1;
  });
}

}  // namespace

bool runtime_participant_binding_physically_contains(
    const RuntimeParticipantBinding& physical,
    const RuntimeParticipantBinding& selected) noexcept {
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
         is_unambiguous_subset(
             available.model_bindings, value.model_bindings,
             [](const auto& left, const auto& right) {
               return left.model_instance_id == right.model_instance_id &&
                      left.model_revision == right.model_revision;
             }) &&
         is_unambiguous_subset(
             available.model_groups, value.model_groups,
             [](const auto& left, const auto& right) {
               return left.model_instance_id == right.model_instance_id &&
                      left.compiled_group_name == right.compiled_group_name &&
                      left.expanded_compiled_joint_names ==
                          right.expanded_compiled_joint_names;
             }) &&
         is_unambiguous_subset(
             available.frame_bindings, value.frame_bindings,
             [](const auto& left, const auto& right) {
               return same_program_frame(left, right);
             }) &&
         is_unambiguous_subset(
             available.resources, value.resources,
             [](const auto& left, const auto& right) {
               return left.resource_id == right.resource_id &&
                      left.source_joint_name == right.source_joint_name &&
                      left.compiled_resource_name ==
                          right.compiled_resource_name &&
                      left.coordinate_kind == right.coordinate_kind;
             }) &&
         is_unambiguous_subset(
             available.collision_body_bindings,
             value.collision_body_bindings,
             [](const auto& left, const auto& right) {
               return left.model_instance_id == right.model_instance_id &&
                      left.model_revision == right.model_revision &&
                      left.compiled_collision_body_names ==
                          right.compiled_collision_body_names;
             }) &&
         available.control_path == value.control_path &&
         available.capability_provider_path == value.capability_provider_path &&
         is_unambiguous_subset(
             available.supporting_capability_provider_paths,
             value.supporting_capability_provider_paths,
             [](const auto& left, const auto& right) { return left == right; }) &&
         available.runtime_unit_epochs == value.runtime_unit_epochs;
}

Result<ParticipantBindingIdentityDTO> runtime_participant_binding_identity(
    const RuntimeParticipantBindingDTO& binding) {
  std::optional<CapabilityProviderBindingIdentityDTO> provider;
  if (binding.capability_provider_path) {
    provider = CapabilityProviderBindingIdentityDTO{
        binding.capability_provider_path->capability_provider_binding_id,
        binding.capability_provider_path->capability_provider_binding_revision};
  }
  if (binding.control_path) {
    const auto& path = *binding.control_path;
    ParticipantBindingIdentityDTO result{path.control_domain_id, path.control_domain_revision,
        ControllerParticipantBindingIdentityDTO{path.control_binding_id, path.control_binding_revision,
                                                std::move(provider)}};
    if (result.locally_valid()) return Result<ParticipantBindingIdentityDTO>::Ok(std::move(result));
  }
  return Result<ParticipantBindingIdentityDTO>::Err({"invalid_request",
      "runtime participant has no exact execution binding identity",
      {{"reason", "runtime_participant_binding_identity_invalid"}}});
}

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
  if ((!value.model_groups.empty() && !unique_nonempty(value.model_groups,
                                                       [](const auto& item) {
                                                         return std::pair(item.model_group_role_id,
                                                                          item.model_instance_id);
                                                       })) ||
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
  if ((!value.frame_bindings.empty() &&
       !unique_nonempty(value.frame_bindings, [](const auto& item) { return item.frame_role_id; })) ||
      std::any_of(value.frame_bindings.begin(), value.frame_bindings.end(), [](const auto& item) {
        return item.frame_role_id.empty() || item.model_instance_id.empty() ||
               item.model_revision.empty() || item.compiled_frame_name.empty();
      })) {
    return invalid("runtime participant frame bindings are incomplete or duplicate",
                   "runtime_frame_binding_invalid");
  }
  if (value.motion_endpoint) {
    const auto& endpoint = *value.motion_endpoint;
    const auto& frame = endpoint.frame;
    const auto& roles = frame.contact_body_role_ids;
    if (!value.control_path || value.model_groups.size() != 1U || endpoint.target_role_id.empty() ||
        endpoint.command_target_id.empty() || frame.frame_role_id.empty() ||
        frame.model_instance_id.empty() || frame.model_revision.empty() ||
        frame.compiled_frame_name.empty() ||
        // TCP-D: the frame may be on a tool mounted on the motion component;
        // the resolver proves that owner against the cell graph.
        roles.size() > kMaxProgramNestedRoleBindings ||
        !sorted_unique(roles, [](const auto& role) { return role; }) ||
        std::any_of(roles.begin(), roles.end(), [](const auto& role) { return role.empty(); }) ||
        endpoint.contact_body_bindings.size() != roles.size()) {
      return invalid("motion endpoint is outside the exact motion component/model",
                     "runtime_motion_endpoint_invalid");
    }
    for (std::size_t index = 0; index < roles.size(); ++index) {
      const auto& body = endpoint.contact_body_bindings[index];
      if (body.component_role_id.empty() || body.target_role_id != endpoint.target_role_id ||
          body.controllable_component_id.empty() ||
          body.command_target_id != endpoint.command_target_id ||
          (body.model_role_id && body.model_role_id->empty()) ||
          body.collision_body_role_id != roles[index] ||
          body.model_instance_id != frame.model_instance_id ||
          body.model_revision != frame.model_revision || body.collision_body_ids.empty() ||
          body.collision_body_ids.size() > kMaxProgramNestedRoleBindings) {
        return invalid("motion endpoint body is outside its exact frame owner/model",
                       "runtime_motion_endpoint_body_invalid");
      }
      for (std::size_t body_index = 0; body_index < body.collision_body_ids.size(); ++body_index) {
        if (body.collision_body_ids[body_index] !=
            make_component_collision_body_id(body.controllable_component_id,
                                             frame.model_instance_id, roles[index], body_index)) {
          return invalid("motion endpoint body does not retain its exact semantic identity",
                         "runtime_motion_endpoint_body_invalid");
        }
      }
    }
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
  const auto has_exact_provider_runtime = [&](const auto& provider) {
    return std::count_if(value.runtime_unit_epochs.begin(), value.runtime_unit_epochs.end(),
                         [&](const auto& occurrence) {
                           return occurrence.runtime_unit_id == provider.provider_runtime_unit_id;
                         }) == 1;
  };
  if ((value.control_path.has_value() && !has_exact_provider_runtime(*value.control_path)) ||
      (value.capability_provider_path.has_value() &&
       !has_exact_provider_runtime(*value.capability_provider_path)) ||
      !std::all_of(value.supporting_capability_provider_paths.begin(),
                   value.supporting_capability_provider_paths.end(), has_exact_provider_runtime)) {
    return invalid("runtime capability provider does not join one exact runtime-unit epoch",
                   "runtime_provider_epoch_join_invalid");
  }
  if (value.command_schema.has_value() && (value.command_schema->command_schema_id.empty() ||
                                           value.command_schema->command_schema_revision.empty())) {
    return invalid("runtime participant command schema is incomplete",
                   "runtime_command_schema_invalid");
  }
  if (!sorted_unique(value.contact_grasp_bindings,
                     [](const auto& policy) { return policy.contact_policy_id; }) ||
      std::any_of(
          value.contact_grasp_bindings.begin(), value.contact_grasp_bindings.end(),
          [&](const auto& policy) {
            if (policy.contact_policy_id.empty() || policy.contact_policy_revision.empty() ||
                policy.runtime_object_id.empty() ||
                policy.bodies_from_motion_endpoint != policy.allowed_body_bindings.empty() ||
                !valid_program_contact_envelope(policy.envelope) ||
                !sorted_unique(policy.allowed_body_bindings, [](const auto& body) {
                  return std::tie(body.component_role_id, body.target_role_id, body.model_role_id,
                                  body.collision_body_role_id);
                })) {
              return true;
            }
            return std::any_of(
                policy.allowed_body_bindings.begin(), policy.allowed_body_bindings.end(),
                [&](const auto& body) {
                  if (body.component_role_id.empty() || body.target_role_id.empty() ||
                      body.controllable_component_id.empty() || body.command_target_id.empty() ||
                      (body.model_role_id.has_value() && body.model_role_id->empty()) ||
                      body.collision_body_role_id.empty() || body.model_instance_id.empty() ||
                      body.model_revision.empty() || body.collision_body_ids.empty()) {
                    return true;
                  }
                  for (std::size_t index = 0U; index < body.collision_body_ids.size(); ++index) {
                    if (body.collision_body_ids[index] !=
                        make_component_collision_body_id(body.controllable_component_id,
                                                         body.model_instance_id,
                                                         body.collision_body_role_id, index)) {
                      return true;
                    }
                  }
                  return false;
                });
          })) {
    return invalid(
        "runtime contact-grasp bindings are incomplete, unsorted, "
        "or inconsistent",
        "runtime_contact_grasp_binding_invalid");
  }
  return Result<RuntimeParticipantBinding>::Ok(RuntimeParticipantBinding(std::move(value)));
}

Result<RuntimeParticipantBinding> bind_runtime_participant_collision_body_aliases(
    RuntimeParticipantBinding binding,
    const std::span<const RuntimeCollisionBodyAliasDTO> aliases) {
  if (aliases.empty() ||
      !sorted_unique(aliases, [](const auto& alias) { return alias.semantic_body_id; }) ||
      std::any_of(aliases.begin(), aliases.end(), [](const auto& alias) {
        return alias.semantic_body_id.empty() || alias.projected_body_id.empty();
      })) {
    return invalid(
        "runtime collision-body alias table is incomplete or "
        "ambiguous",
        "runtime_collision_body_alias_table_invalid");
  }

  std::map<CollisionBodyId, CollisionBodyId> by_semantic;
  for (const auto& alias : aliases) {
    by_semantic.emplace(alias.semantic_body_id, alias.projected_body_id);
  }
  if (std::any_of(aliases.begin(), aliases.end(), [&](const auto& alias) {
        const auto physical = by_semantic.find(alias.projected_body_id);
        return physical == by_semantic.end() || physical->second != alias.projected_body_id;
      })) {
    return invalid(
        "runtime collision-body alias table has no exact physical "
        "fixed point",
        "runtime_collision_body_alias_table_invalid");
  }

  auto value = binding.value();
  const auto project_bodies = [&](auto& bodies) {
    for (auto& body : bodies) {
      std::set<std::string, std::less<>> projected_ids;
      for (auto& body_id : body.collision_body_ids) {
        const auto alias = by_semantic.find(CollisionBodyId(body_id));
        if (alias == by_semantic.end() || !projected_ids.insert(alias->second).second) {
          return false;
        }
        body_id = alias->second;
      }
    }
    return true;
  };
  for (auto& policy : value.contact_grasp_bindings) {
    if (!project_bodies(policy.allowed_body_bindings)) {
      return invalid("runtime contact-grasp body has no unique exact physical projection",
                     "runtime_collision_body_alias_unresolved");
    }
  }
  if (value.motion_endpoint && !project_bodies(value.motion_endpoint->contact_body_bindings)) {
    return invalid("runtime motion-endpoint body has no unique exact physical projection",
                   "runtime_collision_body_alias_unresolved");
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
