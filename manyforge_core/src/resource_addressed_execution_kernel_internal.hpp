#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/detail/motion_resume_intent.hpp>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include "stop_terminal_reducer_internal.hpp"
#include <manyforge_core/component_lifecycle_admission.hpp>

#include "component_lifecycle_journal_codec_internal.hpp"
#include <manyforge_core/types/capability_definition.hpp>
#include <numeric>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace resource_addressed_kernel_detail {

template <typename Function>
class ScopeExit final {
 public:
  explicit ScopeExit(Function function) : function_(std::move(function)) {}
  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ~ScopeExit() { function_(); }

 private:
  Function function_;
};

template <typename Function>
ScopeExit(Function) -> ScopeExit<Function>;

[[nodiscard]] inline bool is_active_stop_fault_latch(
    const std::optional<StopFaultLatchDTO>& latch) noexcept {
  return latch.has_value() && latch->state != StopFaultLatchStateDTO::kCleared;
}

[[nodiscard]] inline ErrorDTO error(std::string code, std::string message, std::string reason) {
  return ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

template <typename T>
[[nodiscard]] inline Result<T> reject(std::string message, std::string reason,
                               ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<T>::Err(error(std::string(to_string(code)), std::move(message), std::move(reason)));
}

template <typename Writer>
[[nodiscard]] inline Result<std::uint64_t> attempt_journal_write(Writer&& write) {
  try {
    return write();
  } catch (const std::exception& exception) {
    return Result<std::uint64_t>::Err(
        error("operation_journal_exception", exception.what(), "operation_journal_exception"));
  } catch (...) {
    return Result<std::uint64_t>::Err(
        error("operation_journal_exception", "journal write threw", "operation_journal_exception"));
  }
}

[[nodiscard]] inline std::vector<RuntimeParticipantBinding> extract_bindings(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::vector<RuntimeParticipantBinding> bindings;
  bindings.reserve(registrations.size());
  for (const auto& registration : registrations) {
    if (registration.engine == nullptr) {
      throw std::invalid_argument("runtime participant registration has no execution engine");
    }
    if (registration.stop_readiness.has_value() && registration.stop_execution_port == nullptr) {
      throw std::invalid_argument(
          "Stop-qualified runtime participant registration has no Stop-stage port");
    }
    if (registration.stop_readiness.has_value() &&
        registration.stop_readiness->value().controllable_component_id !=
            registration.binding.value().controllable_component_id) {
      throw std::invalid_argument(
          "runtime participant registration Stop readiness crosses its binding");
    }
    if (registration.participant_policy.has_value() &&
        (registration.participant_policy->value().controllable_component_id !=
             registration.binding.value().controllable_component_id ||
         registration.participant_policy->value().command_target_id !=
             registration.binding.value().command_target_id)) {
      throw std::invalid_argument(
          "runtime participant registration policy selection crosses its binding");
    }
    if (registration.participant_policy.has_value() &&
        registration.prepared_motion_engine == nullptr) {
      throw std::invalid_argument(
          "motion runtime participant registration has no prepared execution engine");
    }
    bindings.push_back(registration.binding);
  }
  return bindings;
}

[[nodiscard]] inline std::size_t qualified_execution_engine_count(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::set<const IResolvedParticipantExecutionEngine*> engines;
  for (const auto& registration : registrations) {
    if (registration.stop_readiness.has_value() && registration.stop_execution_port != nullptr) {
      engines.insert(registration.engine);
    }
  }
  return engines.size();
}

[[nodiscard]] inline std::vector<IResolvedParticipantExecutionEngine*> distinct_execution_engines(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::vector<IResolvedParticipantExecutionEngine*> engines;
  engines.reserve(registrations.size());
  for (const auto& registration : registrations) {
    if (std::find(engines.begin(), engines.end(), registration.engine) == engines.end()) {
      engines.push_back(registration.engine);
    }
  }
  return engines;
}

[[nodiscard]] inline std::vector<DomainActiveControlConfigurationDTO>
extract_active_control_configurations(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::vector<DomainActiveControlConfigurationDTO> configurations;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value()) {
      continue;
    }
    const auto& control = *binding.control_path;
    configurations.push_back(DomainActiveControlConfigurationDTO{
        control.control_domain_id, control.active_control_configuration_id,
        control.active_control_configuration_generation});
  }
  std::sort(configurations.begin(), configurations.end(), [](const auto& left, const auto& right) {
    if (left.control_domain_id != right.control_domain_id) {
      return left.control_domain_id < right.control_domain_id;
    }
    if (left.active_control_configuration_id != right.active_control_configuration_id) {
      return left.active_control_configuration_id < right.active_control_configuration_id;
    }
    return left.active_control_configuration_generation <
           right.active_control_configuration_generation;
  });
  const auto unique = std::unique(configurations.begin(), configurations.end());
  configurations.erase(unique, configurations.end());
  if (std::adjacent_find(configurations.begin(), configurations.end(),
                         [](const auto& left, const auto& right) {
                           return left.control_domain_id == right.control_domain_id;
                         }) != configurations.end()) {
    throw std::invalid_argument(
        "runtime participants disagree on one active control configuration");
  }
  return configurations;
}

[[nodiscard]] inline bool domain_has_complete_stop_readiness(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations,
    const ControlDomainId& control_domain_id) noexcept {
  bool found = false;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value() ||
        binding.control_path->control_domain_id != control_domain_id) {
      continue;
    }
    found = true;
    if (!registration.stop_readiness.has_value()) {
      return false;
    }
  }
  return found;
}

[[nodiscard]] inline std::vector<DomainActiveControlConfigurationDTO>
extract_published_active_control_configurations(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  auto configurations = extract_active_control_configurations(registrations);
  configurations.erase(std::remove_if(configurations.begin(), configurations.end(),
                                      [&](const auto& configuration) {
                                        return !domain_has_complete_stop_readiness(
                                            registrations, configuration.control_domain_id);
                                      }),
                       configurations.end());
  return configurations;
}

[[nodiscard]] inline std::vector<DomainActiveControlConfigurationDTO> route_publication_identities(
    const std::map<ControlDomainId, RuntimeDomainConfigurationReadbackDTO>& readbacks,
    const RuntimeRouteConfigurationReadbackDTO* replacement = nullptr) {
  std::vector<DomainActiveControlConfigurationDTO> result;
  result.reserve(readbacks.size() + (replacement != nullptr ? 1U : 0U));
  for (const auto& [domain_id, readback] : readbacks) {
    if (readback.provider() != nullptr ||
        (replacement != nullptr &&
         domain_id == replacement->active_configuration.control_domain_id)) {
      continue;
    }
    result.push_back(DomainActiveControlConfigurationDTO{
        domain_id, readback.controller().active_configuration.active_control_configuration_id,
        readback.controller().active_configuration.active_control_configuration_generation});
  }
  if (replacement != nullptr) {
    result.push_back(DomainActiveControlConfigurationDTO{
        replacement->active_configuration.control_domain_id,
        replacement->active_configuration.active_control_configuration_id,
        replacement->active_configuration.active_control_configuration_generation});
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.control_domain_id < rhs.control_domain_id;
    });
  }
  return result;
}

[[nodiscard]] inline Result<std::vector<RouteReadinessPublicationRowDTO>>
derive_route_readiness_projection(
    const ActiveControlConfigurationDTO& configuration,
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  if (!configuration.locally_valid() ||
      configuration.state != ActiveControlConfigurationStateDTO::kReconciled) {
    return reject<std::vector<RouteReadinessPublicationRowDTO>>(
        "route readiness requires one reconciled active configuration",
        "runtime_route_configuration_unreconciled", ErrorCode::kInvalidRequest);
  }

  std::map<ControlBindingId, RouteReadinessPublicationRowDTO> rows_by_binding;
  const auto desired_routes_match = [&](const ControllerLifecycleExpectedDomainDTO& expected) {
    if (expected.desired_configuration.binding_routes.size() !=
        configuration.binding_routes.size()) {
      return false;
    }
    for (std::size_t index = 0U; index < configuration.binding_routes.size(); ++index) {
      const auto& desired = expected.desired_configuration.binding_routes[index];
      const auto& active = configuration.binding_routes[index];
      if (desired.control_binding_id != active.control_binding_id ||
          desired.control_binding_revision != active.control_binding_revision ||
          desired.control_route_id != active.control_route_id) {
        return false;
      }
    }
    return true;
  };
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value() ||
        binding.control_path->control_domain_id != configuration.control_domain_id) {
      continue;
    }
    const auto& path = *binding.control_path;
    const auto selected = std::find_if(
        configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& route) { return route.control_binding_id == path.control_binding_id; });
    if (selected == configuration.binding_routes.end() ||
        selected->control_binding_revision != path.control_binding_revision ||
        selected->control_route_id != path.control_route_id ||
        path.control_domain_revision != configuration.control_domain_revision ||
        path.controller_topology_id != configuration.controller_topology_id ||
        path.topology_revision != configuration.topology_revision ||
        path.activation_set_id != configuration.activation_set_id ||
        path.active_control_configuration_id != configuration.active_control_configuration_id ||
        path.active_control_configuration_generation !=
            configuration.active_control_configuration_generation ||
        !registration.stop_readiness.has_value()) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime registration differs from the published route configuration",
          "runtime_route_registration_mismatch", ErrorCode::kInvariantViolation);
    }
    const auto& readiness = registration.stop_readiness->value();
    const auto& expected = readiness.expected_controller_domain;
    const auto& plan = readiness.resolved_stop_base_plan;
    if (expected.control_domain_id != configuration.control_domain_id ||
        expected.control_domain_revision != configuration.control_domain_revision ||
        expected.controller_topology_id != configuration.controller_topology_id ||
        expected.topology_revision != configuration.topology_revision ||
        expected.desired_configuration.activation_set_id != configuration.activation_set_id ||
        !desired_routes_match(expected) ||
        expected.source_runtime_unit_id != configuration.source_runtime_unit_id ||
        expected.source_runtime_epoch != configuration.source_runtime_epoch ||
        plan.active_control_configuration_id != configuration.active_control_configuration_id ||
        plan.active_control_configuration_generation !=
            configuration.active_control_configuration_generation ||
        plan.source_runtime_unit_id != configuration.source_runtime_unit_id ||
        plan.source_runtime_epoch != configuration.source_runtime_epoch ||
        plan.source_sequence != configuration.source_sequence) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime Stop readiness differs from the published route configuration",
          "runtime_route_readiness_mismatch", ErrorCode::kInvariantViolation);
    }

    RouteReadinessPublicationRowDTO row{
        binding.controllable_component_id, readiness.component_lifecycle_generation,
        path.control_binding_id, path.control_binding_revision, plan.resolved_stop_base_plan_id};
    const auto found = rows_by_binding.find(path.control_binding_id);
    if (found == rows_by_binding.end()) {
      rows_by_binding.emplace(path.control_binding_id, std::move(row));
    } else if (found->second != row) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime aliases disagree on one selected binding readiness row",
          "runtime_route_alias_readiness_mismatch", ErrorCode::kInvariantViolation);
    }
  }

  std::vector<RouteReadinessPublicationRowDTO> rows;
  rows.reserve(rows_by_binding.size());
  for (const auto& selected : configuration.binding_routes) {
    const auto found = rows_by_binding.find(selected.control_binding_id);
    if (found == rows_by_binding.end() ||
        found->second.control_binding_revision != selected.control_binding_revision) {
      return reject<std::vector<RouteReadinessPublicationRowDTO>>(
          "runtime readiness does not cover every selected control binding",
          "runtime_route_readiness_incomplete", ErrorCode::kMissingReference);
    }
    rows.push_back(found->second);
  }
  std::sort(rows.begin(), rows.end(), CanonicalRouteReadinessPublicationRowLess);
  if (rows.size() != configuration.binding_routes.size() ||
      !std::all_of(rows.begin(), rows.end(), [](const auto& row) { return row.locally_valid(); })) {
    return reject<std::vector<RouteReadinessPublicationRowDTO>>(
        "runtime readiness projection has an invalid closed shape",
        "runtime_route_readiness_invalid", ErrorCode::kInvariantViolation);
  }
  return Result<std::vector<RouteReadinessPublicationRowDTO>>::Ok(std::move(rows));
}

[[nodiscard]] inline bool registration_truth_equal(
    const RuntimeParticipantExecutionRegistration& left,
    const RuntimeParticipantExecutionRegistration& right) noexcept {
  const bool readiness_equal =
      left.stop_readiness.has_value() == right.stop_readiness.has_value() &&
      (!left.stop_readiness.has_value() ||
       left.stop_readiness->value() == right.stop_readiness->value());
  const bool participant_policy_equal =
      left.participant_policy.has_value() == right.participant_policy.has_value() &&
      (!left.participant_policy.has_value() ||
       left.participant_policy->value() == right.participant_policy->value());
  return left.binding.value() == right.binding.value() && readiness_equal &&
         participant_policy_equal &&
         left.retained_default_stop_limits == right.retained_default_stop_limits &&
         left.stop_node_category == right.stop_node_category && left.engine == right.engine &&
         left.stop_execution_port == right.stop_execution_port &&
         left.prepared_motion_engine == right.prepared_motion_engine &&
         left.default_operator_stop_profile == right.default_operator_stop_profile &&
         left.default_fault_stop_profile == right.default_fault_stop_profile &&
         left.deployment_maximum_tcp_linear_speed_mps ==
             right.deployment_maximum_tcp_linear_speed_mps &&
         left.intrinsic_direct_control == right.intrinsic_direct_control &&
         left.direct_control_joint_limits == right.direct_control_joint_limits &&
         left.motion_takeover_preparation_budget_s == right.motion_takeover_preparation_budget_s &&
         left.motion_takeover_planning_budget_s == right.motion_takeover_planning_budget_s;
}

[[nodiscard]] inline bool registration_controller_equal_after_product_reseal(
    const RuntimeParticipantExecutionRegistration& left,
    const RuntimeParticipantExecutionRegistration& right) {
  const auto& a = left.binding.value();
  const auto& b = right.binding.value();
  auto candidate_controller_binding = b;
  // A retained controller may receive a fresh product seal and fresh authored
  // Program metadata during Apply. Everything that identifies its physical
  // control/runtime context remains exact. The role IDs are Program aliases;
  // their resolved target, model, frame, resource and provider facts below
  // still have to match. Contact/grasp templates are likewise authored
  // Program inputs consumed by later admitted operations, not controller state.
  candidate_controller_binding.program_id = a.program_id;
  candidate_controller_binding.program_revision = a.program_revision;
  candidate_controller_binding.program_binding_id = a.program_binding_id;
  candidate_controller_binding.retained_bundle_id = a.retained_bundle_id;
  candidate_controller_binding.graph_revision = a.graph_revision;
  candidate_controller_binding.component_role_id = a.component_role_id;
  candidate_controller_binding.target_role_id = a.target_role_id;
  candidate_controller_binding.contact_grasp_bindings = a.contact_grasp_bindings;
  candidate_controller_binding.motion_endpoint = a.motion_endpoint;
  if (a != candidate_controller_binding || left.engine != right.engine ||
      left.stop_execution_port != right.stop_execution_port ||
      left.prepared_motion_engine != right.prepared_motion_engine ||
      left.retained_default_stop_limits != right.retained_default_stop_limits ||
      left.stop_node_category != right.stop_node_category ||
      left.default_operator_stop_profile != right.default_operator_stop_profile ||
      left.default_fault_stop_profile != right.default_fault_stop_profile ||
      left.deployment_maximum_tcp_linear_speed_mps !=
          right.deployment_maximum_tcp_linear_speed_mps ||
      left.intrinsic_direct_control != right.intrinsic_direct_control ||
      left.direct_control_joint_limits != right.direct_control_joint_limits ||
      left.motion_takeover_preparation_budget_s != right.motion_takeover_preparation_budget_s ||
      left.motion_takeover_planning_budget_s != right.motion_takeover_planning_budget_s ||
      left.participant_policy.has_value() != right.participant_policy.has_value() ||
      (left.participant_policy.has_value() &&
       left.participant_policy->value() != right.participant_policy->value()) ||
      left.stop_readiness.has_value() != right.stop_readiness.has_value()) {
    return false;
  }
  if (!left.stop_readiness.has_value()) return true;
  const auto& old_readiness = left.stop_readiness->value();
  const auto& next_readiness = right.stop_readiness->value();
  if (old_readiness.controllable_component_id != next_readiness.controllable_component_id ||
      old_readiness.component_lifecycle_generation !=
          next_readiness.component_lifecycle_generation ||
      old_readiness.expected_controller_domain != next_readiness.expected_controller_domain) {
    return false;
  }
  auto old_plan = old_readiness.resolved_stop_base_plan;
  const auto& next_plan = next_readiness.resolved_stop_base_plan;
  // The content address and product lineage change when the same verified
  // Stop mechanism is re-sealed to the candidate bundle. Every executable
  // Stop fact and its route/source generation must remain identical.
  old_plan.resolved_stop_base_plan_id = next_plan.resolved_stop_base_plan_id;
  old_plan.retained_bundle_id = next_plan.retained_bundle_id;
  old_plan.graph_revision = next_plan.graph_revision;
  return old_plan == next_plan;
}

[[nodiscard]] inline std::set<ControlDomainId> changed_registration_domains(
    const std::vector<RuntimeParticipantExecutionRegistration>& current,
    const std::vector<RuntimeParticipantExecutionRegistration>& candidate) {
  std::set<ControlDomainId> domains;
  for (const auto* registrations : {&current, &candidate}) {
    for (const auto& registration : *registrations) {
      if (registration.binding.value().control_path.has_value()) {
        domains.insert(registration.binding.value().control_path->control_domain_id);
      }
    }
  }
  std::set<ControlDomainId> changed;
  for (const auto& domain : domains) {
    const auto current_count = std::count_if(current.begin(), current.end(), [&](const auto& row) {
      return row.binding.value().control_path.has_value() &&
             row.binding.value().control_path->control_domain_id == domain;
    });
    const auto candidate_count =
        std::count_if(candidate.begin(), candidate.end(), [&](const auto& row) {
          return row.binding.value().control_path.has_value() &&
                 row.binding.value().control_path->control_domain_id == domain;
        });
    if (current_count != candidate_count ||
        std::any_of(current.begin(), current.end(), [&](const auto& existing) {
          if (!existing.binding.value().control_path.has_value() ||
              existing.binding.value().control_path->control_domain_id != domain) {
            return false;
          }
          return std::count_if(candidate.begin(), candidate.end(), [&](const auto& replacement) {
                   return registration_truth_equal(existing, replacement);
                 }) != 1;
        })) {
      changed.insert(domain);
    }
  }
  return changed;
}

struct StopStaticFacts final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::vector<StopScopeComponentSnapshotDTO> components;
};

