#include <algorithm>
#include <cstdint>
#include <limits>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

template <typename Value>
[[nodiscard]] Result<Value> invalid(std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{std::string(to_string(ErrorCode::kInvariantViolation)),
               "runtime participant Stop readiness is not an exact reconciled product",
               {{"reason", std::move(reason)}}});
}

[[nodiscard]] std::vector<ResourceId> participant_resources(
    const RuntimeParticipantBindingDTO& participant) {
  std::vector<ResourceId> resources;
  resources.reserve(participant.resources.size());
  for (const auto& row : participant.resources) {
    resources.push_back(row.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  return resources;
}

[[nodiscard]] bool nominal_scope_matches(const ResolvedStopBasePlanDTO& plan,
                                         const RuntimeControlPathDTO& control_path,
                                         const std::vector<ResourceId>& resources) {
  if (plan.escalation_stages.empty()) {
    return false;
  }
  const auto& nominal = plan.escalation_stages.front();
  if (nominal.containment_granularity != control_path.containment_granularity) {
    return false;
  }
  return std::visit(
      [&](const auto& scope) {
        using Scope = std::decay_t<decltype(scope)>;
        if (scope.activation_set_id != control_path.activation_set_id) {
          return false;
        }
        if constexpr (std::is_same_v<Scope, ResolvedStopResourceContainmentScopeDTO>) {
          return scope.resource_ids == resources;
        } else if constexpr (std::is_same_v<Scope, ResolvedStopControlDomainContainmentScopeDTO>) {
          return scope.control_domain_id == control_path.control_domain_id &&
                 scope.control_domain_revision == control_path.control_domain_revision;
        } else {
          return !scope.controllers.empty();
        }
      },
      nominal.containment_scope);
}

[[nodiscard]] bool nominal_runtime_target_matches(const ResolvedStopBasePlanDTO& plan,
                                                  const RuntimeParticipantBindingDTO& binding,
                                                  const RuntimeControlPathDTO& control_path) {
  if (plan.escalation_stages.empty()) {
    return false;
  }
  const auto& target = plan.escalation_stages.front().command_target;
  if (target.provider_instance_id != control_path.provider_instance_id ||
      target.provider_manifest_id != control_path.provider_manifest_id ||
      target.provider_manifest_revision != control_path.provider_manifest_revision ||
      target.runtime_unit_id != control_path.provider_runtime_unit_id ||
      target.provider_runtime_epoch != control_path.provider_runtime_epoch) {
    return false;
  }
  return std::count_if(binding.runtime_unit_epochs.begin(), binding.runtime_unit_epochs.end(),
                       [&](const RuntimeUnitEpochBindingDTO& row) {
                         return row.runtime_unit_id == target.runtime_unit_id &&
                                row.runtime_epoch == target.runtime_unit_epoch;
                       }) == 1;
}

[[nodiscard]] bool expected_controller_domain_matches(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const RuntimeParticipantBindingDTO& binding, const RuntimeControlPathDTO& control_path,
    const ResolvedStopBasePlanDTO& plan) {
  if (!expected.locally_valid() || expected.control_domain_id != control_path.control_domain_id ||
      expected.control_domain_revision != control_path.control_domain_revision ||
      expected.controller_topology_id != control_path.controller_topology_id ||
      expected.topology_revision != control_path.topology_revision ||
      expected.desired_configuration.activation_set_id != control_path.activation_set_id ||
      expected.source_runtime_unit_id != plan.source_runtime_unit_id ||
      expected.source_runtime_epoch != plan.source_runtime_epoch ||
      std::count_if(binding.runtime_unit_epochs.begin(), binding.runtime_unit_epochs.end(),
                    [&](const RuntimeUnitEpochBindingDTO& row) {
                      return row.runtime_unit_id == expected.source_runtime_unit_id &&
                             row.runtime_epoch == expected.source_runtime_epoch;
                    }) != 1) {
    return false;
  }

  const auto desired_route =
      std::find_if(expected.desired_configuration.binding_routes.begin(),
                   expected.desired_configuration.binding_routes.end(), [&](const auto& route) {
                     return route.control_binding_id == control_path.control_binding_id;
                   });
  const auto expected_binding =
      std::find_if(expected.expected_bindings.begin(), expected.expected_bindings.end(),
                   [&](const auto& candidate) {
                     return candidate.control_binding_id == control_path.control_binding_id;
                   });
  if (desired_route == expected.desired_configuration.binding_routes.end() ||
      desired_route->control_binding_revision != control_path.control_binding_revision ||
      desired_route->control_route_id != control_path.control_route_id ||
      expected_binding == expected.expected_bindings.end() ||
      expected_binding->control_binding_revision != control_path.control_binding_revision ||
      expected_binding->control_interface_contract_id !=
          control_path.control_interface_contract_id ||
      expected_binding->control_interface_revision != control_path.control_interface_revision ||
      expected_binding->gripper_actuation_kind != plan.gripper_actuation_kind) {
    return false;
  }
  return std::any_of(expected.expected_endpoints.begin(), expected.expected_endpoints.end(),
                     [&](const auto& endpoint) {
                       return endpoint.control_binding_id == control_path.control_binding_id &&
                              endpoint.control_binding_revision ==
                                  control_path.control_binding_revision &&
                              endpoint.control_route_id == control_path.control_route_id;
                     });
}

}  // namespace

Result<RuntimeParticipantStopReadiness> verify_runtime_participant_stop_readiness(
    RuntimeParticipantStopReadinessDTO value, const RuntimeParticipantBinding& participant,
    const ControllerLifecycleExpectedDomain& expected_controller_domain) {
  constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;
  const auto& binding = participant.value();
  const auto& plan = value.resolved_stop_base_plan;
  if (value.expected_controller_domain != expected_controller_domain.value()) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_controller_domain_referent_mismatch");
  }
  if (value.controllable_component_id.empty() ||
      value.controllable_component_id != binding.controllable_component_id ||
      !value.component_lifecycle_generation.nonzero() ||
      value.component_lifecycle_generation.value() > kMaximumIJsonInteger) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_component_generation_invalid");
  }
  if (!binding.control_path.has_value()) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_control_path_missing");
  }
  if (!VerifyResolvedStopBasePlan(plan).ok()) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_base_plan_unverified");
  }

  const auto& path = *binding.control_path;
  if (plan.retained_bundle_id != binding.retained_bundle_id ||
      plan.graph_revision != binding.graph_revision ||
      plan.control_domain_id != path.control_domain_id ||
      plan.control_domain_revision != path.control_domain_revision ||
      plan.control_binding_id != path.control_binding_id ||
      plan.control_binding_revision != path.control_binding_revision ||
      plan.controller_topology_id != path.controller_topology_id ||
      plan.topology_revision != path.topology_revision ||
      plan.control_route_id != path.control_route_id ||
      plan.activation_set_id != path.activation_set_id ||
      plan.active_control_configuration_id != path.active_control_configuration_id ||
      plan.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      plan.stop_evidence_policy_id != path.stop_evidence_policy_id ||
      plan.stop_evidence_policy_revision != path.stop_evidence_policy_revision) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_base_plan_join_invalid");
  }
  if (!expected_controller_domain_matches(value.expected_controller_domain, binding, path, plan)) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_controller_domain_join_invalid");
  }

  const auto resources = participant_resources(binding);
  if (resources.empty() || !nominal_scope_matches(plan, path, resources)) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_nominal_scope_invalid");
  }
  if (!nominal_runtime_target_matches(plan, binding, path)) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_nominal_target_invalid");
  }
  return Result<RuntimeParticipantStopReadiness>::Ok(
      RuntimeParticipantStopReadiness(std::move(value)));
}

