#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
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
#include <manyforge_core/stop_timing.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/error_catalog.generated.hpp>
#include <numeric>
#include <pthread.h>
#include <sched.h>
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

// A manual-operation claim names its operation by the operation number in
// `owner_transaction_id`; any other transaction id names none (nullopt).
[[nodiscard]] inline std::optional<OperationNumber> manual_claim_operation(
    const std::optional<CellAdmissionOwnerTransactionId>& transaction) noexcept {
  const auto* const number =
      transaction ? std::get_if<OperationNumber>(&*transaction) : nullptr;
  return number ? std::optional<OperationNumber>(*number) : std::nullopt;
}

[[nodiscard]] inline ErrorDTO error(std::string code, std::string message, std::string reason) {
  return ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

/// The journal write fault's error for records not written without a write
/// error of their own (a full queue, a reused Stop ring slot).
[[nodiscard]] inline ErrorDTO journal_records_lost_error() {
  return ErrorDTO{"operation_journal_records_lost",
                  "operation records were dropped before they could be written", {}};
}

template <typename T>
[[nodiscard]] inline Result<T> reject(std::string message, std::string reason,
                               ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<T>::Err(error(std::string(to_string(code)), std::move(message), std::move(reason)));
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

/// The participant mechanism's current runtime-scale availability; a
/// registration without an engine keeps the qualification-only default.
[[nodiscard]] inline RuntimeScaleMechanismAvailability runtime_scale_availability_of(
    const IResolvedParticipantExecutionEngine* engine) noexcept {
  return engine == nullptr ? RuntimeScaleMechanismAvailability::kAvailable
                           : engine->runtime_scale_availability();
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
    const std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO>& readbacks,
    const RuntimeRouteConfigurationReadbackDTO* replacement = nullptr) {
  std::vector<DomainActiveControlConfigurationDTO> result;
  result.reserve(readbacks.size() + (replacement != nullptr ? 1U : 0U));
  for (const auto& [domain_id, readback] : readbacks) {
    if (replacement != nullptr &&
        domain_id == replacement->active_configuration.control_domain_id) {
      continue;
    }
    result.push_back(DomainActiveControlConfigurationDTO{
        domain_id, readback.active_configuration.active_control_configuration_id,
        readback.active_configuration.active_control_configuration_generation});
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
         left.motion_takeover_planning_budget_s == right.motion_takeover_planning_budget_s &&
         left.motion_endpoint_owner == right.motion_endpoint_owner &&
         left.set_tcp.has_value() == right.set_tcp.has_value() &&
         (!left.set_tcp || left.set_tcp->holds_robot == right.set_tcp->holds_robot);
}

[[nodiscard]] inline bool registration_controller_equal_after_product_reseal(
    const RuntimeParticipantExecutionRegistration& left,
    const RuntimeParticipantExecutionRegistration& right) {
  const auto& a = left.binding.value();
  const auto& b = right.binding.value();
  auto candidate_controller_binding = b;
  // A retained controller may receive a fresh product seal and fresh authored
  // Program metadata during Apply. Everything that identifies its physical
  // control/runtime context remains exact. Resolved target, model, frame,
  // resource and provider facts below still have to match. Contact/grasp
  // templates are authored
  // Program inputs consumed by later admitted operations, not controller state.
  // The moving-takeover budgets are cell context: a retained controller takes
  // the candidate's, so they are not compared.
  candidate_controller_binding.program_revision = a.program_revision;
  candidate_controller_binding.program_binding_id = a.program_binding_id;
  candidate_controller_binding.retained_bundle_id = a.retained_bundle_id;
  candidate_controller_binding.graph_revision = a.graph_revision;
  candidate_controller_binding.collision_rule_bodies = a.collision_rule_bodies;
  candidate_controller_binding.program_frame_bindings = a.program_frame_bindings;
  candidate_controller_binding.program_collision_body_bindings = a.program_collision_body_bindings;
  candidate_controller_binding.frame_associations = a.frame_associations;
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
  std::map<ResourceId, ControllableComponentId> resource_owners;
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
    if (!registration.stop_readiness) {
      // Unqualified registrations do not self-attest lifecycle membership.
      continue;
    }
    const auto& component_id = registration.stop_readiness->value().controllable_component_id;
    const auto generation = registration.stop_readiness->value().component_lifecycle_generation;
    auto [component, inserted] = components.try_emplace(
        component_id, StopScopeComponentSnapshotDTO{component_id, generation, {}});
    if (!inserted && component->second.component_lifecycle_generation != generation) {
      throw std::invalid_argument(
          "runtime participant Stop readiness disagrees on component lifecycle generation");
    }
    for (const auto& resource : binding.resources) {
      const auto [owner, owner_inserted] = resource_owners.emplace(resource.resource_id, component_id);
      if (!owner_inserted && owner->second != component_id) {
        throw std::invalid_argument(
            "runtime Stop readiness assigns one resource to multiple components");
      }
      component->second.resources.push_back(resource.resource_id);
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
    facts.components.push_back(std::move(component));
  }
  return facts;
}

[[nodiscard]] inline const ProgramFrameBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding, const ProgramFrameReferenceDTO& reference) noexcept {
  const auto found = std::find_if(
      binding.frame_bindings.begin(), binding.frame_bindings.end(),
      [&](const ProgramFrameBindingDTO& candidate) { return candidate.reference == reference; });
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
          // A linear pose goal is planned as a Cartesian path (OD-341).
          return CapabilityId(value.linear ? "planning.motion.cartesian_path"
                                           : "planning.motion.pose_goal");
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

/// Turn a motion's collision rules into their admitted form: a collision body
/// becomes the ids the Program Binding holds for it, the objects carried by a
/// collision body (an attached object selection with a role) the objects
/// carried by those ids, the TCP's contact bodies become the ids of the
/// resolved motion endpoint. The other selections are resolved against the
/// move's collision world at validation.
[[nodiscard]] inline Result<std::vector<CollisionRule>> admit_collision_rules(
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding) {
  using Admitted = Result<std::vector<CollisionRule>>;
  const auto unresolved = [](const CollisionRuleDTO& rule, std::string message,
                             std::string selection_kind, std::string reference_id) {
    auto error = reject<std::vector<CollisionRule>>(std::move(message),
                                                    "collision_rule_selection_unresolved",
                                                    ErrorCode::kMissingReference)
                     .error();
    error.details.emplace("collision_rule_id", rule.rule_id);
    error.details.emplace("selection_kind", std::move(selection_kind));
    error.details.emplace("reference_id", std::move(reference_id));
    return Admitted::Err(std::move(error));
  };
  const auto body_ids_of = [](const auto& bodies) {
    std::vector<CollisionBodyId> ids;
    for (const auto& body : bodies) {
      ids.insert(ids.end(), body.collision_body_ids.begin(), body.collision_body_ids.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
  };
  // The binding of one collision body, null when the Program Binding holds
  // none.
  const auto bound_body = [&](const ProgramCollisionBodyReferenceDTO& body)
      -> const ProgramCollisionRuleBodyBindingDTO* {
    const auto bound = std::lower_bound(
        binding.collision_rule_bodies.begin(), binding.collision_rule_bodies.end(), body,
        [](const ProgramCollisionRuleBodyBindingDTO& candidate,
           const ProgramCollisionBodyReferenceDTO& selected) { return candidate.body < selected; });
    return bound != binding.collision_rule_bodies.end() && bound->body == body ? &*bound : nullptr;
  };
  std::vector<CollisionRule> admitted;
  admitted.reserve(motion.collision_rules.size());
  for (const auto& rule : motion.collision_rules) {
    CollisionRule resolved{rule.rule_id, {}, {}, rule.value};
    for (const auto& [authored, selections] :
         {std::pair{&rule.a, &resolved.a}, std::pair{&rule.b, &resolved.b}}) {
      selections->reserve(authored->size());
      for (const auto& selection : *authored) {
        if (const auto* body = std::get_if<CollisionRuleCollisionBodySelection>(&selection)) {
          const auto* bound = bound_body(body->body);
          if (bound == nullptr) {
            return unresolved(rule, "collision rule selects a collision body the Program "
                                    "Binding does not hold",
                              "collision_body",
                              body->body.component_id.value() + "/" +
                                  body->body.collision_body_id.value());
          }
          selections->push_back(CollisionRuleResolvedBodiesSelection{
              body_ids_of(std::vector<ProgramCollisionRuleBodyBindingDTO>{*bound})});
        } else if (const auto* attached =
                       std::get_if<CollisionRuleAttachedObjectSelection>(&selection);
                   attached != nullptr && attached->collision_body_role_id) {
          // The objects carried by one collision body (OD-371).
          const auto* bound = bound_body(ProgramCollisionBodyReferenceDTO{
              attached->component_id, CollisionBodyRoleId(*attached->collision_body_role_id)});
          if (bound == nullptr) {
            return unresolved(rule, "collision rule selects the objects carried by a collision "
                                    "body the Program Binding does not hold",
                              "attached_object",
                              attached->component_id.value() + "/" +
                                  *attached->collision_body_role_id);
          }
          selections->push_back(CollisionRuleResolvedAttachedObjectSelection{
              body_ids_of(std::vector<ProgramCollisionRuleBodyBindingDTO>{*bound})});
        } else if (std::holds_alternative<CollisionRuleActiveTcpContactBodiesSelection>(
                       selection)) {
          if (!binding.motion_endpoint ||
              binding.motion_endpoint->contact_body_bindings.empty()) {
            return unresolved(rule, "collision rule selects the TCP's contact bodies and the "
                                    "move's TCP has none",
                              "active_tcp_contact_bodies", {});
          }
          selections->push_back(CollisionRuleResolvedBodiesSelection{
              body_ids_of(binding.motion_endpoint->contact_body_bindings)});
        } else if (const auto* group =
                       std::get_if<CollisionRuleModelGroupSelection>(&selection)) {
          // Model groups are resolved by the execution host for cell rules;
          // a move's rules cannot name one.
          return unresolved(rule, "collision rule on a move selects a model group; model "
                                  "groups are available on the Deployment's rules only",
                            "model_group",
                            group->component_id.value() + "/" + group->group);
        } else if (const auto* component =
                       std::get_if<CollisionRuleComponentSelection>(&selection)) {
          selections->push_back(*component);
        } else if (const auto* link = std::get_if<CollisionRuleLinkSelection>(&selection)) {
          selections->push_back(*link);
        } else if (const auto* attached =
                       std::get_if<CollisionRuleAttachedObjectSelection>(&selection)) {
          selections->push_back(*attached);
        } else if (std::holds_alternative<CollisionRuleSceneObjectsSelection>(selection)) {
          selections->push_back(CollisionRuleSceneObjectsSelection{});
        } else {
          selections->push_back(std::get<CollisionRuleSceneObjectSelection>(selection));
        }
      }
    }
    admitted.push_back(std::move(resolved));
  }
  return Admitted::Ok(std::move(admitted));
}

// A frame's component is always its owner, never the moving arm.
[[nodiscard]] inline ResolvedFrameDTO resolved_component_frame(const ControllableComponentId& owner,
                                                        const ProgramFrameBindingDTO& frame) {
  return ResolvedComponentModelFrameDTO{owner, frame.model_instance_id, frame.reference.frame_id,
                                        frame.compiled_frame_name, frame.model_revision};
}

[[nodiscard]] inline Result<ExecuteRequestDTO> build_motion_request(
    const OperationIntentDTO& operation, const OperationParticipantIntentDTO& participant,
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding,
    const OperationNumber& operation_id, OperationRevision operation_revision,
    const OperationParticipantId& participant_id, const ControllerGoalId& controller_goal_id,
    const RuntimeParticipantExecutionRegistration& registration,
    const double initial_runtime_scale, const std::uint64_t initial_runtime_scale_sequence = 0U) {
  const auto& participant_policy = *registration.participant_policy;
  if (binding.model_groups.size() != 1U) {
    return reject<ExecuteRequestDTO>("motion requires exactly one retained model group",
                                     "i6_motion_model_group_cardinality_invalid");
  }
  const auto* base = find_frame(binding, motion.base_frame);
  const auto* tcp =
      binding.motion_endpoint &&
              tcp_selector_admits(
                  motion.tcp_endpoint, *binding.motion_endpoint,
                  registration.motion_endpoint_owner.value_or(binding.controllable_component_id))
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
      // The base is the motion component's own frame; the TCP entry belongs to
      // the motion component or a tool mounted on it.
      resolved_component_frame(base->reference.component_id, *base),
      resolved_component_frame(
          registration.motion_endpoint_owner.value_or(binding.controllable_component_id), *tcp),
      {},
      {},
      control.runtime_scale_qualification,
      binding.retained_bundle_id,
      binding.graph_revision,
      ProgramTargetReferenceDTO{binding.controllable_component_id, binding.command_target_id},
      binding.frame_bindings,
      binding.collision_body_bindings,
      binding.program_frame_bindings,
      binding.program_collision_body_bindings};

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
  request.planner = motion.planner;
  request.target_mode = motion.target_mode;
  request.target_reference = motion.target_reference;
  request.offset_reference = motion.offset_reference;
  request.goal_frame_reference = motion.goal_frame_reference;
  request.motion_interrupt_scopes = std::move(effective_interrupt_scopes);
  request.action_span = participant.action_span;
  request.runtime_object_bindings = motion.runtime_object_bindings;
  auto collision_rules = admit_collision_rules(motion, binding);
  if (!collision_rules.ok()) {
    return Result<ExecuteRequestDTO>::Err(collision_rules.error());
  }
  request.collision_rules = std::move(collision_rules.value());
  // A lane that cannot scale is treated as an unqualified route: the motion
  // runs without a runtime-scale contract.
  if (control.runtime_scale_qualification.has_value() &&
      runtime_scale_availability_of(registration.engine) !=
          RuntimeScaleMechanismAvailability::kLaneUnsupported) {
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
                                       initial_runtime_scale,
                                       initial_runtime_scale_sequence};
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
        "the retained gripper mechanism requires one exact command resource",
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
    return reject<GripperCommandRequestDTO>("the command is not the qualified gripper schema",
                                            "i6_command_schema_unsupported",
                                            ErrorCode::kUnsupportedCapability);
  }

  double command_duration_s = 0.0;
  bool wait_for_duration = false;
  for (const auto& option : command.caller_options) {
    const auto* value_s = std::get_if<double>(&option.value);
    const auto* value_bool = std::get_if<bool>(&option.value);
    if (option.option_id.value() == "command_duration_s" && value_s != nullptr) {
      command_duration_s = *value_s;
    } else if (option.option_id.value() == "wait_for_duration" && value_bool != nullptr) {
      wait_for_duration = *value_bool;
    } else {
      return reject<GripperCommandRequestDTO>(
          "the command carries an option not declared by its schema", "command_option_unsupported",
          ErrorCode::kInvalidRequest);
    }
  }

  const auto& value = command.command_value;
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
  request.wait_for_duration = wait_for_duration;
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
    const MotionInterruptNodeCategory node_category, const OperationNumber& operation_id,
    const OperationRevision operation_revision, const OperationParticipantId& participant_id,
    const ProgramRevision& program_revision, const ProgramBindingId& program_binding_id,
    const std::optional<ProgramRunNumber>& program_run_id) {
  if (!profile.has_value()) {
    return Result<std::optional<PreparedParticipantStopPlanDTO>>::Ok(std::nullopt);
  }
  PreparedParticipantStopPlanContextDTO context{
      operation_id,     operation_revision, participant_id,
      program_revision, program_binding_id, program_run_id,
      *profile,         ParticipantStopProfilePurposeDTO::kFault,
      std::nullopt,     node_category};
  auto prepared = MakePreparedParticipantStopPlan(readiness.value().resolved_stop_base_plan,
                                                  std::move(context),
                                                  binding.resources, retained_default_stop_limits);
  if (!prepared.ok()) {
    return Result<std::optional<PreparedParticipantStopPlanDTO>>::Err(prepared.error());
  }
  return Result<std::optional<PreparedParticipantStopPlanDTO>>::Ok(std::move(prepared.value()));
}

/// OD-235: the engine takes the Stop plans it is handed as they are. A
/// take-over hands over a preparation retained since its admission, not one
/// built in this call, so its plans are verified against their referents here
/// before the hand-over, with the result the engine gave before.
[[nodiscard]] inline Result<void> verify_retained_preparation_stop_plans(
    const ResolvedPreparedMotionExecutionRequestDTO& request) {
  // A missing ordinary plan is refused by the engine (`ordinary_stop_plan_missing`).
  if (!request.ordinary_stop_plan) return Result<void>::Ok();
  const auto& base = request.stop_readiness.value().resolved_stop_base_plan;
  const auto& resources = request.binding.value().resources;
  auto ordinary = VerifyPreparedParticipantStopPlan(base, *request.ordinary_stop_plan, resources,
                                                    request.retained_default_stop_limits,
                                                    request.stop_node_category);
  if (!ordinary.ok() || !request.fault_stop_plan) return ordinary;
  return VerifyPreparedParticipantStopPlan(base, *request.fault_stop_plan, resources,
                                           request.retained_default_stop_limits,
                                           request.stop_node_category);
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
      *motion->plan_request.speed_profile.max_tcp_linear_speed_mps,
      std::vector<std::string>{frame_role_id(motion->plan_request.tcp_frame)}});
}

[[nodiscard]] inline Result<PreparedOperationParticipantDTO> build_prepared_capability_command_participant(
    const OperationIntentDTO& intent, const CapabilityCommandIntentDTO& command,
    const RuntimeParticipantBinding& sealed_binding,
    const RuntimeParticipantStopReadiness& readiness,
    std::span<const HardStopResourceLimitDTO> retained_default_stop_limits,
    const MotionInterruptNodeCategory stop_node_category, const OperationNumber& operation_id,
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
                                       schema.completion_rule,
                                       schema.confirmation_rule,
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
/// Stop commits its own Stopped/InterventionRequired at P4.
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

inline const RequestKey& ResourceAddressedExecutionKernel::ReplayRequestIndex::key(
    const ReplayEntry& entry) {
  return entry.intent.request_context.request_key;
}

inline const RequestKey& ResourceAddressedExecutionKernel::ControlReplayRequestIndex::key(
    const ControlReplayEntry& entry) {
  return entry.request.request_context.request_key;
}

inline const RequestKey&
ResourceAddressedExecutionKernel::RuntimeScaleControlReplayRequestIndex::key(
    const RuntimeScaleControlReplayEntry& entry) {
  return entry.request.request_context.request_key;
}

struct ResourceAddressedExecutionKernel::OperationRecoveryReplayEntry final {
  OperationNumber operation_id;
  OperationRevision operation_revision{0U};
  KernelRecoveryActionRequestDTO request;
  KernelRecoveryActionResultDTO result;
  bool outcome_unresolved{false};
};

struct ResourceAddressedExecutionKernel::ProgramCycleClaimRun::MotionContinuation final {
  ProgramNodeExecutionScopeDTO origin;
  OperationNumber operation_id;
  OperationParticipantId participant_id;
  ContentDigest prepared_contract_digest;
  RuntimeParticipantBinding binding;
  std::shared_ptr<const detail::MotionResumeIntent> intent;
  ResolvedFrameDTO base_frame;
  ResolvedFrameDTO tcp_frame;
  PlannerCapabilityBindingDTO planner_capability;
  std::size_t authored_motion_kind{};
  /// Set, with no intent, when a sent take-over newcomer's rest cannot be
  /// captured: its next attempt is refused as Resume refuses a stopped
  /// motion whose continuation cannot be captured (OD-269).
  std::optional<ErrorDTO> resume_refusal{};
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
      !details.contains("contact_rule_violation") &&
      !details.contains("collision_result_raw_contact_rule_violation") &&
      (!details.contains("runtime_query_engine_status") || matches("runtime_query_engine_status", "ok")) &&
      !details.contains("runtime_query_failure_reason");
}

/// The runtime monitor stopped motion because the measured robot state it
/// checks was missing, stale or incomplete, not because of contact: the
/// moving robot's own, or that of another component of the cell (its stream
/// condition, with `peer_component_id`).
[[nodiscard]] inline bool runtime_monitor_observation_lost(
    const std::map<std::string, std::string>& details) {
  const auto contact = details.find("in_collision");
  const auto reason = details.find("reason");
  return (contact == details.end() || contact->second != "true") && reason != details.end() &&
         (reason->second == "stale_observed_robot_state" ||
          reason->second == "observed_state_unavailable" ||
          reason->second == "observed_state_incomplete" ||
          reason->second == error_codes::kComponentStreamStale ||
          reason->second == error_codes::kComponentStreamAbsent);
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
  // Freshness is authorized and consumed by the addressed owner. Its local
  // stopped participant does not own aggregate collision provenance.
  request.fresh_motion_attempt = false;
}

/// One armed rule token and the key of the Stop its trip makes: minted on the
/// first trip and reused while this registration stands (contract 1.4).
struct IoRuleRegistrationSlot final {
  IoRuleRegistrationDTO token;
  std::optional<RequestKey> stop_key{};
};

/// One retained stage-attempt result of a participant's Stop (§3.3 P2).
/// `authoritative` is false for a late result of a superseded attempt.
struct StopRetainedStageResult final {
  std::uint64_t result_sequence{};
  std::int64_t completed_at_ns{};
  bool authoritative{true};
  ParticipantStopStageResultDTO result;
};

struct StopParticipantAttempt final {
  bool started{};
  bool superseded{};
  bool result_consumed{};
  std::optional<StopStageAttemptHandle> handle;
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const RemainingStopBudgetDTO> budget;
  /// The Kernel's own watchdog: a controlled stage's own deadline, and the
  /// software-hard stage's deadline (P7, F4); absent for a gripper cancel.
  std::optional<std::int64_t> stage_deadline_ns;
};

/// The working data of one participant's Stop attempt (§2.2): member
/// `stop_work` of `ActiveOperationParticipant`, created with the participant
/// at admission (`retained_results` reserved then) and reset in place by G4
/// when an attempt starts; a Stop never allocates it. Everything the attempt
/// reads that the participant already holds (binding, readiness, resources,
/// port, engines, the executed plan through `stop.plan`) is read there, never
/// copied. Its per-participant progress logic is today's `ProgressStop`.
struct StopParticipantWork final {
  StopParticipantWork() { retained_results.reserve(kMaxStopEscalationStages * 2U); }

  /// The operation revision after this attempt's acceptance (E4).
  OperationRevision accepted_operation_revision{0U};
  /// The participant's payload kind, fixed at G4 (an enum, not a copy).
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  /// The starting event's acceptance time: stage deadlines run from it.
  std::int64_t accepted_at_ns{};
  std::uint64_t prepared_execution_id{};
  IResolvedPreparedParticipantExecutionEngine* prepared_motion_engine{};
  IResolvedParticipantExecutionEngine* execution_engine{};
  /// What today's transaction held for this participant, fixed at G4: a
  /// runtime-collision or (OD-379) component-fault Stop started it.
  bool runtime_collision_containment{};
  bool carried_object_lost{};
  /// Operation Reset or hold timeout: neither is a degraded Stop.
  bool internal_control_stop{};
  /// The attempt's mode is spring return (S12 with `force_spring_return`):
  /// stage 0 runs with the spring-return flag.
  bool force_spring_return{};
  /// S12 started this attempt: its spring-return point is handed to the
  /// takeover fallback waiting for this event, and each pass wakes the
  /// takeover.
  bool takeover_stop{};
  /// A `kStopFailedHalt` started this attempt: its failure raises no further
  /// Halt (F1 recursion bound).
  bool started_by_failure_halt{};
  bool cancelled_pre_dispatch{};
  bool prepared_cancel_requested{};
  bool prepared_cancel_complete{};
  bool prepared_cancel_terminal_observed{};
  bool terminal{};
  bool contained{};
  /// ProgressStop runs at most once at a time per participant.
  bool progress_in_flight{};
  std::size_t current_stage_index{};
  std::uint64_t next_result_sequence{1U};
  std::optional<ErrorDTO> terminal_error;
  std::array<StopParticipantAttempt, kMaxStopEscalationStages> attempts;
  std::vector<StopRetainedStageResult> retained_results;
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
  /// its Stop was containing it. The Stop lane consumes it as escalation
  /// input under the existing cascade rule (strongest authorized stage).
  bool collision_escalation_pending{};
  std::optional<ErrorDTO> collision_escalation_error;
  /// First collision observed on this participant's Stop path. Containment
  /// with a collision on the Stop path ends InterventionRequired carrying it.
  std::optional<ErrorDTO> stop_path_collision;
  /// Why a weaker stage did not contain this participant before a stronger
  /// (fallback hard) stage did. A contained participant with this cause ends
  /// Stopped with a degraded-Stop error (as `main`).
  std::optional<ErrorDTO> escalation_cause{};
  /// The Stop ramp ended but the controller reported the robot outside its
  /// goal tolerance (`main`'s confirmed-but-not-settled case): the
  /// participant ends InterventionRequired and no further Stop command is
  /// issued.
  bool controlled_stop_unsettled{};
  /// The hard stop is this participant's intended Stop: requested directly
  /// (it then starts at the strongest stage) or chosen by the Stop
  /// planner. Its strongest stage is then not an escalation.
  bool lane_hard_stop_requested{};
  /// The never-dispatched participant was closed after its prepared
  /// cancellation was accepted but before that cancellation's terminal
  /// returned. Its exact settlement is deferred to that terminal.
  bool cancel_terminal_deferred{};
  /// An interrupt-selected response follows its qualified suffix
  /// sequentially. Ordinary/operator and collision strongest jumps stay intact.
  bool selected_interrupt_suffix{};
  bool participant_failure_preserved{};

  /// No subordinate terminal handoff exists for a never-dispatched participant
  /// that either never received an execution identity or whose preparation
  /// failed in its own owner before dispatch.
  [[nodiscard]] bool settlement_not_required() const noexcept {
    return cancelled_pre_dispatch && (prepared_execution_id == 0U || preparation_failed);
  }

  /// G4: reset in place for a new attempt. Frees what the previous attempt
  /// held (results, errors, stage pointers); allocates nothing (the result
  /// vector keeps its capacity).
  void reset_for_attempt() noexcept {
    accepted_operation_revision = OperationRevision(0U);
    payload_kind = OperationParticipantPayloadKind::kMotion;
    accepted_at_ns = 0;
    prepared_execution_id = 0U;
    prepared_motion_engine = nullptr;
    execution_engine = nullptr;
    runtime_collision_containment = false;
    carried_object_lost = false;
    internal_control_stop = false;
    force_spring_return = false;
    takeover_stop = false;
    started_by_failure_halt = false;
    cancelled_pre_dispatch = false;
    prepared_cancel_requested = false;
    prepared_cancel_complete = false;
    prepared_cancel_terminal_observed = false;
    terminal = false;
    contained = false;
    progress_in_flight = false;
    current_stage_index = 0U;
    next_result_sequence = 1U;
    terminal_error.reset();
    for (auto& attempt : attempts) attempt = StopParticipantAttempt{};
    retained_results.clear();
    participant_settlement_requested = false;
    participant_settlement_complete = false;
    participant_settlement_error.reset();
    terminal_projection_complete = false;
    preparation_failed = false;
    unclassified_prepared_terminal.reset();
    prepared_classification_requested = false;
    collision_escalation_pending = false;
    collision_escalation_error.reset();
    stop_path_collision.reset();
    escalation_cause.reset();
    controlled_stop_unsettled = false;
    lane_hard_stop_requested = false;
    cancel_terminal_deferred = false;
    selected_interrupt_suffix = false;
    participant_failure_preserved = false;
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
  /// The take-over's "hand-over pending" mark (OD-175 D3), on the candidate's
  /// row from the "decide" step to the "close" step: the execution id of the
  /// motion it took over, 0 = no mark. While set, the reconcile pass, a Stop's
  /// progress, the speed step and the candidate's own take-over advance leave
  /// this row alone; the close step clears it and wakes a waiting Stop.
  std::uint64_t handover_source_execution_id{};
  std::shared_ptr<const detail::MotionResumeIntent> continuation{};
  std::size_t continuation_node_index{};
  /// Runtime collision observed outside an active Stop, or (OD-379) the
  /// component fault whose Stop started this participant's attempt; that
  /// containment Stop ends it with this cause (InterventionRequired unless a
  /// clean collision Stop) and its recovery class is `runtime_collision`.
  std::optional<ErrorDTO> runtime_collision_error{};
  /// A Stop that did not wait for this command's still-returning dispatch
  /// leaves its exact local settlement here; the dispatch return performs it.
  std::optional<AddressedParticipantStopSettlementRequestDTO> deferred_stop_settlement{};
  /// Motion: the execution whose committed send was still in flight when a
  /// Stop fenced it (fence call (1) `send_outcome_pending`), 0 = none. The
  /// Stop's settlement waits until the participant reports the send's outcome
  /// (`kParticipantDispatchOutcome`), which clears it and wakes the Stop lane.
  std::uint64_t motion_send_outcome_awaited{};
  /// Motion: the execution whose committed send the participant reported as
  /// not sent because the Stop came first (cancelled before dispatch, no
  /// effect), 0 = none. Its Stop then settles as a pre-dispatch cancellation.
  std::uint64_t motion_send_refused_by_stop{};
  /// A Stop that closed without waiting for this never-dispatched
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
  /// Take-over newcomer: a cold promotion retired its sent replacement (its
  /// effect settled under the take-over's Stop) and gave the row an unsent
  /// successor. The newcomer's record never claims "no effect" (OD-285).
  bool sent_before_cold_promotion{};
  /// The exact handover commit detached this private physical owner. Its
  /// immutable public participant row remains part of the original operation.
  bool ownership_transferred{};
  /// Reuses ordinary resolved preparation authority for a pending newcomer;
  /// no caller-supplied priority, source curve or dispatch permission.
  std::optional<ResolvedPreparedMotionExecutionRequestDTO> takeover_preparation{};
  std::optional<std::int64_t> takeover_preparation_budget_ns{};
  std::optional<std::int64_t> takeover_planning_budget_ns{};
  std::uint64_t engine_subscription_id{};

  [[nodiscard]] bool non_motion_transaction() const noexcept {
    return payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
           gripper_actuation == GripperActuationKind::kPneumatic && stop_readiness &&
           stop_readiness->value().resolved_stop_base_plan.gripper_actuation_kind ==
               GripperActuationKind::kPneumatic;
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
  /// still borrows this participant's engine.
  [[nodiscard]] bool preparation_borrow_in_flight() const noexcept {
    return prepare_in_flight || prepared_advance_in_flight ||
           deferred_cancel_settlement.has_value();
  }
  /// Tokens only; the shared signal module owns predicate/source state.
  std::vector<IoRuleRegistrationSlot> io_rule_registrations{};
  std::vector<IoMotionRulePolicyBindingDTO> io_motion_rule_policies{};
  /// Keys of this participant's own containment Stops, per operation
  /// revision (contract 1.4; `owned_kernel_request_key`).
  RevisionRequestKeySlot runtime_collision_stop_key{};
  RevisionRequestKeySlot zero_speed_hold_stop_key{};
  /// §2.1: this participant's stop state (phase, attempt, mode, outcome,
  /// attribution, executed plan, Stop lane index). Created with the
  /// participant; a Stop never allocates it.
  ParticipantStopState stop{};
  /// §2.2: the working data of its current Stop attempt.
  StopParticipantWork stop_work{};
};

/// Exact reserved resource set of one operation: the union over its
/// participants.
[[nodiscard]] inline std::vector<ResourceId> operation_resources(
    const std::vector<ActiveOperationParticipant>& participants) {
  std::set<ResourceId> resources;
  for (const auto& participant : participants) {
    resources.insert(participant.resources.begin(), participant.resources.end());
  }
  return {resources.begin(), resources.end()};
}

// Strict authority shared by already resolved admission/manual checks.
// An authored payload/capability alone never grants the Stop-only exception.
[[nodiscard]] inline bool non_motion_registration(
    const RuntimeParticipantExecutionRegistration& registration) {
  return registration.binding.value().capability_id.value() == "tool.gripper.command" &&
      registration.engine && registration.stop_readiness &&
      registration.stop_readiness->value().resolved_stop_base_plan.gripper_actuation_kind ==
          GripperActuationKind::kPneumatic &&
      registration.engine->gripper_actuation_kind(
          registration.binding.value().command_target_id.value()) == GripperActuationKind::kPneumatic;
}

/// The motion-producing recovery actions gated by operator-alarm attention;
/// acknowledge_and_continue moves nothing and stays open.
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
  bool superseded_by_stop{};
  bool effects_dispatched{};
  std::optional<ProgramNodeExecutionScopeDTO> execution_scope;
};

/// The accepted candidate's immutable link to the sole physical Stop owner.
/// Stop/candidate phases are read from their existing owners, never duplicated.
struct MotionTakeoverSpringFallbackDTO final {
  /// The incumbent's Stop event this fallback waits for (S12); its settled
  /// identity is the incumbent participant's attempt started by this event
  /// (K11).
  StopEventId stop_event_id{0U};
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const SpringBackReturnPointDTO> return_point;
  ErrorDTO smooth_failure;
  std::uint64_t abandoned_preparation_id{};
};

struct ResourceAddressedExecutionKernel::ActiveOperation final {
  OperationIntentDTO intent;
  OperationAcceptedDTO accepted;
  std::vector<ActiveOperationParticipant> participants;
  OperationStateDTO state;
  std::optional<PendingOperationControl> pending_control;
  bool stop_effect_in_flight{};
  bool coordinated_dispatch_barrier_released{};
  std::optional<RuntimeScaleStatusDTO> runtime_scale_status;
  /// A refused or mismatched engine acknowledgement, recorded by the host's
  /// next reconcile pass (run_runtime_scale_step).
  struct PendingRuntimeScaleFailure final {
    IResolvedParticipantExecutionEngine* engine{};
    OperationRevision revision;
    RuntimeScaleGeneration generation;
    ErrorDTO error;
    const char* reason{};
  };
  /// Per-operation speed step behind the stored value (OD-149): run on the
  /// speed-scale thread, or by the host's next reconcile pass when the commit
  /// guard was busy (latest value wins: a newer value overwrites the
  /// request). `request` is empty when only a failure is left to record.
  struct PendingRuntimeScale final {
    std::optional<RuntimeScaleRequestDTO> request;
    std::uint64_t value_sequence{};
    std::optional<RequestKey> control_request_id_if_sole;
    std::optional<PendingRuntimeScaleFailure> failure;
  };
  std::optional<PendingRuntimeScale> pending_runtime_scale{};
  // Ordinary observers serialize capture and publication, never provider
  // work. Stop has its own authoritative linearization and bypasses this gate.
  std::shared_ptr<std::mutex> commit_mutex{std::make_shared<std::mutex>()};
  bool ordinary_commit_in_flight{};
  bool recovery_effect_in_flight{};
  bool coordinated_cancellation_in_flight{};
  std::uint64_t last_capture_order{};
  std::shared_ptr<const ResolvedManualProgramHandover> manual_handover{};
  // Manual Robot Control identity, fixed at admission from the binding's
  // intrinsic direct control. Attention and fencing gate ordinary
  // Run/Resume and motion-producing recovery; they never gate a manual
  // operation, whether or not a manual cell-admission claim still names it.
  bool manual{};
  bool retry_run_ended{false};
  mutable std::vector<KernelRecoveryAssessmentDTO> recovery_assessment_inputs{};
  mutable std::uint64_t recovery_assessment_revision{};
  mutable std::optional<KernelRecoveryAssessmentDTO> published_recovery_assessment{};
  using OperationNode = std::map<OperationNumber, std::unique_ptr<ActiveOperation>>::node_type;
  /// Source-owned accepted metadata; neither node presents duplicate physical
  /// ownership to Stop. The next slot is overwritten, never a waiting queue.
  OperationNode pending_motion_takeover{};
  OperationNode next_motion_takeover{};
  std::optional<PreparedMotionReplacementSourceDTO> takeover_source{};
  bool takeover_advance_in_flight{};
  std::optional<MotionTakeoverSpringFallbackDTO> motion_takeover_spring_fallback{};
  /// E13: a Stop covered this operation while it had a pending or queued
  /// takeover candidate, and was not that takeover's own forced Stop (S12).
  /// The candidates lose their right to commit (K11).
  bool takeover_cancelled{};
  /// The Stop event whose stopping state was last queued for the journal.
  StopEventId journaled_stop_event_id{0U};
  /// Latest-wins ticket: queue generation and index of this operation's
  /// unwritten state record (generation 0: none).
  std::uint64_t journal_state_generation{};
  std::size_t journal_state_index{};


  /// The settled outcome of the operation's Stop, derived from its
  /// participants' stop phases (§2.1): absent while no Stop owns the
  /// operation or a participant is still stopping; `kNoEffect` when the Stop
  /// owned it without stopping any participant; else `kSomeFailed` when a
  /// covered participant ended uncontained, `kStoppedEscalated` when one was
  /// contained by a stronger stage than intended, else `kStopped`.
  [[nodiscard]] std::optional<StopEventResult> terminal_stop_result() const noexcept {
    if (!state.stop_event_id.has_value()) return std::nullopt;
    bool covered = false;
    bool failed = false;
    bool escalated = false;
    for (const auto& participant : participants) {
      switch (participant.stop.phase) {
        case StopPhase::kRunning:
          break;
        case StopPhase::kStopping:
          return std::nullopt;
        case StopPhase::kStopped:
          covered = true;
          escalated = escalated || participant.stop.escalated;
          break;
        case StopPhase::kFailed:
          covered = true;
          failed = true;
          break;
      }
    }
    // A Stop that owned the operation without stopping any participant (all
    // non-motion transactions, base revision-only scope): settled, no effect
    // (the base's empty nominal certificate).
    if (!covered) return StopEventResult::kNoEffect;
    if (failed) return StopEventResult::kSomeFailed;
    return escalated ? StopEventResult::kStoppedEscalated : StopEventResult::kStopped;
  }

  /// The Program's ordinary failure from its own Stop: every stopped
  /// participant carries Program-own attribution and was contained at its
  /// intended stage, and the operation ended Stopped.
  [[nodiscard]] bool ordinary_program_stop() const noexcept {
    if (state.state != OperationStateKind::kStopped ||
        terminal_stop_result() != StopEventResult::kStopped)
      return false;
    return std::all_of(participants.begin(), participants.end(), [](const auto& participant) {
      return participant.stop.phase == StopPhase::kRunning ||
             participant.stop.attribution.attribution_class == StopAttributionClass::kProgramOwn;
    });
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
        coordinated_cancellation_in_flight || stop_effect_in_flight)
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

  /// Only exact typed pneumatic Failures may be consumed without Reset.
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
                           return outcome.stop_event_id && !outcome.stop_result;
                         });
    });
  }

  [[nodiscard]] bool stop_allows_retirement() const noexcept {
    if (has_pending_io_rule_response()) return false;
    if (!state.stop_event_id.has_value()) return true;
    const auto outcome = terminal_stop_result();
    return outcome == StopEventResult::kStopped || outcome == StopEventResult::kStoppedEscalated ||
           outcome == StopEventResult::kNoEffect;
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
    return !state.stop_event_id.has_value() && !stop_effect_in_flight &&
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

  /// Keys of the requests the Kernel itself makes for this operation, stored
  /// here so a repeated trigger of the same cause replays exactly (contract
  /// 1.4; `owned_kernel_request_key`).
  std::optional<RequestKey> admission_closed_stop_key{};
  std::optional<RequestKey> partial_dispatch_stop_key{};
  RevisionRequestKeySlot prepared_dispatch_stop_key{};
  RevisionRequestKeySlot manual_settlement_key{};
  /// Kernel closes of this operation: at (operation revision, assessment
  /// revision); a pending takeover candidate's close at its preparation id;
  /// the unsent incumbent's close.
  RevisionRequestKeySlot recovery_close_key{};
  RevisionRequestKeySlot takeover_close_key{};
  std::optional<RequestKey> unsent_takeover_close_key{};
};