[[nodiscard]] inline StopStaticFacts derive_stop_static_facts(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations,
    const ResourceAddressedExecutionKernelConfig& config,
    const bool require_bootstrap_graph_revision = true) {
  std::optional<GraphRevision> graph_revision =
      registrations.empty() ? config.empty_runtime_graph_revision
                            : (require_bootstrap_graph_revision
                                   ? config.graph_revision
                                   : std::optional<GraphRevision>{});
  std::optional<RetainedBundleId> retained_bundle_id;
  if (registrations.empty()) {
    if (config.empty_runtime_retained_bundle_id.has_value() !=
        config.empty_runtime_graph_revision.has_value()) {
      throw std::invalid_argument(
          "empty runtime retained-bundle and graph identities must be supplied together");
    }
    if (config.empty_runtime_retained_bundle_id.has_value() &&
        (config.empty_runtime_retained_bundle_id->empty() ||
         config.empty_runtime_graph_revision->empty())) {
      throw std::invalid_argument("empty runtime retained-bundle or graph identity is invalid");
    }
    retained_bundle_id = config.empty_runtime_retained_bundle_id;
  }

  std::map<ControllableComponentId, StopScopeComponentSnapshotDTO> components;
  std::map<ResourceId, std::pair<ControllableComponentId, bool>> resource_owners;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!retained_bundle_id.has_value()) {
      retained_bundle_id = binding.retained_bundle_id;
    }
    if (binding.retained_bundle_id != *retained_bundle_id) {
      throw std::invalid_argument(
          "runtime participants do not share one retained bundle for Stop terminal truth");
    }
    if (!graph_revision.has_value()) {
      graph_revision = binding.graph_revision;
    }
    if (binding.graph_revision != *graph_revision) {
      throw std::invalid_argument(
          "runtime participants do not share one current graph revision for Stop resolution");
    }
    if (!registration.stop_readiness && !binding.io_output_binding) {
      // Other unqualified registrations do not self-attest lifecycle membership.
      continue;
    }
    // Output membership comes from its sealed logical channel and actual CCLC
    // generation. It does not create a physical Stop plan or readiness proof.
    const auto& component_id = binding.io_output_binding
                                   ? binding.controllable_component_id
                                   : registration.stop_readiness->value().controllable_component_id;
    const auto generation =
        binding.io_output_binding
            ? binding.io_output_binding->component_lifecycle_generation
            : registration.stop_readiness->value().component_lifecycle_generation;
    auto [component, inserted] = components.try_emplace(
        component_id, StopScopeComponentSnapshotDTO{component_id, generation, {}});
    if (!inserted && component->second.component_lifecycle_generation != generation) {
      throw std::invalid_argument(
          "runtime participant Stop readiness disagrees on component lifecycle generation");
    }
    const auto append_resource = [&](const ResourceId& resource_id, bool logical_output) {
      const auto [owner, owner_inserted] =
          resource_owners.emplace(resource_id, std::pair{component_id, logical_output});
      if (!owner_inserted &&
          ((owner->second.first != component_id && (!logical_output || !owner->second.second)) ||
           owner->second.second != logical_output)) {
        throw std::invalid_argument(
            "runtime Stop readiness assigns one resource to multiple components");
      }
      component->second.resources.push_back(resource_id);
      if (logical_output) component->second.logical_output_resources.push_back(resource_id);
    };
    if (binding.io_output_binding) {
      append_resource(*binding.io_output_binding->signal.output_resource, true);
    } else {
      for (const auto& resource : binding.resources) append_resource(resource.resource_id, false);
    }
  }

  if (!graph_revision.has_value() || graph_revision->empty()) {
    throw std::invalid_argument("runtime Stop resolution has no current graph revision");
  }

  if (!retained_bundle_id.has_value() || retained_bundle_id->empty()) {
    throw std::invalid_argument("runtime Stop terminal truth has no retained-bundle identity");
  }

  StopStaticFacts facts{std::move(*retained_bundle_id), std::move(*graph_revision), {}};
  facts.components.reserve(components.size());
  for (auto& [component_id, component] : components) {
    (void)component_id;
    std::sort(component.resources.begin(), component.resources.end());
    component.resources.erase(std::unique(component.resources.begin(), component.resources.end()),
                              component.resources.end());
    std::sort(component.logical_output_resources.begin(), component.logical_output_resources.end());
    component.logical_output_resources.erase(
        std::unique(component.logical_output_resources.begin(), component.logical_output_resources.end()),
        component.logical_output_resources.end());
    facts.components.push_back(std::move(component));
  }
  return facts;
}

[[nodiscard]] inline const ProgramFrameRoleBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding, const FrameRoleId& role) noexcept {
  const auto found = std::find_if(
      binding.frame_bindings.begin(), binding.frame_bindings.end(),
      [&](const ProgramFrameRoleBindingDTO& candidate) { return candidate.frame_role_id == role; });
  return found == binding.frame_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] inline const RuntimeResourceBindingDTO* find_source_joint(
    const RuntimeParticipantBindingDTO& binding, const std::string& source_joint_name) noexcept {
  const auto found = std::find_if(binding.resources.begin(), binding.resources.end(),
                                  [&](const RuntimeResourceBindingDTO& candidate) {
                                    return candidate.source_joint_name.has_value() &&
                                           *candidate.source_joint_name == source_joint_name;
                                  });
  return found == binding.resources.end() ? nullptr : &*found;
}

[[nodiscard]] inline const ProgramModelRoleBindingDTO* find_model(
    const RuntimeParticipantBindingDTO& binding,
    const ModelInstanceId& model_instance_id) noexcept {
  const auto found = std::find_if(binding.model_bindings.begin(), binding.model_bindings.end(),
                                  [&](const ProgramModelRoleBindingDTO& candidate) {
                                    return candidate.model_instance_id == model_instance_id;
                                  });
  return found == binding.model_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] inline CapabilityId required_planner_capability(const MotionSpecDTO& motion) {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, JointGoalDTO>) {
          return CapabilityId("planning.motion.joint_goal");
        } else if constexpr (std::is_same_v<Value, PoseGoalDTO>) {
          return CapabilityId("planning.motion.pose_goal");
        } else if constexpr (std::is_same_v<Value, NamedTargetDTO>) {
          return CapabilityId("planning.motion.named_target");
        } else {
          return CapabilityId("planning.motion.cartesian_path");
        }
      },
      motion);
}

[[nodiscard]] inline bool same_linked_preview_participant(const RuntimeParticipantBindingDTO& left,
                                                   const RuntimeParticipantBindingDTO& right) {
  auto left_stable_identity = left;
  auto right_stable_identity = right;
  // Supporting providers are selected for each motion payload. A linked
  // pose/Cartesian sequence may therefore use different exact planners while
  // retaining the same robot, resources, control path, epochs, and execution
  // engine. Each complete binding is still resolved and validated before use.
  left_stable_identity.supporting_capability_provider_paths.clear();
  right_stable_identity.supporting_capability_provider_paths.clear();
  return left_stable_identity == right_stable_identity;
}

[[nodiscard]] inline Result<std::optional<ResolvedContactGraspBindingDTO>>
resolve_contact_grasp_binding(const MotionParticipantIntentDTO& motion,
                              const RuntimeParticipantBindingDTO& binding) {
  if (!motion.contact_policy_id.has_value()) {
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(std::nullopt);
  }
  const auto policy = std::lower_bound(
      binding.contact_grasp_bindings.begin(), binding.contact_grasp_bindings.end(),
      *motion.contact_policy_id, [](const auto& candidate, const ContactPolicyId& id) {
        return candidate.contact_policy_id < id;
      });
  if (policy == binding.contact_grasp_bindings.end() ||
      policy->contact_policy_id != *motion.contact_policy_id) {
    // Not a Program-owned contact_grasp: no late runtime-object binding is
    // needed here; collision validation resolves the id against the retained
    // scene policy. Every Program-owned contact_grasp requires this binding.
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(std::nullopt);
  }
  const auto object = std::lower_bound(
      motion.runtime_object_bindings.begin(), motion.runtime_object_bindings.end(),
      policy->runtime_object_id, [](const auto& candidate, const RuntimeObjectId& id) {
        return candidate.runtime_object_id < id;
      });
  if (object == motion.runtime_object_bindings.end() ||
      object->runtime_object_id != policy->runtime_object_id) {
    // Name the one runtime object the policy requires and the motion does not
    // select (spec 210 §4.4, §5.3).
    auto unresolved = reject<std::optional<ResolvedContactGraspBindingDTO>>(
                          "contact-grasp policy has no selected runtime object",
                          "contact_grasp_runtime_object_unresolved", ErrorCode::kMissingReference)
                          .error();
    unresolved.details.emplace("reference_type", "runtime_object");
    unresolved.details.emplace("reference_id", policy->runtime_object_id.value());
    return Result<std::optional<ResolvedContactGraspBindingDTO>>::Err(std::move(unresolved));
  }
  std::vector<CollisionBodyId> body_ids;
  if (policy->bodies_from_motion_endpoint && !binding.motion_endpoint) {
    return reject<std::optional<ResolvedContactGraspBindingDTO>>(
        "contact-grasp policy requires a resolved motion endpoint",
        "contact_grasp_motion_endpoint_unresolved", ErrorCode::kMissingReference);
  }
  const auto& bodies = policy->bodies_from_motion_endpoint
                           ? binding.motion_endpoint->contact_body_bindings
                           : policy->allowed_body_bindings;
  for (const auto& body : bodies) {
    body_ids.insert(body_ids.end(), body.collision_body_ids.begin(), body.collision_body_ids.end());
  }
  std::sort(body_ids.begin(), body_ids.end());
  body_ids.erase(std::unique(body_ids.begin(), body_ids.end()), body_ids.end());
  if (body_ids.empty()) {
    return reject<std::optional<ResolvedContactGraspBindingDTO>>(
        "contact-grasp policy resolved no robot collision bodies",
        "contact_grasp_body_binding_empty", ErrorCode::kInvariantViolation);
  }
  return Result<std::optional<ResolvedContactGraspBindingDTO>>::Ok(
      std::optional<ResolvedContactGraspBindingDTO>{ResolvedContactGraspBindingDTO{
          policy->contact_policy_id, policy->contact_policy_revision, policy->runtime_object_id,
          object->scene_resource_id, std::move(body_ids), policy->envelope}});
}

[[nodiscard]] inline ResolvedFrameDTO resolved_component_frame(const RuntimeParticipantBindingDTO& binding,
                                                        const ProgramFrameRoleBindingDTO& frame) {
  return ResolvedComponentModelFrameDTO{binding.controllable_component_id, frame.model_instance_id,
                                        frame.frame_role_id, frame.compiled_frame_name,
                                        frame.model_revision};
}

[[nodiscard]] inline Result<ExecuteRequestDTO> build_motion_request(
    const OperationIntentDTO& operation, const OperationParticipantIntentDTO& participant,
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding,
    const OperationId& operation_id, OperationRevision operation_revision,
    const OperationParticipantId& participant_id, const ControllerGoalId& controller_goal_id,
    ControllerGoalGeneration controller_goal_generation,
    const RuntimeParticipantExecutionRegistration& registration,
    const double initial_runtime_scale) {
  const auto& participant_policy = *registration.participant_policy;
  if (binding.model_groups.size() != 1U) {
    return reject<ExecuteRequestDTO>("I6 motion requires exactly one retained model group",
                                     "i6_motion_model_group_cardinality_invalid");
  }
  const auto* base = find_frame(binding, motion.base_frame_role_id);
  const auto* tcp =
      binding.motion_endpoint &&
              motion.tcp_endpoint.target_role_id == binding.motion_endpoint->target_role_id &&
              motion.tcp_endpoint.frame_role_id == binding.motion_endpoint->frame.frame_role_id
          ? &binding.motion_endpoint->frame
          : nullptr;
  if (base == nullptr || tcp == nullptr) {
    return reject<ExecuteRequestDTO>("motion frame roles do not resolve through the exact binding",
                                     "motion_frame_role_unresolved");
  }

  const auto& model_group = binding.model_groups.front();
  const auto* group_model = find_model(binding, model_group.model_instance_id);
  if (group_model == nullptr || !binding.control_path.has_value()) {
    return reject<ExecuteRequestDTO>(
        "motion model group or control path is absent from the exact binding",
        "motion_runtime_binding_incomplete", ErrorCode::kInvariantViolation);
  }
  std::vector<std::string> compiled_joint_names;
  compiled_joint_names.reserve(binding.resources.size());
  for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
    const auto& resource = binding.resources[index];
    if (resource.coordinate_kind != CoordinateKind::kAngular ||
        std::count(model_group.expanded_compiled_joint_names.begin(),
                   model_group.expanded_compiled_joint_names.end(),
                   resource.compiled_resource_name) != 1) {
      return reject<ExecuteRequestDTO>(
          "motion requires all-angular resources belonging to the exact retained model group",
          "motion_resource_projection_not_all_angular", ErrorCode::kInvariantViolation);
    }
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  const auto& control = *binding.control_path;
  const auto planner_capability = required_planner_capability(motion.motion);
  const auto* planner_definition = find_builtin_capability_definition(planner_capability);
  if (planner_definition == nullptr) {
    return reject<ExecuteRequestDTO>("motion kind has no registered planning capability",
                                     "motion_planner_capability_unregistered",
                                     ErrorCode::kInvariantViolation);
  }
  const CapabilityDefinitionRevision planner_revision(
      std::string(planner_definition->capability_definition_revision));
  const auto planner_path =
      std::find_if(binding.supporting_capability_provider_paths.begin(),
                   binding.supporting_capability_provider_paths.end(),
                   [&](const RuntimeCapabilityProviderPathDTO& candidate) {
                     return candidate.capability_id == planner_capability &&
                            candidate.capability_definition_revision == planner_revision;
                   });
  if (planner_path == binding.supporting_capability_provider_paths.end()) {
    return reject<ExecuteRequestDTO>(
        "motion planner capability does not resolve through the exact binding",
        "motion_planner_capability_unresolved", ErrorCode::kMissingReference);
  }
  std::vector<PlannerModelBindingDTO> models;
  models.reserve(binding.model_bindings.size());
  for (const auto& model : binding.model_bindings) {
    models.push_back(PlannerModelBindingDTO{model.model_instance_id, model.model_revision});
  }
  std::vector<PlannerRuntimeUnitBindingDTO> runtime_units;
  runtime_units.reserve(binding.runtime_unit_epochs.size());
  for (const auto& runtime : binding.runtime_unit_epochs) {
    runtime_units.push_back(
        PlannerRuntimeUnitBindingDTO{runtime.runtime_unit_id, runtime.runtime_epoch});
  }
  std::vector<ResourceId> bound_resources;
  bound_resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    bound_resources.push_back(resource.resource_id);
  }

  ResolvedExecutionBindingDTO exact_binding{
      operation_id,
      participant_id,
      controller_goal_id,
      controller_goal_generation,
      binding.program_id,
      binding.program_revision,
      binding.program_binding_id,
      operation.program_run_id,
      binding.controllable_component_id,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      PlannerCapabilityBindingDTO{
          planner_path->capability_id, planner_path->capability_definition_revision,
          planner_path->capability_provider_binding_id,
          planner_path->capability_provider_binding_revision, planner_path->provider_instance_id,
          planner_path->provider_manifest_id, planner_path->provider_manifest_revision,
          planner_path->provider_runtime_unit_id, planner_path->provider_runtime_epoch},
      std::move(models),
      PlannerModelGroupBindingDTO{model_group.model_group_role_id, model_group.model_instance_id,
                                  group_model->model_revision, model_group.compiled_group_name,
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
      std::move(bound_resources),
      resolved_component_frame(binding, *base),
      resolved_component_frame(binding, *tcp),
      {},
      {},
      control.runtime_scale_qualification,
      binding.retained_bundle_id,
      binding.graph_revision,
      binding.target_role_id,
      binding.frame_bindings,
      binding.collision_body_bindings};

  ExecuteRequestDTO request(std::move(exact_binding));
  request.move_id = motion.move_id.value();
  request.tolerances = motion.tolerances;
  if (!registration.deployment_maximum_tcp_linear_speed_mps.has_value()) {
    return reject<ExecuteRequestDTO>(
        "motion target has no applied Deployment TCP linear-speed maximum",
        "deployment_motion_speed_missing", ErrorCode::kMissingReference);
  }
  auto effective_speed = apply_deployment_motion_speed_limit(
      *registration.deployment_maximum_tcp_linear_speed_mps,
      motion.speed_profile);
  if (!effective_speed.ok()) {
    return Result<ExecuteRequestDTO>::Err(effective_speed.error());
  }
  // Derived node scopes belong to this local preparation request only. The
  // public intent remains unchanged for journal/replay identity.
  auto effective_interrupt_scopes = motion.motion_interrupt_scopes;
  if (registration.captured_node_interrupt_scope) {
    if (!effective_interrupt_scopes.empty())
      return reject<ExecuteRequestDTO>("captured node scopes cannot be supplied by the caller",
                                       "captured_node_interrupt_scope_conflict");
    effective_interrupt_scopes.push_back(*registration.captured_node_interrupt_scope);
  }
  auto interrupt_admission = validate_motion_interrupt_scope_admission(
      effective_interrupt_scopes, registration.io_motion_rule_policies, &binding);
  if (!interrupt_admission.ok()) {
    return Result<ExecuteRequestDTO>::Err(interrupt_admission.error());
  }
  request.speed_profile = std::move(effective_speed.value());
  request.timeouts = motion.timeouts;
  request.cache_mode = motion.cache_mode;
  request.planning_profile = motion.planning_profile;
  request.target_mode = motion.target_mode;
  request.target_reference = motion.target_reference;
  request.offset_reference = motion.offset_reference;
  request.goal_frame_reference = motion.goal_frame_reference;
  request.motion_interrupt_scopes = std::move(effective_interrupt_scopes);
  request.action_span = participant.action_span;
  request.contact_policy_id =
      motion.contact_policy_id.has_value() ? motion.contact_policy_id->value() : std::string{};
  request.runtime_object_bindings = motion.runtime_object_bindings;
  auto contact_grasp = resolve_contact_grasp_binding(motion, binding);
  if (!contact_grasp.ok()) {
    return Result<ExecuteRequestDTO>::Err(contact_grasp.error());
  }
  request.resolved_contact_grasp = std::move(contact_grasp.value());
  if (control.runtime_scale_qualification.has_value()) {
    const auto& qualification = *control.runtime_scale_qualification;
    request.initial_runtime_scale =
        ResolvedRuntimeScaleCommandDTO{operation_id,
                                       operation_revision,
                                       participant_id,
                                       binding.controllable_component_id,
                                       binding.command_target_id,
                                       binding.capability_id,
                                       control.control_domain_id,
                                       control.control_domain_revision,
                                       control.control_binding_id,
                                       control.control_binding_revision,
                                       controller_goal_id,
                                       controller_goal_generation,
                                       control.controller_topology_id,
                                       control.topology_revision,
                                       control.control_route_id,
                                       qualification.scaling_domain_id,
                                       qualification.coordinated_barrier_id,
                                       request.binding.resources,
                                       control.active_control_configuration_id,
                                       control.active_control_configuration_generation,
                                       RuntimeScaleGeneration(0U),
                                       RuntimeScaleGeneration(0U),
                                       initial_runtime_scale};
  }

  if (const auto* goal = std::get_if<JointGoalDTO>(&motion.motion)) {
    std::set<std::string, std::less<>> seen_joint_names;
    std::vector<ResourceId> resolved_resources;
    std::vector<std::string> compiled_names;
    resolved_resources.reserve(goal->joint_names.size());
    compiled_names.reserve(goal->joint_names.size());
    for (const auto& joint_name : goal->joint_names) {
      const auto* resource = find_source_joint(binding, joint_name);
      if (resource == nullptr || resource->coordinate_kind != CoordinateKind::kAngular ||
          !seen_joint_names.insert(joint_name).second) {
        return reject<ExecuteRequestDTO>(
            "joint goal source joints are missing, duplicated, or non-angular",
            "joint_goal_source_joint_unresolved");
      }
      resolved_resources.push_back(resource->resource_id);
      compiled_names.push_back(resource->compiled_resource_name);
    }
    request.motion = ResolvedJointGoalDTO{std::move(resolved_resources), std::move(compiled_names),
                                          goal->positions_rad};
  } else if (const auto* pose = std::get_if<PoseGoalDTO>(&motion.motion)) {
    request.motion = *pose;
  } else if (const auto* path = std::get_if<CartesianPathDTO>(&motion.motion)) {
    request.motion = *path;
  } else {
    return reject<ExecuteRequestDTO>(
        "named targets require the retained Program target map before planning",
        "named_target_resolution_unavailable", ErrorCode::kMissingReference);
  }

  auto policy_limits = apply_runtime_participant_motion_jerk_policy(
      participant_policy, motion.per_joint_dynamics_limits);
  if (!policy_limits.ok()) return Result<ExecuteRequestDTO>::Err(policy_limits.error());
  auto joint_limits = std::move(policy_limits.value());
  if (registration.intrinsic_direct_control) {
    const auto tighten = [](std::optional<double>& requested,
                            const std::optional<double>& ceiling) {
      if (ceiling.has_value()) {
        requested = requested.has_value() ? std::min(*requested, *ceiling) : *ceiling;
      }
    };
    for (const auto& [joint_name, ceiling] : registration.direct_control_joint_limits) {
      if (ceiling.max_jerk_rad_s3 && participant_policy.value().ordinary_motion_jerk &&
          !participant_policy.value().ordinary_motion_jerk->enabled)
        return reject<ExecuteRequestDTO>(
            "manual jerk cap conflicts with the disabled ordinary-motion profile",
            "ordinary_motion_jerk_disabled");
      for (const auto& value :
           {ceiling.max_velocity_rad_s, ceiling.max_acceleration_rad_s2, ceiling.max_jerk_rad_s3}) {
        if (value.has_value() && (!std::isfinite(*value) || *value <= 0.0)) {
          return reject<ExecuteRequestDTO>("retained manual dynamics ceiling is invalid",
                                           "direct_control_joint_limit_invalid");
        }
      }
      auto& limit = joint_limits[joint_name];
      tighten(limit.max_velocity_rad_s, ceiling.max_velocity_rad_s);
      tighten(limit.max_acceleration_rad_s2, ceiling.max_acceleration_rad_s2);
      tighten(limit.max_jerk_rad_s3, ceiling.max_jerk_rad_s3);
    }
  }
  for (const auto& [joint_name, limit] : joint_limits) {
    const auto* resource = find_source_joint(binding, joint_name);
    if (resource == nullptr || resource->coordinate_kind != CoordinateKind::kAngular) {
      return reject<ExecuteRequestDTO>(
          "motion dynamics source joint does not resolve to an angular resource",
          "motion_dynamics_source_joint_unresolved");
    }
    request.per_resource_dynamics_limits.push_back(ResolvedResourceDynamicsLimitDTO{
        resource->resource_id, resource->compiled_resource_name, limit});
  }
  std::sort(request.per_resource_dynamics_limits.begin(),
            request.per_resource_dynamics_limits.end(),
            [&](const ResolvedResourceDynamicsLimitDTO& lhs,
                const ResolvedResourceDynamicsLimitDTO& rhs) {
              const auto left = std::find(request.binding.resources.begin(),
                                          request.binding.resources.end(), lhs.resource_id);
              const auto right = std::find(request.binding.resources.begin(),
                                           request.binding.resources.end(), rhs.resource_id);
              return left < right;
            });
  return Result<ExecuteRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] inline Result<GripperCommandRequestDTO> build_command_request(
    const CapabilityCommandIntentDTO& command, const RuntimeParticipantBindingDTO& binding,
    GripperActuationKind actuation = GripperActuationKind::kPositionCommanded,
    GripperOutputMode output = GripperOutputMode::kCommanded) {
  if (binding.model_groups.size() > 1U || binding.resources.size() != 1U) {
    return reject<GripperCommandRequestDTO>(
        "the retained I6 gripper mechanism requires one exact command resource",
        "i6_gripper_command_scope_unsupported");
  }
  const auto& resource = binding.resources.front();
  const bool model_backed = resource.source_joint_name.has_value();
  const bool exact_model_scope =
      model_backed && binding.model_groups.size() == 1U &&
      std::count(binding.model_groups.front().expanded_compiled_joint_names.begin(),
                 binding.model_groups.front().expanded_compiled_joint_names.end(),
                 resource.compiled_resource_name) == 1;
  const bool exact_component_local_scope = !model_backed && binding.model_groups.empty();
  if (!exact_model_scope && !exact_component_local_scope) {
    return reject<GripperCommandRequestDTO>(
        "the retained gripper resource has no exact model-backed or component-local scope",
        "i6_gripper_command_scope_unsupported");
  }
  if (command.command_schema_id.value() != kGripperPositionCommandSchemaId ||
      command.command_schema_revision.value() != kGripperPositionCommandSchemaRevision) {
    return reject<GripperCommandRequestDTO>("the command is not the qualified I6 gripper schema",
                                            "i6_command_schema_unsupported",
                                            ErrorCode::kUnsupportedCapability);
  }

  double command_duration_s = 0.0;
  for (const auto& option : command.caller_options) {
    if (option.option_id.value() != "command_duration_s") {
      return reject<GripperCommandRequestDTO>(
          "the command carries an option not declared by its schema", "command_option_unsupported",
          ErrorCode::kInvalidRequest);
    }
    command_duration_s = option.value_s;
  }

  const auto& value = std::get<GripperPositionCommandValueDTO>(command.command_value);
  const auto kind = coordinate_kind(value.position);
  if (kind != binding.resources.front().coordinate_kind) {
    return reject<GripperCommandRequestDTO>(
        "gripper command unit differs from its retained resource",
        "gripper_command_coordinate_kind_mismatch", ErrorCode::kInvalidRequest);
  }
  const double target_position = kind == CoordinateKind::kAngular
                                     ? std::get<AngularPositionDTO>(value.position).value_rad
                                     : std::get<LinearPositionDTO>(value.position).value_m;
  std::optional<double> max_effort;
  if (value.max_effort.has_value()) {
    max_effort = kind == CoordinateKind::kAngular
                     ? std::get<AngularEffortDTO>(*value.max_effort).value_nm
                     : std::get<LinearEffortDTO>(*value.max_effort).value_n;
  }

  GripperCommandRequestDTO request;
  request.gripper_id = binding.command_target_id.value();
  request.group_id =
      model_backed ? binding.model_groups.front().compiled_group_name : std::string{};
  request.joint_name = resource.compiled_resource_name;
  request.target_position = target_position;
  request.duration_s = command_duration_s;
  request.max_effort = max_effort;
  request.commanded_state = "position";
  request.actuation_kind = kind == CoordinateKind::kAngular ? "angular" : "linear";
  request.state_authority = binding.control_path->state_authority_id.value();
  request.completion_policy = actuation == GripperActuationKind::kPneumatic
                                  ? (output == GripperOutputMode::kNone ? "no_output" : "output_delivery")
                                  : "goal_fenced_target_reached";
  request.grasp_confirmation_policy = actuation == GripperActuationKind::kPneumatic
                                         ? "not_required" : "binding_feedback_required";
  request.transport_kind = "resolved_binding";
  request.evidence = {"command_terminal"};
  request.evidence_state = "required_pending";
  request.completion_timeout_s = command.completion_timeout_s.value();
  return Result<GripperCommandRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] inline std::size_t controller_goal_cancel_stage_index(
    const PreparedParticipantStopPlanDTO& plan) {
  const auto found =
      std::find_if(plan.stage_commands.begin(), plan.stage_commands.end(), [](const auto& command) {
        return command.resolved_stage.containment_command.command_kind ==
               StopContainmentCommandKind::kControllerGoalCancel;
      });
  return static_cast<std::size_t>(std::distance(plan.stage_commands.begin(), found));
}