Result<RuntimeParticipantStopReadiness> rebind_runtime_participant_stop_readiness_generation(
    const RuntimeParticipantStopReadiness& verified, const ComponentLifecycleGeneration generation) {
  constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;
  if (!generation.nonzero() || generation.value() > kMaximumIJsonInteger) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_component_generation_invalid");
  }
  auto value = verified.value();
  value.component_lifecycle_generation = generation;
  return Result<RuntimeParticipantStopReadiness>::Ok(
      RuntimeParticipantStopReadiness(std::move(value)));
}

Result<RuntimeParticipantStopReadiness> rebind_runtime_participant_stop_readiness_product(
    const RuntimeParticipantStopReadiness& verified,
    const RuntimeParticipantBinding& predecessor,
    const RuntimeParticipantBinding& candidate,
    const ControllerLifecycleExpectedDomain& expected_domain) {
  const auto& old_binding = predecessor.value();
  const auto& new_binding = candidate.value();
  if (old_binding.controllable_component_id !=
          new_binding.controllable_component_id ||
      old_binding.component_type_id != new_binding.component_type_id ||
      old_binding.component_type_revision !=
          new_binding.component_type_revision ||
      old_binding.command_target_id != new_binding.command_target_id ||
      old_binding.capability_id != new_binding.capability_id ||
      old_binding.capability_definition_revision !=
          new_binding.capability_definition_revision ||
      old_binding.model_bindings != new_binding.model_bindings ||
      old_binding.model_groups != new_binding.model_groups ||
      old_binding.frame_bindings != new_binding.frame_bindings ||
      old_binding.resources != new_binding.resources ||
      old_binding.control_path != new_binding.control_path ||
      old_binding.capability_provider_path !=
          new_binding.capability_provider_path ||
      old_binding.supporting_capability_provider_paths !=
          new_binding.supporting_capability_provider_paths ||
      old_binding.runtime_unit_epochs != new_binding.runtime_unit_epochs) {
    return invalid<RuntimeParticipantStopReadiness>(
        "runtime_participant_stop_product_rebind_changes_controller");
  }
  const auto predecessor_check = verify_runtime_participant_stop_readiness(
      verified.value(), predecessor, expected_domain);
  if (!predecessor_check.ok()) {
    return Result<RuntimeParticipantStopReadiness>::Err(predecessor_check.error());
  }
  auto value = verified.value();
  auto plan = value.resolved_stop_base_plan;
  plan.retained_bundle_id = candidate.value().retained_bundle_id;
  plan.graph_revision = candidate.value().graph_revision;
  const auto sealed_plan = MakeResolvedStopBasePlan(std::move(plan));
  if (!sealed_plan.ok()) {
    return Result<RuntimeParticipantStopReadiness>::Err(sealed_plan.error());
  }
  value.resolved_stop_base_plan = sealed_plan.value();
  return verify_runtime_participant_stop_readiness(std::move(value), candidate,
                                                   expected_domain);
}

}  // namespace manyforge::core