/// §6.3.1, contract C4: the participant `stop` objects of an operation's
/// published state, filled from the participants' stop state when the state
/// is read or captured (never at acceptance: digest and stage id are text).
inline void fill_participant_stop_observations(
    const std::vector<ActiveOperationParticipant>& participants, OperationStateDTO& state) {
  for (std::size_t index = 0U; index < participants.size() && index < state.participants.size();
       ++index) {
    const auto& stop = participants[index].stop;
    auto& row = state.participants[index];
    if (stop.attempt == 0U) {
      row.stop.reset();
      continue;
    }
    ParticipantStopObservationDTO observation;
    observation.phase = stop.phase;
    observation.attempt = stop.attempt;
    observation.stop_event_id = stop.started_by;
    observation.mode = stop.mode;
    observation.attribution_class = stop.attribution.attribution_class;
    if (stop.plan != nullptr) {
      observation.plan_digest = stop.plan->participant_stop_invocation_plan_digest;
      if (stop.containing_stage != kNoStopStage &&
          stop.containing_stage < stop.plan->stage_commands.size())
        observation.containing_stage_id =
            stop.plan->stage_commands[stop.containing_stage].resolved_stage.stop_escalation_stage_id;
    }
    if (stop.containing_stage != kNoStopStage) observation.containing_stage_index = stop.containing_stage;
    observation.escalated = stop.escalated;
    row.stop = std::move(observation);
  }
}