[[nodiscard]] inline Result<std::optional<PreparedParticipantStopPlanDTO>> make_runtime_fault_stop_plan(
    const RuntimeParticipantStopReadiness& readiness,
    const std::optional<MotionInterruptStopProfileDTO>& profile,
    const RuntimeParticipantBindingDTO& binding,
    std::span<const HardStopResourceLimitDTO> retained_default_stop_limits,
    const MotionInterruptNodeCategory node_category, const OperationId& operation_id,
    const OperationRevision operation_revision, const OperationParticipantId& participant_id,
    const ProgramId& program_id, const ProgramRevision& program_revision,
    const ProgramBindingId& program_binding_id, const ProgramRunId& program_run_id) {
  if (!profile.has_value()) {
    return Result<std::optional<PreparedParticipantStopPlanDTO>>::Ok(std::nullopt);
  }
  PreparedParticipantStopPlanContextDTO context{
      operation_id,   operation_revision, participant_id,
      program_id,     program_revision,   program_binding_id,
      program_run_id, *profile,           ParticipantStopProfilePurposeDTO::kFault,
      std::nullopt,   node_category};
  auto prepared = MakePreparedParticipantStopPlan(readiness.value().resolved_stop_base_plan,
                                                  std::move(context),
                                                  binding.resources, retained_default_stop_limits);
  if (!prepared.ok()) {
    return Result<std::optional<PreparedParticipantStopPlanDTO>>::Err(prepared.error());
  }
  return Result<std::optional<PreparedParticipantStopPlanDTO>>::Ok(std::move(prepared.value()));
}

[[nodiscard]] inline bool prepared_fault_stop_plan_matches(
    const std::optional<PreparedParticipantStopPlanDTO>& product,
    const std::shared_ptr<const PreparedParticipantStopPlanDTO>& authority) {
  return authority ? product == std::optional<PreparedParticipantStopPlanDTO>{*authority}
                   : !product.has_value();
}

[[nodiscard]] inline Result<PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics(
    const PreparedOperationParticipantDTO& participant) {
  const auto* motion = std::get_if<PreparedMotionExecutionDTO>(&participant.payload);
  if (motion == nullptr ||
      !motion->validation_evidence.noncollision_result.trajectory_metrics.has_value()) {
    return reject<PreparedMotionDiagnosticsDTO>(
        "prepared motion is missing accepted-trajectory diagnostics",
        "prepared_motion_diagnostics_missing", ErrorCode::kInvariantViolation);
  }
  return Result<PreparedMotionDiagnosticsDTO>::Ok(PreparedMotionDiagnosticsDTO{
      participant.participant_id, participant.prepared_contract_digest, motion->move_id,
      motion->trajectory.joint_names,
      *motion->validation_evidence.noncollision_result.trajectory_metrics,
      motion->requested_tcp_linear_speed_mps,
      motion->deployment_maximum_tcp_linear_speed_mps,
      *motion->plan_request.speed_profile.max_tcp_linear_speed_mps});
}

[[nodiscard]] inline Result<PreparedOperationParticipantDTO> build_prepared_io_output_participant(
    const OperationIntentDTO& intent, const CapabilityCommandIntentDTO& command,
    const RuntimeParticipantBinding& binding, const OperationId& operation,
    OperationRevision revision, const OperationParticipantId& participant) {
  const auto* value = std::get_if<IoSignalWriteCommandValueDTO>(&command.command_value);
  if (!value || !command.caller_options.empty())
    return reject<PreparedOperationParticipantDTO>(
        "output command has unsupported value or options", "io_output_command_value_invalid",
        ErrorCode::kInvalidRequest);
  return MakePreparedIoOutputParticipant(operation, revision, participant, intent.program_run_id,
                                         command, binding);
}

[[nodiscard]] inline ResolvedPreparedCapabilityCommandExecutionRequestDTO
output_preparation_request(std::shared_ptr<const PreparedOperationParticipantDTO> participant,
                           const RuntimeParticipantBinding& binding) {
  const auto& output = std::get<PreparedIoOutputCommandDTO>(participant->payload);
  const auto provider = participant->provider()->lifecycle_provider;
  ResolvedPreparedIoOutputCommandAuthorityDTO authority{output.signal_id, output.output_resource,
                                                        output.command_value.value, provider,
                                                        output.completion_policy};
  return {std::move(participant), binding, std::move(authority)};
}

[[nodiscard]] inline Result<PreparedOperationParticipantDTO> build_prepared_capability_command_participant(
    const OperationIntentDTO& intent, const CapabilityCommandIntentDTO& command,
    const RuntimeParticipantBinding& sealed_binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_stop_limits,
    const MotionInterruptNodeCategory stop_node_category, const OperationId& operation_id,
    const OperationRevision operation_revision, const OperationParticipantId& participant_id,
    const PreparedParticipantStopPlanDTO& ordinary_stop_plan) {
  const auto& binding = sealed_binding.value();
  const auto& control = *binding.control_path;
  auto validated_options = command.caller_options;
  std::sort(validated_options.begin(), validated_options.end(),
            [](const auto& left, const auto& right) { return left.option_id < right.option_id; });
  const auto schema = builtin_gripper_position_command_schema();
  PreparedCapabilityCommandDTO payload{command.command_schema_id,
                                       command.command_schema_revision,
                                       command.command_value,
                                       std::move(validated_options),
                                       {},
                                       {},
                                       command.completion_timeout_s.value(),
                                       schema.completion_rule.value(),
                                       schema.confirmation_rule.value(),
                                       schema.stop_evidence_requirements};
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO> capability_provider;
  if (binding.capability_provider_path.has_value()) {
    const auto& provider = *binding.capability_provider_path;
    capability_provider = PreparedCapabilityProviderRuntimeOccurrenceDTO{
        provider.capability_provider_binding_id,
        provider.capability_provider_binding_revision,
        {provider.provider_instance_id, provider.provider_manifest_id,
         provider.provider_manifest_revision, provider.provider_runtime_unit_id,
         provider.provider_runtime_epoch}};
  }
  PreparedOperationParticipantDTO candidate{
      ContentDigest("pending"),
      operation_id,
      operation_revision,
      participant_id,
      intent.program_id,
      intent.program_revision,
      intent.program_binding_id,
      intent.program_run_id,
      binding.retained_bundle_id,
      binding.graph_revision,
      binding.controllable_component_id,
      readiness.value().component_lifecycle_generation,
      binding.component_type_id,
      binding.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      binding.model_bindings,
      binding.frame_bindings,
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
      {control.provider_instance_id, control.provider_manifest_id,
       control.provider_manifest_revision, control.provider_runtime_unit_id,
       control.provider_runtime_epoch},
      std::move(capability_provider),
      std::move(resources),
      PreparedParticipantFocusedPortDTO{control.provider_runtime_unit_id},
      {},
      {},
      control.active_control_configuration_id,
      control.active_control_configuration_generation,
      binding.runtime_unit_epochs,
      control.stop_evidence_policy_id,
      control.stop_evidence_policy_revision,
      ordinary_stop_plan,
      std::move(payload),
      std::nullopt};
  return MakePreparedOperationParticipant(std::move(candidate), sealed_binding, readiness,
                                          retained_default_stop_limits, stop_node_category);
}

/// Public operation state for a participant's working phase. Participants
/// report working phases and facts only; any other value fails closed.
[[nodiscard]] inline OperationStateKind operation_state(ExecutionState state) noexcept {
  switch (state) {
    case ExecutionState::kIdle:
      return OperationStateKind::kPrepared;
    case ExecutionState::kPlanning:
      return OperationStateKind::kPlanning;
    case ExecutionState::kValidating:
      return OperationStateKind::kValidating;
    case ExecutionState::kExecuting:
      return OperationStateKind::kExecuting;
    default:
      return OperationStateKind::kInterventionRequired;
  }
}

/// The addressed Kernel's outcome for one participant execution that ended
/// outside an active Stop, derived from the participant's reported fact. This
/// is the single place that commits these participant outcomes; an active
/// Stop commits its own Stopped/InterventionRequired from its certificate.
[[nodiscard]] inline OperationStateKind participant_outcome(
    const ParticipantExecutionFact& fact) noexcept {
  switch (fact.kind) {
    case ParticipantExecutionFactKind::kComputedCompletion:
      return OperationStateKind::kCompleted;
    case ParticipantExecutionFactKind::kCancelledBeforeDispatch:
      return OperationStateKind::kStopped;
    case ParticipantExecutionFactKind::kLocalFailure:
      // Includes planning failures; an expired planning deadline is the
      // planning failure `planning_timeout`, never Stopped.
      return fact.effect == ParticipantEffectKnowledge::kEffectUncertain
                 ? OperationStateKind::kInterventionRequired
                 : OperationStateKind::kFailed;
    case ParticipantExecutionFactKind::kPreemptedByReplacement:
      // Only the exact owner-transfer commit may settle a replacement.
      return OperationStateKind::kInterventionRequired;
    case ParticipantExecutionFactKind::kPreemptedByAddressedStop:
      // Only an active Stop fences a participant; reaching here means no Stop
      // owns this containment.
      return OperationStateKind::kInterventionRequired;
  }
  return OperationStateKind::kInterventionRequired;
}

[[nodiscard]] inline OperationStateKind participant_operation_state(const ExecutionStatus& status) noexcept {
  return status.fact.has_value() ? participant_outcome(*status.fact)
                                 : operation_state(status.state);
}

[[nodiscard]] inline std::optional<ErrorDTO> participant_fact_error(const ExecutionStatus& status) {
  return status.fact.has_value() ? status.fact->error : std::nullopt;
}

[[nodiscard]] inline bool terminal(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return true;
    default:
      return false;
  }
}

// Retention is independent of cause selection. Existing permissions may use a
// participant fact only when it belongs to the selected terminal error.
[[nodiscard]] inline const ParticipantExecutionFact* selected_cause_terminal_fact(
    const OperationParticipantStateDTO& participant) {
  return participant.terminal_fact && participant.terminal_fact->error == participant.terminal_error
             ? &*participant.terminal_fact
             : nullptr;
}

[[nodiscard]] inline bool control_admissible(OperationControlKind control,
                                      OperationStateKind state) noexcept {
  switch (control) {
    case OperationControlKind::kReset:
      return state != OperationStateKind::kCompleted;
    case OperationControlKind::kAcknowledgeCompleted:
      return state == OperationStateKind::kCompleted;
  }
  return false;
}

[[nodiscard]] inline const char* control_name(OperationControlKind control) noexcept {
  switch (control) {
    case OperationControlKind::kReset:
      return "reset";
    case OperationControlKind::kAcknowledgeCompleted:
      return "acknowledge_completed";
  }
  return "unknown";
}

[[nodiscard]] inline std::optional<ResourceSlotState> slot_state_for(OperationStateKind state) noexcept {
  switch (state) {
    case OperationStateKind::kDispatching:
    case OperationStateKind::kExecuting:
      return ResourceSlotState::kDispatched;
    case OperationStateKind::kStopping:
      return ResourceSlotState::kStopping;
    case OperationStateKind::kCompleted:
    case OperationStateKind::kStopped:
    case OperationStateKind::kFailed:
    case OperationStateKind::kInterrupted:
    case OperationStateKind::kInterventionRequired:
    case OperationStateKind::kCancelledPreDispatch:
      return ResourceSlotState::kTerminal;
    case OperationStateKind::kQueued:
    case OperationStateKind::kResolving:
    case OperationStateKind::kPlanning:
    case OperationStateKind::kValidating:
    case OperationStateKind::kPrepared:
      return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] inline ErrorDTO status_error(ErrorCode code) {
  return ErrorDTO{std::string(to_string(code)),
                  "subordinate participant reached a terminal error state",
                  {{"reason", "subordinate_terminal_error"}}};
}

[[nodiscard]] inline RuntimeScaleParticipantStatusDTO initial_scale_status(
    const ResolvedRuntimeScaleCommandDTO& command, const RuntimeEpoch& provider_epoch) {
  return {command.participant_id,
          command.controllable_component_id,
          command.command_target_id,
          command.capability_id,
          command.control_domain_id,
          command.control_domain_revision,
          command.control_binding_id,
          command.control_binding_revision,
          command.controller_goal_id,
          command.controller_goal_generation,
          command.controller_topology_id,
          command.topology_revision,
          command.control_route_id,
          command.scaling_domain_id,
          command.coordinated_barrier_id,
          command.resources,
          command.active_control_configuration_id,
          command.active_control_configuration_generation,
          command.requested_runtime_scale_generation,
          command.requested_scale,
          command.requested_scale,
          0.0,
          provider_epoch,
          0U,
          0,
          true,
          RuntimeScaleApplicationState::kPending,
          std::nullopt};
}

[[nodiscard]] inline Result<void> cancel_prepared_participant(
    IResolvedPreparedParticipantExecutionEngine& engine,
    OperationParticipantPayloadKind payload_kind, std::uint64_t preparation_id) {
  try {
    return payload_kind == OperationParticipantPayloadKind::kMotion
               ? engine.CancelPreparedMotion(preparation_id)
               : engine.CancelPreparedCapabilityCommand(preparation_id);
  } catch (const std::exception& exception) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("prepared participant cancellation threw: ") + exception.what(),
              "prepared_participant_cancel_threw"));
  } catch (...) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                   "prepared participant cancellation threw",
                                   "prepared_participant_cancel_threw"));
  }
}

struct RuleSourceOrder final {
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t source_sequence;
  std::uint64_t record_sequence;
};

inline bool source_order_less(const RuleSourceOrder& left, const RuleSourceOrder& right) {
  return std::tie(left.source_id, left.incarnation, left.source_sequence, left.record_sequence) <
         std::tie(right.source_id, right.incarnation, right.source_sequence, right.record_sequence);
}

inline std::optional<RuleSourceOrder> rule_source_order(const IoRuleEvidenceDTO& evidence) {
  std::optional<RuleSourceOrder> selected;
  const auto consider = [&](RuleSourceOrder key) {
    if (!selected || source_order_less(key, *selected)) selected = std::move(key);
  };
  if (!evidence.source_losses.empty()) {
    for (const auto& loss : evidence.source_losses) {
      const auto sequence = loss.sample        ? loss.sample->publication.source_sequence
                            : loss.source_loss ? loss.source_loss->last_accepted_source_sequence
                            : loss.blackboard ? loss.blackboard->last_accepted_message_sequence
                                              : 0U;
      consider({loss.source_id, loss.incarnation, sequence, loss.cursor.record_sequence});
    }
  } else if (evidence.trip) {
    if (evidence.trip->triggering_acquisition) {
      const auto& actual = *evidence.trip->triggering_acquisition;
      consider({actual.source_id, actual.incarnation, actual.source_sequence,
                actual.source_cursor.record_sequence});
    } else {
      // An already-active arm baseline has no causal event. Canonical ordering
      // uses its real qualified supporting samples/cursors, never a made-up one.
      if (evidence.trip->blackboard) {
        const auto& bb = *evidence.trip->blackboard;
        const auto cursor = std::find_if(
            evidence.trip->source_cursors.begin(), evidence.trip->source_cursors.end(),
            [&](const auto& value) { return value.source_id == bb.source_id; });
        if (cursor == evidence.trip->source_cursors.end()) return std::nullopt;
        consider({bb.source_id, bb.incarnation, bb.message_sequence, cursor->record_sequence});
      }
      for (const auto& sample : evidence.trip->matched_samples) {
        const auto& publication = sample.publication;
        const auto cursor = std::find_if(
            evidence.trip->source_cursors.begin(), evidence.trip->source_cursors.end(),
            [&](const auto& value) { return value.source_id == publication.source_id; });
        if (cursor == evidence.trip->source_cursors.end()) return std::nullopt;
        consider({publication.source_id, publication.incarnation, publication.source_sequence,
                  cursor->record_sequence});
      }
    }
  }
  return selected;
}

}  // namespace resource_addressed_kernel_detail

using namespace resource_addressed_kernel_detail;

struct ResourceAddressedExecutionKernel::ReplayEntry final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::ControlReplayEntry final {
  OperationControlRequestDTO request;
  OperationControlAcceptedDTO accepted;
  bool durable{};
};

struct ResourceAddressedExecutionKernel::StopReplayEntry final {
  struct SelectionReceipt final {
    std::vector<std::uint32_t> widened_node_indices;
    std::optional<StopAcceptedDTO> fresh_containment;
  };
  std::variant<StopIntentDTO, ProgramExecutionSelectionRequestDTO> intent;
  std::variant<StopAcceptedDTO, SelectionReceipt> accepted;

  const RequestId& request_id() const {
    return std::visit(
        [](const auto& value) -> const RequestId& { return value.request_context.request_id; },
        intent);
  }
  bool matches(const StopIntentDTO& value) const {
    const auto* physical = std::get_if<StopIntentDTO>(&intent);
    return physical != nullptr && *physical == value;
  }
};

struct ResourceAddressedExecutionKernel::RuntimeScaleReplayEntry final {
  RuntimeScaleRequestDTO request;
  RuntimeScaleAcceptedDTO accepted;
};

struct ResourceAddressedExecutionKernel::RuntimeScaleControlResolution final {
  RuntimeScaleControlSelectorDTO selector;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ScalingDomainId scaling_domain_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  std::optional<CoordinatedScalingBarrierId> coordinated_barrier_id{};

  [[nodiscard]] auto key() const noexcept {
    return std::tie(control_domain_id, control_domain_revision, control_binding_id,
                    control_binding_revision, scaling_domain_id, active_control_configuration_id,
                    active_control_configuration_generation);
  }

  [[nodiscard]] bool same_control_scope(const RuntimeScaleControlResolution& other) const noexcept {
    return key() == other.key();
  }

  friend bool operator==(const RuntimeScaleControlResolution&,
                         const RuntimeScaleControlResolution&) = default;
};

struct ResourceAddressedExecutionKernel::RuntimeScaleControlScope final {
  RuntimeScaleControlSelectorDTO selector;
  std::vector<RuntimeScaleControlResolution> members;

  [[nodiscard]] bool same_control_scope(const RuntimeScaleControlScope& other) const noexcept {
    return members.size() == other.members.size() &&
           std::equal(members.begin(), members.end(), other.members.begin(),
                      [](const auto& a, const auto& b) { return a.same_control_scope(b); });
  }
};

struct ResourceAddressedExecutionKernel::RuntimeScaleControlSetting final {
  RuntimeScaleControlScope resolution;
  RuntimeScaleControlGeneration generation{0U};
  double requested_scale{1.0};
};

struct ResourceAddressedExecutionKernel::RuntimeScaleControlReplayEntry final {
  RuntimeScaleControlRequestDTO request;
  RuntimeScaleControlAcceptedDTO accepted;
  RuntimeScaleControlScope scope;
};

struct ResourceAddressedExecutionKernel::StopFaultRecoveryReplayEntry final {
  using Intent = std::variant<StopFaultRecoveryIntentDTO, IoGlobalRuleRecoveryIntentDTO>;
  using Result = std::variant<StopFaultRecoveryResultDTO, IoGlobalRuleRecoveryResultDTO>;
  Intent intent;
  Result result;
  // Uncertain journal commits retry this exact payload, never a newly evaluated
  // timestamp or a newly minted token. The original latch stays in its row.
  std::optional<IoGlobalRuleRecoveryRecordDTO> pending_global_record{};
  [[nodiscard]] const RequestId& request_id() const {
    return std::visit([](const auto& value) -> const RequestId& {
      return value.request_context.request_id;
    }, intent);
  }
};

struct ResourceAddressedExecutionKernel::OperationRecoveryReplayEntry final {
  OperationId operation_id;
  OperationRevision operation_revision{0U};
  KernelRecoveryActionRequestDTO request;
  KernelRecoveryActionResultDTO result;
  bool outcome_unresolved{false};
};

struct ResourceAddressedExecutionKernel::PartialDispatchStopScope final {
  OperationId operation_id;
  OperationRevision operation_revision{0U};
  std::vector<OperationParticipantId> participant_ids;
};

struct ResourceAddressedExecutionKernel::ProgramCycleClaimRun::MotionContinuation final {
  ProgramNodeExecutionScopeDTO origin;
  OperationId operation_id;
  OperationParticipantId participant_id;
  ContentDigest prepared_contract_digest;
  RuntimeParticipantBinding binding;
  std::shared_ptr<const detail::MotionResumeIntent> intent;
  ResolvedFrameDTO base_frame;
  ResolvedFrameDTO tcp_frame;
  PlannerCapabilityBindingDTO planner_capability;
  std::size_t authored_motion_kind{};
};

[[nodiscard]] inline bool conclusive_runtime_collision(const ErrorDTO& collision) {
  const auto& details = collision.details;
  const auto matches = [&](const char* key, const char* value) {
    const auto found = details.find(key);
    return found != details.end() && found->second == value;
  };
  return matches("in_collision", "true") &&
      (matches("runtime_collision_reason", "runtime_collision_detected") ||
       matches("runtime_collision_reason", "runtime_lookahead_collision")) &&
      !details.contains("contact_policy_violation") &&
      !details.contains("collision_result_raw_contact_grasp_violation") &&
      (!details.contains("runtime_query_engine_status") || matches("runtime_query_engine_status", "ok")) &&
      !details.contains("runtime_query_failure_reason");
}