class ResourceAddressedExecutionKernel::OperationCommitGuard final {
 public:
  OperationCommitGuard(ResourceAddressedExecutionKernel& owner, const OperationNumber& id)
      : owner_(owner), id_(id) {
    {
      std::lock_guard state_lock(owner_.state_mutex_);
      const auto* operation = owner_.find_active_operation_locked(id_);
      if (operation == nullptr) return;
      mutex_ = operation->commit_mutex;
    }
    lock_ = std::unique_lock(*mutex_);
    claim();
  }
  /// Never waits: `claimed()` is false when another writer holds the gate.
  /// A speed step that meets a held gate stays pending for the host's next
  /// reconcile pass (OD-149).
  OperationCommitGuard(ResourceAddressedExecutionKernel& owner, const OperationNumber& id,
                       std::try_to_lock_t)
      : owner_(owner), id_(id) {
    {
      std::lock_guard state_lock(owner_.state_mutex_);
      const auto* operation = owner_.find_active_operation_locked(id_);
      if (operation == nullptr) return;
      mutex_ = operation->commit_mutex;
    }
    lock_ = std::unique_lock(*mutex_, std::try_to_lock);
    if (lock_.owns_lock()) claim();
  }
  [[nodiscard]] bool claimed() const noexcept { return claimed_; }
  ~OperationCommitGuard() { release(); }
  void release() noexcept;