[[nodiscard]] inline std::optional<std::int64_t> motion_takeover_budget_ns(
    const std::optional<double> seconds) noexcept {
  if (!seconds || !std::isfinite(*seconds) || *seconds <= 0.0) return std::nullopt;
  // The existing monotonic clock uses integral nanoseconds. Round one unit
  // conversion upward, without selecting a policy duration or runtime default.
  const auto ns = std::ceil(static_cast<long double>(*seconds) * 1'000'000'000.0L);
  if (!std::isfinite(ns) || ns > static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::nullopt;
  return static_cast<std::int64_t>(ns);
}

[[nodiscard]] inline std::optional<std::int64_t> motion_takeover_planning_budget_ns(
    const std::optional<double> planning_seconds,
    const std::optional<double> preparation_seconds) noexcept {
  if (planning_seconds) return motion_takeover_budget_ns(planning_seconds);
  const auto preparation = motion_takeover_budget_ns(preparation_seconds);
  if (!preparation) return std::nullopt;
  // Derive the default only at smooth-takeover admission, preserving authored
  // omission. Integer 4/5 avoids floating rounding (.5 s -> exactly .4 s) and
  // multiplication overflow. A sub-2 ns total cannot represent a positive,
  // strictly smaller cap; the existing replacement-context check rejects zero.
  return (*preparation / 5) * 4 + ((*preparation % 5) * 4) / 5;
}

inline void bind_recovery_request(KernelRecoveryActionRequestDTO& request,
                           const KernelRecoveryAssessmentDTO& assessment) {
  request.expected_assessment_revision = assessment.assessment_revision;
  request.expected_control_command_epoch = assessment.control_command_epoch;
  request.expected_execution_id = assessment.execution_id;
  request.expected_latch_id = assessment.latch_id;
  request.expected_stop_certificate_id = assessment.stop_certificate_id;
  request.expected_stop_owner_generation = assessment.stop_owner_generation;
  // Freshness is authorized and consumed by the addressed owner. Its local
  // stopped participant does not own aggregate collision provenance.
  request.fresh_motion_attempt = false;
}

struct ActiveOperationParticipant final {
  RuntimeParticipantBinding binding;
  std::optional<RuntimeParticipantStopReadiness> stop_readiness;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
  std::optional<MotionInterruptStopProfileDTO> default_operator_stop_profile;
  std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
  std::optional<MotionInterruptStopProfileDTO> default_fault_stop_profile;
  std::shared_ptr<const PreparedParticipantStopPlanDTO> fault_stop_plan;
  std::optional<PreparedOperationParticipantDTO> prepared_participant;
  IResolvedParticipantExecutionEngine* engine{};
  IResolvedPreparedParticipantExecutionEngine* prepared_motion_engine{};
  IControlDomainExecutionPort* stop_execution_port{};
  ResourceSlotOwnerDTO slot_owner;
  std::vector<ResourceId> resources;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  ResourceSlotState slot_state{ResourceSlotState::kReserved};
  std::uint64_t subordinate_id{};
  std::uint64_t subordinate_generation{};
  // Capability commands retain both identities: the preparation addresses
  // their local execution owner, while subordinate_id becomes the backend
  // command id after dispatch. Motion uses the same value for both.
  std::uint64_t preparation_id{};
  bool stop_cancelled_pre_dispatch{};
  bool reconciliation_in_flight{};
  bool prepare_in_flight{};
  bool prepared_advance_in_flight{};
  bool prepared_dispatch_released{};
  bool dispatch_effect_in_flight{};
  std::optional<ErrorDTO> last_subordinate_error;
  std::optional<ExecuteRequestDTO> motion_request;
  std::shared_ptr<const detail::MotionResumeIntent> continuation{};
  std::size_t continuation_node_index{};
  /// Runtime collision observed outside an active Stop; its containment Stop
  /// ends this participant InterventionRequired with this cause (item 19).
  std::optional<ErrorDTO> runtime_collision_error{};
  /// Q58: a Stop that did not wait for this command's still-returning dispatch
  /// leaves its exact local settlement here; the dispatch return performs it.
  std::optional<AddressedParticipantStopSettlementRequestDTO> deferred_stop_settlement{};
  /// Q60: a Stop that closed without waiting for this never-dispatched
  /// participant's prepared cancellation to return leaves its exact local
  /// settlement here. The cancellation's own terminal hands it to the Stop
  /// lane, which performs it; until then the participant stays borrowed, so
  /// Reset, recovery and the next operation on its resources cannot overtake
  /// the late reply.
  struct DeferredPreDispatchCancelSettlement final {
    AddressedParticipantStopSettlementRequestDTO request;
    StopControlWorkToken stop_token;
    std::optional<OperationStateKind> returned_state{};
    std::optional<ErrorDTO> returned_error{};
    bool settlement_in_flight{};
  };
  std::optional<DeferredPreDispatchCancelSettlement> deferred_cancel_settlement{};
  GripperActuationKind gripper_actuation{GripperActuationKind::kPositionCommanded};

  // Set only by actual selected moving containment, including partial Stops.
  bool stop_containment_owned{};
  // Authority is the exact returned engine session's irreversible precommit
  // cancellation, independently of the earlier historical Kernel release.
  bool prepared_engine_no_effect_confirmed{};
  /// The exact handover commit detached this private physical owner. Its
  /// immutable public participant row remains part of the original operation.
  bool ownership_transferred{};
  /// Reuses ordinary resolved preparation authority for a pending newcomer;
  /// no caller-supplied priority, source curve or dispatch permission.
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> takeover_preparation{};
  std::optional<std::int64_t> takeover_preparation_budget_ns{};
  std::optional<std::int64_t> takeover_planning_budget_ns{};

  [[nodiscard]] bool non_motion_transaction() const noexcept {
    return payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
           (binding.value().io_output_binding.has_value() ||
            (gripper_actuation == GripperActuationKind::kPneumatic && stop_readiness &&
             stop_readiness->value().resolved_stop_base_plan.gripper_actuation_kind ==
                 GripperActuationKind::kPneumatic));
  }
  [[nodiscard]] bool stop_owns_reconciliation() const noexcept {
    return stop_containment_owned;
  }

  [[nodiscard]] bool finishes_current_gripper_transaction() const noexcept {
    return payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
           gripper_actuation == GripperActuationKind::kPneumatic &&
           !stop_cancelled_pre_dispatch;
  }

  [[nodiscard]] bool permanent_preparation_no_effect() const noexcept {
    const bool never_prepared = preparation_id == 0U && subordinate_id == 0U &&
                                !prepared_dispatch_released;
    return non_motion_transaction() && stop_cancelled_pre_dispatch && subordinate_generation == 0U &&
           slot_state == ResourceSlotState::kTerminal && !dispatch_effect_in_flight &&
           !prepare_in_flight && !prepared_advance_in_flight && !reconciliation_in_flight &&
           !deferred_cancel_settlement && !deferred_stop_settlement &&
           (never_prepared || prepared_engine_no_effect_confirmed);
  }

  /// A Prepare call, prepared read or deferred pre-dispatch cancellation
  /// (Q60) still borrows this participant's engine.
  [[nodiscard]] bool preparation_borrow_in_flight() const noexcept {
    return prepare_in_flight || prepared_advance_in_flight ||
           deferred_cancel_settlement.has_value();
  }
  /// Tokens only; the shared signal module owns predicate/source state.
  std::vector<IoRuleRegistrationDTO> io_rule_registrations{};
  std::vector<IoMotionRulePolicyBindingDTO> io_motion_rule_policies{};
};

/// Exact reserved resource set of one operation: the union over its
/// participants (spec 450 §5.2 coordinated rule).
[[nodiscard]] inline std::vector<ResourceId> operation_resources(
    const std::vector<ActiveOperationParticipant>& participants) {
  std::set<ResourceId> resources;
  for (const auto& participant : participants) {
    resources.insert(participant.resources.begin(), participant.resources.end());
  }
  return {resources.begin(), resources.end()};
}

// Strict M1 authority shared by already resolved admission/manual checks.
// An authored payload/capability alone never grants the Stop-only exception.
[[nodiscard]] inline bool non_motion_registration(
    const RuntimeParticipantExecutionRegistration& registration) {
  if (registration.binding.value().io_output_binding)
    return registration.engine && registration.prepared_motion_engine &&
           !registration.stop_readiness && !registration.binding.value().control_path;
  return registration.binding.value().capability_id.value() == "tool.gripper.command" &&
      registration.engine && registration.stop_readiness &&
      registration.stop_readiness->value().resolved_stop_base_plan.gripper_actuation_kind ==
          GripperActuationKind::kPneumatic &&
      registration.engine->gripper_actuation_kind(
          registration.binding.value().command_target_id.value()) == GripperActuationKind::kPneumatic;
}

/// The motion-producing recovery actions gated by operator-alarm attention
/// (spec 450 §5.2); acknowledge_and_continue moves nothing and stays open.
[[nodiscard]] inline bool motion_producing_recovery(const RecoveryAction action) noexcept {
  return action == RecoveryAction::kRetryMotionFromCurrentState ||
         action == RecoveryAction::kRetryAfterSceneRepair ||
         action == RecoveryAction::kReplanFromStoppedState;
}

struct PendingOperationControl final {
  OperationControlAcceptedDTO accepted;
  std::size_t expected_effects{};
  std::size_t completed_effects{};
  std::optional<ErrorDTO> first_failure;
  bool durable{};
  bool superseded_by_stop{};
  bool effects_dispatched{};
  std::optional<ProgramNodeExecutionScopeDTO> execution_scope;
};

/// The accepted candidate's immutable link to the sole physical Stop owner.
/// Stop/candidate phases are read from their existing owners, never duplicated.
struct MotionTakeoverSpringFallbackDTO final {
  StopId stop_id;
  StopGeneration stop_generation;
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const SpringBackReturnPointDTO> return_point;
  ErrorDTO smooth_failure;
  std::uint64_t abandoned_preparation_id{};
  std::shared_ptr<const StopCertificateDTO> certificate{};
};

struct ResourceAddressedExecutionKernel::ActiveOperation final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
  std::vector<ActiveOperationParticipant> participants;
  OperationStateDTO state;
  std::optional<PendingOperationControl> pending_control;
  bool runtime_scale_effect_in_flight{};
  bool stop_effect_in_flight{};
  bool ordinary_acceptance_pending{};
  bool ordinary_acceptance_durable{};
  bool coordinated_dispatch_barrier_released{};
  std::optional<RuntimeScaleStatusDTO> runtime_scale_status;
  // Ordinary captures serialize only capture/write/publication, never provider
  // work. Stop has its own authoritative linearization and bypasses this gate.
  std::shared_ptr<std::mutex> commit_mutex{std::make_shared<std::mutex>()};
  bool ordinary_commit_in_flight{};
  bool recovery_effect_in_flight{};
  bool coordinated_cancellation_in_flight{};
  std::uint64_t last_capture_order{};
  struct SettledStop final {
    StopId stop_id;
    StopGeneration generation;
    StopCertificateResultDTO result;
    std::optional<StopCertificateRefDTO> certificate;
    bool requested_controlled_stop_succeeded{false};
  };
  // One current settlement fact, not a pin on rotating diagnostic history.
  // A newer Stop cannot borrow its predecessor's result.
  std::optional<SettledStop> settled_stop{};
  std::shared_ptr<const ResolvedManualProgramHandover> manual_handover{};
  // Manual Robot Control identity, fixed at admission from the binding's
  // intrinsic direct control. Spec 450 §5.2 / spec 530 §11 gate ordinary
  // Run/Resume and motion-producing recovery; they never gate a manual
  // operation, whether or not a manual cell-admission claim still names it.
  bool manual{};
  bool retry_run_ended{false};
  mutable std::vector<KernelRecoveryAssessmentDTO> recovery_assessment_inputs{};
  mutable std::uint64_t recovery_assessment_revision{};
  mutable std::optional<KernelRecoveryAssessmentDTO> published_recovery_assessment{};
  using OperationNode = std::map<OperationId, std::unique_ptr<ActiveOperation>>::node_type;
  /// Source-owned accepted metadata; neither node presents duplicate physical
  /// ownership to Stop. The next slot is overwritten, never a waiting queue.
  OperationNode pending_motion_takeover{};
  OperationNode next_motion_takeover{};
  std::optional<PreparedMotionReplacementSourceDTO> takeover_source{};
  bool takeover_advance_in_flight{};
  std::optional<MotionTakeoverSpringFallbackDTO> motion_takeover_spring_fallback{};


  [[nodiscard]] std::optional<StopCertificateResultDTO> terminal_stop_result() const noexcept {
    if (settled_stop.has_value() && state.active_stop_id == settled_stop->stop_id &&
        state.active_stop_generation == settled_stop->generation)
      return settled_stop->result;
    return std::nullopt;
  }

  [[nodiscard]] bool cancelled_pneumatic_preparation_shape() const noexcept {
    if (state.state != OperationStateKind::kCancelledPreDispatch || state.participants.empty() ||
        state.participants.size() != participants.size()) return false;
    for (std::size_t index = 0U; index < participants.size(); ++index)
      if (!participants[index].non_motion_transaction() ||
          state.participants[index].state != OperationStateKind::kCancelledPreDispatch ||
          state.participants[index].payload_kind != OperationParticipantPayloadKind::kCapabilityCommand ||
          state.participants[index].terminal_error) return false;
    return true;
  }
  [[nodiscard]] bool permanent_preparation_no_effect() const noexcept {
    if (!cancelled_pneumatic_preparation_shape() || pending_control || recovery_effect_in_flight ||
        coordinated_cancellation_in_flight || stop_effect_in_flight || runtime_scale_effect_in_flight)
      return false;
    return std::all_of(participants.begin(), participants.end(), [](const auto& row) {
      return row.permanent_preparation_no_effect();
    });
  }

  [[nodiscard]] bool ordinary_pneumatic_acknowledgement() const noexcept {
    return !participants.empty() &&
        std::all_of(participants.begin(), participants.end(), [](const auto& row) {
          return row.non_motion_transaction() && !row.stop_owns_reconciliation();
        });
  }

  /// DX778: only exact typed pneumatic Failures may be consumed without Reset.
  [[nodiscard]] bool pneumatic_final_failure() const noexcept {
    if (state.state != OperationStateKind::kFailed || state.participants.empty() ||
        state.participants.size() != participants.size()) return false;
    bool failed = false;
    for (std::size_t index = 0; index < participants.size(); ++index) {
      const auto& row = state.participants[index];
      if (row.state == OperationStateKind::kCompleted) continue;
      if (row.state != OperationStateKind::kFailed ||
          !participants[index].finishes_current_gripper_transaction() ||
          !row.terminal_error ||
          !row.terminal_error->details.contains("pneumatic_transaction_final") ||
          row.terminal_error->details.at("pneumatic_transaction_final") != "true") return false;
      failed = true;
    }
    return failed;
  }

  [[nodiscard]] bool has_pending_io_rule_response() const noexcept {
    return std::any_of(state.participants.begin(), state.participants.end(), [](const auto& row) {
      return std::any_of(row.io_rule_outcomes.begin(), row.io_rule_outcomes.end(),
                         [](const auto& outcome) {
                           return outcome.stop_id && !outcome.stop_certificate_id;
                         });
    });
  }

  [[nodiscard]] bool stop_allows_retirement() const noexcept {
    if (has_pending_io_rule_response()) return false;
    if (!state.active_stop_id.has_value()) return true;
    const auto outcome = terminal_stop_result();
    return outcome == StopCertificateResultDTO::kSucceededNominal ||
           outcome == StopCertificateResultDTO::kSucceededEscalated;
  }

  [[nodiscard]] ActiveOperationParticipant* participant_for(
      const IResolvedParticipantExecutionEngine* selected_engine) noexcept {
    const auto found = std::find_if(
        participants.begin(), participants.end(),
        [&](const auto& participant) { return participant.engine == selected_engine; });
    return found == participants.end() ? nullptr : &*found;
  }

  [[nodiscard]] const ActiveOperationParticipant* participant_for(
      const IResolvedParticipantExecutionEngine* selected_engine) const noexcept {
    const auto found = std::find_if(
        participants.begin(), participants.end(),
        [&](const auto& participant) { return participant.engine == selected_engine; });
    return found == participants.end() ? nullptr : &*found;
  }

  [[nodiscard]] bool dispatch_effect_in_flight() const noexcept {
    return std::any_of(participants.begin(), participants.end(), [](const auto& participant) {
      return participant.dispatch_effect_in_flight;
    });
  }

  [[nodiscard]] bool reconciliation_in_flight() const noexcept {
    return std::any_of(participants.begin(), participants.end(), [](const auto& participant) {
      return participant.reconciliation_in_flight;
    });
  }

  /// Exact Kernel-owned proof that this held operation can never have caused,
  /// and can no longer cause, a physical effect: no participant crossed the
  /// dispatch barrier, every slot is already terminal (so the barrier refuses
  /// any late prepared product), and no preparation, advance, dispatch,
  /// cancellation, reconciliation, control, recovery or Stop borrow is still
  /// open. Progress values and observed poses are never part of this proof.
  [[nodiscard]] bool never_dispatched_and_quiescent() const noexcept {
    return !state.active_stop_id.has_value() && !stop_effect_in_flight &&
           !recovery_effect_in_flight && !coordinated_cancellation_in_flight &&
           !coordinated_dispatch_barrier_released && !pending_control.has_value() &&
           std::all_of(participants.begin(), participants.end(), [](const auto& participant) {
             return !participant.prepared_dispatch_released &&
                    !participant.dispatch_effect_in_flight && !participant.prepare_in_flight &&
                    !participant.prepared_advance_in_flight &&
                    !participant.reconciliation_in_flight &&
                    participant.slot_state == ResourceSlotState::kTerminal;
           });
  }

  /// A failure held for intervention that never crossed the dispatch
  /// barrier. It keeps its slots, recovery authority and admission exclusion,
  /// but has no physical effect for Stop to contain or preview to wait for.
  [[nodiscard]] bool held_without_physical_effect() const noexcept {
    return state.state == OperationStateKind::kInterventionRequired &&
           never_dispatched_and_quiescent();
  }

  [[nodiscard]] bool containment_effect_uncertain() const noexcept {
    return stop_effect_in_flight || dispatch_effect_in_flight() ||
           state.state == OperationStateKind::kInterrupted ||
           (state.state == OperationStateKind::kInterventionRequired &&
            !held_without_physical_effect());
  }
};

class ResourceAddressedExecutionKernel::OperationCommitGuard final {
 public:
  OperationCommitGuard(ResourceAddressedExecutionKernel& owner, const OperationId& id)
      : owner_(owner), id_(id) {
    {
      std::lock_guard state_lock(owner_.state_mutex_);
      const auto* operation = owner_.find_active_operation_locked(id_);
      if (operation == nullptr) return;
      mutex_ = operation->commit_mutex;
    }
    lock_ = std::unique_lock(*mutex_);
    std::lock_guard state_lock(owner_.state_mutex_);
    auto* operation = owner_.find_active_operation_locked(id_);
    if (operation != nullptr && operation->commit_mutex == mutex_) {
      operation->ordinary_commit_in_flight = true;
      claimed_ = true;
    }
  }
  ~OperationCommitGuard() { release(); }
  void release() noexcept {
    if (!lock_.owns_lock()) return;
    try {
      std::lock_guard state_lock(owner_.state_mutex_);
      auto* operation = owner_.find_active_operation_locked(id_);
      if (claimed_ && operation != nullptr && operation->commit_mutex == mutex_) {
        operation->ordinary_commit_in_flight = false;
        // A cleanup callback may have completed during durable I/O. Retire
        // only after the last ordinary writer no longer references its row.
        owner_.settle_completed_control_locked(id_);
      }
    } catch (...) {
      // Destruction must still release the ordinary gate during exception
      // unwinding. No provider call or storage retry belongs in this guard.
      std::lock_guard state_lock(owner_.state_mutex_);
      owner_.motion_admission_closed_ = true;
    }
    lock_.unlock();
  }

 private:
  ResourceAddressedExecutionKernel& owner_;
  OperationId id_;
  std::shared_ptr<std::mutex> mutex_;
  std::unique_lock<std::mutex> lock_;
  bool claimed_{};
};

[[nodiscard]] inline OperationStateKind aggregate_participant_state(
    const std::vector<OperationParticipantStateDTO>& participants) noexcept {
  const auto all_state = [&](OperationStateKind state) {
    return std::all_of(participants.begin(), participants.end(),
                       [&](const auto& participant) { return participant.state == state; });
  };
  const auto any_state = [&](OperationStateKind state) {
    return std::any_of(participants.begin(), participants.end(),
                       [&](const auto& participant) { return participant.state == state; });
  };
  const auto final_output_failure = [](const auto& row) {
    if (row.state != OperationStateKind::kFailed ||
        row.payload_kind != OperationParticipantPayloadKind::kCapabilityCommand ||
        !row.binding_identity.provider() || !row.output_evidence || !row.terminal_fact ||
        row.terminal_fact->kind != ParticipantExecutionFactKind::kLocalFailure ||
        row.terminal_fact->error != row.terminal_error)
      return false;
    return (*row.output_evidence == IoOutputEvidence::kNotSubmitted &&
            row.terminal_fact->effect == ParticipantEffectKnowledge::kNoEffect) ||
           (*row.output_evidence == IoOutputEvidence::kSourceRejected &&
            row.terminal_fact->effect == ParticipantEffectKnowledge::kEffectSettled);
  };
  const auto completed_or = [&](const OperationStateKind state) {
    return !participants.empty() &&
           std::all_of(participants.begin(), participants.end(), [&](const auto& participant) {
             return participant.state == OperationStateKind::kCompleted ||
                    participant.state == state ||
                    ((participant.state == OperationStateKind::kFailed &&
                      participant.terminal_error &&
                      participant.terminal_error->details.contains("pneumatic_transaction_final") &&
                      participant.terminal_error->details.at("pneumatic_transaction_final") ==
                          "true") ||
                     final_output_failure(participant));
           });
  };
  const auto final_pneumatic_failure = [](const auto& row) {
    return row.state == OperationStateKind::kFailed && row.terminal_error &&
           row.terminal_error->details.contains("pneumatic_transaction_final") &&
           row.terminal_error->details.at("pneumatic_transaction_final") == "true";
  };
  if (any_state(OperationStateKind::kInterventionRequired) ||
      any_state(OperationStateKind::kInterrupted) ||
      std::any_of(participants.begin(), participants.end(), [&](const auto& row) {
        return row.state == OperationStateKind::kFailed && !final_pneumatic_failure(row) &&
               !final_output_failure(row);
      }))
    return OperationStateKind::kInterventionRequired;
  if (any_state(OperationStateKind::kFailed) &&
      std::all_of(participants.begin(), participants.end(), [&](const auto& row) {
        return row.state == OperationStateKind::kCompleted || final_pneumatic_failure(row) ||
               final_output_failure(row);
      }))
    return OperationStateKind::kFailed;
  if (all_state(OperationStateKind::kCompleted)) {
    return OperationStateKind::kCompleted;
  }
  if (completed_or(OperationStateKind::kStopped)) {
    return OperationStateKind::kStopped;
  }
  if (completed_or(OperationStateKind::kCancelledPreDispatch)) {
    return OperationStateKind::kCancelledPreDispatch;
  }
  if (any_state(OperationStateKind::kStopping)) {
    return OperationStateKind::kStopping;
  }
  if (any_state(OperationStateKind::kExecuting)) {
    return OperationStateKind::kExecuting;
  }
  if (any_state(OperationStateKind::kDispatching)) {
    return OperationStateKind::kDispatching;
  }
  if (all_state(OperationStateKind::kPrepared)) {
    return OperationStateKind::kPrepared;
  }
  if (any_state(OperationStateKind::kValidating)) {
    return OperationStateKind::kValidating;
  }
  if (any_state(OperationStateKind::kPlanning)) {
    return OperationStateKind::kPlanning;
  }
  return OperationStateKind::kResolving;
}

[[nodiscard]] inline Result<std::vector<ResourceSlotReservationOwnerDTO>> retained_owner_rows(
    const std::vector<ActiveOperationParticipant>& participants,
    const ResourceSlotTableSnapshotDTO& snapshot) {
  std::vector<ResourceSlotReservationOwnerDTO> rows;
  rows.reserve(participants.size());
  for (const auto& participant : participants) {
    bool any_owned = false;
    bool any_unowned = false;
    for (const auto& resource : participant.resources) {
      const auto found = std::lower_bound(
          snapshot.slots.begin(), snapshot.slots.end(), resource,
          [](const auto& slot, const ResourceId& id) { return slot.resource_id < id; });
      if (found == snapshot.slots.end() || found->resource_id != resource) {
        return reject<std::vector<ResourceSlotReservationOwnerDTO>>(
            "operation participant resource disappeared from the slot table",
            "operation_control_resource_missing", ErrorCode::kInvariantViolation);
      }
      if (found->owner.has_value()) {
        if (*found->owner != participant.slot_owner) {
          return reject<std::vector<ResourceSlotReservationOwnerDTO>>(
              "operation participant resource has a different current owner",
              "operation_control_resource_owner_mismatch", ErrorCode::kInvariantViolation);
        }
        any_owned = true;
      } else {
        any_unowned = true;
      }
    }
    if (any_owned && any_unowned) {
      return reject<std::vector<ResourceSlotReservationOwnerDTO>>(
          "operation participant resource ownership is only partially retained",
          "operation_control_partial_resource_ownership", ErrorCode::kInvariantViolation);
    }
    if (any_owned) {
      rows.push_back(
          ResourceSlotReservationOwnerDTO{participant.slot_owner, participant.resources});
    }
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return std::tie(left.owner.operation_id, left.owner.participant_id) <
           std::tie(right.owner.operation_id, right.owner.participant_id);
  });
  return Result<std::vector<ResourceSlotReservationOwnerDTO>>::Ok(std::move(rows));
}

inline constexpr std::string_view kOperationControlStopSource = "kernel.operation_control";
inline constexpr std::string_view kOperationResetStopReason = "operation_reset";
inline constexpr std::string_view kZeroSpeedHoldStopSource = "kernel.zero_speed_hold";
inline constexpr std::string_view kZeroSpeedHoldStopReason = "zero_speed_hold_timeout";

enum class InternalStopKind : std::uint8_t { kNone, kReset, kZeroSpeedHold };

/// Internal Stops with their own Stop choice: Reset while busy runs the
/// deployment's operator Stop profile and issues the lane's hard stop only
/// on its timeout or when no profile is configured (Q27, as `main`
/// EK:8082-8221); the zero-speed hold timeout issues the lane's hard stop
/// directly (Q31). Neither is a degraded Stop.
[[nodiscard]] inline InternalStopKind internal_stop_kind(const StopIntentDTO* intent) noexcept {
  if (intent == nullptr || !intent->reason.has_value()) return InternalStopKind::kNone;
  const auto& source = intent->request_context.source_label;
  if (source == kOperationControlStopSource && *intent->reason == kOperationResetStopReason)
    return InternalStopKind::kReset;
  if (source == kZeroSpeedHoldStopSource && *intent->reason == kZeroSpeedHoldStopReason)
    return InternalStopKind::kZeroSpeedHold;
  return InternalStopKind::kNone;
}

/// `main`'s degraded-Stop error: the controlled Stop did not contain the
/// motion and the fallback hard stop did (item 25). The operation still ends
/// Stopped; this error explains the downgrade.
[[nodiscard]] inline ErrorDTO degraded_stop_error(const ErrorDTO& cause,
                                           const PreparedParticipantStopPlanDTO& plan,
                                           const std::size_t containing_stage_index) {
  const auto cause_reason = cause.details.find("reason");
  const bool deadline = cause_reason != cause.details.end() &&
                        (cause_reason->second == "participant_stop_stage_deadline_expired" ||
                         cause_reason->second == "participant_stop_stage_command_phase_expired");
  ErrorDTO degraded{
      std::string(kErrorMotionInterruptBackendStopFailed),
      deadline ? "Controlled Stop exceeded the effective Stop deadline; the hard-stop floor "
                 "stopped the motion"
               : "Controlled Stop failed; the hard-stop floor stopped the motion",
      {{"reason", deadline ? "in_stop_completion_timeout" : "operator_stop_profile_failed"},
       {"controlled_stop_error_code", cause.code},
       {"emergency_fallback", "legacy_stop_succeeded"},
       {"hard_stop_transitioned", "true"}}};
  if (cause_reason != cause.details.end()) {
    degraded.details.emplace("controlled_stop_error_reason", cause_reason->second);
  }
  if (plan.participant_stop_profile.has_value()) {
    degraded.details.emplace(
        "requested_stop_mode",
        std::string(stop_mode_string(plan.participant_stop_profile->stop_mode)));
  }
  if (containing_stage_index < plan.stage_commands.size()) {
    degraded.details.emplace(
        "hard_stop_floor", plan.stage_commands[containing_stage_index]
                               .resolved_stage.stop_escalation_stage_id.value());
  }
  return degraded;
}

/// The controlled stage reports the hard stop as the right Stop: Q33, the
/// Stop planner chose it (`main`'s ros2_control operator path); Q45, the
/// controller reports an applied speed factor of exactly 0, so the Stop is
/// the plain cancel. That hard stop ends Stopped with no error; a hard stop
/// used as a fallback after a failed soft Stop or spring-back keeps the
/// degraded error.
[[nodiscard]] inline bool planner_selected_hard_stop(const ErrorDTO& cause) {
  const auto disposition = cause.details.find("stop_disposition");
  return disposition != cause.details.end() &&
         (disposition->second == "planner_selected_hard_stop" ||
          disposition->second == "applied_factor_zero_hard_stop");
}

/// Q18: a Stop ramp that ended with the robot outside the controller's goal
/// tolerance is `main`'s confirmed-but-not-settled case.
[[nodiscard]] inline bool controlled_stop_unsettled_result(const ParticipantStopStageResultDTO& result) {
  if (result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched ||
      result.outcome != ParticipantStopStageOutcomeDTO::kFailed || !result.error.has_value()) {
    return false;
  }
  const auto reason = result.error->details.find("reason");
  return reason != result.error->details.end() &&
         reason->second == "stop_goal_tolerance_violated";
}

[[nodiscard]] inline Result<std::vector<OperationControlEffectRequest>> reset_effect_requests(
    const OperationId& operation_id, const std::vector<ActiveOperationParticipant>& participants,
    const OperationRevision operation_revision) {
  std::vector<OperationControlEffectRequest> requests;
  requests.reserve(participants.size());
  for (const auto& participant : participants) {
    if (participant.non_motion_transaction() && participant.stop_cancelled_pre_dispatch) {
      if (participant.permanent_preparation_no_effect()) continue;
      return reject<std::vector<OperationControlEffectRequest>>(
          "cancelled command still lacks permanent returned-owner no-effect proof",
          "operation_effect_in_flight", ErrorCode::kInvalidStateTransition);
    }
    // A prepared motion already owns process-local ExecutionKernel state even
    // when Stop cancels it before controller dispatch. The Stop transaction
    // has certified that no effect crossed the barrier, so Reset must retire
    // that exact inner execution without issuing another physical Stop. A
    // prepared capability command has no backend command identity until
    // dispatch and therefore still requires no reset effect in this case.
    const bool certified_pre_dispatch_motion =
        participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
        participant.stop_cancelled_pre_dispatch && participant.subordinate_id != 0U;
    if ((!participant.prepared_dispatch_released || participant.stop_cancelled_pre_dispatch) &&
        !certified_pre_dispatch_motion) {
      continue;
    }
    if (participant.engine == nullptr || participant.subordinate_id == 0U ||
        (participant.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
         (participant.subordinate_generation == 0U ||
          participant.binding.value().command_target_id.empty()))) {
      return reject<std::vector<OperationControlEffectRequest>>(
          "dispatched Reset participant has incomplete subordinate identity",
          "operation_reset_subordinate_identity_invalid", ErrorCode::kInvariantViolation);
    }
    requests.push_back(
        OperationControlEffectRequest{participant.engine,
                                      operation_id,
                                      participant.slot_owner.participant_id,
                                      operation_revision,
                                      OperationControlKind::kReset,
                                      participant.subordinate_id,
                                      participant.payload_kind,
                                      participant.binding.value().command_target_id.value(),
                                      participant.subordinate_generation});
  }
  return Result<std::vector<OperationControlEffectRequest>>::Ok(std::move(requests));
}

struct StopParticipantAttempt final {
  bool started{};
  bool superseded{};
  bool result_consumed{};
  std::optional<StopStageAttemptHandle> handle;
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const RemainingStopBudgetDTO> budget;
  /// The Kernel's own watchdog: only a controlled stage's own deadline (Q51);
  /// absent for a hard stage (Q57) and a gripper cancel (Q58).
  std::optional<std::int64_t> stage_deadline_ns;
};

struct StopParticipantWork final {
  OperationId operation_id;
  OperationRevision accepted_operation_revision{0U};
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation{0U};
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness readiness;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory node_category{MotionInterruptNodeCategory::kGeneric};
  PreparedParticipantStopPlanDTO plan;
  std::vector<ResourceId> resources;
  IControlDomainExecutionPort* port{};
  IResolvedPreparedParticipantExecutionEngine* prepared_motion_engine{};
  IResolvedParticipantExecutionEngine* execution_engine{};
  std::uint64_t prepared_execution_id{};
  bool cancelled_pre_dispatch{};
  bool prepared_cancel_requested{};
  bool prepared_cancel_complete{};
  bool prepared_cancel_terminal_observed{};
  bool terminal{};
  bool contained{};
  std::size_t current_stage_index{};
  std::uint64_t next_result_sequence{1U};
  std::optional<ErrorDTO> terminal_error;
  std::array<StopParticipantAttempt, kMaxStopEscalationStages> attempts;
  std::vector<StopAttemptedStageResultDTO> retained_results;
  bool participant_settlement_requested{};
  bool participant_settlement_complete{};
  std::optional<ErrorDTO> participant_settlement_error;
  bool terminal_projection_complete{};
  /// The participant's own prepared owner reported kPreparationFailed for
  /// this exact execution: nothing was dispatched, no effect crossed the
  /// barrier, and the owner already committed its own failure terminal,
  /// which this Stop neither settles nor rewrites.
  bool preparation_failed{};
  /// A non-Stopped motion terminal for this never-dispatched execution. An
  /// already-failed preparation publishes the same failure terminal as a
  /// failed cancellation; only the prepared owner's exact snapshot, read once
  /// on demand outside the owner lock, classifies it.
  std::optional<ErrorDTO> unclassified_prepared_terminal;
  bool prepared_classification_requested{};
  /// A runtime-collision fact arrived for this dispatched participant while
  /// this Stop was containing it. The Stop lane consumes it as escalation
  /// input under the existing cascade rule (strongest authorized stage).
  bool collision_escalation_pending{};
  std::optional<ErrorDTO> collision_escalation_error;
  /// First collision observed on this participant's Stop path. Containment
  /// with a collision on the Stop path ends InterventionRequired carrying it.
  std::optional<ErrorDTO> stop_path_collision;
  /// Why a weaker stage did not contain this participant before a stronger
  /// (fallback hard) stage did. A contained participant with this cause ends
  /// Stopped with a degraded-Stop error (item 25, as `main`).
  std::optional<ErrorDTO> escalation_cause{};
  /// The Stop ramp ended but the controller reported the robot outside its
  /// goal tolerance (Q18, `main`'s confirmed-but-not-settled case): the
  /// participant ends InterventionRequired and no further Stop command is
  /// issued.
  bool controlled_stop_unsettled{};
  /// The hard stop is this participant's intended Stop: requested directly
  /// (Q27, Q31; it then starts at the strongest stage) or chosen by the Stop
  /// planner (Q33). Its strongest stage is then not an escalation.
  bool lane_hard_stop_requested{};
  /// Q60: the never-dispatched participant was closed after its prepared
  /// cancellation was accepted but before that cancellation's terminal
  /// returned. Its exact settlement is deferred to that terminal.
  bool cancel_terminal_deferred{};
  GripperActuationKind gripper_actuation{GripperActuationKind::kPositionCommanded};
  /// Contract77: an interrupt-selected response follows its qualified suffix
  /// sequentially. Ordinary/operator and collision strongest jumps stay intact.
  bool selected_interrupt_suffix{};
  bool participant_failure_preserved{};

  /// No subordinate terminal handoff exists for a never-dispatched participant
  /// that either never received an execution identity or whose preparation
  /// failed in its own owner before dispatch.
  [[nodiscard]] bool settlement_not_required() const noexcept {
    return cancelled_pre_dispatch && (prepared_execution_id == 0U || preparation_failed);
  }
};

inline void fail_prepared_cancellation(StopParticipantWork& work, ErrorDTO failure) {
  if (work.terminal) return;
  work.terminal = true;
  work.contained = false;
  work.terminal_error = std::move(failure);
}

inline void observe_prepared_cancellation_terminal(StopParticipantWork& work,
                                                   const std::uint64_t execution_id,
                                                   const OperationStateKind state,
                                                   std::optional<ErrorDTO> failure) {
  if (!work.cancelled_pre_dispatch || work.prepared_execution_id != execution_id ||
      !terminal(state) || work.terminal)
    return;
  if (state == OperationStateKind::kStopped) {
    work.prepared_cancel_terminal_observed = true;
    return;
  }
  auto terminal_failure =
      failure.value_or(error(std::string(to_string(ErrorCode::kExecutionAborted)),
                             "prepared participant failed while settling pre-dispatch cancellation",
                             "prepared_cancel_terminal_failed"));
  if (work.payload_kind == OperationParticipantPayloadKind::kMotion) {
    if (!work.unclassified_prepared_terminal.has_value())
      work.unclassified_prepared_terminal = std::move(terminal_failure);
    return;
  }
  fail_prepared_cancellation(work, std::move(terminal_failure));
}

inline void settle_failed_preparation(StopParticipantWork& work, ErrorDTO failure) {
  if (work.terminal) return;
  work.preparation_failed = true;
  work.prepared_cancel_terminal_observed = true;
  work.contained = true;
  work.terminal = true;
  work.terminal_error = std::move(failure);
}

struct ResourceAddressedExecutionKernel::InterruptStopSelection final {
  IoRuleRegistrationDTO registration;
  IoRuleEvidenceDTO evidence;
  PreparedParticipantStopPlanDTO plan;
  InterruptSourceId source_id;
  InterruptPreemptionAction action;
  int priority;
  IoRuleStopScopeDTO stop_scope;
  IoRulePreparedStopEntryDTO stop_entry;
  IoRulePreparedStopEntryDTO success_through;
};

struct ResourceAddressedExecutionKernel::GlobalInterruptStopParticipant final {
  OperationId operation_id;
  OperationParticipantId participant_id;
  std::uint64_t preparation_id;
  ContentDigest prepared_contract_digest;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  PreparedParticipantStopPlanDTO plan;
  IoRulePreparedStopEntryDTO stop_entry;
};

struct ResourceAddressedExecutionKernel::GlobalInterruptStopSelection final {
  IoRuleRegistrationDTO registration;
  IoRuleEvidenceDTO evidence;
  bool everything;
  int priority;
  std::vector<GlobalInterruptStopParticipant> participants;
  std::vector<ResourceId> scope_resources;
};

struct ResourceAddressedExecutionKernel::StopTransaction final {
  bool active{};
  StopControlWorkToken token;
  std::optional<StopId> stop_id;
  StopGeneration stop_generation{0U};
  bool full_stop{};
  bool persistence_degraded{};
  EmergencyStopProjectionStateDTO acceptance_projection_state{
      EmergencyStopProjectionStateDTO::kPending};
  bool progress_in_flight{};
  bool fresh_scope_complete{};
  bool terminal{};
  std::optional<StopAcceptedDTO> accepted;
  EmergencyStopProjectedIntentDTO projected_intent;
  std::vector<StopAffectedOperationRevisionDTO> newly_fenced_operation_revisions;
  std::vector<ResourceId> newly_captured_resources;
  /// Only scope first fenced by this transaction. Predecessor operations keep
  /// progressing under their original work tokens and are never redispatched.
  std::vector<OperationId> operations;
  /// Empty means the complete participant closure of `operations`. A nonempty
  /// set is reserved for the Kernel-generated partial-dispatch containment
  /// path and names exactly the physical dispatch calls already released.
  std::vector<OperationParticipantId> participant_ids;
  std::vector<ResourceId> resources;
  std::vector<StopControlWorkToken> predecessor_tokens;
  std::vector<StopCertificateDTO> predecessor_certificates;
  bool participant_work_initialized{};
  std::vector<StopParticipantWork> participants;
  std::optional<StopCertificateDTO> certificate;
  std::optional<StopControlWorkToken> terminal_projection_token;
  std::optional<std::string> malformed_ingress_reason;
  std::optional<StopFaultLatchDTO> fault_latch;
  /// Stop-path collision facts that arrived before participant work existed;
  /// consumed when the work is initialized.
  std::vector<std::tuple<OperationId, OperationParticipantId, ErrorDTO>>
      pending_stop_path_collisions;
  std::optional<OperationId> motion_takeover_candidate_id{};
  bool force_spring_return{};
  std::optional<InterruptStopSelection> interrupt_selection{};
  std::vector<InterruptStopSelection> disjoint_interrupt_selections{};
  /// Logical arbitration never changes the immutable initial physical plan,
  /// stage attempts, or deadline. A higher-priority fault owns its own fact.
  std::vector<InterruptStopSelection> interrupt_winners{};
  /// Immutable physical selection. Later global logical winners join the
  /// existing transaction without changing this plan, membership or budget.
  std::optional<GlobalInterruptStopSelection> global_interrupt_selection{};
  std::vector<GlobalInterruptStopSelection> global_interrupt_winners{};
  std::vector<internal::StopProofPredecessorScope> proof_predecessor_scopes{};
};

// A scoped global Stop owns only its captured physical/logical participants.
// Sharing an enclosing operation does not transfer a sibling's guard or result.
template <typename Transaction>
[[nodiscard]] inline bool scoped_global_stop_excludes_participant(
    const Transaction& transaction, const OperationId& operation,
    const OperationParticipantId& participant, const std::vector<ResourceId>& resources) {
  if (transaction.full_stop || transaction.global_interrupt_winners.empty()) return false;
  for (const auto& winner : transaction.global_interrupt_winners)
    for (const auto& selected : winner.participants)
      if (selected.operation_id == operation && selected.participant_id == participant) return false;
  for (const auto& winner : transaction.interrupt_winners) {
    const auto& scope = std::get<IoOperationRuleScopeDTO>(winner.registration.scope);
    if (scope.operation_id == operation && scope.participant_id == participant) return false;
  }
  if (transaction.global_interrupt_selection &&
      !transaction.global_interrupt_selection->everything)
    return true;  // Initial addressed selection is an exact participant set.
  return !std::all_of(resources.begin(), resources.end(), [&](const auto& resource) {
    return std::binary_search(transaction.resources.begin(), transaction.resources.end(), resource);
  });
}