 private:
  void claim() {
    std::lock_guard state_lock(owner_.state_mutex_);
    auto* operation = owner_.find_active_operation_locked(id_);
    if (operation != nullptr && operation->commit_mutex == mutex_) {
      operation->ordinary_commit_in_flight = true;
      claimed_ = true;
    }
  }

  ResourceAddressedExecutionKernel& owner_;
  OperationNumber id_;
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
  const auto completed_or = [&](const OperationStateKind state) {
    return !participants.empty() &&
           std::all_of(participants.begin(), participants.end(), [&](const auto& participant) {
             return participant.state == OperationStateKind::kCompleted ||
                    participant.state == state ||
                    ((participant.state == OperationStateKind::kFailed &&
                      participant.terminal_error &&
                      participant.terminal_error->details.contains("pneumatic_transaction_final") &&
                      participant.terminal_error->details.at("pneumatic_transaction_final") ==
                          "true"));
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
        return row.state == OperationStateKind::kFailed && !final_pneumatic_failure(row);
      }))
    return OperationStateKind::kInterventionRequired;
  if (any_state(OperationStateKind::kFailed) &&
      std::all_of(participants.begin(), participants.end(), [&](const auto& row) {
        return row.state == OperationStateKind::kCompleted || final_pneumatic_failure(row);
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

/// `main`'s degraded-Stop error: the controlled Stop did not contain the
/// motion and the fallback hard stop did. The operation still ends
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

/// The controlled stage reports the hard stop as the right Stop: the
/// Stop planner chose it (`main`'s ros2_control operator path), or the
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

/// A Stop ramp that ended with the robot outside the controller's goal
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
    const OperationNumber& operation_id, const std::vector<ActiveOperationParticipant>& participants,
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

struct ResourceAddressedExecutionKernel::GlobalInterruptStopParticipant final {
  OperationNumber operation_id;
  OperationParticipantId participant_id;
  std::uint64_t preparation_id;
  ContentDigest prepared_contract_digest;
  ControllerGoalId controller_goal_id;
  PreparedParticipantStopPlanDTO plan;
  IoRulePreparedStopEntryDTO stop_entry;
};

struct ResourceAddressedExecutionKernel::GlobalInterruptStopSelection final {
  // A `stop` rule's trip, or any active rule that lost its signals
  // (`watch_lost`; no registration when its setup failed): the operator's
  // Stop on the rule's scope.
  InterruptPolicyId policy_id;
  std::optional<IoRuleRegistrationDTO> registration;
  std::optional<IoRuleEvidenceDTO> evidence;
  bool watch_lost;
  bool everything;
  // `after_stop: fail_node`: the Program's own Stop, its motion fails with
  // `interrupt_fault`; otherwise the operator's hold.
  bool fail_node;
  int priority;
  std::vector<GlobalInterruptStopParticipant> participants;
  std::vector<ResourceId> scope_resources;
};

namespace resource_addressed_kernel_detail {

inline constexpr std::uint64_t kMaximumKernelJsonInteger = 9'007'199'254'740'991ULL;

[[nodiscard]] inline std::int64_t checked_deadline(std::int64_t now, std::int64_t duration) noexcept {
  if (now < 0 || duration <= 0 || duration > kMaximumStopDurationNs ||
      now > std::numeric_limits<std::int64_t>::max() - duration) {
    return -1;
  }
  return now + duration;
}

/// This robot's own controlled-Stop deadline, measured from the Stop
/// acceptance and computed at Stop time from the stage's exact prepared
/// controlled-Stop parameters (`max_stop_time_s`, else deceleration time +
/// 0.5 s). Absent for every other stage: a hard stage is bounded by its
/// participant with the lane's stop-completion timeout and a gripper
/// cancel is fire-and-forget.
[[nodiscard]] inline std::optional<Duration> controlled_stage_deadline_ns(
    const PreparedStopStageCommandDTO& command) {
  if (command.resolved_stage.containment_command.command_kind !=
          StopContainmentCommandKind::kMotionControlledStop ||
      !command.validated_contract_parameters.has_value()) {
    return std::nullopt;
  }
  return controlled_stop_deadline_from_contract_parameters(*command.validated_contract_parameters);
}

/// The per-robot Stop-time values handed with one stage: no shared
/// cell-wide budget.
[[nodiscard]] inline RemainingStopBudgetDTO remaining_stop_budget(
    const PreparedStopStageCommandDTO& command, const StopEventId stop_event_id,
    const std::int64_t now, const std::int64_t accepted_at_ns) {
  RemainingStopBudgetDTO budget{stop_event_id, std::max<std::int64_t>(0, now - accepted_at_ns),
                                std::nullopt};
  if (const auto deadline = controlled_stage_deadline_ns(command)) {
    budget.remaining_controlled_deadline_ns =
        std::max<std::int64_t>(0, *deadline - budget.elapsed_since_stop_ns);
  }
  return budget;
}

/// The Kernel's own watchdog for one controlled stage: from the Stop request
/// its controlled deadline plus a normal motion's no-report bound for a Stop
/// trajectory as long as that deadline (a dispatched Stop trajectory
/// finishes like a normal motion, `controlled_stage_watchdog_ns`). The
/// software-hard stage has its own (`hard_stage_watchdog_deadline`); a
/// gripper cancel has none.
[[nodiscard]] inline std::optional<std::int64_t> stage_watchdog_deadline(
    const RemainingStopBudgetDTO& budget, const std::int64_t now) noexcept {
  if (!budget.remaining_controlled_deadline_ns.has_value()) {
    return std::nullopt;
  }
  const auto deadline = checked_deadline(
      now, controlled_stage_watchdog_ns(*budget.remaining_controlled_deadline_ns +
                                        budget.elapsed_since_stop_ns) -
               budget.elapsed_since_stop_ns);
  return deadline < 0 ? std::nullopt : std::optional(deadline);
}

/// P7 (F4): the Kernel's watchdog of the software-hard stage, stage start +
/// `controlled_stage_watchdog_ns(hard_stop_completion_timeout_ns)`: just past
/// the stage helper's own failure at the lane timeout, so it fires only when
/// the port never reports. None when the lane has no hard-stop timeout (C20).
[[nodiscard]] inline std::optional<std::int64_t> hard_stage_watchdog_deadline(
    const PreparedStopStageCommandDTO& command, const Duration hard_stop_completion_timeout_ns,
    const std::int64_t now) noexcept {
  if (command.resolved_stage.command_qualification.stop_action_class ==
          StopActionClass::kControlled ||
      hard_stop_completion_timeout_ns <= 0)
    return std::nullopt;
  const auto deadline =
      checked_deadline(now, controlled_stage_watchdog_ns(hard_stop_completion_timeout_ns));
  return deadline < 0 ? std::nullopt : std::optional(deadline);
}

/// One stage of a participant's Stop attempt, resolved from the participant's
/// own binding, readiness and executed plan (`stop.plan`). Called on the Stop
/// lane (never at acceptance).
[[nodiscard]] inline Result<ResolvedParticipantStopStageDTO> resolve_participant_stop_stage(
    const ActiveOperationParticipant& item, const OperationNumber& operation_id,
    const ControllerGoalId& controller_goal_id, const std::size_t stage_index,
    const RemainingStopBudgetDTO& budget) {
  const auto& plan = *item.stop.plan;
  const auto& binding = item.binding.value();
  const auto& path = *binding.control_path;
  const auto& selected = plan.stage_commands[stage_index].resolved_stage;
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

  ResolvedParticipantStopStageDTO candidate{item.stop.started_by,
                                            operation_id,
                                            plan.operation_revision,
                                            plan.participant_id,
                                            controller_goal_id,
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
                                            plan,
                                            stage_index,
                                            ContentDigest("pending")};
  return MakeResolvedParticipantStopStage(
      std::move(candidate), budget, item.stop_readiness->value().resolved_stop_base_plan,
      item.binding, item.retained_default_stop_limits, item.stop_node_category, item.resources);
}

}  // namespace resource_addressed_kernel_detail

namespace resource_addressed_kernel_detail {
inline bool global_rule_registration_matches(const IoRuleRegistrationDTO& token,
    const IoRuleStateContextDTO& context, const InterruptPolicyId& policy) {
  return token.catalog_generation == context.catalog_generation && token.registration_id != 0U &&
         token.policy_id == policy && token.scope == IoRuleScopeDTO{IoActivationRuleScopeDTO{
             context.retained_bundle_id, context.graph_revision}};
}
}  // namespace resource_addressed_kernel_detail

// One installed global rule. An inactive rule holds no
// registration. An active rule watches through its registration; without one
// (`qualification_error`) it can't watch and gates its scope until a fresh
// rearm succeeds. A trip is consumed once; the registration is renewed when
// the condition is false again so the next edge trips anew.
struct ResourceAddressedExecutionKernel::GlobalIoRuleRuntime final {
  IoRuleStateDTO state;
  std::optional<IoRuleRegistrationDTO> registration;
  std::optional<IoRuleEvidenceDTO> evidence;
  std::optional<ErrorDTO> qualification_error;
  bool active{};
  bool trip_consumed{};
  // Watched at least once since Load or activation: a missing Program key
  // baseline before that is not a loss (the run publishes its keys first).
  bool watched{};
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

/// The journal writer queue: 4,096 places, plus 194 that only Stop rows use.
inline constexpr std::size_t kJournalQueueCapacity = 4096U;
inline constexpr std::size_t kJournalQueueStopReserve = 194U;

/// The one journal writer: an ordinary thread that takes the posted records
/// in action order and writes them. It only writes (OD-149).
class ResourceAddressedExecutionKernel::JournalWriter final {
 public:
  explicit JournalWriter(ResourceAddressedExecutionKernel& owner)
      : owner_(owner), worker_([this](std::stop_token token) noexcept { Run(token); }) {
    while (!ready_.load(std::memory_order_acquire)) {
      ready_.wait(false, std::memory_order_acquire);
    }
  }

  ~JournalWriter() {
    Close();
    worker_.request_stop();
    Wake();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  /// Records were posted. Allocation-free and wait-free for the caller.
  void Wake() noexcept {
    wake_sequence_.fetch_add(1U, std::memory_order_release);
    wake_sequence_.notify_one();
  }

  /// Shutdown: posts are still taken and written until the drain ends.
  void Close() noexcept {
    accepting_.store(false, std::memory_order_release);
    Wake();
    NotifyDrainState();
  }

  /// Shutdown, when the drain starts: one line with the records still to be
  /// written, so the count is in the log before anything outside can end
  /// the process (OD-187 (3)). Atomic reads only.
  void ReportQueuedAtDrain() const noexcept {
    const auto posted = owner_.journal_posted_count_.load(std::memory_order_acquire);
    const auto written = written_count_.load(std::memory_order_acquire);
    (void)std::fprintf(stderr, "manyforge_journal_drain queued=%llu\n",
                       static_cast<unsigned long long>(posted - written));
    (void)std::fflush(stderr);
  }

  [[nodiscard]] bool DrainFor(const std::chrono::nanoseconds timeout) noexcept {
    bool drained = false;
    try {
      std::unique_lock lock(drain_mutex_);
      drained = drain_changed_.wait_for(lock, timeout, [this] {
        return written_count_.load(std::memory_order_acquire) ==
               owner_.journal_posted_count_.load(std::memory_order_acquire);
      });
    } catch (...) {
    }
    if (!drained) ReportUndrained();
    return drained;
  }

  /// Every record posted up to `posted_through` was handed to the journal.
  void MarkWritten(const std::uint64_t posted_through) noexcept {
    written_count_.store(posted_through, std::memory_order_release);
  }

  /// Shutdown: the one line with the records not written in this start,
  /// printed once (after the drain, or when its bound first passes).
  void ReportShutdown() noexcept {
    if (accepting_.load(std::memory_order_acquire) || shutdown_reported_.exchange(true)) return;
    const auto posted = owner_.journal_posted_count_.load(std::memory_order_acquire);
    const auto written = written_count_.load(std::memory_order_acquire);
    owner_.report_journal_records_not_written(posted - written);
  }

 private:
  void NotifyDrainState() noexcept {
    std::lock_guard lock(drain_mutex_);
    drain_changed_.notify_all();
  }

  /// The drain bound passed with records still unwritten; `WaitForDrain`
  /// logs `drain_stuck` as for every other lane.
  void ReportUndrained() noexcept { ReportShutdown(); }

  void Run(const std::stop_token stop_token) noexcept {
    static_cast<void>(::pthread_setname_np(::pthread_self(), "mf_journal"));
    // Never inherit a real-time class from the constructing thread.
    const sched_param ordinary{};
    static_cast<void>(pthread_setschedparam(pthread_self(), SCHED_OTHER, &ordinary));
    taken_.reserve(kJournalQueueCapacity + kJournalQueueStopReserve);
    ready_.store(true, std::memory_order_release);
    ready_.notify_all();
    auto observed_wake = wake_sequence_.load(std::memory_order_acquire);
    while (!stop_token.stop_requested()) {
      if (owner_.write_journal_batch(taken_)) {
        NotifyDrainState();
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

  ResourceAddressedExecutionKernel& owner_;
  std::atomic_bool accepting_{true};
  std::atomic_bool ready_{};
  std::atomic_uint64_t wake_sequence_{};
  std::atomic_uint64_t written_count_{};
  std::atomic_bool shutdown_reported_{};
  /// The writer's half of the double buffer, swapped with the Kernel's queue.
  std::vector<JournalRecord> taken_;
  std::mutex drain_mutex_;
  std::condition_variable drain_changed_;
  std::jthread worker_;
};

// A pending speed step is not handed on here: the host's next reconcile pass
// runs it (OD-149).
inline void ResourceAddressedExecutionKernel::OperationCommitGuard::release() noexcept {
  if (!lock_.owns_lock()) return;
  try {
    std::lock_guard state_lock(owner_.state_mutex_);
    auto* operation = owner_.find_active_operation_locked(id_);
    if (claimed_ && operation != nullptr && operation->commit_mutex == mutex_) {
      operation->ordinary_commit_in_flight = false;
      // A cleanup callback may have completed while the gate was held across
      // an engine status call. Retire only after the gate is released.
      owner_.settle_completed_control_locked(id_);
    }
  } catch (...) {
    // Destruction must still release the ordinary gate during exception
    // unwinding. No provider call or storage retry belongs in this guard.
  }
  lock_.unlock();
}

struct ResourceAddressedExecutionKernel::EngineSubscription final {
  IResolvedParticipantExecutionEngine* engine{};
  SubscriptionHandle handle{};
  std::uint64_t sequence_id{};
  // First registered component for this possibly shared engine.
  std::string component_id{};
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
    if (intent.participants.size() != 1U ||
        intent.coordination_policy != OperationCoordinationPolicy::kIndependent) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "preview admits one independent participant", "i6_preview_single_participant_only");
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
    preview_tcp_capture_.reset();
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

    const PreviewNumber preview(next_preview_id_++);
    // A preview is not an operation. Its request carries the preview number
    // in the operation field (and so in its participant and goal), which only
    // the planner's presence check reads.
    const OperationNumber operation_id(preview.value());
    const OperationParticipantId participant_id{operation_id, 1U};
    const ControllerGoalId controller_goal_id{operation_id, 1U, 1U};
    preview_tcp_capture_ = resolved.value().preview_tcp_capture;
    return build_motion_request(
        intent, intent.participants.front(),
        std::get<MotionParticipantIntentDTO>(intent.participants.front().payload),
        resolved.value().binding.value(), operation_id, OperationRevision(1U), participant_id,
        controller_goal_id, resolved.value(), 1.0);
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
      const std::stop_token stop_token,
      std::optional<MotionPathPreview>* path_preview) override {
    if (path_preview != nullptr) path_preview->reset();
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
    auto result = motion_.preview_motion(std::move(prepared.value()), std::move(projection_inventory),
                                         std::move(start_state), planning_attempt_index, stop_token,
                                         path_preview);
    if (result.ok()) result.value().resolved_tcp = preview_tcp_capture_;
    return result;
  }

 private:
  ResourceAddressedExecutionKernel& owner_;
  RuntimeParticipantBinding primary_binding_;
  RuntimeParticipantPolicySelection primary_participant_policy_;
  IResolvedParticipantExecutionEngine& primary_engine_;
  IMotionPreview& motion_;
  std::optional<PreviewTcpCapture> preview_tcp_capture_{};
  // This is intentionally retained for the complete multi-leg candidate
  // transaction. The independent control path never takes this mutex.
  std::unique_lock<std::mutex> submission_lock_;
  std::uint64_t next_preview_id_{1U};
};

}  // namespace manyforge::core