/// Snapshot/result ownership includes logical responses on inherited operations.
/// Physical command ownership remains exclusively transaction.operations.
template <typename Transaction>
[[nodiscard]] inline std::vector<OperationId> stop_operation_snapshot_ids(
    const Transaction& transaction) {
  auto ids = transaction.operations;
  for (const auto& winner : transaction.interrupt_winners)
    ids.push_back(std::get<IoOperationRuleScopeDTO>(winner.registration.scope).operation_id);
  for (const auto& winner : transaction.global_interrupt_winners)
    for (const auto& participant : winner.participants) ids.push_back(participant.operation_id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

struct ResourceAddressedExecutionKernel::StopTerminalRecord final {
  StopStatusDTO status;
  StopCertificateDTO certificate;
  std::optional<StopFaultLatchDTO> fault_latch;
  std::optional<StopControlWorkToken> terminal_projection_token;
  std::unique_ptr<StopSurveillanceReducerState> surveillance_state;
  std::optional<ScheduleHandle> surveillance_schedule_handle;
  std::uint64_t surveillance_schedule_generation{};
  bool surveillance_callback_pending{};
  std::optional<StopControlWorkToken> surveillance_projection_token;
  std::int64_t next_surveillance_sample_ns{};
  std::int64_t next_surveillance_checkpoint_ns{};
  std::optional<StopAcceptedDTO> accepted{};
};

struct ResourceAddressedExecutionKernel::StopProjectionJob final {
  StopControlWorkToken token;
  bool active{};
  std::optional<EmergencyStopAcceptanceRecordDTO> acceptance;
  std::optional<EmergencyStopProjectedIntentDTO> intent;
  std::optional<StopAcceptedDTO> accepted;
  std::optional<EmergencyStopAffectedStateSnapshotDTO> affected_state;
  std::optional<StopStatusDTO> terminal_status;
  std::optional<StopCertificateDTO> terminal_certificate;
  std::optional<StopFaultLatchDTO> terminal_fault_latch;
  std::vector<OperationStateDTO> terminal_operation_states;
  std::optional<StopPostTerminalSurveillanceDTO> surveillance_checkpoint;
  std::optional<StopFaultLatchDTO> surveillance_fault_latch;
};

namespace resource_addressed_kernel_detail {

inline constexpr std::uint64_t kMaximumKernelJsonInteger = 9'007'199'254'740'991ULL;
inline constexpr std::size_t kMaximumResidentStopTerminalRecords = 64U;
// Recognized Stop authority is already linearized, and containment never
// depends on this sole non-critical projection worker. Each Stop projection is
// one journal attempt: the journal hands its writer to a waiting Stop
// projection right after the one write in progress, so a failed attempt is a
// genuine storage fault and fails closed through the persistence fault latch.
// The status records that single attempt as its retry generation.
inline constexpr std::uint64_t kStopJournalProjectionAttempt = 1U;

[[nodiscard]] inline std::int64_t checked_deadline(std::int64_t now, std::int64_t duration) noexcept {
  if (now < 0 || duration <= 0 || duration > kMaximumStopDurationNs ||
      now > std::numeric_limits<std::int64_t>::max() - duration) {
    return -1;
  }
  return now + duration;
}

/// Q28/Q51: this robot's own controlled-Stop deadline, measured from the Stop
/// acceptance and computed at Stop time from the stage's exact prepared
/// controlled-Stop parameters (`max_stop_time_s`, else deceleration time +
/// 0.5 s). Absent for every other stage: a hard stage is bounded by its
/// participant with the lane's stop-completion timeout (Q57) and a gripper
/// cancel is fire-and-forget (Q58).
[[nodiscard]] inline std::optional<Duration> controlled_stage_deadline_ns(
    const PreparedStopStageCommandDTO& command) {
  if (command.resolved_stage.containment_command.command_kind !=
          StopContainmentCommandKind::kMotionControlledStop ||
      !command.validated_contract_parameters.has_value()) {
    return std::nullopt;
  }
  return controlled_stop_deadline_from_contract_parameters(*command.validated_contract_parameters);
}

/// The per-robot Stop-time values handed with one stage (Q51): no shared
/// cell-wide budget.
[[nodiscard]] inline RemainingStopBudgetDTO remaining_stop_budget(
    const PreparedStopStageCommandDTO& command, const StopId& stop_id,
    const StopGeneration stop_generation, const std::int64_t now,
    const std::int64_t accepted_at_ns) {
  RemainingStopBudgetDTO budget{stop_id, stop_generation,
                                std::max<std::int64_t>(0, now - accepted_at_ns), std::nullopt};
  if (const auto deadline = controlled_stage_deadline_ns(command)) {
    budget.remaining_controlled_deadline_ns =
        std::max<std::int64_t>(0, *deadline - budget.elapsed_since_stop_ns);
  }
  return budget;
}

/// The Kernel's own watchdog for one stage: only a controlled stage's own
/// deadline (as `main`'s controlled-Stop deadline watchdog). The Kernel arms
/// no watchdog of its own for a hard stage (Q57) or a gripper cancel (Q58).
[[nodiscard]] inline std::optional<std::int64_t> stage_watchdog_deadline(
    const RemainingStopBudgetDTO& budget, const std::int64_t now) noexcept {
  if (!budget.remaining_controlled_deadline_ns.has_value()) {
    return std::nullopt;
  }
  const auto deadline = checked_deadline(now, *budget.remaining_controlled_deadline_ns);
  return deadline < 0 ? std::nullopt : std::optional(deadline);
}

[[nodiscard]] inline Result<ResolvedParticipantStopStageDTO> resolve_participant_stop_stage(
    const StopParticipantWork& item, const StopId& stop_id, const StopGeneration stop_generation,
    const std::size_t stage_index, const RemainingStopBudgetDTO& budget) {
  const auto& binding = item.binding.value();
  const auto& path = *binding.control_path;
  const auto& selected = item.plan.stage_commands[stage_index].resolved_stage;
  std::optional<CapabilityProviderBindingId> provider_binding_id;
  std::optional<CapabilityProviderBindingRevision> provider_binding_revision;
  const auto provider_matches = [&](const RuntimeCapabilityProviderPathDTO& provider) {
    return provider.provider_instance_id == selected.command_target.provider_instance_id &&
           provider.provider_manifest_id == selected.command_target.provider_manifest_id &&
           provider.provider_manifest_revision ==
               selected.command_target.provider_manifest_revision &&
           provider.provider_runtime_unit_id == selected.command_target.runtime_unit_id &&
           provider.provider_runtime_epoch == selected.command_target.provider_runtime_epoch;
  };
  const auto select_provider = [&](const RuntimeCapabilityProviderPathDTO& provider) {
    provider_binding_id = provider.capability_provider_binding_id;
    provider_binding_revision = provider.capability_provider_binding_revision;
  };
  const bool control_path_matches =
      selected.command_target.provider_instance_id == path.provider_instance_id &&
      selected.command_target.provider_manifest_id == path.provider_manifest_id &&
      selected.command_target.provider_manifest_revision == path.provider_manifest_revision &&
      selected.command_target.runtime_unit_id == path.provider_runtime_unit_id &&
      selected.command_target.provider_runtime_epoch == path.provider_runtime_epoch;
  switch (selected.containment_command.command_kind) {
    case StopContainmentCommandKind::kControllerGoalCancel:
    case StopContainmentCommandKind::kMotionControlledStop:
      break;
    case StopContainmentCommandKind::kCapabilityCommandCancel:
    case StopContainmentCommandKind::kSafeOutputCommand:
    case StopContainmentCommandKind::kLogicalCancel:
    case StopContainmentCommandKind::kRegistered:
      // Absence of a capability-provider binding names the direct control
      // path.  Preserve that authority when a supporting capability is
      // co-located in the same provider/runtime unit; target identity alone
      // cannot make the supporting path more specific than the direct path.
      if (control_path_matches) {
        break;
      }
      if (binding.capability_provider_path.has_value() &&
          provider_matches(*binding.capability_provider_path)) {
        select_provider(*binding.capability_provider_path);
      } else {
        const auto supporting =
            std::find_if(binding.supporting_capability_provider_paths.begin(),
                         binding.supporting_capability_provider_paths.end(), provider_matches);
        if (supporting != binding.supporting_capability_provider_paths.end()) {
          select_provider(*supporting);
        }
      }
      break;
  }

  ResolvedParticipantStopStageDTO candidate{stop_id,
                                            stop_generation,
                                            item.operation_id,
                                            item.plan.operation_revision,
                                            item.plan.participant_id,
                                            item.controller_goal_id,
                                            item.controller_goal_generation,
                                            binding.controllable_component_id,
                                            binding.command_target_id,
                                            binding.capability_id,
                                            path.control_domain_id,
                                            path.control_domain_revision,
                                            path.control_binding_id,
                                            path.control_binding_revision,
                                            path.active_control_configuration_id,
                                            path.active_control_configuration_generation,
                                            path.controller_topology_id,
                                            path.topology_revision,
                                            path.control_route_id,
                                            std::move(provider_binding_id),
                                            std::move(provider_binding_revision),
                                            selected.command_target.provider_runtime_epoch,
                                            item.resources,
                                            selected.containment_granularity,
                                            path.stop_evidence_policy_id,
                                            path.stop_evidence_policy_revision,
                                            item.plan,
                                            stage_index,
                                            ContentDigest("pending")};
  return MakeResolvedParticipantStopStage(
      std::move(candidate), budget, item.readiness.value().resolved_stop_base_plan, item.binding,
      item.retained_default_stop_limits, item.node_category, item.resources);
}

[[nodiscard]] inline StopRequestedSelectorSnapshotDTO requested_selector_snapshot(
    const EmergencyStopProjectedIntentDTO& intent, const ContentDigest& digest,
    const std::optional<std::string>& malformed_reason) {
  if (const auto* ordinary = std::get_if<StopIntentDTO>(&intent)) {
    const auto kind = std::visit(
        [](const auto& rows) {
          using Rows = std::decay_t<decltype(rows)>;
          if constexpr (std::is_same_v<Rows, std::vector<StopOperationTargetDTO>>) {
            return StopRequestedSelectorKindDTO::kOrdinaryOperations;
          } else if constexpr (std::is_same_v<Rows, std::vector<StopComponentTargetDTO>>) {
            return StopRequestedSelectorKindDTO::kOrdinaryComponents;
          } else if constexpr (std::is_same_v<Rows, std::vector<StopResourceTargetDTO>>) {
            return StopRequestedSelectorKindDTO::kOrdinaryResources;
          } else {
            return StopRequestedSelectorKindDTO::kInternalProgramRun;
          }
        },
        ordinary->selector);
    return StopRequestedSelectorSnapshotDTO{kind, digest, ordinary->selector, std::nullopt};
  }
  if (std::holds_alternative<FullStopIntentDTO>(intent)) {
    return StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull, digest,
                                            std::nullopt, std::nullopt};
  }
  return StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kMalformedIngress, digest,
                                          std::nullopt, malformed_reason};
}

[[nodiscard]] inline StopParticipantScopeFactsDTO participant_scope_facts(
    const StopParticipantWork& item) {
  const auto& binding = item.binding.value();
  const auto& path = *binding.control_path;
  const auto terminal_settlement = [&] {
    using State = StopParticipantScopeFactsDTO::TerminalSettlementState;
    if (item.settlement_not_required() && item.contained) {
      return State::kNotRequired;
    }
    if (item.cancelled_pre_dispatch &&
        (item.terminal_error.has_value() || item.participant_settlement_error.has_value())) {
      return State::kContainedUnsettled;
    }
    if (!item.contained) {
      return State::kNotRequired;
    }
    if (!item.participant_settlement_complete) {
      return State::kPending;
    }
    return item.participant_settlement_error.has_value() ? State::kContainedUnsettled
                                                         : State::kSettled;
  }();
  return StopParticipantScopeFactsDTO{
      item.accepted_operation_revision,
      binding.controllable_component_id,
      binding.command_target_id,
      binding.capability_id,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      item.controller_goal_id,
      item.controller_goal_generation,
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      path.provider_runtime_epoch,
      item.resources,
      item.readiness.value().resolved_stop_base_plan.containment_granularity,
      terminal_settlement};
}

[[nodiscard]] inline StopScopeEntryDTO participant_scope_entry(
    const StopParticipantWork& item, const StopId& stop_id, const StopGeneration stop_generation,
    const std::int64_t accepted_at_ns) {
  return StopScopeEntryDTO{
      StopScopeEntryKeyDTO{stop_id, stop_generation,
                           StopParticipantScopeKeyDTO{item.operation_id, item.plan.participant_id}},
      participant_scope_facts(item),
      StopScopeEntryDispositionDTO::kStoppable,
      item.cancelled_pre_dispatch ? StopScopeDispatchStateDTO::kNeverDispatchedCancelled
                                  : StopScopeDispatchStateDTO::kDispatched,
      item.readiness.value().resolved_stop_base_plan.stop_evidence_policy_id,
      item.readiness.value().resolved_stop_base_plan.stop_evidence_policy_revision,
      item.plan.participant_stop_invocation_plan_digest,
      accepted_at_ns};
}

[[nodiscard]] inline bool stop_scope_entry_key_less(const StopScopeEntryKeyDTO& left,
                                             const StopScopeEntryKeyDTO& right) noexcept {
  if (left.origin_stop_id != right.origin_stop_id) {
    return left.origin_stop_id < right.origin_stop_id;
  }
  if (left.origin_stop_generation != right.origin_stop_generation) {
    return left.origin_stop_generation < right.origin_stop_generation;
  }
  if (left.subject.index() != right.subject.index()) {
    return left.subject.index() < right.subject.index();
  }
  if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&left.subject)) {
    const auto& other = std::get<StopParticipantScopeKeyDTO>(right.subject);
    return std::tie(participant->operation_id, participant->participant_id) <
           std::tie(other.operation_id, other.participant_id);
  }
  return std::get<ResourceId>(left.subject) < std::get<ResourceId>(right.subject);
}

[[nodiscard]] inline bool stop_surveillance_source_plan_less(
    const StopSurveillanceSourcePlanDTO& left,
    const StopSurveillanceSourcePlanDTO& right) noexcept {
  if (stop_scope_entry_key_less(left.scope_key, right.scope_key)) {
    return true;
  }
  if (stop_scope_entry_key_less(right.scope_key, left.scope_key)) {
    return false;
  }
  return left.evidence_requirement.evidence_item_id < right.evidence_requirement.evidence_item_id;
}

[[nodiscard]] inline Result<std::vector<StopSurveillanceSourcePlanDTO>> stop_surveillance_source_plans(
    const StopCertificateDTO& certificate) {
  std::vector<StopSurveillanceSourcePlanDTO> plans;
  for (const auto& attempt : certificate.attempted_stage_results) {
    if (attempt.authority != StopStageResultAuthorityDTO::kAuthoritative) {
      continue;
    }
    const auto stage_index = attempt.result.request.stage_index;
    const auto& stages = attempt.result.request.participant_stop_plan.stage_commands;
    if (stage_index >= stages.size()) {
      return reject<std::vector<StopSurveillanceSourcePlanDTO>>(
          "terminal Stop result names an invalid prepared stage", "stop_surveillance_stage_invalid",
          ErrorCode::kInvariantViolation);
    }
    for (const auto& resolved : stages[stage_index].resolved_stage.evidence_requirements) {
      plans.push_back(StopSurveillanceSourcePlanDTO{attempt.scope_key, resolved.requirement,
                                                    resolved.resolved_source});
    }
  }
  std::sort(plans.begin(), plans.end(), stop_surveillance_source_plan_less);
  plans.erase(std::unique(plans.begin(), plans.end()), plans.end());
  if (plans.empty() || plans.size() > kMaximumStopSurveillanceSources ||
      !std::all_of(plans.begin(), plans.end(), [](const auto& plan) {
        return !stop_surveillance_source_plan_field_error(plan).has_value();
      })) {
    return reject<std::vector<StopSurveillanceSourcePlanDTO>>(
        "terminal Stop has no canonical surveillance evidence sources",
        "stop_surveillance_sources_unavailable", ErrorCode::kMissingReference);
  }
  return Result<std::vector<StopSurveillanceSourcePlanDTO>>::Ok(std::move(plans));
}

[[nodiscard]] inline bool stop_requires_surveillance(const StopCertificateDTO& certificate,
                                              const StopFaultLatchDTO& latch) noexcept {
  // Q32: a component-incident containment no longer waits for observed
  // measured rest after its Stop finished.
  static_cast<void>(latch);
  return certificate.result == StopCertificateResultDTO::kPartial ||
         certificate.result == StopCertificateResultDTO::kFailedUncontained;
}

[[nodiscard]] inline StopFaultTriggerDTO primary_fault_trigger(const StopCertificateDTO& certificate,
                                                        const bool persistence_degraded) noexcept {
  if (certificate.result == StopCertificateResultDTO::kFailedUncontained) {
    return StopFaultTriggerDTO::kFailedUncontained;
  }
  if (certificate.result == StopCertificateResultDTO::kPartial) {
    return StopFaultTriggerDTO::kPartial;
  }
  if (certificate.result == StopCertificateResultDTO::kSucceededEscalated) {
    return StopFaultTriggerDTO::kEscalated;
  }
  if (certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull) {
    return StopFaultTriggerDTO::kScopePromotion;
  }
  return persistence_degraded ? StopFaultTriggerDTO::kPersistenceDegraded
                              : StopFaultTriggerDTO::kPhysicalRestUnproven;
}

[[nodiscard]] inline std::string fault_trigger_reason(const StopFaultTriggerDTO trigger) {
  switch (trigger) {
    case StopFaultTriggerDTO::kScopePromotion:
      return "stop_scope_promoted";
    case StopFaultTriggerDTO::kEscalated:
      return "stop_escalated";
    case StopFaultTriggerDTO::kPartial:
      return "stop_partial";
    case StopFaultTriggerDTO::kFailedUncontained:
      return "stop_failed_uncontained";
    case StopFaultTriggerDTO::kPhysicalRestUnproven:
      return "stop_physical_rest_unproven";
    case StopFaultTriggerDTO::kPersistenceDegraded:
      return "stop_persistence_degraded";
    case StopFaultTriggerDTO::kPostReserveFenceFailed:
      return "stop_post_reserve_fence_failed";
  }
  return "stop_fault";
}

/// Records one latched trigger in `latch.observed_triggers` (distinct, in
/// first-seen order, so the enum bounds the list). `latch.trigger` must already
/// be the primary cause. A new trigger that is not the primary writes one
/// typed line, so a lower-priority cause is never dropped silently.
inline void observe_fault_trigger(StopFaultLatchDTO& latch, const StopFaultTriggerDTO trigger) {
  auto& observed = latch.observed_triggers;
  if (std::find(observed.begin(), observed.end(), trigger) != observed.end()) return;
  observed.push_back(trigger);
  if (trigger == latch.trigger) return;
  const auto name = stop_fault_trigger_name(trigger);
  const auto primary = stop_fault_trigger_name(latch.trigger);
  (void)std::fprintf(stderr,
                     "manyforge_fault code=stop_fault_trigger_observed trigger=%.*s primary=%.*s\n",
                     static_cast<int>(name.size()), name.data(),
                     static_cast<int>(primary.size()), primary.data());
}

[[nodiscard]] inline int fault_trigger_priority(const StopFaultTriggerDTO trigger) noexcept {
  switch (trigger) {
    // An unfenced accepted Stop is the root cause; a later certificate never
    // replaces it.
    case StopFaultTriggerDTO::kPostReserveFenceFailed:
      return 7;
    case StopFaultTriggerDTO::kFailedUncontained:
      return 6;
    case StopFaultTriggerDTO::kPartial:
      return 5;
    case StopFaultTriggerDTO::kPhysicalRestUnproven:
      return 4;
    case StopFaultTriggerDTO::kEscalated:
      return 3;
    case StopFaultTriggerDTO::kScopePromotion:
      return 2;
    case StopFaultTriggerDTO::kPersistenceDegraded:
      return 1;
  }
  return 0;
}

/// Operations and resources a fault latch holds for physical recovery. A
/// pre-dispatch-cancelled participant whose no-effect outcome settled
/// (exactly the entries the terminal reducer does not count as unsettled) has
/// no stop stage and no motion-rest obligation; its resources are only latched
/// when another captured subject also covers them.
[[nodiscard]] inline std::pair<std::vector<OperationId>, std::vector<ResourceId>> fault_latch_capture(
    const StopCertificateDTO& certificate) {
  using Settlement = StopParticipantScopeFactsDTO::TerminalSettlementState;
  std::vector<OperationId> operations;
  std::vector<ResourceId> resources;
  operations.reserve(certificate.scope_entries.size());
  resources.reserve(certificate.scope_entries.size());
  for (const auto& entry : certificate.scope_entries) {
    if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&entry.key.subject)) {
      operations.push_back(participant->operation_id);
      const auto& facts = std::get<StopParticipantScopeFactsDTO>(entry.facts);
      const bool settled_without_effect =
          entry.dispatch_state == StopScopeDispatchStateDTO::kNeverDispatchedCancelled &&
          (facts.terminal_settlement == Settlement::kSettled ||
           facts.terminal_settlement == Settlement::kNotRequired);
      if (!settled_without_effect) {
        resources.insert(resources.end(), facts.resources.begin(), facts.resources.end());
      }
    } else {
      resources.push_back(std::get<ResourceId>(entry.key.subject));
    }
  }
  std::sort(operations.begin(), operations.end());
  operations.erase(std::unique(operations.begin(), operations.end()), operations.end());
  std::sort(resources.begin(), resources.end());
  resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
  return {std::move(operations), std::move(resources)};
}

[[nodiscard]] inline bool terminal_fault_required(const StopCertificateDTO& certificate,
                                           const bool persistence_degraded) noexcept {
  // Items 25 and Q29: no Stop outcome (escalated, partial or uncontained)
  // latches the cell by itself; `main` had no cell-wide Stop-fault latch.
  return persistence_degraded ||
         certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull;
}

[[nodiscard]] inline StopFaultLatchDTO persistence_fault_latch(
    const CellId& cell_id, const RequestOwnerEpoch& owner_epoch,
    const RetainedBundleId& retained_bundle_id, const GraphRevision& graph_revision,
    const StopCertificateDTO& certificate, const StopFaultLatchGeneration generation,
    StopDurableProjectionStatusDTO terminal_projection) {
  auto [operations, resources] = fault_latch_capture(certificate);
  const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                        certificate.stop_certificate_revision, certificate.stop_id,
                                        certificate.stop_generation};
  return StopFaultLatchDTO{
      cell_id,
      certificate.stop_id,
      certificate.stop_generation,
      reference,
      StopFaultLatchId("stop-fault." + owner_epoch.value() + "." +
                       std::to_string(generation.value())),
      generation,
      StopFaultTriggerDTO::kPersistenceDegraded,
      true,
      std::move(operations),
      std::move(resources),
      retained_bundle_id,
      graph_revision,
      std::move(terminal_projection),
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable, std::nullopt,
                                     std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForDurableRecord,
      "stop_persistence_degraded",
      {},
      {StopFaultTriggerDTO::kPersistenceDegraded}};
}

}  // namespace resource_addressed_kernel_detail

namespace resource_addressed_kernel_detail {
inline bool global_rule_registration_matches(const IoRuleRegistrationDTO& token,
    const IoRuleStateContextDTO& context, const InterruptPolicyId& policy) {
  return token.owner_epoch == context.owner_epoch &&
         token.catalog_generation == context.catalog_generation && token.registration_id != 0U &&
         token.policy_id == policy && token.scope == IoRuleScopeDTO{IoActivationRuleScopeDTO{
             context.retained_bundle_id, context.graph_revision}};
}
}  // namespace resource_addressed_kernel_detail

struct ResourceAddressedExecutionKernel::GlobalIoRuleRuntime final {
  IoRuleStateDTO state;
  std::optional<IoRuleRegistrationDTO> registration;
  std::optional<IoRuleEvidenceDTO> evidence;
  std::optional<ErrorDTO> qualification_error;
  bool response_consumed{};
  bool waiting_for_inactive{};
  std::optional<StopAcceptedDTO> response{};
  std::optional<IoRuleRegistrationDTO> latch_registration{};
  std::optional<IoRuleEvidenceDTO> latched_evidence{};
  std::optional<IoRuleEvidenceDTO> last_source_loss{};
  bool source_loss_latch{};
};

struct ResourceAddressedExecutionKernel::StagedGlobalIoRuleCatalog final {
  IoRuleAppliedContextDTO context;
  std::shared_ptr<const IoActivationRuleCatalogDTO> catalog;
  std::shared_ptr<IIoSignalService> service;
  std::optional<ConfigurationFenceStatusQueryDTO> fence;
  std::optional<CellAdmissionClaimDTO> parent;
  std::vector<IoRuleRegistrationDTO> registrations;
  std::shared_ptr<const IoRuleWaitingForProgramCatalogDTO> waiting_for_program{};
};

class ResourceAddressedExecutionKernel::StopProjectionWorker final {
 public:
  StopProjectionWorker(const std::size_t capacity, ResourceAddressedExecutionKernel& owner)
      : capacity_(capacity),
        owner_(owner),
        requested_(std::make_unique<std::atomic_uint64_t[]>(capacity)),
        delivered_(std::make_unique<std::atomic_uint64_t[]>(capacity)),
        worker_([this](std::stop_token token) noexcept { Run(token); }) {
    while (!ready_.load(std::memory_order_acquire)) {
      ready_.wait(false, std::memory_order_acquire);
    }
  }

  ~StopProjectionWorker() {
    Close();
    worker_.request_stop();
    Wake();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void Publish(const StopControlWorkToken token) noexcept {
    api_calls_in_flight_.fetch_add(1U, std::memory_order_acq_rel);
    if (!accepting_.load(std::memory_order_acquire) || token.slot >= capacity_ ||
        !token.locally_valid()) {
      api_calls_in_flight_.fetch_sub(1U, std::memory_order_acq_rel);
      NotifyDrainState();
      return;
    }
    auto retained = requested_[token.slot].load(std::memory_order_relaxed);
    bool published = false;
    while (token.generation > retained) {
      if (requested_[token.slot].compare_exchange_weak(
              retained, token.generation, std::memory_order_release, std::memory_order_relaxed)) {
        published = true;
        break;
      }
    }
    if (published) {
      Wake();
    }
    api_calls_in_flight_.fetch_sub(1U, std::memory_order_acq_rel);
    NotifyDrainState();
  }

  void PublishGlobalRuleStates() noexcept {
    if (!accepting_.load(std::memory_order_acquire)) return;
    global_requested_.store(true, std::memory_order_release);
    Wake();
  }

  void Close() noexcept {
    accepting_.store(false, std::memory_order_release);
    Wake();
    NotifyDrainState();
  }

  [[nodiscard]] bool DrainFor(const std::chrono::nanoseconds timeout) noexcept {
    try {
      std::unique_lock lock(drain_mutex_);
      return drain_changed_.wait_for(lock, timeout, [this] {
        if (callback_in_flight_.load(std::memory_order_acquire) ||
            global_requested_.load(std::memory_order_acquire)) {
          return false;
        }
        if (api_calls_in_flight_.load(std::memory_order_acquire) != 0U) {
          return false;
        }
        for (std::size_t index = 0U; index < capacity_; ++index) {
          if (requested_[index].load(std::memory_order_acquire) !=
              delivered_[index].load(std::memory_order_acquire)) {
            return false;
          }
        }
        return true;
      });
    } catch (...) {
      return false;
    }
  }

 private:
  void Wake() noexcept {
    wake_sequence_.fetch_add(1U, std::memory_order_release);
    wake_sequence_.notify_one();
  }

  void NotifyDrainState() noexcept {
    std::lock_guard lock(drain_mutex_);
    drain_changed_.notify_all();
  }

  void Run(const std::stop_token stop_token) noexcept {
    ready_.store(true, std::memory_order_release);
    ready_.notify_all();
    auto observed_wake = wake_sequence_.load(std::memory_order_acquire);
    while (!stop_token.stop_requested()) {
      bool progressed = false;
      for (std::size_t index = 0U; index < capacity_; ++index) {
        const auto requested = requested_[index].load(std::memory_order_acquire);
        if (requested <= delivered_[index].load(std::memory_order_acquire)) {
          continue;
        }
        callback_in_flight_.store(true, std::memory_order_release);
        owner_.ProjectStop(
            StopControlWorkToken{static_cast<StopControlSlotIndex>(index), requested});
        callback_in_flight_.store(false, std::memory_order_release);
        delivered_[index].store(requested, std::memory_order_release);
        progressed = true;
        if (!accepting_.load(std::memory_order_acquire)) {
          NotifyDrainState();
        }
      }
      if (global_requested_.exchange(false, std::memory_order_acq_rel)) {
        callback_in_flight_.store(true, std::memory_order_release);
        owner_.ProjectGlobalIoRuleStates();
        callback_in_flight_.store(false, std::memory_order_release);
        progressed = true;
        NotifyDrainState();
      }
      if (progressed) {
        continue;
      }
      const auto current_wake = wake_sequence_.load(std::memory_order_acquire);
      if (current_wake != observed_wake) {
        observed_wake = current_wake;
        continue;
      }
      wake_sequence_.wait(observed_wake, std::memory_order_acquire);
      observed_wake = wake_sequence_.load(std::memory_order_acquire);
    }
    NotifyDrainState();
  }

  std::size_t capacity_{};
  ResourceAddressedExecutionKernel& owner_;
  std::unique_ptr<std::atomic_uint64_t[]> requested_;
  std::unique_ptr<std::atomic_uint64_t[]> delivered_;
  std::atomic_bool accepting_{true};
  std::atomic_bool ready_{};
  std::atomic_bool callback_in_flight_{};
  std::atomic_bool global_requested_{};
  std::atomic_size_t api_calls_in_flight_{};
  std::atomic_uint64_t wake_sequence_{};
  std::mutex drain_mutex_;
  std::condition_variable drain_changed_;
  std::jthread worker_;
};

struct ResourceAddressedExecutionKernel::EngineSubscription final {
  IResolvedParticipantExecutionEngine* engine{};
  SubscriptionHandle handle{};
};

struct ResourceAddressedExecutionKernel::EngineEventIngress final
    : std::enable_shared_from_this<EngineEventIngress> {
  class Entry final {
   public:
    Entry() = default;
    Entry(std::shared_ptr<EngineEventIngress> ingress,
          ResourceAddressedExecutionKernel* owner)
        : ingress_(std::move(ingress)), owner_(owner) {}
    Entry(Entry&& other) noexcept
        : ingress_(std::move(other.ingress_)), owner_(std::exchange(other.owner_, nullptr)) {}
    Entry& operator=(Entry&&) = delete;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    ~Entry() {
      if (ingress_ == nullptr || owner_ == nullptr) return;
      std::lock_guard lock(ingress_->mutex);
      if (--ingress_->entered == 0U) ingress_->drained.notify_all();
    }

    [[nodiscard]] ResourceAddressedExecutionKernel* owner() const noexcept { return owner_; }

   private:
    std::shared_ptr<EngineEventIngress> ingress_;
    ResourceAddressedExecutionKernel* owner_{};
  };

  [[nodiscard]] Entry Enter() {
    std::lock_guard lock(mutex);
    if (owner == nullptr) return {};
    ++entered;
    return Entry(shared_from_this(), owner);
  }

  void Open(ResourceAddressedExecutionKernel& value) {
    std::lock_guard lock(mutex);
    if (owner != nullptr || entered != 0U) {
      throw std::logic_error("Kernel event ingress was opened more than once");
    }
    owner = &value;
  }

  void Close() {
    std::lock_guard lock(mutex);
    owner = nullptr;
  }

  void Drain() {
    std::unique_lock lock(mutex);
    drained.wait(lock, [&] { return entered == 0U; });
  }

 private:
  std::mutex mutex;
  std::condition_variable drained;
  ResourceAddressedExecutionKernel* owner{};
  std::size_t entered{};
};

class ResourceAddressedExecutionKernel::CellCloseAdmissionLease final
    : public ICellCloseAdmissionLease {
 public:
  explicit CellCloseAdmissionLease(ResourceAddressedExecutionKernel& owner)
      : owner_(owner), admission_lock_(*owner.admission_mutex_) {}

  [[nodiscard]] Result<void> ValidatePrerequisites() const override {
    // DX583/Q121: inspect actual Kernel facts under the existing admission
    // lease and a brief state lock. Neither state nor submission spans SQL;
    // Stop remains recognizable, and callers recheck immediately precommit.
    // D6: an active Stop-fault latch never blocks Close. Close moves nothing;
    // the latch is a fact about its robots and is kept (never cleared here).
    std::lock_guard state_lock(owner_.state_mutex_);
    owner_.refresh_stop_fault_latch_locked();
    for (const auto& [id, operation] : owner_.active_operations_) {
      if (operation->containment_effect_uncertain())
        return Result<void>::Err(ErrorDTO{"cell_close_containment_uncertain", "Close cannot infer rest from an uncertain terminal", {{"operation_id", id.value()}}});
      switch (operation->state.state) {
        case OperationStateKind::kQueued:
        case OperationStateKind::kResolving:
        case OperationStateKind::kPlanning:
        case OperationStateKind::kValidating:
        case OperationStateKind::kPrepared:
        case OperationStateKind::kDispatching:
        case OperationStateKind::kExecuting:
        case OperationStateKind::kStopping:
          return Result<void>::Err(ErrorDTO{"cell_close_operation_nonterminal", "Close requires ordinary Stop before nonterminal work can retire", {{"operation_id", id.value()}}});
        case OperationStateKind::kCompleted:
        case OperationStateKind::kStopped:
        case OperationStateKind::kFailed:
        case OperationStateKind::kInterrupted:
        case OperationStateKind::kInterventionRequired:
        case OperationStateKind::kCancelledPreDispatch:
          break;
      }
    }
    return Result<void>::Ok();
  }

  [[nodiscard]] Result<void> PrepareCommandEligibilityClosure(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) override {
    if (prepared_ || !intent.locally_valid())
      return Result<void>::Err(error("cell_close_eligibility_preparation_invalid",
                                    "Close eligibility requires one exact valid retirement",
                                    "invalid_or_repeated_preparation"));
    const auto current = ValidatePrerequisites();
    if (!current.ok()) return current;
    try {
      auto prepared = intent;
      std::vector<ComponentCommandReadinessDTO*> entries;
      entries.reserve(intent.expected_targets.size());
      {
        std::lock_guard state_lock(owner_.state_mutex_);
        for (const auto& target : intent.expected_targets) {
          const auto entry = owner_.component_command_readiness_.find(
              target.controllable_component_id);
          // Absence is already command-closed. The projection map does not
          // erase entries, and this same admission lease excludes publishers.
          if (entry != owner_.component_command_readiness_.end())
            entries.push_back(&entry->second);
        }
      }
      prepared_.emplace(std::move(prepared));
      entries_ = std::move(entries);
      return Result<void>::Ok();
    } catch (...) {
      return Result<void>::Err(error("cell_close_eligibility_preparation_unavailable",
                                    "Close eligibility values could not be prepared before retirement",
                                    "preparation_allocation_failed"));
    }
  }

  [[nodiscard]] bool CloseCommandEligibilityAfterRetirement(
      const CCLCLifecycleTargetRetirementIntentDTO& intent,
      const CCLCLifecycleTargetRetirementResultDTO& retired) noexcept override {
    // Complete equality is comparison-only: locally_valid() on a constructed
    // result would copy vectors and must never run after durable commitment.
    if (!prepared_ || consumed_ || *prepared_ != intent ||
        retired.replay_status != RequestReplayStatusDTO::kAcceptedNew ||
        retired.request_context != prepared_->request_context ||
        retired.scope != prepared_->scope ||
        retired.retired_targets != prepared_->expected_targets)
      return false;
    std::lock_guard state_lock(owner_.state_mutex_);
    for (auto* entry : entries_) entry->command_eligible = false;
    owner_.close_retired_product_ = true;
    consumed_ = true;
    return true;
  }

  [[nodiscard]] bool CloseCommandEligibilityForUnknownRetirement(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) noexcept override {
    if (!prepared_ || consumed_ || *prepared_ != intent) return false;
    std::lock_guard state_lock(owner_.state_mutex_);
    for (auto* entry : entries_) entry->command_eligible = false;
    owner_.close_retired_product_ = true;
    consumed_ = true;
    return true;
  }

 private:
  ResourceAddressedExecutionKernel& owner_;
  std::unique_lock<std::recursive_mutex> admission_lock_;
  std::optional<CCLCLifecycleTargetRetirementIntentDTO> prepared_;
  std::vector<ComponentCommandReadinessDTO*> entries_;
  bool consumed_{};
};


struct ResourceAddressedExecutionKernel::PreviewSession final : IAddressedMotionPreviewSession {
  PreviewSession(ResourceAddressedExecutionKernel& owner, RuntimeParticipantBinding primary_binding,
                 RuntimeParticipantPolicySelection primary_participant_policy,
                 IResolvedParticipantExecutionEngine& primary_engine, IMotionPreview& motion,
                 std::unique_lock<std::mutex> submission_lock)
      : owner_(owner),
        primary_binding_(std::move(primary_binding)),
        primary_participant_policy_(std::move(primary_participant_policy)),
        primary_engine_(primary_engine),
        motion_(motion),
        submission_lock_(std::move(submission_lock)) {}

  [[nodiscard]] Result<RuntimeParticipantBindingDTO> resolve_binding(
      const OperationIntentDTO& intent) const override {
    auto resolved = resolve_registration(intent);
    if (!resolved.ok()) return Result<RuntimeParticipantBindingDTO>::Err(resolved.error());
    return Result<RuntimeParticipantBindingDTO>::Ok(resolved.value().binding.value());
  }

 private:
  [[nodiscard]] Result<RuntimeParticipantExecutionRegistration> resolve_registration(
      const OperationIntentDTO& intent) const {
    auto validation = validate_operation_intent(intent);
    if (!validation.ok()) {
      return Result<RuntimeParticipantExecutionRegistration>::Err(validation.error());
    }
    if (intent.request_context.request_owner_epoch != owner_.config_.request_owner_epoch) {
      return Result<RuntimeParticipantExecutionRegistration>::Err(
          error("stale_request_owner_epoch", "preview request names a stale Kernel owner epoch",
                "stale_request_owner_epoch"));
    }
    if (intent.participants.size() != 1U ||
        intent.coordination_policy != OperationCoordinationPolicy::kIndependent) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "I6 preview admits one independent participant", "i6_preview_single_participant_only");
    }
    auto resolved = owner_.resolve_registration(intent, 0U);
    if (!resolved.ok()) {
      return Result<RuntimeParticipantExecutionRegistration>::Err(resolved.error());
    }
    if (resolved.value().engine != &primary_engine_) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "linked preview motions cross their exact execution engine",
          "motion_preview_engine_changed", ErrorCode::kInvariantViolation);
    }
    if (!resolved.value().participant_policy.has_value() ||
        resolved.value().participant_policy->value() != primary_participant_policy_.value()) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "linked preview motions cross their exact participant motion policy",
          "motion_preview_participant_policy_changed", ErrorCode::kInvariantViolation);
    }
    const auto available = owner_.validate_preview_participant(intent, resolved.value());
    if (!available.ok())
      return Result<RuntimeParticipantExecutionRegistration>::Err(available.error());
    return resolved;
  }

 public:
  [[nodiscard]] Result<ExecuteRequestDTO> prepare_motion(
      const OperationIntentDTO& intent) override {
    auto resolved = resolve_registration(intent);
    if (!resolved.ok()) {
      return Result<ExecuteRequestDTO>::Err(resolved.error());
    }
    if (!same_linked_preview_participant(resolved.value().binding.value(),
                                         primary_binding_.value())) {
      return reject<ExecuteRequestDTO>(
          "linked preview motions cross their exact runtime participant",
          "motion_preview_binding_changed");
    }
    if (!std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)) {
      return reject<ExecuteRequestDTO>("preview preparation requires a motion participant",
                                       "motion_preview_payload_not_motion",
                                       ErrorCode::kInvalidRequest);
    }
    if (next_preview_id_ == 0U || next_preview_id_ == std::numeric_limits<std::uint64_t>::max()) {
      return reject<ExecuteRequestDTO>("preview identity space is exhausted",
                                       "motion_preview_id_exhausted", ErrorCode::kInternalError);
    }

    const std::string prefix = "preview:" + owner_.config_.request_owner_epoch.value() + ":" +
                               std::to_string(next_preview_id_++);
    const OperationId operation_id(prefix);
    const OperationParticipantId participant_id(prefix + ":participant:1");
    const ControllerGoalId controller_goal_id(prefix + ":controller-goal:1");
    return build_motion_request(
        intent, intent.participants.front(),
        std::get<MotionParticipantIntentDTO>(intent.participants.front().payload),
        resolved.value().binding.value(), operation_id, OperationRevision(1U), participant_id,
        controller_goal_id, ControllerGoalGeneration(1U), resolved.value(), 1.0);
  }

  [[nodiscard]] Result<RobotStateDTO> current_state() const override {
    return motion_.current_preview_state(owner_.clock_.now());
  }

  [[nodiscard]] Result<std::vector<Pose3DTO>> resolve_reference_poses(
      const OperationIntentDTO& motion_context, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& start_state,
      std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings) override {
    auto prepared = prepare_motion(motion_context);
    if (!prepared.ok()) {
      return Result<std::vector<Pose3DTO>>::Err(prepared.error());
    }
    prepared.value().runtime_object_bindings = std::move(runtime_object_bindings);
    return motion_.resolve_preview_reference_poses(
        std::move(prepared.value()), std::move(references), projection_inventory, start_state);
  }

  [[nodiscard]] Result<MotionPreviewResultDTO> preview_motion(
      OperationIntentDTO intent, CollisionWorldLeaseInventory projection_inventory,
      RobotStateDTO start_state, const std::uint32_t planning_attempt_index,
      const std::stop_token stop_token) override {
    if (!intent.expected_scene_revision.has_value() ||
        *intent.expected_scene_revision !=
            projection_inventory.projection_binding.projection_revision.scene_revision) {
      return reject<MotionPreviewResultDTO>(
          "preview intent does not name the exact retained scene revision",
          "motion_preview_scene_revision_mismatch", ErrorCode::kSceneChangedDuringValidation);
    }
    auto prepared = prepare_motion(intent);
    if (!prepared.ok()) {
      return Result<MotionPreviewResultDTO>::Err(prepared.error());
    }
    return motion_.preview_motion(std::move(prepared.value()), std::move(projection_inventory),
                                  std::move(start_state), planning_attempt_index, stop_token);
  }

 private:
  ResourceAddressedExecutionKernel& owner_;
  RuntimeParticipantBinding primary_binding_;
  RuntimeParticipantPolicySelection primary_participant_policy_;
  IResolvedParticipantExecutionEngine& primary_engine_;
  IMotionPreview& motion_;
  // This is intentionally retained for the complete multi-leg candidate
  // transaction. The independent control path never takes this mutex.
  std::unique_lock<std::mutex> submission_lock_;
  std::uint64_t next_preview_id_{1U};
};

}  // namespace manyforge::core
