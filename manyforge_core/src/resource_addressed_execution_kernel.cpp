#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <manyforge_core/detail/coordinated_motion_preparation.hpp>
#include <manyforge_core/detail/motion_resume_intent.hpp>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <numeric>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

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

[[nodiscard]] bool is_active_stop_fault_latch(
    const std::optional<StopFaultLatchDTO>& latch) noexcept {
  return latch.has_value() && latch->state != StopFaultLatchStateDTO::kCleared;
}

[[nodiscard]] ErrorDTO error(std::string code, std::string message, std::string reason) {
  return ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

template <typename T>
[[nodiscard]] Result<T> reject(std::string message, std::string reason,
                               ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<T>::Err(error(std::string(to_string(code)), std::move(message), std::move(reason)));
}

template <typename Writer>
[[nodiscard]] Result<std::uint64_t> attempt_journal_write(Writer&& write) {
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

[[nodiscard]] std::vector<RuntimeParticipantBinding> extract_bindings(
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

[[nodiscard]] std::size_t qualified_execution_engine_count(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
  std::set<const IResolvedParticipantExecutionEngine*> engines;
  for (const auto& registration : registrations) {
    if (registration.stop_readiness.has_value() && registration.stop_execution_port != nullptr) {
      engines.insert(registration.engine);
    }
  }
  return engines.size();
}

[[nodiscard]] std::vector<IResolvedParticipantExecutionEngine*> distinct_execution_engines(
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

[[nodiscard]] std::vector<DomainActiveControlConfigurationDTO>
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

[[nodiscard]] bool domain_has_complete_stop_readiness(
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

[[nodiscard]] std::vector<DomainActiveControlConfigurationDTO>
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

[[nodiscard]] std::vector<DomainActiveControlConfigurationDTO> route_publication_identities(
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

[[nodiscard]] Result<std::vector<RouteReadinessPublicationRowDTO>>
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

[[nodiscard]] bool registration_truth_equal(
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
         left.direct_control_joint_limits == right.direct_control_joint_limits;
}

[[nodiscard]] bool registration_controller_equal_after_product_reseal(
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
  if (a != candidate_controller_binding ||
      left.engine != right.engine ||
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
      old_readiness.expected_controller_domain != next_readiness.expected_controller_domain ||
      old_readiness.maximum_controlled_stop_duration_ns !=
          next_readiness.maximum_controlled_stop_duration_ns) {
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

[[nodiscard]] std::set<ControlDomainId> changed_registration_domains(
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
  std::optional<StopDeadlineBudgetDTO> deadline_budget;
  std::vector<StopScopeComponentSnapshotDTO> components;
};

[[nodiscard]] StopStaticFacts derive_stop_static_facts(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations,
    const ResourceAddressedExecutionKernelConfig& config,
    const std::optional<StopDeadlineBudgetDTO>* selected_deadline_budget = nullptr,
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
  std::optional<StopDeadlineBudgetDTO> deadline_budget = selected_deadline_budget == nullptr
                                                             ? config.cell_stop_deadline_budget
                                                             : *selected_deadline_budget;
  if (deadline_budget.has_value()) {
    deadline_budget->phases.clear();
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
    if (!registration.stop_readiness.has_value()) {
      // Unqualified registrations remain observable and may later be replaced
      // under a configuration fence, but Execute cannot admit them. They do
      // not self-attest component lifecycle membership or a Stop plan here.
      continue;
    }
    const auto& readiness = registration.stop_readiness->value();
    const auto& budget = readiness.resolved_stop_base_plan.deadline_budget;
    if (!deadline_budget.has_value()) {
      deadline_budget =
          StopDeadlineBudgetDTO{budget.outer_duration_ns, budget.kernel_completion_reserve_ns, {}};
    }
    if (budget.outer_duration_ns != deadline_budget->outer_duration_ns ||
        budget.kernel_completion_reserve_ns != deadline_budget->kernel_completion_reserve_ns) {
      throw std::invalid_argument(
          "runtime participants do not share one cell-wide Stop deadline envelope");
    }

    auto [component, inserted] = components.try_emplace(
        readiness.controllable_component_id,
        StopScopeComponentSnapshotDTO{
            readiness.controllable_component_id, readiness.component_lifecycle_generation, {}});
    if (!inserted && component->second.component_lifecycle_generation !=
                         readiness.component_lifecycle_generation) {
      throw std::invalid_argument(
          "runtime participant Stop readiness disagrees on component lifecycle generation");
    }
    for (const auto& resource : binding.resources) {
      const auto [owner, owner_inserted] =
          resource_owners.emplace(resource.resource_id, readiness.controllable_component_id);
      if (!owner_inserted && owner->second != readiness.controllable_component_id) {
        throw std::invalid_argument(
            "runtime Stop readiness assigns one resource to multiple components");
      }
      component->second.resources.push_back(resource.resource_id);
    }
  }

  if (!graph_revision.has_value() || graph_revision->empty()) {
    throw std::invalid_argument("runtime Stop resolution has no current graph revision");
  }
  if (deadline_budget.has_value() &&
      (deadline_budget->outer_duration_ns <= 0 ||
       deadline_budget->outer_duration_ns > kMaximumStopDurationNs ||
       deadline_budget->kernel_completion_reserve_ns <= 0 ||
       deadline_budget->kernel_completion_reserve_ns >= deadline_budget->outer_duration_ns)) {
    throw std::invalid_argument("runtime Stop resolution has no valid cell-wide deadline envelope");
  }
  if (!deadline_budget.has_value() && !components.empty()) {
    throw std::invalid_argument(
        "Stop-qualified runtime participants have no cell-wide deadline envelope");
  }

  if (!retained_bundle_id.has_value() || retained_bundle_id->empty()) {
    throw std::invalid_argument("runtime Stop terminal truth has no retained-bundle identity");
  }

  StopStaticFacts facts{
      std::move(*retained_bundle_id), std::move(*graph_revision), std::move(deadline_budget), {}};
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

[[nodiscard]] const ProgramFrameRoleBindingDTO* find_frame(
    const RuntimeParticipantBindingDTO& binding, const FrameRoleId& role) noexcept {
  const auto found = std::find_if(
      binding.frame_bindings.begin(), binding.frame_bindings.end(),
      [&](const ProgramFrameRoleBindingDTO& candidate) { return candidate.frame_role_id == role; });
  return found == binding.frame_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] const RuntimeResourceBindingDTO* find_source_joint(
    const RuntimeParticipantBindingDTO& binding, const std::string& source_joint_name) noexcept {
  const auto found = std::find_if(binding.resources.begin(), binding.resources.end(),
                                  [&](const RuntimeResourceBindingDTO& candidate) {
                                    return candidate.source_joint_name.has_value() &&
                                           *candidate.source_joint_name == source_joint_name;
                                  });
  return found == binding.resources.end() ? nullptr : &*found;
}

[[nodiscard]] const ProgramModelRoleBindingDTO* find_model(
    const RuntimeParticipantBindingDTO& binding,
    const ModelInstanceId& model_instance_id) noexcept {
  const auto found = std::find_if(binding.model_bindings.begin(), binding.model_bindings.end(),
                                  [&](const ProgramModelRoleBindingDTO& candidate) {
                                    return candidate.model_instance_id == model_instance_id;
                                  });
  return found == binding.model_bindings.end() ? nullptr : &*found;
}

[[nodiscard]] CapabilityId required_planner_capability(const MotionSpecDTO& motion) {
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

[[nodiscard]] bool same_linked_preview_participant(const RuntimeParticipantBindingDTO& left,
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

[[nodiscard]] Result<std::optional<ResolvedContactGraspBindingDTO>> resolve_contact_grasp_binding(
    const MotionParticipantIntentDTO& motion, const RuntimeParticipantBindingDTO& binding) {
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
    // Other contact-policy modes remain selected from the retained scene
    // policy. Only Program-owned contact_grasp requires this late binding.
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
  for (const auto& body : policy->allowed_body_bindings) {
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
          object->scene_resource_id, std::move(body_ids), policy->max_penetration_m,
          policy->terminal_window_start_fraction}});
}

[[nodiscard]] ResolvedFrameDTO resolved_component_frame(const RuntimeParticipantBindingDTO& binding,
                                                        const ProgramFrameRoleBindingDTO& frame) {
  return ResolvedComponentModelFrameDTO{binding.controllable_component_id, frame.model_instance_id,
                                        frame.frame_role_id, frame.compiled_frame_name,
                                        frame.model_revision};
}

[[nodiscard]] Result<ExecuteRequestDTO> build_motion_request(
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
  const auto* tcp = find_frame(binding, motion.tcp_frame_role_id);
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
  auto interrupt_admission = validate_motion_interrupt_scope_admission(motion.motion_interrupt_scopes);
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
  request.motion_interrupt_scopes = motion.motion_interrupt_scopes;
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

[[nodiscard]] Result<GripperCommandRequestDTO> build_command_request(
    const CapabilityCommandIntentDTO& command, const RuntimeParticipantBindingDTO& binding) {
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
  request.completion_policy = "goal_fenced_target_reached";
  request.grasp_confirmation_policy = "binding_feedback_required";
  request.transport_kind = "resolved_binding";
  request.evidence = {"command_terminal"};
  request.evidence_state = "required_pending";
  request.completion_timeout_s = command.completion_timeout_s;
  return Result<GripperCommandRequestDTO>::Ok(std::move(request));
}

[[nodiscard]] Result<std::optional<PreparedParticipantStopPlanDTO>> make_runtime_fault_stop_plan(
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
  auto prepared = MakePreparedParticipantStopPlan(readiness.value(), std::move(context),
                                                  binding.resources, retained_default_stop_limits);
  if (!prepared.ok()) {
    return Result<std::optional<PreparedParticipantStopPlanDTO>>::Err(prepared.error());
  }
  return Result<std::optional<PreparedParticipantStopPlanDTO>>::Ok(std::move(prepared.value()));
}

[[nodiscard]] bool prepared_fault_stop_plan_matches(
    const std::optional<PreparedParticipantStopPlanDTO>& product,
    const std::shared_ptr<const PreparedParticipantStopPlanDTO>& authority) {
  return authority ? product == std::optional<PreparedParticipantStopPlanDTO>{*authority}
                   : !product.has_value();
}

[[nodiscard]] Result<PreparedMotionDiagnosticsDTO> prepared_motion_diagnostics(
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

[[nodiscard]] Result<PreparedOperationParticipantDTO> build_prepared_capability_command_participant(
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
                                       command.completion_timeout_s,
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

[[nodiscard]] OperationStateKind operation_state(ExecutionState state) noexcept {
  switch (state) {
    case ExecutionState::kIdle:
      return OperationStateKind::kPrepared;
    case ExecutionState::kPlanning:
      return OperationStateKind::kPlanning;
    case ExecutionState::kValidating:
      return OperationStateKind::kValidating;
    case ExecutionState::kExecuting:
      return OperationStateKind::kExecuting;
    case ExecutionState::kInterrupted:
      return OperationStateKind::kInterrupted;
    case ExecutionState::kStopping:
      return OperationStateKind::kStopping;
    case ExecutionState::kStopped:
      return OperationStateKind::kStopped;
    case ExecutionState::kCompleted:
      return OperationStateKind::kCompleted;
    case ExecutionState::kFailed:
      return OperationStateKind::kFailed;
    case ExecutionState::kInterventionRequired:
      return OperationStateKind::kInterventionRequired;
  }
  return OperationStateKind::kInterventionRequired;
}

[[nodiscard]] bool terminal(OperationStateKind state) noexcept {
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

[[nodiscard]] bool control_admissible(OperationControlKind control,
                                      OperationStateKind state) noexcept {
  switch (control) {
    case OperationControlKind::kReset:
      return state != OperationStateKind::kCompleted;
    case OperationControlKind::kAcknowledgeCompleted:
      return state == OperationStateKind::kCompleted;
  }
  return false;
}

[[nodiscard]] const char* control_name(OperationControlKind control) noexcept {
  switch (control) {
    case OperationControlKind::kReset:
      return "reset";
    case OperationControlKind::kAcknowledgeCompleted:
      return "acknowledge_completed";
  }
  return "unknown";
}

[[nodiscard]] std::optional<ResourceSlotState> slot_state_for(OperationStateKind state) noexcept {
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

[[nodiscard]] ErrorDTO status_error(ErrorCode code) {
  return ErrorDTO{std::string(to_string(code)),
                  "subordinate participant reached a terminal error state",
                  {{"reason", "subordinate_terminal_error"}}};
}

[[nodiscard]] RuntimeScaleParticipantStatusDTO initial_scale_status(
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

[[nodiscard]] Result<void> cancel_prepared_participant(
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

}  // namespace

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
  StopFaultRecoveryIntentDTO intent;
  StopFaultRecoveryResultDTO result;
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

struct ActiveOperationParticipant final {
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness stop_readiness;
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
};

/// Exact reserved resource set of one operation: the union over its
/// participants (spec 450 §5.2 coordinated rule).
[[nodiscard]] static std::vector<ResourceId> operation_resources(
    const std::vector<ActiveOperationParticipant>& participants) {
  std::set<ResourceId> resources;
  for (const auto& participant : participants) {
    resources.insert(participant.resources.begin(), participant.resources.end());
  }
  return {resources.begin(), resources.end()};
}

/// The motion-producing recovery actions gated by operator-alarm attention
/// (spec 450 §5.2); acknowledge_and_continue moves nothing and stays open.
[[nodiscard]] static bool motion_producing_recovery(const RecoveryAction action) noexcept {
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
  std::shared_ptr<detail::CoordinatedMotionPreparation> common_timing{};
  mutable std::vector<KernelRecoveryAssessmentDTO> recovery_assessment_inputs{};
  mutable std::uint64_t recovery_assessment_revision{};

  [[nodiscard]] std::optional<StopCertificateResultDTO> terminal_stop_result() const noexcept {
    if (settled_stop.has_value() && state.active_stop_id == settled_stop->stop_id &&
        state.active_stop_generation == settled_stop->generation)
      return settled_stop->result;
    return std::nullopt;
  }

  [[nodiscard]] bool stop_allows_retirement() const noexcept {
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

void ResourceAddressedExecutionKernel::stamp_operation_capture_locked(ActiveOperation& operation,
                                                                      OperationStateDTO& state) {
  if (operation.last_capture_order == static_cast<std::uint64_t>(INT64_MAX)) {
    motion_admission_closed_ = true;
    throw std::overflow_error("operation journal capture order exhausted");
  }
  state.journal_capture_order = ++operation.last_capture_order;
}

Result<bool> ResourceAddressedExecutionKernel::record_operation_state_locked(
    std::unique_lock<std::mutex>& state_lock, OperationStateDTO& state,
    const RuntimeScaleStatusDTO* scale, const char* reason,
    std::span<const PreparedMotionDiagnosticsDTO> diagnostics) {
  auto* operation = find_active_operation_locked(state.operation_id);
  if (operation == nullptr || !operation->ordinary_commit_in_flight)
    return reject<bool>("operation capture has no ordinary commit guard",
                        "operation_commit_guard_missing", ErrorCode::kInvariantViolation);
  if (operation->recovery_effect_in_flight || operation->coordinated_cancellation_in_flight)
    return Result<bool>::Ok(false);
  const auto prior = operation->state;
  const auto identity = operation->commit_mutex;
  const auto retained_scale =
      scale == nullptr ? std::optional<RuntimeScaleStatusDTO>{} : std::optional(*scale);
  stamp_operation_capture_locked(*operation, state);
  state_lock.unlock();
  auto written = attempt_journal_write([&] {
    return journal_.RecordOperationState(config_.request_owner_epoch, state,
                                         retained_scale ? &*retained_scale : nullptr, reason,
                                         diagnostics);
  });
  state_lock.lock();
  if (!written.ok()) return Result<bool>::Err(written.error());
  operation = find_active_operation_locked(state.operation_id);
  if (operation == nullptr || operation->commit_mutex != identity ||
      operation->state.operation_revision != prior.operation_revision ||
      operation->state.active_stop_id != prior.active_stop_id ||
      operation->state.active_stop_generation != prior.active_stop_generation ||
      operation->state.journal_capture_order != prior.journal_capture_order)
    return Result<bool>::Ok(false);
  // Progress-only samples do not need durable writes. Do not overwrite a
  // newer such sample on an unchanged participant when this capture commits.
  for (std::size_t index = 0; index < state.participants.size(); ++index) {
    auto& row = state.participants[index];
    if (row.state == prior.participants[index].state &&
        row.progress == prior.participants[index].progress)
      row.progress = operation->state.participants[index].progress;
  }
  state.updated_at_ns = std::max(state.updated_at_ns, operation->state.updated_at_ns);
  return Result<bool>::Ok(true);
}

[[nodiscard]] OperationStateKind aggregate_participant_state(
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
                    participant.state == state;
           });
  };
  if (any_state(OperationStateKind::kInterventionRequired) ||
      any_state(OperationStateKind::kFailed) || any_state(OperationStateKind::kInterrupted)) {
    return OperationStateKind::kInterventionRequired;
  }
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

[[nodiscard]] Result<std::vector<ResourceSlotReservationOwnerDTO>> retained_owner_rows(
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

[[nodiscard]] Result<std::vector<OperationControlEffectRequest>> reset_effect_requests(
    const OperationId& operation_id, const std::vector<ActiveOperationParticipant>& participants,
    const OperationRevision operation_revision) {
  std::vector<OperationControlEffectRequest> requests;
  requests.reserve(participants.size());
  for (const auto& participant : participants) {
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
  std::int64_t stage_deadline_ns{};
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

  /// No subordinate terminal handoff exists for a never-dispatched participant
  /// that either never received an execution identity or whose preparation
  /// failed in its own owner before dispatch.
  [[nodiscard]] bool settlement_not_required() const noexcept {
    return cancelled_pre_dispatch && (prepared_execution_id == 0U || preparation_failed);
  }
};

static void fail_prepared_cancellation(StopParticipantWork& work, ErrorDTO failure) {
  if (work.terminal) return;
  work.terminal = true;
  work.contained = false;
  work.terminal_error = std::move(failure);
}

static void observe_prepared_cancellation_terminal(StopParticipantWork& work,
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

static void settle_failed_preparation(StopParticipantWork& work, ErrorDTO failure) {
  if (work.terminal) return;
  work.preparation_failed = true;
  work.prepared_cancel_terminal_observed = true;
  work.contained = true;
  work.terminal = true;
  work.terminal_error = std::move(failure);
}

struct ResourceAddressedExecutionKernel::StopTransaction final {
  bool active{};
  StopControlWorkToken token;
  std::optional<StopId> stop_id;
  StopGeneration stop_generation{0U};
  std::int64_t outer_deadline_ns{};
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
};

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

namespace {

constexpr std::uint64_t kMaximumKernelJsonInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaximumResidentStopTerminalRecords = 64U;
// Recognized Stop authority is already linearized, and containment never
// depends on this sole non-critical projection worker. Each Stop projection is
// one journal attempt: the journal hands its writer to a waiting Stop
// projection right after the one write in progress, so a failed attempt is a
// genuine storage fault and fails closed through the persistence fault latch.
// The status records that single attempt as its retry generation.
constexpr std::uint64_t kStopJournalProjectionAttempt = 1U;

[[nodiscard]] std::int64_t checked_deadline(std::int64_t now, std::int64_t duration) noexcept {
  if (now < 0 || duration <= 0 || duration > kMaximumStopDurationNs ||
      now > std::numeric_limits<std::int64_t>::max() - duration) {
    return -1;
  }
  return now + duration;
}

[[nodiscard]] RemainingStopBudgetDTO remaining_stop_budget(
    const PreparedParticipantStopPlanDTO& plan, const StopId& stop_id,
    const StopGeneration stop_generation, const std::int64_t now,
    const std::int64_t outer_deadline_ns) {
  const auto remaining_outer = now >= outer_deadline_ns ? 0 : outer_deadline_ns - now;
  RemainingStopBudgetDTO budget{stop_id, stop_generation, remaining_outer, {}};
  budget.phases.reserve(plan.deadline_budget.phases.size());
  const auto elapsed = plan.deadline_budget.outer_duration_ns - remaining_outer;
  for (const auto& phase : plan.deadline_budget.phases) {
    const auto remaining =
        remaining_outer <= plan.deadline_budget.kernel_completion_reserve_ns
            ? 0
            : std::max<std::int64_t>(
                  0, std::min(phase.deadline_offset_ns - elapsed,
                              remaining_outer - plan.deadline_budget.kernel_completion_reserve_ns));
    budget.phases.push_back({phase.phase_id, remaining});
  }
  return budget;
}

[[nodiscard]] std::optional<std::int64_t> remaining_phase_offset(
    const RemainingStopBudgetDTO& budget, const StopBudgetPhaseId& phase_id) noexcept {
  const auto found = std::find_if(budget.phases.begin(), budget.phases.end(),
                                  [&](const auto& phase) { return phase.phase_id == phase_id; });
  return found == budget.phases.end() ? std::nullopt
                                      : std::optional(found->remaining_deadline_offset_ns);
}

[[nodiscard]] std::optional<std::int64_t> stage_watchdog_deadline(
    const PreparedStopStageCommandDTO& command, const RemainingStopBudgetDTO& budget,
    const std::int64_t now) noexcept {
  auto maximum =
      remaining_phase_offset(budget, command.resolved_stage.containment_command.command_phase_id);
  if (!maximum.has_value() || *maximum <= 0) {
    return std::nullopt;
  }
  for (const auto& evidence : command.resolved_stage.evidence_requirements) {
    const auto remaining = remaining_phase_offset(budget, evidence.requirement.phase_id);
    if (!remaining.has_value()) {
      return std::nullopt;
    }
    maximum = std::max(*maximum, *remaining);
  }
  return checked_deadline(now, *maximum);
}

[[nodiscard]] Result<ResolvedParticipantStopStageDTO> resolve_participant_stop_stage(
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

[[nodiscard]] StopRequestedSelectorSnapshotDTO requested_selector_snapshot(
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

[[nodiscard]] StopEscalationStageKind stage_kind(const PreparedParticipantStopPlanDTO& plan,
                                                 const std::size_t index) noexcept {
  return plan.stage_commands[index].resolved_stage.stage_kind;
}

[[nodiscard]] StopParticipantScopeFactsDTO participant_scope_facts(
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

[[nodiscard]] StopScopeEntryDTO participant_scope_entry(
    const StopParticipantWork& item, const StopId& stop_id, const StopGeneration stop_generation,
    const std::int64_t accepted_at_ns, const std::int64_t remaining_outer_duration_ns) {
  std::vector<StopAcceptedPhaseBudgetDTO> phases;
  phases.reserve(item.plan.deadline_budget.phases.size());
  const auto elapsed = item.plan.deadline_budget.outer_duration_ns - remaining_outer_duration_ns;
  for (const auto& phase : item.plan.deadline_budget.phases) {
    phases.push_back(
        StopAcceptedPhaseBudgetDTO{phase.phase_id, phase.deadline_offset_ns,
                                   std::max<std::int64_t>(0, phase.deadline_offset_ns - elapsed)});
  }
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
      accepted_at_ns,
      remaining_outer_duration_ns,
      std::move(phases)};
}

[[nodiscard]] bool stop_scope_entry_key_less(const StopScopeEntryKeyDTO& left,
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

[[nodiscard]] bool stop_surveillance_source_plan_less(
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

[[nodiscard]] Result<std::vector<StopSurveillanceSourcePlanDTO>> stop_surveillance_source_plans(
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

[[nodiscard]] bool stop_requires_surveillance(const StopCertificateDTO& certificate,
                                              const StopFaultLatchDTO& latch) noexcept {
  return certificate.result == StopCertificateResultDTO::kPartial ||
         certificate.result == StopCertificateResultDTO::kFailedUncontained ||
         latch.trigger == StopFaultTriggerDTO::kPhysicalRestUnproven;
}

[[nodiscard]] StopFaultTriggerDTO primary_fault_trigger(const StopCertificateDTO& certificate,
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

[[nodiscard]] std::string fault_trigger_reason(const StopFaultTriggerDTO trigger) {
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
  }
  return "stop_fault";
}

[[nodiscard]] int fault_trigger_priority(const StopFaultTriggerDTO trigger) noexcept {
  switch (trigger) {
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
[[nodiscard]] std::pair<std::vector<OperationId>, std::vector<ResourceId>> fault_latch_capture(
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

[[nodiscard]] bool terminal_fault_required(const StopCertificateDTO& certificate,
                                           const bool persistence_degraded) noexcept {
  return persistence_degraded ||
         certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull ||
         certificate.result != StopCertificateResultDTO::kSucceededNominal;
}

[[nodiscard]] StopFaultLatchDTO persistence_fault_latch(
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
      {}};
}

}  // namespace

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

  void Close() noexcept {
    accepting_.store(false, std::memory_order_release);
    Wake();
    NotifyDrainState();
  }

  [[nodiscard]] bool DrainFor(const std::chrono::nanoseconds timeout) noexcept {
    try {
      std::unique_lock lock(drain_mutex_);
      return drain_changed_.wait_for(lock, timeout, [this] {
        if (callback_in_flight_.load(std::memory_order_acquire)) {
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
  std::atomic_size_t api_calls_in_flight_{};
  std::atomic_uint64_t wake_sequence_{};
  std::mutex drain_mutex_;
  std::condition_variable drain_changed_;
  std::jthread worker_;
};

ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationId& operation_id) noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

const ResourceAddressedExecutionKernel::ActiveOperation*
ResourceAddressedExecutionKernel::find_active_operation_locked(
    const OperationId& operation_id) const noexcept {
  const auto found = active_operations_.find(operation_id);
  return found == active_operations_.end() ? nullptr : found->second.get();
}

std::vector<OperationId> ResourceAddressedExecutionKernel::durable_operation_retention_ids_locked()
    const {
  std::vector<ResourceId> unresolved_resources;
  for (const auto& slot : resource_slots_->snapshot()) {
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
      unresolved_resources.push_back(slot.resource_id);
    }
  }
  std::sort(unresolved_resources.begin(), unresolved_resources.end());

  std::vector<OperationId> retained;
  retained.reserve(active_operations_.size() + historical_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    (void)operation;
    retained.push_back(operation_id);
  }
  if (manual_operation_claim_.has_value()) {
    retained.emplace_back(manual_operation_claim_->intent.owner_transaction_id);
  }
  for (const auto& operation : historical_operations_) {
    const bool replaced_before_settlement = std::any_of(
        operation.participants.begin(), operation.participants.end(), [](const auto& participant) {
          return participant.terminal_error.has_value() &&
                 participant.terminal_error->code == "execution_host_replaced";
        });
    const bool references_unresolved_resource = std::any_of(
        operation.participants.begin(), operation.participants.end(), [&](const auto& participant) {
          return std::any_of(participant.resources.begin(), participant.resources.end(),
                             [&](const auto& resource) {
                               return std::binary_search(unresolved_resources.begin(),
                                                         unresolved_resources.end(), resource);
                             });
        });
    if (replaced_before_settlement && references_unresolved_resource) {
      retained.push_back(operation.operation_id);
    }
  }
  std::sort(retained.begin(), retained.end());
  retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
  return retained;
}

bool ResourceAddressedExecutionKernel::evict_oldest_historical_operation_locked(
    const std::span<const OperationId> protected_operation_ids) {
  const auto compactable = std::find_if(
      historical_operations_.begin(), historical_operations_.end(), [&](const auto& operation) {
        return !std::binary_search(protected_operation_ids.begin(), protected_operation_ids.end(),
                                   operation.operation_id);
      });
  if (compactable == historical_operations_.end()) {
    return false;
  }
  const auto retired_id = compactable->operation_id;
  historical_operations_.erase(compactable);
  std::erase_if(replay_ledger_, [&](const ReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  std::erase_if(control_replay_ledger_, [&](const ControlReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  std::erase_if(runtime_scale_replay_ledger_, [&](const RuntimeScaleReplayEntry& entry) {
    return entry.accepted.operation_id == retired_id;
  });
  std::erase_if(operation_recovery_replay_ledger_, [&](const OperationRecoveryReplayEntry& entry) {
    return entry.operation_id == retired_id;
  });
  return true;
}

bool ResourceAddressedExecutionKernel::make_operation_admission_capacity_locked() {
  const auto protected_operation_ids = durable_operation_retention_ids_locked();
  while (!historical_operations_.empty() &&
         active_operations_.size() + historical_operations_.size() >=
             config_.request_replay_capacity) {
    if (!evict_oldest_historical_operation_locked(protected_operation_ids)) {
      return false;
    }
  }
  return active_operations_.size() + historical_operations_.size() <
         config_.request_replay_capacity;
}

void ResourceAddressedExecutionKernel::retain_historical_operation_locked(OperationStateDTO state) {
  // Terminal operation detail is diagnostic history, not execution
  // authority. Keep it and the latest state-conditioned correlations bounded
  // together. Active/uncertain operations remain in active_operations_ and
  // are therefore never selected for this compaction.
  const auto protected_operation_ids = durable_operation_retention_ids_locked();
  while (!historical_operations_.empty() &&
         active_operations_.size() + historical_operations_.size() >
             config_.request_replay_capacity) {
    if (!evict_oldest_historical_operation_locked(protected_operation_ids)) {
      break;
    }
  }
  historical_operations_.push_back(std::move(state));
}

StopScopeResolverSnapshotDTO ResourceAddressedExecutionKernel::capture_stop_scope_locked(
    const ResourceSlotTableSnapshotDTO& slots) const {
  StopScopeResolverSnapshotDTO snapshot{config_.request_owner_epoch,
                                        *stop_graph_revision_,
                                        slots.operation_resource_generation,
                                        cell_stop_generation_,
                                        {},
                                        stop_component_scope_,
                                        {},
                                        {}};
  snapshot.operations.reserve(active_operations_.size() + historical_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    const bool effect_uncertain = operation->containment_effect_uncertain();
    std::vector<ControllableComponentId> components;
    std::vector<ResourceId> resources;
    components.reserve(operation->participants.size());
    for (const auto& participant : operation->participants) {
      components.push_back(participant.binding.value().controllable_component_id);
      resources.insert(resources.end(), participant.resources.begin(), participant.resources.end());
    }
    std::sort(components.begin(), components.end());
    components.erase(std::unique(components.begin(), components.end()), components.end());
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    snapshot.operations.push_back(StopScopeOperationSnapshotDTO{
        operation_id, operation->state.program_run_id, operation->state.operation_revision,
        operation->state.active_stop_generation.value_or(StopGeneration(0U)),
        operation->state.state, true, effect_uncertain, std::move(components), std::move(resources),
        operation->manual_handover
            ? std::optional<ProgramRunId>{operation->manual_handover->capture.program_run_id}
            : std::nullopt});
  }
  std::set<ProgramRunId> unresolved_historical_runs;
  std::vector<ResourceId> unresolved_resources;
  for (const auto& slot : slots.slots) {
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal)
      unresolved_resources.push_back(slot.resource_id);
  }
  std::sort(unresolved_resources.begin(), unresolved_resources.end());
  for (const auto& operation : historical_operations_) {
    if (active_operations_.contains(operation.operation_id)) {
      continue;
    }
    const bool unresolved_predecessor = std::any_of(
        operation.participants.begin(), operation.participants.end(), [&](const auto& participant) {
          return std::any_of(participant.resources.begin(), participant.resources.end(),
                             [&](const auto& resource) {
                               return std::binary_search(unresolved_resources.begin(),
                                                         unresolved_resources.end(), resource);
                             });
        });
    if (unresolved_predecessor) {
      // This historical terminal kind is not terminal physical evidence.
      // Leave the selector unknown so the existing resolver captures the full
      // orphan-resource scope, not a nonresident operation it cannot fence.
      unresolved_historical_runs.insert(operation.program_run_id);
      continue;
    }
    snapshot.operations.push_back(
        StopScopeOperationSnapshotDTO{operation.operation_id,
                                      operation.program_run_id,
                                      operation.operation_revision,
                                      operation.active_stop_generation.value_or(StopGeneration(0U)),
                                      operation.state,
                                      false,
                                      false,
                                      {},
                                      {}});
  }
  std::sort(
      snapshot.operations.begin(), snapshot.operations.end(),
      [](const auto& left, const auto& right) { return left.operation_id < right.operation_id; });

  snapshot.known_program_runs.reserve(program_run_stop_fences_.size() + 3U);
  if (current_program_run_id_.has_value()) {
    snapshot.known_program_runs.push_back(*current_program_run_id_);
  }
  if (program_cycle_claim_run_.has_value()) {
    snapshot.known_program_runs.push_back(program_cycle_claim_run_->program_run_id);
    if (!unresolved_resources.empty()) {
      // The cell-wide recovered claim may precede every operation record.
      // Orphan inventory still prevents certifying that run as empty/rested.
      unresolved_historical_runs.insert(program_cycle_claim_run_->program_run_id);
    }
  }
  if (pending_program_run_admission_.has_value()) {
    snapshot.known_program_runs.push_back(*pending_program_run_admission_);
  }
  for (const auto& fence : program_run_stop_fences_) {
    snapshot.known_program_runs.push_back(fence.program_run_id);
  }
  std::sort(snapshot.known_program_runs.begin(), snapshot.known_program_runs.end());
  snapshot.known_program_runs.erase(
      std::unique(snapshot.known_program_runs.begin(), snapshot.known_program_runs.end()),
      snapshot.known_program_runs.end());
  std::erase_if(snapshot.known_program_runs,
                [&](const auto& run) { return unresolved_historical_runs.contains(run); });
  // A settled sibling in the same retired run cannot turn an unresolved run
  // selector into a successful empty capture.
  std::erase_if(snapshot.operations, [&](const auto& operation) {
    return !active_operations_.contains(operation.operation_id) &&
           unresolved_historical_runs.contains(operation.program_run_id);
  });

  snapshot.resources.reserve(slots.slots.size());
  for (const auto& slot : slots.slots) {
    bool effect_uncertain = slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal;
    if (slot.owner.has_value()) {
      const auto* operation = find_active_operation_locked(slot.owner->operation_id);
      effect_uncertain =
          effect_uncertain || operation == nullptr || operation->containment_effect_uncertain();
    }
    snapshot.resources.push_back(
        StopScopeResourceSnapshotDTO{slot.resource_id, slot.stop_generation, effect_uncertain});
  }
  return snapshot;
}

std::optional<OperationId> ResourceAddressedExecutionKernel::find_event_operation_locked(
    const IResolvedParticipantExecutionEngine* engine, const std::uint64_t subordinate_id) const {
  if (engine == nullptr || subordinate_id == 0U) {
    return std::nullopt;
  }
  std::optional<OperationId> result;
  for (const auto& [operation_id, operation] : active_operations_) {
    const auto* participant = operation->participant_for(engine);
    if (participant == nullptr || participant->subordinate_id != subordinate_id) {
      continue;
    }
    if (result.has_value()) {
      // Admission prevents one engine instance from owning two operations.
      // Fail closed if corrupted state ever violates that invariant.
      return std::nullopt;
    }
    result = operation_id;
  }
  return result;
}

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

ResourceAddressedExecutionKernel::ResourceAddressedExecutionKernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
    IScheduler& scheduler, IOperationJournal& journal,
    ResourceAddressedExecutionKernelConfig config)
    : registry_(extract_bindings(registrations)),
      admission_mutex_(config.operation_admission_gate != nullptr
                           ? config.operation_admission_gate->shared_admission_mutex()
                           : std::make_shared<std::recursive_mutex>()),
      resource_slots_(std::make_shared<ResourceSlotTable>(
          registrations.empty() ? config.empty_runtime_resource_inventory
                                : registry_.resource_inventory(),
          extract_published_active_control_configurations(registrations), admission_mutex_)),
      registrations_(std::move(registrations)),
      clock_(clock),
      scheduler_(scheduler),
      journal_(journal),
      config_(std::move(config)),
      engine_event_ingress_(std::make_shared<EngineEventIngress>()),
      emergency_stop_reserve_(config_.emergency_stop_correlation_capacity),
      stop_transactions_(config_.emergency_stop_correlation_capacity + 1U),
      stop_correlations_(config_.emergency_stop_correlation_capacity + 1U),
      stop_projection_jobs_(config_.stop_forensic_projection_capacity == 0U
                                ? 2U * (config_.emergency_stop_correlation_capacity + 1U) +
                                      kMaximumResidentStopTerminalRecords
                                : config_.stop_forensic_projection_capacity),
      stop_projection_generations_(stop_projection_jobs_.size()) {
  if (!config_.empty_runtime_resource_inventory.empty() &&
      (!registrations_.empty() || !config_.empty_runtime_retained_bundle_id.has_value() ||
       !config_.empty_runtime_graph_revision.has_value() ||
       !std::is_sorted(config_.empty_runtime_resource_inventory.begin(),
                       config_.empty_runtime_resource_inventory.end()) ||
       std::adjacent_find(config_.empty_runtime_resource_inventory.begin(),
                          config_.empty_runtime_resource_inventory.end()) !=
           config_.empty_runtime_resource_inventory.end())) {
    throw std::invalid_argument(
        "bootstrap resource inventory lacks one exact empty-runtime product");
  }
  if (config_.operator_alarm_attention.has_value() &&
      (config_.operator_alarm_attention->maximum_snapshot_age_ns <= 0 ||
       config_.operator_alarm_attention->host_boot_id.empty())) {
    throw std::invalid_argument(
        "operator-alarm attention policy needs a positive age bound and the host boot id");
  }
  if (config_.request_owner_epoch.empty() || config_.cell_id.empty() ||
      config_.request_replay_capacity == 0U ||
      config_.request_replay_capacity > kMaxI6OperationReplayEntries ||
      config_.cell_admission_claim_capacity == 0U ||
      config_.cell_admission_claim_capacity > kMaximumCellAdmissionClaims) {
    throw std::invalid_argument(
        "addressed Kernel request-owner epoch, cell identity, or replay bound is invalid");
  }
  if (config_.operation_admission_gate != nullptr &&
      !resource_slots_->shares_admission_mutex(
          config_.operation_admission_gate->shared_admission_mutex())) {
    throw std::invalid_argument(
        "addressed Kernel admission gate does not share its resource mutex");
  }
  const auto maximum_projection_capacity =
      2U * (config_.emergency_stop_correlation_capacity + 1U) + kMaximumResidentStopTerminalRecords;
  if (stop_projection_jobs_.empty() || stop_projection_jobs_.size() > maximum_projection_capacity) {
    throw std::invalid_argument("addressed Kernel forensic projection bound is invalid");
  }
  if (config_.stop_surveillance.has_value()) {
    const auto& surveillance = *config_.stop_surveillance;
    if (surveillance.observer == nullptr || surveillance.policy.sample_cadence_ns <= 0 ||
        surveillance.policy.checkpoint_interval_ns < surveillance.policy.sample_cadence_ns ||
        surveillance.policy.retained_checkpoint_limit == 0U ||
        surveillance.policy.retained_checkpoint_limit > kMaximumStopSurveillanceCheckpoints) {
      throw std::invalid_argument("addressed Kernel Stop-surveillance owner is invalid");
    }
  }
  auto stop_facts = derive_stop_static_facts(registrations_, config_);
  stop_retained_bundle_id_ = std::move(stop_facts.retained_bundle_id);
  stop_graph_revision_ = std::move(stop_facts.graph_revision);
  cell_stop_deadline_budget_ = std::move(stop_facts.deadline_budget);
  stop_component_scope_ = std::move(stop_facts.components);
  for (const auto& configuration : extract_active_control_configurations(registrations_)) {
    if (!domain_has_complete_stop_readiness(registrations_, configuration.control_domain_id)) {
      runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
    }
  }
  replay_ledger_.reserve(config_.request_replay_capacity);
  control_replay_ledger_.reserve(config_.request_replay_capacity);
  stop_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_control_replay_ledger_.reserve(config_.request_replay_capacity);
  runtime_scale_control_settings_.reserve(registrations_.size());
  stop_fault_recovery_replay_ledger_.reserve(config_.request_replay_capacity);
  operation_recovery_replay_ledger_.reserve(config_.request_replay_capacity);
  program_run_stop_fences_.reserve(config_.request_replay_capacity);
  stop_terminal_history_.reserve(kMaximumResidentStopTerminalRecords);
  for (std::size_t index = 0U; index < stop_transactions_.size(); ++index) {
    stop_transactions_[index] = std::make_unique<StopTransaction>();
  }
  for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
    stop_projection_jobs_[index] = std::make_unique<StopProjectionJob>();
  }
  stop_projection_worker_ =
      std::make_unique<StopProjectionWorker>(stop_projection_jobs_.size(), *this);
  stop_control_executor_ = std::make_unique<BoundedStopControlExecutor>(
      emergency_stop_reserve_.correlation_capacity() + 1U, clock_,
      static_cast<IStopControlWorkReceiver&>(*this));
  if (!stop_control_executor_->prequalified_ready() ||
      stop_control_executor_->capacity() != emergency_stop_reserve_.correlation_capacity() + 1U) {
    throw std::runtime_error("StopControl executor failed its reserve-capacity admission join");
  }
  const auto initially_qualified_participants = qualified_execution_engine_count(registrations_);
  prequalified_max_live_stop_participants_ =
      config_.prequalified_max_live_stop_participants == 0U
          ? std::max<std::size_t>(1U, initially_qualified_participants)
          : config_.prequalified_max_live_stop_participants;
  if (initially_qualified_participants > prequalified_max_live_stop_participants_ ||
      prequalified_max_live_stop_participants_ >
          kMaximumPrequalifiedStopStageAttemptLanes / kMaxStopEscalationStages) {
    throw std::invalid_argument(
        "Stop-qualified runtime participants exceed the prequalified attempt-isolation bound");
  }
  stop_stage_attempt_executor_ = std::make_unique<BoundedStopStageAttemptExecutor>(
      prequalified_max_live_stop_participants_ * kMaxStopEscalationStages,
      prequalified_max_live_stop_participants_,
      static_cast<IStopStageAttemptCompletionReceiver&>(*this));
  if (!stop_stage_attempt_executor_->prequalified_ready() ||
      stop_stage_attempt_executor_->prequalified_max_live_participants() !=
          prequalified_max_live_stop_participants_) {
    throw std::runtime_error("Stop-stage attempt isolation failed its admission prequalification");
  }
  auto operation_control_engines = distinct_execution_engines(registrations_);
  if (!operation_control_engines.empty()) {
    operation_control_executor_ = std::make_unique<BoundedOperationControlExecutor>(
        std::move(operation_control_engines),
        static_cast<IOperationControlEffectCompletionReceiver&>(*this));
    if (!operation_control_executor_->prequalified_ready()) {
      throw std::runtime_error("operation-control isolation failed its admission prequalification");
    }
  }

  std::vector<ResourceId> recovery_inventory;
  for (const auto& slot : resource_slots_->snapshot())
    recovery_inventory.push_back(slot.resource_id);
  auto recovery = journal_.BeginOwnerEpoch(config_.request_owner_epoch, clock_.now(),
                                           config_.request_replay_capacity, recovery_inventory);
  if (!recovery.ok()) {
    throw std::runtime_error("addressed Kernel durable owner admission failed: " +
                             recovery.error().code + ": " + recovery.error().message);
  }
  historical_operations_ = std::move(recovery.value().historical_operations);
  cell_admission_owner_ = std::make_unique<KernelCellAdmissionOwner>(
      config_.request_owner_epoch,
      std::min(config_.request_replay_capacity, config_.cell_admission_claim_capacity), clock_,
      journal_, std::move(recovery.value().recovered_cell_admission_claims),
      recovery.value().cell_admission_generation_high_water,
      config_.cell_admission_intervention_validator,
      [this](const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release) {
        if (claim.intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation) {
          return validate_manual_operation_settlement(claim, release);
        }
        return config_.cell_admission_settlement_validator
                   ? config_.cell_admission_settlement_validator(claim, release)
                   : reject<void>("settlement owner is unavailable",
                                  "settlement_owner_reader_unavailable");
      },
      [this](const std::vector<CellAdmissionClaimDTO>& held,
             const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current,
             const CellAdmissionReconcileDTO& request) {
        const auto predecessor = std::find_if(current.begin(), current.end(), [](const auto& row) {
          return row.owner_role == CellAdmissionOwnerRoleDTO::kManualOperationOwner;
        });
        const auto successor = std::find_if(
            request.current_owner_epochs.begin(), request.current_owner_epochs.end(),
            [](const auto& row) {
              return row.owner_role == CellAdmissionOwnerRoleDTO::kManualOperationOwner;
            });
        if (predecessor != current.end() &&
            (successor == request.current_owner_epochs.end() || *successor != *predecessor)) {
          return validate_manual_owner_reconciliation(held, current, request);
        }
        return config_.cell_admission_reconcile_validator
                   ? config_.cell_admission_reconcile_validator(held, current, request)
                   : reject<CellAdmissionReconcileValidationDTO>(
                         "reconciliation owner is unavailable",
                         "reconciliation_owner_reader_unavailable");
      },
      std::move(recovery.value().recovered_cell_admission_membership),
      std::move(recovery.value().recovered_cell_admission_current_owners),
      config_.manager_liveness_required_set, recovery.value().cell_claim_generation_high_water,
      [this](const ProgramNodeAdmissionEvidenceDTO& evidence) {
        return validate_program_node_claim(evidence);
      },
      [this] {
        std::lock_guard state_lock(state_mutex_);
        return is_active_stop_fault_latch(active_stop_fault_latch_);
      });
  refresh_execution_claims();  // construction is single-threaded
  if (recovery.value().recovered_stops.size() > kMaximumResidentStopTerminalRecords) {
    throw std::runtime_error("recovered Stop terminal history exceeds its fixed bound");
  }
  for (const auto& recovered : recovery.value().recovered_stops) {
    cell_stop_generation_ = std::max(cell_stop_generation_, recovered.accepted.stop_generation);
    if (!recovered.terminal_certificate.has_value()) {
      // An accepted Stop with no projected terminal certificate is never
      // reconstructed or redispatched. BeginOwnerEpoch supplies the complete
      // current inventory fence for an unclosed predecessor; keep motion
      // admission closed until the separate reconciliation owner clears it.
      motion_admission_unreconciled_ = true;
      continue;
    }
    const auto& certificate = *recovered.terminal_certificate;
    const auto terminal_state = [&] {
      switch (certificate.result) {
        case StopCertificateResultDTO::kSucceededNominal:
        case StopCertificateResultDTO::kSucceededEscalated:
          return StopStatusStateDTO::kSucceeded;
        case StopCertificateResultDTO::kPartial:
          return StopStatusStateDTO::kPartial;
        case StopCertificateResultDTO::kFailedUncontained:
          return StopStatusStateDTO::kFailed;
      }
      return StopStatusStateDTO::kFailed;
    }();
    const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                          certificate.stop_certificate_revision,
                                          certificate.stop_id, certificate.stop_generation};
    StopStatusDTO status{
        recovered.accepted,
        terminal_state,
        certificate.scope_entries,
        {},
        certificate.attempted_stage_results,
        certificate.evidence_items,
        certificate.unsettled,
        certificate.failed,
        StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                       std::nullopt},
        StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                       std::nullopt},
        recovered.surveillance.has_value()
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                             std::nullopt, std::nullopt},
        reference,
        recovered.fault_latch,
        recovered.surveillance};
    if (stop_terminal_projection_field_error(status, certificate, recovered.fault_latch)
            .has_value()) {
      throw std::runtime_error("recovered Stop terminal projection is invalid");
    }
    std::unique_ptr<StopSurveillanceReducerState> recovered_surveillance_state;
    std::int64_t next_surveillance_sample_ns = 0;
    std::int64_t next_surveillance_checkpoint_ns = 0;
    if (recovered.surveillance.has_value()) {
      if (!config_.stop_surveillance.has_value() || !recovered.fault_latch.has_value()) {
        motion_admission_closed_ = true;
      } else {
        auto source_plans = stop_surveillance_source_plans(certificate);
        if (!source_plans.ok()) {
          throw std::runtime_error("recovered Stop surveillance sources are unavailable");
        }
        auto adopted = AdoptStopSurveillanceReducerState(
            config_.request_owner_epoch, certificate, *recovered.fault_latch,
            *recovered.surveillance, std::move(source_plans.value()));
        if (!adopted.ok()) {
          throw std::runtime_error("recovered Stop surveillance checkpoint is invalid");
        }
        recovered_surveillance_state =
            std::make_unique<StopSurveillanceReducerState>(std::move(adopted.value()));
        const auto now = clock_.now();
        const auto& policy = config_.stop_surveillance->policy;
        if (now > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns ||
            now > std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
          throw std::runtime_error("recovered Stop surveillance deadline is exhausted");
        }
        next_surveillance_sample_ns = now + policy.sample_cadence_ns;
        next_surveillance_checkpoint_ns = now + policy.checkpoint_interval_ns;
      }
    }
    stop_terminal_history_.push_back(std::make_unique<StopTerminalRecord>(StopTerminalRecord{
        std::move(status), certificate, recovered.fault_latch, std::nullopt,
        std::move(recovered_surveillance_state), std::nullopt, 0U, false, std::nullopt,
        next_surveillance_sample_ns, next_surveillance_checkpoint_ns}));
    if (recovered.fault_latch.has_value()) {
      cell_stop_fault_latch_generation_ =
          std::max(cell_stop_fault_latch_generation_, recovered.fault_latch->latch_generation);
      if (recovered.fault_latch->state != StopFaultLatchStateDTO::kCleared &&
          (!active_stop_fault_latch_.has_value() ||
           active_stop_fault_latch_->latch_generation < recovered.fault_latch->latch_generation)) {
        active_stop_fault_latch_ = recovered.fault_latch;
        motion_admission_closed_ = true;
      }
    }
    if (recovered.surveillance.has_value() &&
        recovered.surveillance->state == StopPostTerminalSurveillanceStateDTO::kMonitoring) {
      motion_admission_closed_ = true;
    }
  }
  if (recovery.value().replaced_unclosed_owner_epoch.has_value()) {
    motion_admission_unreconciled_ = true;
  }
  if (!recovery.value().unreconciled_resources.empty()) {
    motion_admission_unreconciled_ = true;
    auto marked = resource_slots_->mark_unreconciled_external_or_orphan_goal(
        recovery.value().unreconciled_resources);
    if (!marked.ok()) {
      throw std::runtime_error("addressed Kernel durable orphan recovery failed: " +
                               marked.error().code + ": " + marked.error().message);
    }
  }

  for (const auto& record : stop_terminal_history_) {
    if (record != nullptr && record->surveillance_state != nullptr &&
        record->surveillance_state->projection.state ==
            StopPostTerminalSurveillanceStateDTO::kMonitoring) {
      ScheduleStopSurveillance(record->certificate.stop_certificate_id,
                               record->next_surveillance_sample_ns);
    }
  }
  try {
    for (const auto& registration : registrations_) {
      if (std::any_of(engine_subscriptions_.begin(), engine_subscriptions_.end(),
                      [&](const EngineSubscription& item) {
                        return item.engine == registration.engine;
                      })) {
        continue;
      }
      auto* engine = registration.engine;
      auto ingress = engine_event_ingress_;
      const auto handle = engine->subscribe_events(
          [ingress = std::move(ingress), engine](const KernelEvent& event) {
            auto entered = ingress->Enter();
            if (auto* owner = entered.owner()) owner->on_engine_event(engine, event);
          });
      engine_subscriptions_.push_back(EngineSubscription{engine, handle});
    }
    // Opening is the final construction step. A callback captured while the
    // constructor was still assembling subscriptions observes closed ingress
    // rather than a partially constructed Kernel.
    engine_event_ingress_->Open(*this);
  } catch (...) {
    for (const auto& subscription : engine_subscriptions_) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    engine_subscriptions_.clear();
    throw;
  }
}

ResourceAddressedExecutionKernel::~ResourceAddressedExecutionKernel() {
  // Engine callbacks are the outermost asynchronous ingress. Closing the
  // guard first prevents a callback already copied by a publisher from
  // acquiring the Kernel. Unsubscribe removes future snapshots, and Drain
  // waits for callbacks that entered before Close while all state, scheduled
  // surveillance, and worker lanes they may reach are still alive and open.
  engine_event_ingress_->Close();
  for (const auto& subscription : engine_subscriptions_) {
    subscription.engine->unsubscribe_events(subscription.handle);
  }
  engine_subscriptions_.clear();
  engine_event_ingress_->Drain();

  std::vector<ScheduleHandle> surveillance_handles;
  if (config_.stop_surveillance.has_value()) {
    {
      std::unique_lock state_lock(state_mutex_);
      stop_surveillance_shutting_down_ = true;
      if (!stop_surveillance_schedule_cv_.wait_for(state_lock, std::chrono::seconds(5), [&] {
            return stop_surveillance_schedule_publications_ == 0U;
          })) {
        std::terminate();
      }
      for (const auto& record : stop_terminal_history_) {
        if (record != nullptr && record->surveillance_schedule_handle.has_value()) {
          surveillance_handles.push_back(*record->surveillance_schedule_handle);
          record->surveillance_schedule_handle.reset();
          record->surveillance_callback_pending = false;
        }
      }
    }
    for (const auto handle : surveillance_handles) {
      try {
        scheduler_.cancel(handle);
      } catch (...) {
        std::terminate();
      }
    }
    try {
      if (!scheduler_.drain_for(SchedulerPriority::Monitoring, std::chrono::seconds(5))) {
        std::terminate();
      }
    } catch (...) {
      std::terminate();
    }
  }
  // With participant events drained, each lane can close its own ingress and
  // settle all causally accepted work before its worker object is retired.
  if (operation_control_executor_ != nullptr) {
    operation_control_executor_->CloseIngress();
    if (!operation_control_executor_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
  }
  if (stop_stage_attempt_executor_ != nullptr) {
    stop_stage_attempt_executor_->CloseIngress();
    if (!stop_stage_attempt_executor_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
  }
  if (stop_control_executor_ != nullptr) {
    // Attempt completion callbacks remain allowed to rearm/publish their
    // owning StopControl slot until every isolated provider call has drained.
    // Drain those causal wakeups before closing StopControl ingress.
    if (!stop_control_executor_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
    stop_control_executor_->CloseIngress();
  }
  if (stop_projection_worker_ != nullptr) {
    // Accepted Stop projections settle here, while the journal, clock,
    // scheduler and every participant engine are still alive.
    stop_projection_worker_->Close();
    if (!stop_projection_worker_->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
  }
  // Only now may the drained lanes themselves be destroyed. Nothing can reach
  // them afterwards, so a lane cannot publish into the journal below.
  operation_control_executor_.reset();
  stop_control_executor_.reset();
  stop_stage_attempt_executor_.reset();
  stop_projection_worker_.reset();
  // The durable journal is the final authority for clean-close eligibility.
  // At this point every Kernel-owned asynchronous lane has drained, so it can
  // atomically prove that all operations and Stop projections are terminal.
  // A rejection or storage failure deliberately leaves the epoch unclosed;
  // the next owner then performs conservative complete-inventory recovery.
  bool orphan_reconciliation_complete = false;
  {
    std::lock_guard state_lock(state_mutex_);
    orphan_reconciliation_complete = !motion_admission_unreconciled_;
  }
  if (orphan_reconciliation_complete) {
    try {
      static_cast<void>(journal_.CloseOwnerEpoch(config_.request_owner_epoch, clock_.now()));
    } catch (...) {
    }
  }
}

AuthoringAdmissionDTO ResourceAddressedExecutionKernel::GetAuthoringAdmission() const {
  return cell_admission_owner_->GetAuthoringAdmission();
}

bool ResourceAddressedExecutionKernel::has_external_route_recovery_claims() const {
  return cell_admission_owner_->has_unresolved_claims_except(
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner);
}

Result<void> ResourceAddressedExecutionKernel::SetRouteRecoveryPending(bool pending) {
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->SetRouteRecoveryPending(pending);
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::UpdateCellAdmissionReadiness(
    const CellAdmissionReadinessSnapshotDTO& snapshot) {
  std::lock_guard admission_lock(*admission_mutex_);
  auto updated = cell_admission_owner_->UpdateCellAdmissionReadiness(snapshot);
  if (updated.ok()) {
    std::lock_guard state_lock(state_mutex_);
    current_program_run_id_ =
        snapshot.program_cycle_status_current && snapshot.program_cycle_status.has_value()
            ? snapshot.program_cycle_status->program_run_id
            : std::nullopt;
  }
  return updated;
}

Result<CellAdmissionClaimDTO> ResourceAddressedExecutionKernel::AcquireCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent) {
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
      intent.owner_role == CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
    return reject<CellAdmissionClaimDTO>(
        "CCLC recovery requires exact Kernel predecessor verification",
        "cclc_route_recovery_requires_owner_reconciliation", ErrorCode::kInvalidRequest);
  }
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kManualOperation) {
    return reject<CellAdmissionClaimDTO>(
        "manual claims are minted only by exact Kernel operation admission",
        "manual_operation_claim_requires_execute", ErrorCode::kInvalidRequest);
  }
  // A lifecycle claim is a whole-cell configuration mutation. Serialize the
  // physical work check with every command submission, then retain the
  // admission lock through the durable claim so Start cannot win in between.
  std::unique_lock<std::mutex> submit_lock(submission_mutex_, std::defer_lock);
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle) {
    submit_lock.lock();
  }
  std::lock_guard admission_lock(*admission_mutex_);
  if (intent.claim_kind == CellAdmissionClaimKindDTO::kComponentLifecycle) {
    std::lock_guard state_lock(state_mutex_);
    // Every blocking condition and the identities it concerns, so a caller
    // can tell exactly what must settle; nothing is retried or stopped.
    std::vector<std::string> blocking;
    if (pending_program_run_admission_.has_value()) blocking.emplace_back("program_run_admission_pending");
    if (!active_operations_.empty()) blocking.emplace_back("operations_active");
    if (operation_control_reconfiguration_busy_locked()) blocking.emplace_back("operation_control_busy");
    if (motion_admission_unreconciled_) blocking.emplace_back("motion_admission_unreconciled");
    if (manager_liveness_motion_inhibited_) blocking.emplace_back("manager_liveness_inhibited");
    std::vector<std::string> busy_resources;
    for (const auto& slot : resource_slots_->snapshot()) {
      if (slot.state != ResourceSlotState::kFree || slot.owner.has_value()) {
        busy_resources.push_back(slot.resource_id.value());
      }
    }
    if (!busy_resources.empty()) blocking.emplace_back("resources_not_free");
    if (!blocking.empty()) {
      const auto join = [](const auto& values) {
        std::string joined;
        for (std::size_t index = 0U; index < values.size() && index < 16U; ++index) {
          if (!joined.empty()) joined += ',';
          joined += values[index];
        }
        return joined;
      };
      std::vector<std::string> operations;
      for (const auto& [operation_id, operation] : active_operations_) {
        static_cast<void>(operation);
        operations.push_back(operation_id.value());
      }
      auto rejected = reject<CellAdmissionClaimDTO>(
          "lifecycle mutation requires settled physical and Program work",
          blocking.size() == 1U && !busy_resources.empty()
              ? "component_lifecycle_resources_not_settled"
              : "component_lifecycle_effects_not_settled");
      auto error = rejected.error();
      error.details["blocking_conditions"] = join(blocking);
      if (!operations.empty()) error.details["active_operation_ids"] = join(operations);
      if (!busy_resources.empty()) error.details["busy_resource_ids"] = join(busy_resources);
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
  }
  auto acquired = cell_admission_owner_->AcquireCellAdmissionClaim(intent);
  if (acquired.ok() && intent.claim_kind == CellAdmissionClaimKindDTO::kProgramCycle) {
    refresh_execution_claims();
  }
  return acquired;
}

Result<CellAdmissionClaimDTO>
ResourceAddressedExecutionKernel::AcquireRouteRecoveryCellAdmissionClaim(
    const CellAdmissionClaimIntentDTO& intent, const ActiveControlConfigurationDTO& predecessor) {
  if (!predecessor.locally_valid() ||
      predecessor.state != ActiveControlConfigurationStateDTO::kReconciled) {
    return reject<CellAdmissionClaimDTO>("route recovery predecessor is invalid",
                                         "route_recovery_predecessor_invalid",
                                         ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->AcquireRouteRecoveryClaim(
      intent, [&](const auto& offered, const auto& parent) -> Result<void> {
        std::lock_guard state_lock(state_mutex_);
        const auto found =
            runtime_route_configuration_readbacks_.find(predecessor.control_domain_id);
        if (parent ||
            offered.scope.configurations !=
                std::vector<CellAdmissionConfigurationScopeDTO>{
                    {predecessor.control_domain_id, predecessor.active_control_configuration_id,
                     predecessor.active_control_configuration_generation}} ||
            found == runtime_route_configuration_readbacks_.end() ||
            found->second.active_configuration != predecessor ||
            found->second.operation_admission_open || !found->second.readiness_rows.empty() ||
            !runtime_route_configuration_publication_required_.contains(
                predecessor.control_domain_id)) {
          return reject<void>("route recovery has no exact admission-closed adopted predecessor",
                              "route_recovery_predecessor_not_current");
        }
        const bool active_stop_fault = is_active_stop_fault_latch(active_stop_fault_latch_);
        if ((!active_stop_fault && motion_admission_closed_) || motion_admission_unreconciled_ ||
            manager_liveness_motion_inhibited_ || !active_operations_.empty() ||
            operation_control_reconfiguration_busy_locked()) {
          return reject<void>("route recovery conflicts with resident or unresolved work",
                              "route_recovery_effects_not_settled");
        }
        const auto slots = resource_slots_->snapshot();
        if (std::any_of(slots.begin(), slots.end(), [](const auto& slot) {
              return slot.state != ResourceSlotState::kFree || slot.owner.has_value();
            })) {
          return reject<void>("route recovery has reserved or unknown physical effects",
                              "route_recovery_resources_not_settled");
        }
        std::optional<RuntimeEpoch> replacement_epoch;
        for (const auto& registration : registrations_) {
          const auto& path = registration.binding.value().control_path;
          if (!path || path->control_domain_id != predecessor.control_domain_id) continue;
          if (registration.stop_readiness ||
              path->provider_runtime_unit_id != predecessor.source_runtime_unit_id ||
              path->provider_runtime_epoch == predecessor.source_runtime_epoch ||
              (replacement_epoch && *replacement_epoch != path->provider_runtime_epoch)) {
            return reject<void>("route recovery has no unique replacement runtime epoch",
                                "route_recovery_epoch_not_current");
          }
          replacement_epoch = path->provider_runtime_epoch;
        }
        if (!replacement_epoch) {
          return reject<void>("route recovery has no current physical participant",
                              "route_recovery_participant_missing");
        }
        return Result<void>::Ok();
      });
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReleaseCellAdmissionClaim(
    const CellAdmissionClaimReleaseDTO& release) {
  std::lock_guard admission_lock(*admission_mutex_);
  auto released = cell_admission_owner_->ReleaseCellAdmissionClaim(release);
  if (released.ok()) {
    refresh_execution_claims();
  }
  return released;
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReconcileCellAdmissionClaims(
    const CellAdmissionReconcileDTO& reconcile) {
  std::lock_guard admission_lock(*admission_mutex_);
  auto reconciled = cell_admission_owner_->ReconcileCellAdmissionClaims(reconcile);
  if (reconciled.ok()) {
    refresh_execution_claims();
  }
  return reconciled;
}

void ResourceAddressedExecutionKernel::refresh_execution_claims() {
  // Read actual remaining authority, not a pre-reconciliation request or a
  // replayed receipt. Do not acquire the inner owner lock under state_mutex_.
  const auto claim = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kProgramCycle);
  auto manual = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kManualOperation);
  std::optional<ProgramCycleClaimRun> retired;
  std::optional<ProgramRunStopFence> retired_fence;
  std::lock_guard state_lock(state_mutex_);
  manual_operation_claim_.swap(manual);
  if (!claim.has_value()) {
    retired = std::move(program_cycle_claim_run_);
    program_cycle_claim_run_.reset();
  } else if (!program_cycle_claim_run_.has_value() ||
             program_cycle_claim_run_->claim_generation != claim->claim_generation) {
    retired = std::move(program_cycle_claim_run_);
    program_cycle_claim_run_ = ProgramCycleClaimRun{
        ProgramRunId(claim->intent.owner_transaction_id), claim->claim_generation,
        claim->intent.owner_epoch, claim->intent.program_cycle_evidence->applied_program_revision};
  }
  // An exact still-held claim retains its immutable capture and all holds.
  if (retired.has_value()) {
    const auto fence = std::find_if(
        program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
        [&](const auto& row) { return row.program_run_id == retired->program_run_id; });
    if (fence != program_run_stop_fences_.end()) {
      retired_fence = std::move(*fence);
      program_run_stop_fences_.erase(fence);
    }
    std::erase_if(stop_replay_ledger_, [&](const auto& row) {
      const auto* selection = std::get_if<ProgramExecutionSelectionRequestDTO>(&row.intent);
      return selection != nullptr && selection->program_run_id == retired->program_run_id;
    });
  }
  // retired is destroyed after state_lock, outside Stop's critical section.
}

Result<std::optional<CellAdmissionSettlementEvidenceDTO>>
ResourceAddressedExecutionKernel::manual_operation_settlement_locked(
    const CellAdmissionClaimDTO& claim) const {
  using Evidence = std::optional<CellAdmissionSettlementEvidenceDTO>;
  const bool predecessor = claim.intent.owner_epoch != config_.request_owner_epoch;
  const OperationId operation_id(claim.intent.owner_transaction_id);
  if (pending_program_run_admission_.has_value() || active_operations_.contains(operation_id) ||
      (predecessor && motion_admission_unreconciled_)) {
    return Result<Evidence>::Ok(std::nullopt);
  }
  const auto slots = resource_slots_->snapshot();
  for (const auto& resource : claim.intent.scope.resources) {
    const auto slot = std::find_if(slots.begin(), slots.end(),
                                   [&](const auto& row) { return row.resource_id == resource; });
    if (slot == slots.end() || slot->state != ResourceSlotState::kFree) {
      return Result<Evidence>::Ok(std::nullopt);
    }
  }
  const auto operation =
      std::find_if(historical_operations_.begin(), historical_operations_.end(),
                   [&](const auto& row) { return row.operation_id == operation_id; });
  const bool no_effect = operation == historical_operations_.end();
  // Absence is proof only within this still-live Kernel incarnation. Its
  // admission guard runs after the pending Execute drains, and a held claim
  // protects any accepted operation from diagnostic-history eviction.
  // A successor never infers no effect from a cache miss. Its complete orphan
  // inventory must first pass the existing fresh native settlement checks.
  const auto kind = predecessor ? CellAdmissionSettlementKindDTO::kReconciled
                    : no_effect ? CellAdmissionSettlementKindDTO::kProvenNoEffect
                                : CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
  using content_identity::CanonicalValue;
  const auto digest = content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
          {"schema", CanonicalValue("manyforge.manual_operation_settlement.v1")},
          {"operation_id", CanonicalValue(operation_id.value())},
          {"claim_generation", CanonicalValue(std::to_string(claim.claim_generation.value()))},
          {"settlement", CanonicalValue(std::string(to_string(kind)))},
          {"operation_revision",
           CanonicalValue(no_effect ? std::string{}
                                    : std::to_string(operation->operation_revision.value()))},
          {"capture_order",
           CanonicalValue(no_effect ? std::string{}
                                    : std::to_string(operation->journal_capture_order))},
      })));
  return Result<Evidence>::Ok(CellAdmissionSettlementEvidenceDTO{kind, ContentDigest(digest)});
}

Result<void> ResourceAddressedExecutionKernel::validate_manual_operation_settlement(
    const CellAdmissionClaimDTO& claim, const CellAdmissionClaimReleaseDTO& release) const {
  std::lock_guard state_lock(state_mutex_);
  if (!manual_operation_claim_.has_value() || *manual_operation_claim_ != claim ||
      claim.intent.owner_epoch != config_.request_owner_epoch) {
    return reject<void>("manual claim differs from current Kernel authority",
                        "manual_operation_claim_stale");
  }
  const auto evidence = manual_operation_settlement_locked(claim);
  if (!evidence.ok()) return Result<void>::Err(evidence.error());
  if (!evidence.value().has_value() || *evidence.value() != release.settlement) {
    return reject<void>("manual operation has not retired with exact settlement",
                        "manual_operation_settlement_not_proven");
  }
  return Result<void>::Ok();
}

Result<CellAdmissionReconcileValidationDTO>
ResourceAddressedExecutionKernel::validate_manual_owner_reconciliation(
    const std::vector<CellAdmissionClaimDTO>& held,
    const std::vector<CellAdmissionCurrentOwnerEpochDTO>& current,
    const CellAdmissionReconcileDTO& request) const {
  constexpr auto role = CellAdmissionOwnerRoleDTO::kManualOperationOwner;
  auto expected = current;
  const auto owner = std::find_if(expected.begin(), expected.end(),
                                  [](const auto& row) { return row.owner_role == role; });
  if (owner == expected.end() || owner->owner_epoch == config_.request_owner_epoch)
    return reject<CellAdmissionReconcileValidationDTO>("manual predecessor owner is not exact",
                                                       "manual_owner_reconciliation_not_exact");
  owner->owner_epoch = config_.request_owner_epoch;
  const auto digest =
      CellAdmissionOwnerEpochReconciliationEvidenceDigest(role, config_.request_owner_epoch);
  if (!digest.ok() || request.kernel_owner_epoch != config_.request_owner_epoch ||
      request.current_owner_epochs != expected || request.retained_claims != held ||
      request.reconciliation_evidence_digest != digest.value())
    return reject<CellAdmissionReconcileValidationDTO>(
        "manual reconciliation changes unrelated authority",
        "manual_owner_reconciliation_not_exact");
  CellAdmissionReconcileValidationDTO validation{std::move(expected), {}};
  std::lock_guard state_lock(state_mutex_);
  for (const auto& claim : held) {
    if (claim.intent.owner_role != role) continue;
    if (claim.intent.claim_kind != CellAdmissionClaimKindDTO::kManualOperation ||
        !manual_operation_claim_.has_value() || *manual_operation_claim_ != claim)
      return reject<CellAdmissionReconcileValidationDTO>(
          "manual claim differs from Kernel authority", "manual_operation_claim_stale");
    const auto settlement = manual_operation_settlement_locked(claim);
    if (!settlement.ok())
      return Result<CellAdmissionReconcileValidationDTO>::Err(settlement.error());
    if (!settlement.value().has_value() ||
        settlement.value()->settlement_kind != CellAdmissionSettlementKindDTO::kReconciled)
      return reject<CellAdmissionReconcileValidationDTO>("predecessor resources are not reconciled",
                                                         "manual_operation_settlement_not_proven");
    validation.settlements.push_back({claim, role, config_.request_owner_epoch,
                                      CellAdmissionSettlementKindDTO::kReconciled, digest.value()});
  }
  return Result<CellAdmissionReconcileValidationDTO>::Ok(std::move(validation));
}

Result<void> ResourceAddressedExecutionKernel::reconcile_manual_operation_admission() {
  std::unique_lock submission_lock(submission_mutex_, std::try_to_lock);
  if (!submission_lock.owns_lock()) return Result<void>::Ok();
  std::optional<OperationStateDTO> terminal;
  {
    std::lock_guard state_lock(state_mutex_);
    if (manual_operation_claim_.has_value()) {
      const auto* operation = find_active_operation_locked(
          OperationId(manual_operation_claim_->intent.owner_transaction_id));
      if (operation != nullptr &&
          (operation->state.state == OperationStateKind::kCompleted ||
           operation->state.state == OperationStateKind::kStopped ||
           operation->state.state == OperationStateKind::kCancelledPreDispatch)) {
        // This is only a candidate. Clean-terminal eligibility is decided
        // once, inside apply_control's operation commit/state guard.
        terminal = operation->state;
      }
    }
  }
  if (terminal.has_value()) {
    const auto request_id = content_identity::sha256_digest(
        config_.request_owner_epoch.value() + "\nmanual-settlement\n" +
        terminal->operation_id.value() + "\n" +
        std::to_string(terminal->operation_revision.value()));
    const OperationControlRequestDTO request{
        {RequestId(request_id), config_.request_owner_epoch, "kernel.manual_settlement",
         terminal->operation_id.value()},
        terminal->operation_id,
        terminal->operation_revision,
        "retire owner-confirmed clean terminal manual operation",
        {}};
    const auto retired = apply_control(request,
                                       terminal->state == OperationStateKind::kCompleted
                                           ? OperationControlKind::kAcknowledgeCompleted
                                           : OperationControlKind::kReset,
                                       true);
    if (!retired.ok()) {
      const auto reason = retired.error().details.find("reason");
      // The existing exact operation-control gate wins a concurrent Stop or
      // terminal transition. Reconsider only these races on the owner wake.
      if (reason != retired.error().details.end() &&
          (reason->second == "operation_revision_stale" ||
           reason->second == "operation_not_resident" ||
           reason->second == "operation_control_state_invalid" ||
           reason->second == "operation_control_busy" ||
           reason->second == "operation_effect_in_flight" ||
           reason->second == "manual_operation_not_clean_terminal"))
        return Result<void>::Ok();
      return Result<void>::Err(retired.error());
    }
  }
  return reconcile_manual_operation_admission_locked();
}

Result<void> ResourceAddressedExecutionKernel::reconcile_manual_operation_admission_locked() {
  std::lock_guard admission_lock(*admission_mutex_);
  const auto replacement = cell_admission_owner_->manual_owner_reconciliation_request();
  if (!replacement.ok()) return Result<void>::Err(replacement.error());
  if (replacement.value().has_value()) {
    const auto reconciled =
        cell_admission_owner_->ReconcileCellAdmissionClaims(*replacement.value());
    if (!reconciled.ok()) return Result<void>::Err(reconciled.error());
    refresh_execution_claims();
  }
  std::optional<CellAdmissionClaimDTO> claim;
  std::optional<CellAdmissionSettlementEvidenceDTO> evidence;
  {
    std::lock_guard state_lock(state_mutex_);
    if (!manual_operation_claim_.has_value()) return Result<void>::Ok();
    claim = manual_operation_claim_;
    auto current = manual_operation_settlement_locked(*claim);
    if (!current.ok()) return Result<void>::Err(current.error());
    evidence = std::move(current.value());
  }
  if (!evidence.has_value()) return Result<void>::Ok();
  const auto released = cell_admission_owner_->ReleaseCellAdmissionClaim(
      {claim->intent.owner_role, claim->intent.owner_epoch, claim->intent.owner_transaction_id,
       claim->intent.request_id, claim->intent.request_payload_digest,
       claim->acquired_admission_generation, claim->claim_generation, *evidence});
  if (!released.ok()) return Result<void>::Err(released.error());
  refresh_execution_claims();
  return Result<void>::Ok();
}

Result<CellAdmissionRecoveryReceiptDTO>
ResourceAddressedExecutionKernel::GetCellAdmissionRecoveryReceipt(
    const CellAdmissionRecoveryReceiptQueryDTO& query) const {
  return cell_admission_owner_->GetCellAdmissionRecoveryReceipt(query);
}

Result<void> ResourceAddressedExecutionKernel::ValidateProgramResetSceneAuthorization(
    const ProgramResetSceneAuthorizationDTO& authorization) const {
  std::lock_guard admission_lock(*admission_mutex_);
  return cell_admission_owner_->ValidateProgramResetSceneAuthorization(authorization);
}

Result<void> ResourceAddressedExecutionKernel::Publish(
    const ManagerLivenessObservationDTO& observation) {
  // Deliberately bypasses the operation admission mutex: receiver-local
  // containment and Stop acceptance must not wait on liveness publication.
  return cell_admission_owner_->Publish(observation);
}

Result<OperatorAlarmAttentionAcceptedDTO>
ResourceAddressedExecutionKernel::PublishOperatorAlarmAttention(
    const OperatorAlarmAttentionSnapshotDTO& snapshot) {
  if (auto valid = validate_operator_alarm_attention_snapshot(snapshot); !valid.ok()) {
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(valid.error());
  }
  if (snapshot.cell_id != config_.cell_id) {
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
        error("operator_alarm_attention_cell_mismatch",
              "operator-alarm attention projection names another cell",
              "operator_alarm_attention_cell_mismatch"));
  }
  // Single-host precondition (spec 530 §11): owner incarnations are
  // CLOCK_BOOTTIME stamps, comparable only within the Kernel's own host boot.
  // A projection from another host or boot is refused, never compared.
  if (config_.operator_alarm_attention.has_value() &&
      snapshot.owner_host_boot_id != config_.operator_alarm_attention->host_boot_id) {
    return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
        error("operator_alarm_attention_host_mismatch",
              "operator-alarm attention was published from another host boot",
              "operator_alarm_attention_host_mismatch"));
  }
  // A small bounded copy under the state lock that admission reads; Stop
  // acceptance never waits on any I/O here.
  //
  // Fencing contract (spec 530 §11). Owner epochs are ordered by their
  // incarnation alone, so the Kernel keeps no list of replaced epochs: a new
  // epoch is accepted only with a strictly greater incarnation than the
  // retained one; an equal incarnation under another epoch, or a smaller one,
  // is refused. Within one epoch the incarnation is fixed, generations are
  // monotonic, and one generation names exactly one content: identical
  // content is the heartbeat that refreshes the receipt time, while different
  // content under the same generation is refused.
  std::lock_guard state_lock(state_mutex_);
  if (operator_alarm_attention_.has_value()) {
    const auto& retained = operator_alarm_attention_->snapshot;
    if (retained.owner_epoch == snapshot.owner_epoch) {
      if (snapshot.owner_host_boot_id != retained.owner_host_boot_id ||
          snapshot.owner_incarnation_us != retained.owner_incarnation_us) {
        return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
            error("operator_alarm_attention_conflict",
                  "operator-alarm owner epoch was republished with another incarnation",
                  "operator_alarm_attention_owner_incarnation_conflict"));
      }
      if (snapshot.generation < retained.generation) {
        return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
            error("operator_alarm_attention_stale",
                  "operator-alarm attention generation precedes the retained one",
                  "operator_alarm_attention_generation_stale"));
      }
      if (snapshot.generation == retained.generation && snapshot != retained) {
        return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
            error("operator_alarm_attention_conflict",
                  "operator-alarm attention generation was republished with different content",
                  "operator_alarm_attention_generation_conflict"));
      }
    } else if (snapshot.owner_incarnation_us == retained.owner_incarnation_us) {
      return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
          error("operator_alarm_attention_conflict",
                "another operator-alarm owner epoch claims the retained incarnation",
                "operator_alarm_attention_owner_incarnation_conflict"));
    } else if (snapshot.owner_incarnation_us < retained.owner_incarnation_us) {
      return Result<OperatorAlarmAttentionAcceptedDTO>::Err(
          error("operator_alarm_attention_stale",
                "operator-alarm attention names an older owner incarnation",
                "operator_alarm_attention_owner_incarnation_stale"));
    }
  }
  operator_alarm_attention_ = RetainedOperatorAlarmAttentionDTO{snapshot, clock_.now()};
  return Result<OperatorAlarmAttentionAcceptedDTO>::Ok(
      OperatorAlarmAttentionAcceptedDTO{snapshot.owner_epoch, snapshot.generation});
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::operator_alarm_attention_refusal_locked(
    const std::vector<ResourceId>& reserved_resources) const {
  if (!config_.operator_alarm_attention.has_value()) return std::nullopt;
  std::optional<OperatorAlarmAttentionConfigurationDTO> active_configuration;
  if (stop_retained_bundle_id_.has_value() && stop_graph_revision_.has_value()) {
    active_configuration =
        OperatorAlarmAttentionConfigurationDTO{*stop_retained_bundle_id_, *stop_graph_revision_};
  }
  return evaluate_operator_alarm_motion_attention(
      operator_alarm_attention_, *config_.operator_alarm_attention, config_.cell_id,
      active_configuration, clock_.now(), reserved_resources);
}

std::vector<ResourceId> ResourceAddressedExecutionKernel::program_selection_resources_locked(
    const std::vector<std::uint32_t>& node_indices) const {
  std::set<ResourceId> resources;
  if (!program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      !program_cycle_claim_run_->capture->table || !program_cycle_claim_run_->capture->binding) {
    return {};
  }
  const auto& capture = *program_cycle_claim_run_->capture;
  const auto& nodes = capture.table->nodes;
  for (const auto index : node_indices) {
    if (index >= nodes.size()) continue;
    for (const auto& effect : nodes[index].target_effects) {
      if (!effect.effectful) continue;
      // The exact sealed Program Binding names each target role's resources.
      for (const auto& target : capture.binding->target_role_bindings) {
        if (target.target_role_id != effect.target_role_id) continue;
        for (const auto& role : target.resource_role_bindings) {
          resources.insert(role.resource_ids.begin(), role.resource_ids.end());
        }
      }
    }
  }
  return {resources.begin(), resources.end()};
}

Result<void> ResourceAddressedExecutionKernel::check_program_node_motion_attention(
    const ProgramNodeMotionAttentionCheckDTO& request) const {
  std::lock_guard state_lock(state_mutex_);
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch ||
      !program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      !program_cycle_claim_run_->capture->table ||
      request.program_run_id != program_cycle_claim_run_->program_run_id ||
      request.program_runner_owner_epoch != program_cycle_claim_run_->program_runner_owner_epoch ||
      request.claim_generation != program_cycle_claim_run_->claim_generation) {
    return reject<void>("Program node check does not name the current captured owner/claim",
                        "program_selection_stale", ErrorCode::kInvalidStateTransition);
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto node = std::lower_bound(
      nodes.begin(), nodes.end(), request.node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id < id; });
  if (node == nodes.end() || node->instance_id != request.node_instance_id) {
    return reject<void>("Program node is not present in the captured Program",
                        "program_node_unknown", ErrorCode::kInvalidRequest);
  }
  if (auto refusal = operator_alarm_attention_refusal_locked(program_selection_resources_locked(
          {static_cast<std::uint32_t>(node - nodes.begin())}))) {
    return Result<void>::Err(std::move(*refusal));
  }
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::LatchAndContain(
    const ManagerLivenessLossIntentDTO& intent) noexcept {
  try {
    if (!cell_admission_owner_->LatchManagerLivenessLoss(intent)) {
      return;
    }
    StopGeneration expected_stop_generation(0U);
    {
      std::lock_guard state_lock(state_mutex_);
      // This is the execution-side fail-safe latch. It is not a second
      // liveness timer or state owner and is deliberately set before Stop I/O.
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
      expected_stop_generation = cell_stop_generation_;
    }
    const auto material = config_.request_owner_epoch.value() + "\nmanager-liveness-loss\n" +
                          intent.cell_id.value() + "\n" + intent.receiver_runtime_unit_id.value() +
                          "\n" + intent.receiver_runtime_epoch.value() + "\n" +
                          std::to_string(intent.receiver_status_sequence);
    FullStopIntentDTO containment{
        PublicRequestContextDTO{RequestId(manyforge::content_identity::sha256_digest(material)),
                                config_.request_owner_epoch, "kernel.manager_liveness",
                                intent.receiver_runtime_unit_id.value()},
        expected_stop_generation,
        RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                              std::nullopt}};
    auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)});
    if (!stopped.ok()) {
      std::lock_guard state_lock(state_mutex_);
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
    } else if (stopped.value().replay_status == RequestReplayStatusDTO::kAcceptedNew &&
               stopped.value().joined_predecessor_stops.empty()) {
      // Attribute only a fresh containment Stop that joined no operator Stop:
      // its latch, if any, was caused by this availability-class loss alone.
      std::lock_guard state_lock(state_mutex_);
      attribute_containment_stop_locked(stopped.value(),
                                        StopFaultLatchOriginDTO::kProviderAvailabilityLoss);
    }
  } catch (...) {
    try {
      std::lock_guard state_lock(state_mutex_);
      manager_liveness_motion_inhibited_ = true;
      motion_admission_closed_ = true;
    } catch (...) {
    }
  }
}

void ResourceAddressedExecutionKernel::attribute_containment_stop_locked(
    const StopAcceptedDTO& stopped, const StopFaultLatchOriginDTO origin) {
  constexpr std::size_t kMaximumAttributedContainmentStops = 64U;
  if (attributed_containment_stops_.size() >= kMaximumAttributedContainmentStops) {
    attributed_containment_stops_.erase(attributed_containment_stops_.begin());
  }
  attributed_containment_stops_.push_back({stopped.stop_id, stopped.stop_generation, origin});
}

Result<ComponentIncidentContainmentReceiptDTO>
ResourceAddressedExecutionKernel::RequestComponentIncidentContainment(
    const ComponentIncidentContainmentIntentDTO& intent) {
  if (!intent.locally_valid() || !content_identity::is_utf8_nfc(intent.incident_id)) {
    return reject<ComponentIncidentContainmentReceiptDTO>(
        "component incident containment request is invalid", "incident_containment_invalid",
        ErrorCode::kInvalidRequest);
  }
  StopGeneration expected_stop_generation(0U);
  std::optional<ProgramRunId> program_run_id;
  {
    std::lock_guard state_lock(state_mutex_);
    // Closed before any Stop I/O, exactly like the liveness fail-safe.
    motion_admission_closed_ = true;
    for (const auto& [incident_id, receipt] : incident_containments_) {
      if (incident_id == intent.incident_id) {
        return Result<ComponentIncidentContainmentReceiptDTO>::Ok(receipt);
      }
    }
    expected_stop_generation = cell_stop_generation_;
    if (program_cycle_claim_run_.has_value()) {
      program_run_id = program_cycle_claim_run_->program_run_id;
    }
  }
  // The request identity is this Kernel's own: a repeated request for the
  // same incident replays the same Stop instead of issuing another.
  const auto material = config_.request_owner_epoch.value() + "\ncomponent-incident\n" +
                        config_.cell_id.value() + "\n" + intent.incident_id;
  FullStopIntentDTO containment{
      PublicRequestContextDTO{RequestId(manyforge::content_identity::sha256_digest(material)),
                              config_.request_owner_epoch, "cclc.component_incident",
                              intent.incident_id},
      expected_stop_generation,
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}};
  auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)}, nullptr,
                             nullptr, StopFaultTriggerDTO::kPhysicalRestUnproven);
  if (!stopped.ok()) {
    return Result<ComponentIncidentContainmentReceiptDTO>::Err(stopped.error());
  }
  ComponentIncidentContainmentReceiptDTO receipt{stopped.value().stop_id,
                                                 stopped.value().stop_generation,
                                                 {},
                                                 std::move(program_run_id)};
  receipt.affected_operation_ids.reserve(stopped.value().affected_operation_revisions.size());
  for (const auto& affected : stopped.value().affected_operation_revisions) {
    receipt.affected_operation_ids.push_back(affected.operation_id);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (stopped.value().replay_status == RequestReplayStatusDTO::kAcceptedNew &&
        stopped.value().joined_predecessor_stops.empty()) {
      attribute_containment_stop_locked(stopped.value(),
                                        StopFaultLatchOriginDTO::kComponentIncident);
    }
    constexpr std::size_t kMaximumRememberedIncidentContainments = 64U;
    if (incident_containments_.size() >= kMaximumRememberedIncidentContainments) {
      incident_containments_.erase(incident_containments_.begin());
    }
    incident_containments_.emplace_back(intent.incident_id, receipt);
  }
  return Result<ComponentIncidentContainmentReceiptDTO>::Ok(std::move(receipt));
}

Result<AuthoringAdmissionDTO> ResourceAddressedExecutionKernel::ReplaceManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set) {
  return cell_admission_owner_->ReplaceManagerLivenessRequiredSet(required_set);
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_participants(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  return reconfigure_runtime_participants_impl(std::move(registrations),
                                               std::move(participant_resolver), false, nullptr);
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_product(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver,
    std::optional<StopDeadlineBudgetDTO> cell_stop_deadline_budget,
    std::function<Result<void>()> commit_publication) {
  return reconfigure_runtime_participants_impl(
      std::move(registrations), std::move(participant_resolver), false,
      &cell_stop_deadline_budget, nullptr, nullptr, nullptr, nullptr, nullptr,
      commit_publication ? &commit_publication : nullptr);
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_component_set(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver,
    std::optional<StopDeadlineBudgetDTO> cell_stop_deadline_budget,
    RetainedBundleId candidate_retained_bundle_id,
    GraphRevision candidate_graph_revision,
    const ConfigurationFenceId& apply_fence_id,
    const ConfigurationFenceScopeDTO& scope,
    const std::vector<ControllableComponentId>& retained_components,
    std::function<Result<void>()> commit_publication) {
  if (candidate_retained_bundle_id.empty() || candidate_graph_revision.empty() ||
      apply_fence_id.empty() || !scope.valid() ||
      !std::is_sorted(retained_components.begin(), retained_components.end()) ||
      std::adjacent_find(retained_components.begin(), retained_components.end()) !=
          retained_components.end()) {
    return reject<void>("component-set transition has no exact Apply fence scope",
                        "runtime_component_set_scope_invalid", ErrorCode::kInvalidRequest);
  }
  return reconfigure_runtime_participants_impl(
      std::move(registrations), std::move(participant_resolver), false,
      &cell_stop_deadline_budget, &apply_fence_id, &scope,
      &retained_components, &candidate_retained_bundle_id,
      &candidate_graph_revision, commit_publication ? &commit_publication : nullptr);
}

Result<void>
ResourceAddressedExecutionKernel::stage_current_runtime_configuration_facts_under_apply_fence(
    const ConfigurationFenceScopeDTO& scope) {
  if (!scope.valid() || scope.resources.empty()) {
    return reject<void>("candidate configuration staging scope is invalid",
                        "runtime_configuration_staging_scope_invalid", ErrorCode::kInvalidRequest);
  }
  if (config_.operation_admission_gate == nullptr) {
    return reject<void>("candidate configuration staging has no durable admission gate",
                        "runtime_configuration_staging_gate_missing", ErrorCode::kMissingReference);
  }

  try {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    if (config_.operation_admission_gate->current_fenced_resources(scope.resources) !=
        scope.resources) {
      return reject<void>("candidate configuration staging is not covered by its Apply fence",
                          "runtime_configuration_staging_fence_missing",
                          ErrorCode::kExecutionRejected);
    }

    const auto declared = extract_active_control_configurations(registrations_);
    auto admission = resource_slots_->admission_snapshot({});
    auto staged = admission.active_control_configurations;
    for (const auto& domain : scope.domains) {
      const auto existing = std::lower_bound(
          staged.begin(), staged.end(), domain.control_domain_id,
          [](const auto& row, const auto& domain_id) { return row.control_domain_id < domain_id; });
      if (existing != staged.end() && existing->control_domain_id == domain.control_domain_id) {
        staged.erase(existing);
      }
      if (!domain.candidate_control_domain_revision.has_value()) {
        continue;
      }
      const auto candidate = std::lower_bound(
          declared.begin(), declared.end(), domain.control_domain_id,
          [](const auto& row, const auto& domain_id) { return row.control_domain_id < domain_id; });
      if (candidate == declared.end() || candidate->control_domain_id != domain.control_domain_id) {
        return reject<void>("candidate configuration staging has no installed domain fact",
                            "runtime_configuration_staging_domain_missing",
                            ErrorCode::kMissingReference);
      }
      const bool revision_matches =
          std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& registration) {
            const auto& binding = registration.binding.value();
            return binding.control_path.has_value() &&
                   binding.control_path->control_domain_id == domain.control_domain_id &&
                   binding.control_path->control_domain_revision ==
                       *domain.candidate_control_domain_revision;
          });
      if (!revision_matches) {
        return reject<void>("candidate configuration staging crossed its retained domain revision",
                            "runtime_configuration_staging_domain_revision_mismatch",
                            ErrorCode::kInvalidRequest);
      }
      staged.insert(std::lower_bound(staged.begin(), staged.end(), candidate->control_domain_id,
                                     [](const auto& row, const auto& domain_id) {
                                       return row.control_domain_id < domain_id;
                                     }),
                    *candidate);
    }
    return resource_slots_->reconfigure_runtime_inventory(
        registry_.resource_inventory(), std::move(staged),
        std::move(admission.route_published_active_control_configurations));
  } catch (const std::exception& exception) {
    return reject<void>(std::string("candidate configuration staging failed: ") + exception.what(),
                        "runtime_configuration_staging_failed", ErrorCode::kInvariantViolation);
  } catch (...) {
    return reject<void>("candidate configuration staging failed",
                        "runtime_configuration_staging_failed", ErrorCode::kInvariantViolation);
  }
}

Result<void> ResourceAddressedExecutionKernel::reconfigure_runtime_participants_impl(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver,
    const bool retain_unready_configuration_facts,
    const std::optional<StopDeadlineBudgetDTO>* replacement_stop_deadline_budget,
    const ConfigurationFenceId* apply_fence_id,
    const ConfigurationFenceScopeDTO* apply_scope,
    const std::vector<ControllableComponentId>* retained_components,
    const RetainedBundleId* candidate_retained_bundle_id,
    const GraphRevision* candidate_graph_revision,
    const std::function<Result<void>()>* commit_publication) {
  if ((candidate_retained_bundle_id == nullptr) !=
      (candidate_graph_revision == nullptr)) {
    return reject<void>("component-set transition has incomplete product identity",
                        "runtime_component_set_product_invalid",
                        ErrorCode::kInvalidRequest);
  }
  if (candidate_retained_bundle_id != nullptr &&
      std::any_of(registrations.begin(), registrations.end(),
                  [&](const auto& registration) {
                    const auto& binding = registration.binding.value();
                    return binding.retained_bundle_id !=
                               *candidate_retained_bundle_id ||
                           binding.graph_revision != *candidate_graph_revision;
                  })) {
    return reject<void>("component-set participants differ from candidate product",
                        "runtime_component_set_product_mismatch",
                        ErrorCode::kInvalidRequest);
  }
  if (qualified_execution_engine_count(registrations) > prequalified_max_live_stop_participants_) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              "candidate runtime participants exceed prequalified Stop-stage isolation",
              "stop_stage_attempt_prequalification_exceeded"));
  }
  std::optional<RuntimeParticipantRegistry> candidate_registry;
  try {
    candidate_registry.emplace(extract_bindings(registrations));
  } catch (const std::exception& exception) {
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInvalidRequest)),
              std::string("candidate runtime participant registry is invalid: ") + exception.what(),
              "runtime_participant_reconfiguration_invalid"));
  }
  std::vector<DomainActiveControlConfigurationDTO> candidate_configurations;
  std::vector<DomainActiveControlConfigurationDTO> declared_candidate_configurations;
  std::vector<DomainActiveControlConfigurationDTO> table_configurations;
  std::vector<DomainActiveControlConfigurationDTO> table_route_publications;
  std::optional<StopStaticFacts> candidate_stop_facts;
  std::set<ControlDomainId> changed_domains;
  try {
    declared_candidate_configurations = extract_active_control_configurations(registrations);
    candidate_configurations = extract_published_active_control_configurations(registrations);
    changed_domains = changed_registration_domains(registrations_, registrations);
    table_configurations = candidate_configurations;
    // A route published for one domain must not erase a durable predecessor
    // fact adopted for an unchanged domain merely because that other domain
    // is still awaiting fresh Stop-readiness publication.  A first-time
    // unqualified domain has no table fact to retain, while any registration
    // change still removes or replaces its old fact below.
    for (const auto& current :
         resource_slots_->admission_snapshot({}).active_control_configurations) {
      if (changed_domains.contains(current.control_domain_id) ||
          std::any_of(table_configurations.begin(), table_configurations.end(),
                      [&](const auto& candidate) {
                        return candidate.control_domain_id == current.control_domain_id;
                      })) {
        continue;
      }
      table_configurations.push_back(current);
    }
    std::sort(table_configurations.begin(), table_configurations.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.control_domain_id < rhs.control_domain_id;
              });
    if (registrations.empty()) {
      candidate_stop_facts.emplace(StopStaticFacts{
          candidate_retained_bundle_id != nullptr
              ? *candidate_retained_bundle_id
              : *stop_retained_bundle_id_,
          candidate_graph_revision != nullptr ? *candidate_graph_revision
                                              : *stop_graph_revision_,
          replacement_stop_deadline_budget == nullptr
              ? cell_stop_deadline_budget_
              : *replacement_stop_deadline_budget,
          {}});
    } else {
      if (replacement_stop_deadline_budget == nullptr) {
        candidate_stop_facts.emplace(derive_stop_static_facts(
            registrations, config_, nullptr, apply_scope == nullptr));
      } else {
        candidate_stop_facts.emplace(
            derive_stop_static_facts(registrations, config_,
                                     replacement_stop_deadline_budget,
                                     apply_scope == nullptr));
      }
      if (candidate_retained_bundle_id != nullptr &&
          (candidate_stop_facts->retained_bundle_id !=
               *candidate_retained_bundle_id ||
           candidate_stop_facts->graph_revision !=
               *candidate_graph_revision)) {
        throw std::invalid_argument(
            "component-set Stop facts differ from candidate product");
      }
    }
  } catch (const std::exception& exception) {
    return Result<void>::Err(error(
        std::string(to_string(ErrorCode::kInvalidRequest)),
        std::string("candidate runtime control configuration is invalid: ") + exception.what(),
        "runtime_participant_reconfiguration_invalid"));
  }

  std::lock_guard submit_lock(submission_mutex_);
  if (apply_scope != nullptr) {
    if (apply_fence_id == nullptr || config_.operation_admission_gate == nullptr ||
        !config_.operation_admission_gate->exact_fence_is_held(*apply_fence_id,
                                                                *apply_scope) ||
        (!apply_scope->resources.empty() &&
         !config_.operation_admission_gate->exact_fence_covers(
             *apply_fence_id, apply_scope->resources))) {
      return reject<void>("component-set transition is outside its exact Apply fence",
                          "runtime_component_set_fence_missing",
                          ErrorCode::kExecutionRejected);
    }
    const auto check_changed = [&](const auto& source, const auto& other,
                                   const bool predecessor_side) {
      for (const auto& registration : source) {
        if (std::any_of(other.begin(), other.end(), [&](const auto& row) {
              return registration_truth_equal(registration, row);
            })) {
          continue;
        }
        const auto& binding = registration.binding.value();
        if (retained_components != nullptr &&
            std::binary_search(retained_components->begin(), retained_components->end(),
                               binding.controllable_component_id) &&
            std::any_of(other.begin(), other.end(), [&](const auto& row) {
              return registration_controller_equal_after_product_reseal(registration, row);
            })) {
          continue;
        }
        if (!std::binary_search(apply_scope->controllable_component_ids.begin(),
                                apply_scope->controllable_component_ids.end(),
                                binding.controllable_component_id) ||
            !binding.control_path.has_value() ||
            std::any_of(binding.resources.begin(), binding.resources.end(),
                        [&](const auto& row) {
                          return !std::binary_search(apply_scope->resources.begin(),
                                                     apply_scope->resources.end(),
                                                     row.resource_id);
                        })) {
          return false;
        }
        const auto& control_path = *binding.control_path;
        const auto domain_scope = std::find_if(
            apply_scope->domains.begin(), apply_scope->domains.end(), [&](const auto& row) {
              return row.control_domain_id == control_path.control_domain_id;
            });
        const auto binding_scope = std::find_if(
            apply_scope->bindings.begin(), apply_scope->bindings.end(), [&](const auto& row) {
              return row.control_binding_id == control_path.control_binding_id;
            });
        if (domain_scope == apply_scope->domains.end() ||
            binding_scope == apply_scope->bindings.end() ||
            (predecessor_side
                 ? domain_scope->predecessor_control_domain_revision
                 : domain_scope->candidate_control_domain_revision) !=
                std::optional(control_path.control_domain_revision) ||
            (predecessor_side
                 ? binding_scope->predecessor_control_binding_revision
                 : binding_scope->candidate_control_binding_revision) !=
                std::optional(control_path.control_binding_revision)) {
          return false;
        }
      }
      return true;
    };
    if (!check_changed(registrations_, registrations, true) ||
        !check_changed(registrations, registrations_, false)) {
      return reject<void>("component-set transition changes an unfenced participant",
                          "runtime_component_set_scope_incomplete",
                          ErrorCode::kExecutionRejected);
    }
  }
  std::vector<EngineSubscription> added_subscriptions;
  try {
    for (const auto& registration : registrations) {
      const auto already_subscribed = std::any_of(
          engine_subscriptions_.begin(), engine_subscriptions_.end(),
          [&](const EngineSubscription& item) { return item.engine == registration.engine; });
      const auto newly_subscribed = std::any_of(
          added_subscriptions.begin(), added_subscriptions.end(),
          [&](const EngineSubscription& item) { return item.engine == registration.engine; });
      if (already_subscribed || newly_subscribed) {
        continue;
      }
      auto* engine = registration.engine;
      auto ingress = engine_event_ingress_;
      const auto handle = engine->subscribe_events(
          [ingress = std::move(ingress), engine](const KernelEvent& event) {
            auto entered = ingress->Enter();
            if (auto* owner = entered.owner()) owner->on_engine_event(engine, event);
          });
      added_subscriptions.push_back(EngineSubscription{engine, handle});
    }
  } catch (const std::exception& exception) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(
        error(std::string(to_string(ErrorCode::kInternalError)),
              std::string("candidate runtime event subscription failed: ") + exception.what(),
              "runtime_participant_subscription_failed"));
  } catch (...) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "candidate runtime event subscription failed",
                                   "runtime_participant_subscription_failed"));
  }

  try {
    engine_subscriptions_.reserve(engine_subscriptions_.size() + added_subscriptions.size());
  } catch (...) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInternalError)),
                                   "candidate runtime subscription table reached capacity",
                                   "runtime_participant_subscription_capacity_unavailable"));
  }

  std::vector<IResolvedParticipantExecutionEngine*> control_engines;
  bool control_reconfiguration_busy = false;
  bool retain_control_executor = false;
  bool update_control_executor_in_place = false;
  {
    std::lock_guard state_lock(state_mutex_);
    if (operation_control_reconfiguration_busy_locked()) {
      control_reconfiguration_busy = true;
    } else {
      control_engines = operation_control_engines_locked(registrations);
      const auto current_engines = operation_control_engines_locked(registrations_);
      retain_control_executor =
          (operation_control_executor_ != nullptr || control_engines.empty()) &&
          std::set<IResolvedParticipantExecutionEngine*>(control_engines.begin(),
                                                          control_engines.end()) ==
              std::set<IResolvedParticipantExecutionEngine*>(current_engines.begin(),
                                                              current_engines.end());
      update_control_executor_in_place = !retain_control_executor &&
                                         operation_control_executor_ != nullptr &&
                                         !control_engines.empty();
      operation_control_reconfiguration_in_progress_ = true;
    }
  }
  if (control_reconfiguration_busy) {
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                   "operation-control work prevents lane reconfiguration",
                                   "operation_control_reconfiguration_busy"));
  }

  std::unique_ptr<BoundedOperationControlExecutor> candidate_control_executor;
  try {
    if (!retain_control_executor && !update_control_executor_in_place &&
        !control_engines.empty()) {
      candidate_control_executor = std::make_unique<BoundedOperationControlExecutor>(
          std::move(control_engines),
          static_cast<IOperationControlEffectCompletionReceiver&>(*this));
      if (!candidate_control_executor->prequalified_ready()) {
        throw std::runtime_error("candidate operation-control lanes are not ready");
      }
    }
  } catch (const std::exception& exception) {
    {
      std::lock_guard state_lock(state_mutex_);
      operation_control_reconfiguration_in_progress_ = false;
    }
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(error(
        std::string(to_string(ErrorCode::kInternalError)),
        std::string("candidate operation-control lane construction failed: ") + exception.what(),
        "operation_control_reconfiguration_failed"));
  }

  std::optional<ErrorDTO> table_error;
  std::optional<ErrorDTO> control_lane_error;
  std::optional<ErrorDTO> publication_commit_error;
  std::unique_ptr<BoundedOperationControlExecutor> retired_control_executor;
  {
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    table_route_publications.reserve(runtime_route_configuration_readbacks_.size());
    for (const auto& [domain_id, readback] : runtime_route_configuration_readbacks_) {
      if (changed_domains.contains(domain_id)) {
        const auto candidate =
            std::find_if(candidate_configurations.begin(), candidate_configurations.end(),
                         [&](const auto& item) { return item.control_domain_id == domain_id; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback.active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback.active_configuration.active_control_configuration_generation) {
          continue;
        }
      }
      table_route_publications.push_back(DomainActiveControlConfigurationDTO{
          domain_id, readback.active_configuration.active_control_configuration_id,
          readback.active_configuration.active_control_configuration_generation});
    }
    // Preparation can create subscriptions and worker lanes outside admission.
    // Verify the delegation again under the lock that also serializes release,
    // before changing any inventory or invoking the publication callback.
    const bool authority_current =
        apply_scope == nullptr ||
        (config_.operation_admission_gate->exact_fence_is_held(
             *apply_fence_id, *apply_scope) &&
         (apply_scope->resources.empty() ||
          config_.operation_admission_gate->exact_fence_covers(
              *apply_fence_id, apply_scope->resources)));
    auto table_result = authority_current
        ? resource_slots_->reconfigure_runtime_inventory(
              candidate_registry->resource_inventory(),
              retain_unready_configuration_facts ? declared_candidate_configurations
                                                 : table_configurations,
              table_route_publications)
        : reject<void>("component-set transition lost its exact Apply fence",
                       "runtime_component_set_fence_missing",
                       ErrorCode::kExecutionRejected);
    if (!table_result.ok()) {
      table_error = table_result.error();
    } else {
      for (auto readback = runtime_route_configuration_readbacks_.begin();
           readback != runtime_route_configuration_readbacks_.end();) {
        if (!changed_domains.contains(readback->first)) {
          ++readback;
          continue;
        }
        const auto candidate = std::find_if(
            candidate_configurations.begin(), candidate_configurations.end(),
            [&](const auto& item) { return item.control_domain_id == readback->first; });
        if (candidate == candidate_configurations.end() ||
            candidate->active_control_configuration_id !=
                readback->second.active_configuration.active_control_configuration_id ||
            candidate->active_control_configuration_generation !=
                readback->second.active_configuration.active_control_configuration_generation) {
          readback = runtime_route_configuration_readbacks_.erase(readback);
        } else {
          ++readback;
        }
      }
      for (auto required = runtime_route_configuration_publication_required_.begin();
           required != runtime_route_configuration_publication_required_.end();) {
        if (!changed_domains.contains(*required)) {
          ++required;
          continue;
        }
        const auto present = std::any_of(
            declared_candidate_configurations.begin(), declared_candidate_configurations.end(),
            [&](const auto& item) { return item.control_domain_id == *required; });
        required = present ? std::next(required)
                           : runtime_route_configuration_publication_required_.erase(required);
      }
      for (const auto& configuration : declared_candidate_configurations) {
        if (!changed_domains.contains(configuration.control_domain_id)) {
          continue;
        }
        const auto current =
            runtime_route_configuration_readbacks_.find(configuration.control_domain_id);
        if (current == runtime_route_configuration_readbacks_.end()) {
          runtime_route_configuration_publication_required_.insert(configuration.control_domain_id);
        }
      }
      if (commit_publication != nullptr && *commit_publication) {
        try {
          auto committed = (*commit_publication)();
          if (!committed.ok()) {
            const bool under_apply_fence = apply_scope != nullptr;
            publication_commit_error = error(
                std::string(to_string(ErrorCode::kInvariantViolation)),
                std::string("candidate publication commit failed after Kernel inventory update; ") +
                    (under_apply_fence
                         ? "admission remains closed and the Apply fence must remain held: "
                         : "Kernel admission remains closed: ") +
                    committed.error().message,
                under_apply_fence ? "runtime_component_set_publication_commit_failed"
                                  : "runtime_product_publication_commit_failed");
          }
        } catch (const std::exception& exception) {
          const bool under_apply_fence = apply_scope != nullptr;
          publication_commit_error = error(
              std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("candidate publication commit threw after Kernel inventory update; ") +
                  (under_apply_fence
                       ? "admission remains closed and the Apply fence must remain held: "
                       : "Kernel admission remains closed: ") +
                  exception.what(),
              under_apply_fence ? "runtime_component_set_publication_commit_failed"
                                : "runtime_product_publication_commit_failed");
        } catch (...) {
          const bool under_apply_fence = apply_scope != nullptr;
          publication_commit_error = error(
              std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("candidate publication commit threw after Kernel inventory update; ") +
                  (under_apply_fence
                       ? "admission remains closed and the Apply fence must remain held"
                       : "Kernel admission remains closed"),
              under_apply_fence ? "runtime_component_set_publication_commit_failed"
                                : "runtime_product_publication_commit_failed");
        }
      }
      if (publication_commit_error.has_value()) {
        motion_admission_closed_ = true;
      } else {
        registry_ = std::move(*candidate_registry);
        registrations_ = std::move(registrations);
        stop_retained_bundle_id_ = std::move(candidate_stop_facts->retained_bundle_id);
        stop_graph_revision_ = std::move(candidate_stop_facts->graph_revision);
        config_.graph_revision = stop_graph_revision_;
        if (candidate_retained_bundle_id != nullptr) {
          config_.empty_runtime_retained_bundle_id = *candidate_retained_bundle_id;
          config_.empty_runtime_graph_revision = *candidate_graph_revision;
        }
        cell_stop_deadline_budget_ = std::move(candidate_stop_facts->deadline_budget);
        if (replacement_stop_deadline_budget != nullptr) {
          config_.cell_stop_deadline_budget = *replacement_stop_deadline_budget;
        }
        stop_component_scope_ = std::move(candidate_stop_facts->components);
        config_.participant_resolver = std::move(participant_resolver);
        engine_subscriptions_.insert(engine_subscriptions_.end(),
                                     std::make_move_iterator(added_subscriptions.begin()),
                                     std::make_move_iterator(added_subscriptions.end()));
        if (update_control_executor_in_place) {
          const auto update = operation_control_executor_->ReconfigureEngines(control_engines);
          if (update != OperationControlLaneUpdateStatus::kApplied) {
            // Inventory may already have changed. Do not publish a partial lane
            // update as a successful installation or release the Apply fence.
            motion_admission_closed_ = true;
            control_lane_error =
                error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      "incremental operation-control lane update could not be verified",
                      "operation_control_incremental_update_failed");
          }
        } else if (!retain_control_executor) {
          retired_control_executor = std::move(operation_control_executor_);
          operation_control_executor_ = std::move(candidate_control_executor);
        }
      }
    }
    operation_control_reconfiguration_in_progress_ = false;
  }
  if (table_error.has_value()) {
    if (candidate_control_executor != nullptr) {
      candidate_control_executor->CloseIngress();
      if (!candidate_control_executor->DrainFor(std::chrono::seconds(5))) {
        std::terminate();
      }
      candidate_control_executor.reset();
    }
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(std::move(*table_error));
  }
  if (publication_commit_error.has_value()) {
    if (candidate_control_executor != nullptr) {
      candidate_control_executor->CloseIngress();
      if (!candidate_control_executor->DrainFor(std::chrono::seconds(5))) {
        std::terminate();
      }
      candidate_control_executor.reset();
    }
    for (const auto& subscription : added_subscriptions) {
      subscription.engine->unsubscribe_events(subscription.handle);
    }
    return Result<void>::Err(std::move(*publication_commit_error));
  }
  if (control_lane_error.has_value()) {
    return Result<void>::Err(std::move(*control_lane_error));
  }
  if (retired_control_executor != nullptr) {
    retired_control_executor->CloseIngress();
    if (!retired_control_executor->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
    retired_control_executor.reset();
  }
  return Result<void>::Ok();
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::adopt_runtime_route_configuration_predecessor(
    ActiveControlConfigurationDTO configuration,
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!configuration.locally_valid() ||
      configuration.state != ActiveControlConfigurationStateDTO::kReconciled) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor is not one reconciled configuration",
        "runtime_route_predecessor_invalid", ErrorCode::kInvalidRequest);
  }

  std::vector<DomainActiveControlConfigurationDTO> declared;
  try {
    declared = extract_active_control_configurations(registrations);
  } catch (const std::exception& exception) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        std::string("durable route predecessor registrations are invalid: ") + exception.what(),
        "runtime_route_predecessor_registration_invalid", ErrorCode::kInvalidRequest);
  }
  const auto selected = std::find_if(declared.begin(), declared.end(), [&](const auto& row) {
    return row.control_domain_id == configuration.control_domain_id;
  });
  if (selected == declared.end() ||
      selected->active_control_configuration_id != configuration.active_control_configuration_id ||
      selected->active_control_configuration_generation !=
          configuration.active_control_configuration_generation) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor differs from its runtime registrations",
        "runtime_route_predecessor_registration_mismatch", ErrorCode::kInvalidRequest);
  }
  bool found_domain = false;
  for (const auto& registration : registrations) {
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value() ||
        binding.control_path->control_domain_id != configuration.control_domain_id) {
      continue;
    }
    found_domain = true;
    const auto& path = *binding.control_path;
    const auto route = std::find_if(
        configuration.binding_routes.begin(), configuration.binding_routes.end(),
        [&](const auto& row) { return row.control_binding_id == path.control_binding_id; });
    if (registration.stop_readiness.has_value() ||
        path.provider_runtime_unit_id != configuration.source_runtime_unit_id ||
        path.control_domain_revision != configuration.control_domain_revision ||
        path.controller_topology_id != configuration.controller_topology_id ||
        path.topology_revision != configuration.topology_revision ||
        path.activation_set_id != configuration.activation_set_id ||
        route == configuration.binding_routes.end() ||
        route->control_binding_revision != path.control_binding_revision ||
        route->control_route_id != path.control_route_id) {
      return reject<RuntimeRouteConfigurationReadbackDTO>(
          "durable route predecessor crosses its current runtime binding",
          "runtime_route_predecessor_binding_mismatch", ErrorCode::kInvalidRequest);
    }
  }
  if (!found_domain) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor has no runtime participant",
        "runtime_route_predecessor_registration_missing", ErrorCode::kMissingReference);
  }

  RuntimeRouteConfigurationReadbackDTO candidate{std::move(configuration), {}, false};
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO> staged;
  try {
    staged.emplace(candidate.active_configuration.control_domain_id, candidate);
  } catch (...) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "durable route predecessor readback could not be staged",
        "runtime_route_predecessor_capacity_unavailable", ErrorCode::kInternalError);
  }
  auto staged_node = staged.extract(staged.begin());

  auto reconfigured = reconfigure_runtime_participants_impl(
      std::move(registrations), std::move(participant_resolver), true, nullptr);
  if (!reconfigured.ok()) {
    return Result<RuntimeRouteConfigurationReadbackDTO>::Err(reconfigured.error());
  }
  {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    auto publications =
        route_publication_identities(runtime_route_configuration_readbacks_, &candidate);
    auto publication_inventory =
        resource_slots_->replace_route_publication_inventory(std::move(publications));
    if (!publication_inventory.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(publication_inventory.error());
    }
    runtime_route_configuration_readbacks_.erase(candidate.active_configuration.control_domain_id);
    runtime_route_configuration_readbacks_.insert(std::move(staged_node));
    runtime_route_configuration_publication_required_.insert(
        candidate.active_configuration.control_domain_id);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(std::move(candidate));
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::publish_runtime_route_configuration(
    ActiveControlConfigurationDTO configuration,
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!configuration.locally_valid()) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route publication has an invalid active configuration",
        "runtime_route_configuration_invalid", ErrorCode::kInvalidRequest);
  }

  std::vector<RouteReadinessPublicationRowDTO> readiness_rows;
  const bool reconciled = configuration.state == ActiveControlConfigurationStateDTO::kReconciled;
  if (reconciled) {
    auto projected = derive_route_readiness_projection(configuration, registrations);
    if (!projected.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(projected.error());
    }
    readiness_rows = std::move(projected.value());
  } else if (configuration.state !=
                 ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled ||
             !registrations.empty() || participant_resolver) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "unreconciled route publication cannot install runtime participants",
        "runtime_route_unreconciled_registration_present", ErrorCode::kInvalidRequest);
  }

  RuntimeRouteConfigurationReadbackDTO candidate{std::move(configuration),
                                                 std::move(readiness_rows), reconciled};
  std::map<ControlDomainId, RuntimeRouteConfigurationReadbackDTO> staged_readback;
  try {
    staged_readback.emplace(candidate.active_configuration.control_domain_id, candidate);
  } catch (...) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route readback could not be staged before publication",
        "runtime_route_readback_capacity_unavailable", ErrorCode::kInternalError);
  }
  auto staged_node = staged_readback.extract(staged_readback.begin());
  if (reconciled) {
    auto installed =
        reconfigure_runtime_participants(std::move(registrations), std::move(participant_resolver));
    if (!installed.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(installed.error());
    }
  }
  std::lock_guard submit_lock(submission_mutex_);
  if (reconciled) {
    auto current_rows =
        derive_route_readiness_projection(candidate.active_configuration, registrations_);
    if (!current_rows.ok() || current_rows.value() != candidate.readiness_rows) {
      return reject<RuntimeRouteConfigurationReadbackDTO>(
          "runtime registration changed before route publication readback",
          "runtime_route_publication_raced", ErrorCode::kInvariantViolation);
    }
  }
  {
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    auto publications =
        route_publication_identities(runtime_route_configuration_readbacks_, &candidate);
    auto publication_inventory =
        resource_slots_->replace_route_publication_inventory(std::move(publications));
    if (!publication_inventory.ok()) {
      return Result<RuntimeRouteConfigurationReadbackDTO>::Err(publication_inventory.error());
    }
    runtime_route_configuration_readbacks_.erase(candidate.active_configuration.control_domain_id);
    runtime_route_configuration_readbacks_.insert(std::move(staged_node));
    runtime_route_configuration_publication_required_.erase(
        candidate.active_configuration.control_domain_id);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(std::move(candidate));
}

Result<RuntimeRouteConfigurationReadbackDTO>
ResourceAddressedExecutionKernel::runtime_route_configuration_readback(
    const ControlDomainId& control_domain_id,
    const ActiveControlConfigurationId& active_configuration_id,
    const ActiveControlConfigurationGeneration active_configuration_generation) const {
  if (control_domain_id.empty() || active_configuration_id.empty() ||
      !active_configuration_generation.nonzero()) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route readback query has an invalid identity",
        "runtime_route_readback_query_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto found = runtime_route_configuration_readbacks_.find(control_domain_id);
  if (found == runtime_route_configuration_readbacks_.end() ||
      found->second.active_configuration.active_control_configuration_id !=
          active_configuration_id ||
      found->second.active_configuration.active_control_configuration_generation !=
          active_configuration_generation) {
    return reject<RuntimeRouteConfigurationReadbackDTO>(
        "runtime route configuration readback is absent or stale", "runtime_route_readback_missing",
        ErrorCode::kMissingReference);
  }
  return Result<RuntimeRouteConfigurationReadbackDTO>::Ok(found->second);
}

Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>
ResourceAddressedExecutionKernel::current_runtime_route_configuration(
    const ControlDomainId& control_domain_id) const {
  if (control_domain_id.empty()) {
    return reject<std::optional<RuntimeRouteConfigurationReadbackDTO>>(
        "runtime route current-fact query has an empty domain identity",
        "runtime_route_current_query_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto found = runtime_route_configuration_readbacks_.find(control_domain_id);
  if (found == runtime_route_configuration_readbacks_.end()) {
    return Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>::Ok(std::nullopt);
  }
  return Result<std::optional<RuntimeRouteConfigurationReadbackDTO>>::Ok(found->second);
}

Result<void> ResourceAddressedExecutionKernel::require_held_lifecycle_claim_locked(
    const LifecycleTransactionId& lifecycle_transaction_id) const {
  const auto claim = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kComponentLifecycle);
  if (lifecycle_transaction_id.empty() || !claim ||
      claim->intent.owner_transaction_id != lifecycle_transaction_id.value() ||
      claim->intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
    return reject<void>("component command readiness requires its exact held CCLC claim",
                        "component_readiness_claim_not_held");
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::require_idle_for_component_readiness_locked() const {
  // The exact lifecycle claim (or the Apply fence during composition) is
  // whole-cell and exclusive, so no command can be admitted meanwhile. Verify
  // that nothing was resident when it was taken.
  if (!active_operations_.empty() || operation_control_reconfiguration_busy_locked()) {
    return reject<void>("component command readiness changed under resident physical work",
                        "component_readiness_effects_not_settled");
  }
  const auto slots = resource_slots_->snapshot();
  if (std::any_of(slots.begin(), slots.end(), [](const auto& slot) {
        return slot.state != ResourceSlotState::kFree || slot.owner.has_value();
      })) {
    return reject<void>("component command readiness has reserved physical resources",
                        "component_readiness_resources_not_settled");
  }
  return Result<void>::Ok();
}

std::optional<ErrorDTO> ResourceAddressedExecutionKernel::component_command_refusal_locked(
    const RuntimeParticipantExecutionRegistration& registration) const {
  const auto& binding = registration.binding.value();
  if (!binding.control_path.has_value() ||
      !runtime_route_configuration_readbacks_.contains(binding.control_path->control_domain_id)) {
    // No route authority governs this domain, so there is no lifecycle
    // projection to consult; the route checks decide admission as before.
    return std::nullopt;
  }
  const auto found = component_command_readiness_.find(binding.controllable_component_id);
  const auto* readiness =
      registration.stop_readiness.has_value() ? &registration.stop_readiness->value() : nullptr;
  if (found == component_command_readiness_.end() || !found->second.command_eligible ||
      readiness == nullptr ||
      found->second.component_lifecycle_generation != readiness->component_lifecycle_generation) {
    ErrorDTO refusal = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                             "the component is not enabled for commands",
                             "component_command_not_eligible");
    refusal.details.emplace("controllable_component_id", binding.controllable_component_id.value());
    if (found != component_command_readiness_.end() &&
        found->second.component_lifecycle_generation.has_value()) {
      refusal.details.emplace(
          "published_component_lifecycle_generation",
          std::to_string(found->second.component_lifecycle_generation->value()));
    }
    if (readiness != nullptr) {
      refusal.details.emplace("participant_component_lifecycle_generation",
                              std::to_string(readiness->component_lifecycle_generation.value()));
    }
    return refusal;
  }
  return std::nullopt;
}

Result<void> ResourceAddressedExecutionKernel::require_component_routes_current_locked(
    const ControllableComponentId& controllable_component_id, const bool command_eligible) const {
  if (!command_eligible) {
    return Result<void>::Ok();
  }
  bool any_domain = false;
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.controllable_component_id != controllable_component_id ||
        !binding.control_path.has_value()) {
      continue;
    }
    any_domain = true;
    const auto& domain = binding.control_path->control_domain_id;
    const auto readback = runtime_route_configuration_readbacks_.find(domain);
    if (!registration.stop_readiness.has_value() ||
        runtime_route_configuration_publication_required_.contains(domain) ||
        readback == runtime_route_configuration_readbacks_.end() ||
        readback->second.active_configuration.state !=
            ActiveControlConfigurationStateDTO::kReconciled ||
        !readback->second.operation_admission_open ||
        readback->second.active_configuration.active_control_configuration_id !=
            binding.control_path->active_control_configuration_id ||
        readback->second.active_configuration.active_control_configuration_generation !=
            binding.control_path->active_control_configuration_generation) {
      ErrorDTO refusal = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                               "an enabled component requires every one of its routes to be current",
                               "component_readiness_route_not_current");
      refusal.details.emplace("control_domain_id", domain.value());
      return Result<void>::Err(std::move(refusal));
    }
  }
  if (!any_domain) {
    return reject<void>("an enabled component requires installed route and Stop readiness",
                        "component_readiness_participants_missing", ErrorCode::kMissingReference);
  }
  return Result<void>::Ok();
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::close_component_command_eligibility(
    const LifecycleTransactionId& lifecycle_transaction_id,
    const ControllableComponentId& controllable_component_id) {
  if (lifecycle_transaction_id.empty() || controllable_component_id.empty()) {
    return reject<ComponentCommandReadinessDTO>("component readiness closure identity is invalid",
                                                "component_readiness_identity_invalid",
                                                ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (auto held = require_held_lifecycle_claim_locked(lifecycle_transaction_id); !held.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(held.error());
  }
  std::lock_guard state_lock(state_mutex_);
  try {
    auto [entry, inserted] = component_command_readiness_.try_emplace(
        controllable_component_id,
        ComponentCommandReadinessDTO{controllable_component_id, std::nullopt, false});
    static_cast<void>(inserted);
    entry->second.command_eligible = false;
    return Result<ComponentCommandReadinessDTO>::Ok(entry->second);
  } catch (...) {
    return reject<ComponentCommandReadinessDTO>(
        "component readiness closure could not be recorded",
        "component_readiness_capacity_unavailable", ErrorCode::kInternalError);
  }
}

Result<ComponentCommandReadinessDTO>
ResourceAddressedExecutionKernel::publish_component_command_readiness(
    const ComponentCommandReadinessPublicationDTO& publication,
    RuntimeParticipantAdmissionResolver participant_resolver) {
  if (!publication.locally_valid()) {
    return reject<ComponentCommandReadinessDTO>("component readiness publication is invalid",
                                                "component_readiness_publication_invalid",
                                                ErrorCode::kInvalidRequest);
  }
  const auto& component = publication.controllable_component_id;
  // Each authority is proven positively; absence of another claim is never
  // permission to enable commands.
  const auto authorize_locked = [&]() -> Result<void> {
    if (publication.authority == ComponentReadinessAuthorityDTO::kLifecycleClaim) {
      return require_held_lifecycle_claim_locked(*publication.lifecycle_transaction_id);
    }
    if (cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kComponentLifecycle)) {
      return reject<void>("composition cannot publish readiness under a lifecycle claim",
                          "component_readiness_lifecycle_claim_held");
    }
    if (publication.authority == ComponentReadinessAuthorityDTO::kRestartExclusion) {
      if (!cell_admission_owner_->route_recovery_pending()) {
        return reject<void>("restart readiness requires the Kernel route-recovery exclusion",
                            "component_readiness_restart_exclusion_not_held");
      }
      return Result<void>::Ok();
    }
    // Parent Apply: this Apply's own held fence (not merely any fence) must
    // cover every resource of the component while readiness changes.
    std::vector<ResourceId> resources;
    for (const auto& registration : registrations_) {
      if (registration.binding.value().controllable_component_id != component) continue;
      for (const auto& resource : registration.binding.value().resources) {
        resources.push_back(resource.resource_id);
      }
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()), resources.end());
    if (config_.operation_admission_gate == nullptr || resources.empty() ||
        !config_.operation_admission_gate->exact_fence_covers(*publication.parent_apply_fence_id,
                                                              resources)) {
      return reject<void>("Apply readiness requires the held parent fence over the component",
                          "component_readiness_apply_fence_not_held");
    }
    return Result<void>::Ok();
  };

  // Close first and re-seal outside the short serialization boundary. Any
  // failure below leaves the component command-closed.
  std::vector<RuntimeParticipantExecutionRegistration> rebound;
  bool generation_changed = false;
  {
    std::lock_guard submit_lock(submission_mutex_);
    std::lock_guard admission_lock(*admission_mutex_);
    if (auto authorized = authorize_locked(); !authorized.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(authorized.error());
    }
    std::lock_guard state_lock(state_mutex_);
    // Composition reads durable facts before publishing. A newer lifecycle
    // result published meanwhile must never be overtaken by that read.
    if (const auto current = component_command_readiness_.find(component);
        publication.authority != ComponentReadinessAuthorityDTO::kLifecycleClaim &&
        current != component_command_readiness_.end() &&
        current->second.component_lifecycle_generation.has_value() &&
        current->second.component_lifecycle_generation->value() >
            publication.component_lifecycle_generation.value()) {
      return reject<ComponentCommandReadinessDTO>(
          "composition readiness is older than the published component generation",
          "component_readiness_generation_stale", ErrorCode::kInvalidStateTransition);
    }
    if (auto idle = require_idle_for_component_readiness_locked(); !idle.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(idle.error());
    }
    if (auto routes = require_component_routes_current_locked(component,
                                                              publication.command_eligible);
        !routes.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(routes.error());
    }
    try {
      auto [entry, inserted] = component_command_readiness_.try_emplace(
          component, ComponentCommandReadinessDTO{component, std::nullopt, false});
      static_cast<void>(inserted);
      entry->second.command_eligible = false;
      rebound = registrations_;
    } catch (...) {
      return reject<ComponentCommandReadinessDTO>(
          "component readiness publication could not be staged",
          "component_readiness_capacity_unavailable", ErrorCode::kInternalError);
    }
  }
  for (auto& registration : rebound) {
    if (registration.binding.value().controllable_component_id != component ||
        !registration.stop_readiness.has_value()) {
      continue;
    }
    if (registration.stop_readiness->value().component_lifecycle_generation ==
        publication.component_lifecycle_generation) {
      continue;
    }
    auto refreshed = rebind_runtime_participant_stop_readiness_generation(
        *registration.stop_readiness, publication.component_lifecycle_generation);
    if (!refreshed.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(refreshed.error());
    }
    registration.stop_readiness = std::move(refreshed.value());
    generation_changed = true;
  }
  if (generation_changed) {
    if (!participant_resolver && config_.participant_resolver) {
      return reject<ComponentCommandReadinessDTO>(
          "a re-sealed component requires its rebuilt participant resolver",
          "component_readiness_resolver_missing", ErrorCode::kMissingReference);
    }
    // Reuses the same engines and route configurations: an unchanged
    // configuration keeps its route readback and generation.
    auto reconfigured = reconfigure_runtime_participants_impl(
        std::move(rebound), std::move(participant_resolver), false, nullptr);
    if (!reconfigured.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(reconfigured.error());
    }
  }

  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard admission_lock(*admission_mutex_);
  if (auto authorized = authorize_locked(); !authorized.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(authorized.error());
  }
  std::lock_guard state_lock(state_mutex_);
  if (auto idle = require_idle_for_component_readiness_locked(); !idle.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(idle.error());
  }
  std::set<ControlDomainId> component_domains;
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.controllable_component_id != component || !binding.control_path.has_value()) {
      continue;
    }
    if (registration.stop_readiness.has_value() &&
        registration.stop_readiness->value().component_lifecycle_generation !=
            publication.component_lifecycle_generation) {
      return reject<ComponentCommandReadinessDTO>(
          "component participants changed during readiness publication",
          "component_readiness_generation_raced", ErrorCode::kInvariantViolation);
    }
    component_domains.insert(binding.control_path->control_domain_id);
  }
  if (auto routes = require_component_routes_current_locked(component,
                                                            publication.command_eligible);
      !routes.ok()) {
    return Result<ComponentCommandReadinessDTO>::Err(routes.error());
  }
  // Refresh each affected route's readiness rows to the published generation
  // without changing its configuration or route generation.
  std::map<ControlDomainId, std::vector<RouteReadinessPublicationRowDTO>> refreshed_rows;
  for (const auto& domain : component_domains) {
    const auto readback = runtime_route_configuration_readbacks_.find(domain);
    if (readback == runtime_route_configuration_readbacks_.end() ||
        readback->second.active_configuration.state !=
            ActiveControlConfigurationStateDTO::kReconciled ||
        !readback->second.operation_admission_open ||
        runtime_route_configuration_publication_required_.contains(domain)) {
      continue;
    }
    auto rows = derive_route_readiness_projection(readback->second.active_configuration,
                                                  registrations_);
    if (!rows.ok()) {
      return Result<ComponentCommandReadinessDTO>::Err(rows.error());
    }
    refreshed_rows.emplace(domain, std::move(rows.value()));
  }
  for (auto& [domain, rows] : refreshed_rows) {
    runtime_route_configuration_readbacks_.at(domain).readiness_rows = std::move(rows);
  }
  auto& entry = component_command_readiness_.at(component);
  entry.component_lifecycle_generation = publication.component_lifecycle_generation;
  entry.command_eligible = publication.command_eligible;
  return Result<ComponentCommandReadinessDTO>::Ok(entry);
}

Result<ComponentCommandReadinessDTO> ResourceAddressedExecutionKernel::component_command_readiness(
    const ControllableComponentId& controllable_component_id) const {
  if (controllable_component_id.empty()) {
    return reject<ComponentCommandReadinessDTO>("component readiness query identity is invalid",
                                                "component_readiness_identity_invalid",
                                                ErrorCode::kInvalidRequest);
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  const auto found = component_command_readiness_.find(controllable_component_id);
  return Result<ComponentCommandReadinessDTO>::Ok(
      found == component_command_readiness_.end()
          ? ComponentCommandReadinessDTO{controllable_component_id, std::nullopt, false}
          : found->second);
}

bool ResourceAddressedExecutionKernel::participant_engine_in_use(
    const IResolvedParticipantExecutionEngine* engine) const {
  if (engine == nullptr) {
    return false;
  }
  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  return std::any_of(
             active_operations_.begin(), active_operations_.end(),
             [&](const auto& item) { return item.second->participant_for(engine) != nullptr; }) ||
         std::any_of(registrations_.begin(), registrations_.end(),
                     [&](const auto& registration) { return registration.engine == engine; });
}

bool ResourceAddressedExecutionKernel::operation_control_reconfiguration_busy_locked() const {
  return operation_control_reconfiguration_in_progress_ ||
         (operation_control_executor_ != nullptr &&
          operation_control_executor_->has_active_calls()) ||
         std::any_of(active_operations_.begin(), active_operations_.end(), [](const auto& item) {
           return item.second->pending_control.has_value() ||
                  item.second->ordinary_commit_in_flight ||
                  item.second->recovery_effect_in_flight ||
                  item.second->coordinated_cancellation_in_flight ||
                  std::any_of(item.second->participants.begin(), item.second->participants.end(),
                              [](const auto& participant) {
                                return participant.prepare_in_flight ||
                                       participant.prepared_advance_in_flight;
                              });
         });
}

std::vector<IResolvedParticipantExecutionEngine*>
ResourceAddressedExecutionKernel::operation_control_engines_locked(
    const std::vector<RuntimeParticipantExecutionRegistration>& registrations) const {
  auto engines = distinct_execution_engines(registrations);
  // A replaced provider remains responsible for cleanup of its resident
  // operations. Retiring an unrelated provider must not remove those lanes.
  for (const auto& [operation_id, operation] : active_operations_) {
    (void)operation_id;
    for (const auto& participant : operation->participants) {
      if (std::find(engines.begin(), engines.end(), participant.engine) == engines.end())
        engines.push_back(participant.engine);
    }
  }
  return engines;
}

Result<void> ResourceAddressedExecutionKernel::release_retired_engine(
    IResolvedParticipantExecutionEngine* engine) {
  if (engine == nullptr) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kInvalidRequest)),
                                   "retired participant engine is absent",
                                   "retired_participant_engine_absent"));
  }
  std::lock_guard submit_lock(submission_mutex_);
  const auto subscription =
      std::find_if(engine_subscriptions_.begin(), engine_subscriptions_.end(),
                   [&](const EngineSubscription& item) { return item.engine == engine; });
  if (subscription == engine_subscriptions_.end()) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kMissingReference)),
                                   "retired participant engine has no subscription",
                                   "retired_participant_engine_subscription_absent"));
  }
  bool control_reconfiguration_busy = false;
  std::vector<IResolvedParticipantExecutionEngine*> control_engines;
  {
    std::lock_guard state_lock(state_mutex_);
    if (std::any_of(
            active_operations_.begin(), active_operations_.end(),
            [&](const auto& item) { return item.second->participant_for(engine) != nullptr; }) ||
        std::any_of(registrations_.begin(), registrations_.end(),
                    [&](const auto& registration) { return registration.engine == engine; })) {
      return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                     "retired participant engine remains in use",
                                     "retired_participant_engine_in_use"));
    }
    control_reconfiguration_busy = operation_control_reconfiguration_busy_locked();
    if (!control_reconfiguration_busy) {
      control_engines = operation_control_engines_locked(registrations_);
      operation_control_reconfiguration_in_progress_ = true;
    }
  }
  if (control_reconfiguration_busy) {
    return Result<void>::Err(error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                   "operation-control work prevents engine retirement",
                                   "operation_control_reconfiguration_busy"));
  }

  std::unique_ptr<BoundedOperationControlExecutor> retired_control_executor;
  {
    std::lock_guard state_lock(state_mutex_);
    if (control_engines.empty()) {
      retired_control_executor = std::move(operation_control_executor_);
    } else if (operation_control_executor_ == nullptr ||
               operation_control_executor_->ReconfigureEngines(std::move(control_engines)) !=
                   OperationControlLaneUpdateStatus::kApplied) {
      operation_control_reconfiguration_in_progress_ = false;
      return Result<void>::Err(error(
          std::string(to_string(ErrorCode::kExecutionRejected)),
          "retired engine cleanup could not retain the current operation-control lanes",
          "operation_control_reconfiguration_failed"));
    }
    operation_control_reconfiguration_in_progress_ = false;
  }
  if (retired_control_executor != nullptr) {
    retired_control_executor->CloseIngress();
    if (!retired_control_executor->DrainFor(std::chrono::seconds(5))) {
      std::terminate();
    }
    retired_control_executor.reset();
  }
  engine->unsubscribe_events(subscription->handle);
  engine_subscriptions_.erase(subscription);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_preview_participant(
    const OperationIntentDTO& intent, const RuntimeParticipantExecutionRegistration& registration) {
  // The caller holds ordinary submission ordering, never Stop's mutex across
  // capture resolution or allocation. Preview grants no execution authority.
  std::optional<ResolvedManualProgramHandover> handover;
  if (registration.intrinsic_direct_control) {
    auto current = resolve_manual_program_handover(intent);
    if (!current.ok()) return Result<void>::Err(current.error());
    handover = std::move(current.value());
    std::optional<ManualProgramHandoverContextDTO> expected;
    if (handover) {
      const auto& capture = handover->capture;
      expected = ManualProgramHandoverContextDTO{capture.program_runner_owner_epoch,
                                                 capture.program_run_id, capture.claim_generation,
                                                 handover->selection_revision};
    }
    if (expected != intent.manual_handover)
      return reject<void>("manual preview must name the current held Program context",
                          "manual_handover_scope_stale");
  } else if (intent.manual_handover) {
    return reject<void>("manual preview requires an intrinsic direct-control target",
                        "direct_control_product_mismatch");
  }
  std::vector<ResourceId> resources;
  resources.reserve(registration.binding.value().resources.size());
  for (const auto& resource : registration.binding.value().resources)
    resources.push_back(resource.resource_id);
  std::sort(resources.begin(), resources.end());
  std::lock_guard state_lock(state_mutex_);
  if (motion_admission_closed_ || motion_admission_unreconciled_)
    return reject<void>("preview is unavailable while motion admission is closed",
                        "motion_admission_closed", ErrorCode::kInvalidStateTransition);
  if (handover) {
    const auto held = validate_manual_program_handover_locked(*handover);
    if (!held.ok()) return held;
  }
  for (const auto& [id, operation] : active_operations_) {
    (void)id;
    // Effect-free preview of a correction for a held, never-dispatched
    // failure is the recovery workflow. Execution admission stays exclusive.
    if (operation->held_without_physical_effect()) continue;
    for (const auto& participant : operation->participants) {
      if (participant.engine == registration.engine ||
          std::any_of(participant.resources.begin(), participant.resources.end(),
                      [&](const auto& resource) {
                        return std::binary_search(resources.begin(), resources.end(), resource);
                      })) {
        return reject<void>(
            "preview target still owns an unsettled operation",
            std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)
                ? "motion_preview_operation_busy"
                : "capability_command_preview_operation_busy");
      }
    }
  }
  return Result<void>::Ok();
}

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

Result<std::unique_ptr<IAddressedMotionPreviewSession>>
ResourceAddressedExecutionKernel::begin_motion_preview_session(
    const OperationIntentDTO& primary_motion) {
  std::unique_lock submission_lock(submission_mutex_);
  auto validation = validate_operation_intent(primary_motion);
  if (!validation.ok()) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(validation.error());
  }
  if (primary_motion.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(
        error("stale_request_owner_epoch", "preview request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  if (primary_motion.participants.size() != 1U ||
      primary_motion.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      !std::holds_alternative<MotionParticipantIntentDTO>(
          primary_motion.participants.front().payload)) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "I6 preview requires one independent motion participant",
        "i6_preview_primary_motion_invalid", ErrorCode::kInvalidRequest);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(primary_motion.request_context.request_id.value())
            .has_value()) {
      return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
  }
  auto resolved = resolve_registration(primary_motion, 0U);
  if (!resolved.ok()) {
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(resolved.error());
  }
  const auto available = validate_preview_participant(primary_motion, resolved.value());
  if (!available.ok())
    return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Err(available.error());
  auto* engine = resolved.value().engine;
  auto* motion = dynamic_cast<IMotionPreview*>(engine);
  if (motion == nullptr) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "resolved participant has no non-dispatching motion preview",
        "motion_preview_engine_unavailable", ErrorCode::kUnsupportedCapability);
  }
  if (!resolved.value().participant_policy.has_value()) {
    return reject<std::unique_ptr<IAddressedMotionPreviewSession>>(
        "resolved participant has no sealed motion policy",
        "runtime_participant_policy_missing", ErrorCode::kMissingReference);
  }
  std::unique_ptr<IAddressedMotionPreviewSession> session = std::make_unique<PreviewSession>(
      *this, std::move(resolved.value().binding), std::move(*resolved.value().participant_policy),
      *engine, *motion, std::move(submission_lock));
  return Result<std::unique_ptr<IAddressedMotionPreviewSession>>::Ok(std::move(session));
}

Result<CapabilityCommandPreviewResultDTO>
ResourceAddressedExecutionKernel::preview_capability_command(const OperationIntentDTO& intent) {
  std::lock_guard submit_lock(submission_mutex_);
  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(error(
        "stale_request_owner_epoch", "capability-command preview names a stale Kernel owner epoch",
        "stale_request_owner_epoch"));
  }
  if (intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      !std::holds_alternative<CapabilityCommandIntentDTO>(intent.participants.front().payload)) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "I6 command preview requires one independent capability command",
        "i6_preview_capability_command_invalid", ErrorCode::kInvalidRequest);
  }
  auto resolved = resolve_registration(intent, 0U);
  if (!resolved.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(resolved.error());
  }
  const auto available = validate_preview_participant(intent, resolved.value());
  if (!available.ok()) return Result<CapabilityCommandPreviewResultDTO>::Err(available.error());
  auto* engine = resolved.value().engine;
  if (engine == nullptr) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "resolved command participant has no registered subordinate engine",
        "runtime_participant_engine_missing", ErrorCode::kInvariantViolation);
  }
  const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
  auto request = build_command_request(command, resolved.value().binding.value());
  if (!request.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(request.error());
  }
  auto preflight = engine->validate_gripper_command(request.value());
  if (!preflight.ok()) {
    return Result<CapabilityCommandPreviewResultDTO>::Err(preflight.error());
  }
  const auto status = engine->get_gripper_status(request.value().gripper_id);
  if (status.state == GripperCommandState::kExecuting) {
    return reject<CapabilityCommandPreviewResultDTO>(
        "gripper command preview cannot validate a busy target", "gripper_busy");
  }
  return Result<CapabilityCommandPreviewResultDTO>::Ok(CapabilityCommandPreviewResultDTO{
      resolved.value().binding.value(), std::move(request.value())});
}

const RuntimeParticipantExecutionRegistration* ResourceAddressedExecutionKernel::registration_for(
    const RuntimeParticipantBinding& binding) const noexcept {
  const auto found = std::find_if(registrations_.begin(), registrations_.end(),
                                  [&](const RuntimeParticipantExecutionRegistration& registration) {
                                    return registration.binding.value() == binding.value();
                                  });
  return found == registrations_.end() ? nullptr : &*found;
}

Result<RuntimeParticipantExecutionRegistration>
ResourceAddressedExecutionKernel::resolve_registration(const OperationIntentDTO& operation,
                                                       const std::size_t participant_index) const {
  if (!config_.participant_resolver) {
    auto binding = registry_.resolve(operation, participant_index);
    if (!binding.ok()) {
      return Result<RuntimeParticipantExecutionRegistration>::Err(binding.error());
    }
    const auto* registration = registration_for(binding.value());
    if (registration == nullptr || registration->engine == nullptr) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "resolved participant has no registered subordinate engine",
          "runtime_participant_engine_missing", ErrorCode::kInvariantViolation);
    }
    return Result<RuntimeParticipantExecutionRegistration>::Ok(*registration);
  }

  Result<RuntimeParticipantExecutionRegistration> resolved =
      Result<RuntimeParticipantExecutionRegistration>::Err(
          error(std::string(to_string(ErrorCode::kInternalError)),
                "product-backed participant resolver did not return a result",
                "runtime_participant_resolver_failed"));
  try {
    resolved = config_.participant_resolver(operation, participant_index);
  } catch (const std::exception& exception) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              std::string("product-backed participant resolution failed: ") + exception.what(),
              "runtime_participant_resolver_failed"));
  } catch (...) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(error(
        std::string(to_string(ErrorCode::kInvariantViolation)),
        "product-backed participant resolution failed", "runtime_participant_resolver_failed"));
  }
  if (!resolved.ok()) {
    return resolved;
  }
  auto validated = RuntimeParticipantRegistry::validate_resolved(operation, participant_index,
                                                                 resolved.value().binding);
  if (!validated.ok()) {
    return Result<RuntimeParticipantExecutionRegistration>::Err(validated.error());
  }
  const auto engine_is_pinned =
      std::any_of(registrations_.begin(), registrations_.end(), [&](const auto& item) {
        return item.engine != nullptr && item.engine == resolved.value().engine;
      });
  if (!engine_is_pinned) {
    return reject<RuntimeParticipantExecutionRegistration>(
        "product-backed participant selected an unpinned subordinate engine",
        "runtime_participant_engine_unpinned", ErrorCode::kInvariantViolation);
  }
  const auto& inventory = registry_.resource_inventory();
  for (const auto& resource : resolved.value().binding.value().resources) {
    if (!std::binary_search(inventory.begin(), inventory.end(), resource.resource_id)) {
      return reject<RuntimeParticipantExecutionRegistration>(
          "product-backed participant widened the active resource inventory",
          "runtime_participant_resource_outside_inventory", ErrorCode::kInvariantViolation);
    }
  }
  return resolved;
}

Result<ResourceAddressedExecutionKernel::RuntimeScaleControlResolution>
ResourceAddressedExecutionKernel::resolve_runtime_scale_control_target_locked(
    const RuntimeScaleControlSelectorDTO& selector) const {
  if (selector.program_binding_id.empty() || selector.component_role_id.empty() ||
      selector.target_role_id.empty()) {
    return reject<RuntimeScaleControlResolution>("runtime-scale control selector is incomplete",
                                                 "runtime_scale_control_selector_invalid",
                                                 ErrorCode::kInvalidRequest);
  }

  std::optional<RuntimeScaleControlResolution> selected;
  bool target_found = false;
  for (const auto& registration : registrations_) {
    const auto& binding = registration.binding.value();
    if (binding.program_binding_id != selector.program_binding_id ||
        binding.component_role_id != selector.component_role_id ||
        binding.target_role_id != selector.target_role_id) {
      continue;
    }
    target_found = true;
    // The persistent operator setting belongs to the motion binding. A
    // qualified operation-scale capability authorizes a command to an already
    // addressed operation; it does not introduce another setting/authority.
    if (binding.capability_id.value() != "execution.trajectory.execute" ||
        !binding.control_path.has_value() ||
        !binding.control_path->runtime_scale_qualification.has_value()) {
      continue;
    }
    const auto& control = *binding.control_path;
    const auto& qualification = *control.runtime_scale_qualification;
    RuntimeScaleControlResolution candidate{selector,
                                            control.control_domain_id,
                                            control.control_domain_revision,
                                            control.control_binding_id,
                                            control.control_binding_revision,
                                            qualification.scaling_domain_id,
                                            control.active_control_configuration_id,
                                            control.active_control_configuration_generation,
                                            qualification.coordinated_barrier_id};
    if (selected.has_value() && *selected != candidate) {
      return reject<RuntimeScaleControlResolution>(
          "runtime-scale target resolves to more than one control scope",
          "runtime_scale_control_scope_ambiguous", ErrorCode::kInvalidStateTransition);
    }
    selected = std::move(candidate);
  }
  // Managed runtimes retain physical registrations independently of any
  // authored Program. Resolve a captured Program role through its immutable
  // binding to that same physical target instead of installing aliases for
  // every saved Program in the execution host.
  if (!selected.has_value() && program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
      program_cycle_claim_run_->capture->binding &&
      program_cycle_claim_run_->capture->program_binding_id == selector.program_binding_id) {
    const auto& capture = *program_cycle_claim_run_->capture;
    const auto target =
        std::find_if(capture.binding->target_role_bindings.begin(),
                     capture.binding->target_role_bindings.end(), [&](const auto& binding) {
                       return binding.component_role_id == selector.component_role_id &&
                              binding.target_role_id == selector.target_role_id;
                     });
    if (target != capture.binding->target_role_bindings.end()) {
      target_found = true;
      for (const auto& registration : registrations_) {
        const auto& binding = registration.binding.value();
        if (binding.controllable_component_id != target->controllable_component_id ||
            binding.command_target_id != target->command_target_id ||
            binding.capability_id.value() != "execution.trajectory.execute" ||
            !binding.control_path || !binding.control_path->runtime_scale_qualification)
          continue;
        const auto& control = *binding.control_path;
        const auto& qualification = *control.runtime_scale_qualification;
        RuntimeScaleControlResolution candidate{selector,
                                                control.control_domain_id,
                                                control.control_domain_revision,
                                                control.control_binding_id,
                                                control.control_binding_revision,
                                                qualification.scaling_domain_id,
                                                control.active_control_configuration_id,
                                                control.active_control_configuration_generation,
                                                qualification.coordinated_barrier_id};
        if (selected && *selected != candidate) {
          return reject<RuntimeScaleControlResolution>(
              "captured runtime-scale target resolves to more than one control scope",
              "runtime_scale_control_scope_ambiguous", ErrorCode::kInvalidStateTransition);
        }
        selected = std::move(candidate);
      }
    }
  }
  if (!selected.has_value()) {
    return reject<RuntimeScaleControlResolution>(
        target_found ? "runtime-scale target has no qualified control binding"
                     : "runtime-scale target is absent from the current Program Binding",
        target_found ? "runtime_scale_control_unsupported" : "runtime_scale_control_target_missing",
        target_found ? ErrorCode::kUnsupportedCapability : ErrorCode::kMissingReference);
  }
  return Result<RuntimeScaleControlResolution>::Ok(std::move(*selected));
}

Result<ResourceAddressedExecutionKernel::RuntimeScaleControlScope>
ResourceAddressedExecutionKernel::resolve_runtime_scale_control_locked(
    const RuntimeScaleControlSelectorDTO& selector) const {
  RuntimeScaleControlScope scope{selector, {}};
  if (selector.group_targets.empty()) {
    auto member = resolve_runtime_scale_control_target_locked(selector);
    if (!member.ok()) return Result<RuntimeScaleControlScope>::Err(member.error());
    scope.members.push_back(std::move(member.value()));
  } else {
    if (!selector.component_role_id.empty() || !selector.target_role_id.empty() ||
        selector.group_targets.size() < 2U ||
        selector.group_targets.size() > kMaxOperationParticipants) {
      return reject<RuntimeScaleControlScope>(
          "group velocity requires one complete bounded target set",
          "runtime_scale_control_selector_invalid", ErrorCode::kInvalidRequest);
    }
    scope.members.reserve(selector.group_targets.size());
    for (const auto& target : selector.group_targets) {
      auto member = resolve_runtime_scale_control_target_locked(
          {selector.program_binding_id, target.component_role_id, target.target_role_id});
      if (!member.ok()) return Result<RuntimeScaleControlScope>::Err(member.error());
      scope.members.push_back(std::move(member.value()));
    }
    std::sort(scope.members.begin(), scope.members.end(),
              [](const auto& a, const auto& b) { return a.key() < b.key(); });
    if (std::adjacent_find(scope.members.begin(), scope.members.end(),
                           [](const auto& a, const auto& b) { return a.same_control_scope(b); }) !=
        scope.members.end()) {
      return reject<RuntimeScaleControlScope>("group velocity targets alias the same control scope",
                                              "runtime_scale_group_member_duplicate",
                                              ErrorCode::kInvalidRequest);
    }
  }
  return Result<RuntimeScaleControlScope>::Ok(std::move(scope));
}

double ResourceAddressedExecutionKernel::runtime_scale_setting_for_binding_locked(
    const RuntimeParticipantBindingDTO& binding) const noexcept {
  if (!binding.control_path.has_value() ||
      !binding.control_path->runtime_scale_qualification.has_value()) {
    return 1.0;
  }
  const auto& control = *binding.control_path;
  const auto& qualification = *control.runtime_scale_qualification;
  const RuntimeScaleControlResolution resolution{
      RuntimeScaleControlSelectorDTO{binding.program_binding_id, binding.component_role_id,
                                     binding.target_role_id},
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      qualification.scaling_domain_id,
      control.active_control_configuration_id,
      control.active_control_configuration_generation};
  const auto found =
      std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                   [&](const RuntimeScaleControlSetting& setting) {
                     return setting.resolution.members.size() == 1U &&
                            setting.resolution.members.front().same_control_scope(resolution);
                   });
  return found == runtime_scale_control_settings_.end() ? 1.0 : found->requested_scale;
}

Result<RuntimeScaleControlStatusDTO> ResourceAddressedExecutionKernel::get_runtime_scale_control(
    const RuntimeScaleControlSelectorDTO& selector) const {
  std::lock_guard state_lock(state_mutex_);
  return runtime_scale_control_locked(selector, false);
}

Result<RuntimeScaleControlStatusDTO>
ResourceAddressedExecutionKernel::present_runtime_scale_control(
    const RuntimeScaleControlSelectorDTO& selector) const {
  std::lock_guard state_lock(state_mutex_);
  return runtime_scale_control_locked(selector, true);
}

Result<RuntimeScaleControlStatusDTO> ResourceAddressedExecutionKernel::runtime_scale_control_locked(
    const RuntimeScaleControlSelectorDTO& selector, const bool present_group) const {
  const auto resolved = resolve_runtime_scale_control_locked(selector);
  if (!resolved.ok()) {
    return Result<RuntimeScaleControlStatusDTO>::Ok(
        RuntimeScaleControlStatusDTO{selector, RuntimeScaleControlGeneration(0U), 1.0, false, true,
                                     std::nullopt,  // control_domain_id
                                     std::nullopt,  // control_domain_revision
                                     std::nullopt,  // control_binding_id
                                     std::nullopt,  // control_binding_revision
                                     std::nullopt,  // scaling_domain_id
                                     std::nullopt,  // active_control_configuration_id
                                     std::nullopt,  // active_control_configuration_generation
                                     std::nullopt,  // active_operation_id
                                     std::nullopt,  // active_operation_revision
                                     std::nullopt,  // active_operation_scale_generation
                                     std::nullopt,  // effective_scale
                                     std::nullopt,  // application_state
                                     resolved.error()});
  }
  const auto& scope = resolved.value();
  const bool group = scope.members.size() > 1U;
  const auto setting =
      std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                   [&](const RuntimeScaleControlSetting& candidate) {
                     return candidate.resolution.same_control_scope(scope);
                   });
  const auto generation = setting == runtime_scale_control_settings_.end()
                              ? RuntimeScaleControlGeneration(0U)
                              : setting->generation;
  const auto requested_scale =
      setting == runtime_scale_control_settings_.end() ? 1.0 : setting->requested_scale;
  const auto member_effective_scale = [&](const RuntimeScaleControlResolution& member) {
    const auto effective = std::find_if(
        runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
        [&](const RuntimeScaleControlSetting& candidate) {
          return candidate.resolution.members.size() == 1U &&
                 candidate.resolution.members.front().same_control_scope(member);
        });
    return effective == runtime_scale_control_settings_.end() ? 1.0
                                                              : effective->requested_scale;
  };

  const auto matches_scope = [&](const RuntimeParticipantBindingDTO& binding) {
    if (!binding.control_path || !binding.control_path->runtime_scale_qualification) return false;
    const auto& control = *binding.control_path;
    const auto& qualification = *control.runtime_scale_qualification;
    return std::any_of(scope.members.begin(), scope.members.end(), [&](const auto& member) {
      return control.control_domain_id == member.control_domain_id &&
             control.control_domain_revision == member.control_domain_revision &&
             control.control_binding_id == member.control_binding_id &&
             control.control_binding_revision == member.control_binding_revision &&
             qualification.scaling_domain_id == member.scaling_domain_id &&
             control.active_control_configuration_id == member.active_control_configuration_id &&
             control.active_control_configuration_generation ==
                 member.active_control_configuration_generation;
    });
  };
  std::vector<const ActiveOperation*> active;
  for (const auto& [operation_id, operation] : active_operations_) {
    static_cast<void>(operation_id);
    if (terminal(operation->state.state)) {
      continue;
    }
    bool matches = false;
    for (const auto& participant : operation->participants) {
      const auto& binding = participant.binding.value();
      if (participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
          !binding.control_path.has_value() ||
          !binding.control_path->runtime_scale_qualification.has_value()) {
        continue;
      }
      matches = matches_scope(binding);
      if (matches) {
        break;
      }
    }
    if (!matches) {
      continue;
    }
    active.push_back(operation.get());
  }

  if (present_group && !group && active.size() == 1U &&
      active.front()->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
      active.front()->participants.size() > 1U &&
      std::all_of(active.front()->participants.begin(), active.front()->participants.end(),
                  [](const auto& row) {
                    return row.payload_kind == OperationParticipantPayloadKind::kMotion;
                  })) {
    RuntimeScaleControlSelectorDTO group_selector{active.front()->intent.program_binding_id,
                                                  ComponentRoleId(""), TargetRoleId("")};
    for (const auto& participant : active.front()->participants) {
      const auto& binding = participant.binding.value();
      group_selector.group_targets.push_back({binding.component_role_id, binding.target_role_id});
    }
    return runtime_scale_control_locked(group_selector, false);
  }

  // An ordinary execution group consists of independent operations. Present
  // its one authoritative velocity setting when the selected target belongs
  // to exactly one current multi-motion group. The target set is derived from
  // the sealed Program capture; the browser never invents group membership.
  if (present_group && !group && program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
      !active.empty()) {
    const auto& capture = *program_cycle_claim_run_->capture;
    std::optional<std::uint32_t> selected_node;
    for (const auto* operation : active) {
      for (std::size_t participant_index = 0;
           participant_index < operation->participants.size() &&
           participant_index < operation->intent.participants.size();
           ++participant_index) {
        const auto& participant = operation->participants[participant_index];
        if (participant.binding.value().component_role_id != selector.component_role_id ||
            participant.binding.value().target_role_id != selector.target_role_id)
          continue;
        const auto& execution_scope =
            operation->intent.participants[participant_index].execution_scope;
        if (!execution_scope) continue;
        const auto node =
            std::lower_bound(capture.table->nodes.begin(), capture.table->nodes.end(),
                             execution_scope->node_instance_id,
                             [](const auto& row, const auto& id) { return row.instance_id < id; });
        if (node != capture.table->nodes.end() &&
            node->instance_id == execution_scope->node_instance_id) {
          selected_node = static_cast<std::uint32_t>(node - capture.table->nodes.begin());
          break;
        }
      }
      if (selected_node) break;
    }
    const ProgramExecutionSelectionDTO* selected_group = nullptr;
    if (selected_node) {
      for (const auto& candidate : capture.table->named_selections) {
        if (!std::binary_search(candidate.node_indices.begin(), candidate.node_indices.end(),
                                *selected_node))
          continue;
        if (selected_group != nullptr) {
          selected_group = nullptr;
          break;
        }
        selected_group = &candidate;
      }
    }
    if (selected_group != nullptr) {
      RuntimeScaleControlSelectorDTO group_selector{capture.program_binding_id, ComponentRoleId(""),
                                                    TargetRoleId("")};
      for (const auto node_index : selected_group->node_indices) {
        const auto& node = capture.table->nodes[node_index];
        for (const auto& effect : node.target_effects) {
          if (!effect.effectful ||
              effect.capability_id != CapabilityId("execution.trajectory.execute"))
            continue;
          const auto target = std::find_if(
              capture.binding->target_role_bindings.begin(),
              capture.binding->target_role_bindings.end(),
              [&](const auto& binding) { return binding.target_role_id == effect.target_role_id; });
          if (target == capture.binding->target_role_bindings.end()) continue;
          group_selector.group_targets.push_back(
              {target->component_role_id, target->target_role_id});
        }
      }
      std::sort(group_selector.group_targets.begin(), group_selector.group_targets.end(),
                [](const auto& left, const auto& right) {
                  return std::tie(left.component_role_id, left.target_role_id) <
                         std::tie(right.component_role_id, right.target_role_id);
                });
      group_selector.group_targets.erase(
          std::unique(group_selector.group_targets.begin(), group_selector.group_targets.end()),
          group_selector.group_targets.end());
      if (group_selector.group_targets.size() > 1U)
        return runtime_scale_control_locked(group_selector, false);
    }
  }

  const auto& first = scope.members.front();
  RuntimeScaleControlStatusDTO status{selector,
                                      generation,
                                      requested_scale,
                                      true,
                                      false,
                                      first.control_domain_id,
                                      first.control_domain_revision,
                                      first.control_binding_id,
                                      first.control_binding_revision,
                                      first.scaling_domain_id,
                                      first.active_control_configuration_id,
                                      first.active_control_configuration_generation,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt};
  if (group) {
    status.control_domain_id.reset();
    status.control_domain_revision.reset();
    status.control_binding_id.reset();
    status.control_binding_revision.reset();
    status.scaling_domain_id.reset();
    status.active_control_configuration_id.reset();
    status.active_control_configuration_generation.reset();
    for (const auto& member : scope.members) {
      status.group_members.push_back(
          {{member.selector.component_role_id, member.selector.target_role_id},
           member.control_domain_id,
           member.control_domain_revision,
           member.control_binding_id,
           member.control_binding_revision,
           member.scaling_domain_id,
           member.active_control_configuration_id,
           member.active_control_configuration_generation});
    }
    const auto first_effective = member_effective_scale(scope.members.front());
    if (std::all_of(scope.members.begin(), scope.members.end(), [&](const auto& member) {
          return std::abs(member_effective_scale(member) - first_effective) <= 1e-12;
        })) {
      status.effective_scale = first_effective;
    } else {
      // A null effective value with an applied state explicitly means that
      // the member settings are mixed, not that evidence is unavailable.
      status.application_state = RuntimeScaleApplicationState::kApplied;
    }
  }
  if (!group && active.size() > 1U) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "more than one active motion matches the runtime-scale control scope",
                          "runtime_scale_control_active_ambiguous");
    return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
  }
  if (active.size() == 1U) {
    status.active_operation_id = active.front()->state.operation_id;
    status.active_operation_revision = active.front()->state.operation_revision;
    if (active.front()->runtime_scale_status.has_value()) {
      status.active_operation_scale_generation =
          active.front()->runtime_scale_status->runtime_scale_generation;
      status.effective_scale = active.front()->runtime_scale_status->effective_scale;
      status.application_state = active.front()->runtime_scale_status->state;
    }
  } else if (group && !active.empty()) {
    const auto first_scale =
        active.front()->runtime_scale_status
            ? std::optional(active.front()->runtime_scale_status->effective_scale)
            : std::nullopt;
    if (first_scale && std::all_of(active.begin(), active.end(), [&](const auto* operation) {
          return operation->runtime_scale_status &&
                 std::abs(operation->runtime_scale_status->effective_scale - *first_scale) <= 1e-12;
        }))
      status.effective_scale = *first_scale;
    else
      status.effective_scale.reset();
    if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
          return operation->runtime_scale_status &&
                 operation->runtime_scale_status->state == RuntimeScaleApplicationState::kFailed;
        }))
      status.application_state = RuntimeScaleApplicationState::kFailed;
    else if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
               return operation->runtime_scale_status &&
                      operation->runtime_scale_status->state ==
                          RuntimeScaleApplicationState::kRejected;
             }))
      status.application_state = RuntimeScaleApplicationState::kRejected;
    else if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
               return operation->runtime_scale_status && operation->runtime_scale_status->state ==
                                                             RuntimeScaleApplicationState::kPending;
             }))
      status.application_state = RuntimeScaleApplicationState::kPending;
    else
      status.application_state = RuntimeScaleApplicationState::kApplied;
  }
  // Retain the setting and live observation while the exact route is closed.
  // A retained scaling capability is not evidence of current route readiness.
  const bool route_closed =
      std::any_of(scope.members.begin(), scope.members.end(), [&](const auto& member) {
        const auto route = runtime_route_configuration_readbacks_.find(member.control_domain_id);
        return runtime_route_configuration_publication_required_.contains(
                   member.control_domain_id) ||
               (route != runtime_route_configuration_readbacks_.end() &&
                (!route->second.operation_admission_open ||
                 route->second.active_configuration.active_control_configuration_id !=
                     member.active_control_configuration_id ||
                 route->second.active_configuration.active_control_configuration_generation !=
                     member.active_control_configuration_generation));
      });
  if (route_closed) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "runtime-scale control requires a current reconciled route",
                          "runtime_route_configuration_not_ready");
    return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
  }
  if (active.empty()) {
    return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
  }
  if (std::any_of(active.begin(), active.end(), [](const auto* operation) {
        return operation->state.state == OperationStateKind::kStopping ||
               operation->state.active_stop_id.has_value();
      })) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "runtime-scale control is frozen while Stop is active",
                          "runtime_scale_control_stop_active");
  } else if (!group &&
             active.front()->intent.coordination_policy ==
                 OperationCoordinationPolicy::kCoordinatedAll &&
             active.front()->participants.size() > 1U) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "synchronized motion requires its group velocity control",
                          "runtime_scale_group_control_required");
  } else if (group && std::any_of(active.begin(), active.end(), [&](const auto* operation) {
               return std::any_of(operation->participants.begin(), operation->participants.end(),
                                  [&](const auto& participant) {
                                    return participant.payload_kind !=
                                               OperationParticipantPayloadKind::kMotion ||
                                           !matches_scope(participant.binding.value());
                                  });
             })) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "velocity group overlaps a different active execution scope",
                          "runtime_scale_group_scope_busy");
  } else if (group && active.size() == 1U &&
             active.front()->intent.coordination_policy ==
                 OperationCoordinationPolicy::kCoordinatedAll) {
    const auto* coordinated = active.front();
    const auto shared =
        coordinated->common_timing ? coordinated->common_timing->execution_clock() : nullptr;
    const auto now = clock_.now();
    const auto expected_state = coordinated->state.state;
    bool ready =
        expected_state == OperationStateKind::kExecuting &&
        shared && shared->sample_now() && coordinated->runtime_scale_status &&
        coordinated->runtime_scale_status->participants.size() == coordinated->participants.size();
    for (std::size_t index = 0; ready && index < coordinated->participants.size(); ++index) {
      const auto& row = coordinated->runtime_scale_status->participants[index];
      const auto freshness = coordinated->participants[index]
                                 .binding.value()
                                 .control_path->runtime_scale_qualification->freshness_budget_ns;
      ready = coordinated->state.participants[index].state == expected_state && row.supported &&
              !row.error && row.acknowledgement_sequence != 0U &&
              row.acknowledgement_timestamp_ns >= 0 && row.acknowledgement_timestamp_ns <= now &&
              now - row.acknowledgement_timestamp_ns <= freshness;
    }
    if (!ready) {
      status.frozen = true;
      status.reason =
          error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                "synchronized velocity requires every participant ready on the shared clock",
                "coordinated_runtime_scale_not_ready");
    }
  } else if (group && std::any_of(active.begin(), active.end(), [](const auto* operation) {
               return operation->state.state != OperationStateKind::kPlanning &&
                      operation->state.state != OperationStateKind::kValidating &&
                      operation->state.state != OperationStateKind::kPrepared &&
                      operation->state.state != OperationStateKind::kDispatching &&
                      operation->state.state != OperationStateKind::kExecuting;
             })) {
    status.frozen = true;
    status.reason = error(std::string(to_string(ErrorCode::kInvalidStateTransition)),
                          "a group motion is not in a velocity-scalable state",
                          "runtime_scale_group_state_invalid");
  }
  return Result<RuntimeScaleControlStatusDTO>::Ok(std::move(status));
}

Result<OperationAcceptedDTO> ResourceAddressedExecutionKernel::execute(
    const OperationIntentDTO& intent) {
  std::lock_guard submit_lock(submission_mutex_);
  bool manual_claim_acquired = false;
  // Declared before the pending-admission guard: it runs after pending work
  // is cleared, but while Execute still owns ordinary submission ordering.
  const ScopeExit settle_manual_admission([&] {
    if (!manual_claim_acquired) return;
    try {
      const auto settled = reconcile_manual_operation_admission_locked();
      if (settled.ok()) return;
    } catch (...) {
    }
    // The durable claim itself keeps conflicting work closed. The existing
    // reconciliation lane retries release and surfaces an unresolved error;
    // do not introduce a second, permanently latched admission inhibit.
  });

  auto validation = validate_operation_intent(intent);
  if (!validation.ok()) {
    return Result<OperationAcceptedDTO>::Err(validation.error());
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }
  const bool single_independent =
      intent.participants.size() == 1U &&
      intent.coordination_policy == OperationCoordinationPolicy::kIndependent;
  const bool coordinated =
      intent.participants.size() > 1U &&
      intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll;
  if (!single_independent && !coordinated) {
    return reject<OperationAcceptedDTO>(
        "operation participant cardinality differs from its coordination policy",
        "operation_coordination_shape_invalid");
  }

  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay =
        std::find_if(replay_ledger_.begin(), replay_ledger_.end(), [&](const ReplayEntry& entry) {
          return entry.intent.request_context.request_id == intent.request_context.request_id;
        });
    if (replay != replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<OperationAcceptedDTO>::Err(
            error("request_id_conflict", "operation request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationAcceptedDTO>::Ok(std::move(accepted));
    }
    // A retained receipt is observation of the original decision, not fresh
    // motion admission. Keep it readable while Stop/fault fences reject every
    // request absent from the replay ledger.
    if (motion_admission_closed_) {
      return reject<OperationAcceptedDTO>(
          "new motion admission is closed by an active emergency Stop or fault",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto control_request = std::find_if(
        control_replay_ledger_.begin(), control_replay_ledger_.end(),
        [&](const ControlReplayEntry& entry) {
          return entry.request.request_context.request_id == intent.request_context.request_id;
        });
    if (control_request != control_replay_ledger_.end()) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by an operation control",
          "request_id_conflict"));
    }
    if (std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.request_id() == intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    })) {
      return Result<OperationAcceptedDTO>::Err(error(
          "request_id_conflict", "operation request ID was already used by another Kernel command",
          "request_id_conflict"));
    }
    if (!make_operation_admission_capacity_locked()) {
      return Result<OperationAcceptedDTO>::Err(
          error("operation_capacity_full", "active operation capacity reached its fixed bound",
                "operation_capacity_full"));
    }
    if (const auto scope = validate_program_execution_scope_locked(intent); !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    if (pending_program_run_admission_.has_value()) {
      return reject<OperationAcceptedDTO>("another Program-run admission is already being prepared",
                                          "program_run_admission_invariant",
                                          ErrorCode::kInvariantViolation);
    }
    pending_program_run_admission_ = intent.program_run_id;
  }
  const ScopeExit clear_pending_program_run([&] {
    std::lock_guard state_lock(state_mutex_);
    if (pending_program_run_admission_ == std::optional<ProgramRunId>{intent.program_run_id}) {
      pending_program_run_admission_.reset();
    }
  });

  if (coordinated) {
    return execute_coordinated_motion_after_preflight(intent);
  }

  auto resolved = resolve_registration(intent, 0U);
  if (!resolved.ok()) {
    return Result<OperationAcceptedDTO>::Err(resolved.error());
  }
  auto* engine = resolved.value().engine;
  if (engine == nullptr) {
    return reject<OperationAcceptedDTO>("resolved participant has no registered subordinate engine",
                                        "runtime_participant_engine_missing",
                                        ErrorCode::kInvariantViolation);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const auto engine_in_use = std::any_of(
        active_operations_.begin(), active_operations_.end(),
        [&](const auto& item) { return item.second->participant_for(engine) != nullptr; });
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "the resolved single-participant engine already owns an unsettled operation",
          "participant_engine_busy");
    }
  }
  if (next_operation_id_ == 0U || next_operation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<OperationAcceptedDTO>("operation identity space is exhausted",
                                        "operation_id_exhausted", ErrorCode::kInternalError);
  }

  const auto& binding = resolved.value().binding.value();
  if (!binding.control_path.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no active control path",
                                        "runtime_participant_control_path_missing",
                                        ErrorCode::kMissingReference);
  }
  const auto& control = *binding.control_path;
  if (runtime_route_configuration_publication_required_.contains(control.control_domain_id)) {
    return reject<OperationAcceptedDTO>(
        "runtime route configuration has not been published after reconfiguration",
        "runtime_route_publication_required", ErrorCode::kInvalidStateTransition);
  }
  const auto route_readback =
      runtime_route_configuration_readbacks_.find(control.control_domain_id);
  if (route_readback != runtime_route_configuration_readbacks_.end() &&
      (!route_readback->second.operation_admission_open ||
       route_readback->second.active_configuration.active_control_configuration_id !=
           control.active_control_configuration_id ||
       route_readback->second.active_configuration.active_control_configuration_generation !=
           control.active_control_configuration_generation)) {
    return reject<OperationAcceptedDTO>("runtime route configuration is unreconciled or stale",
                                        "runtime_route_configuration_not_ready",
                                        ErrorCode::kInvalidStateTransition);
  }
  if (auto refusal = component_command_refusal_locked(resolved.value()); refusal.has_value()) {
    return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
  }
  if (!resolved.value().stop_readiness.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no verified Stop readiness",
                                        "runtime_participant_stop_readiness_missing",
                                        ErrorCode::kMissingReference);
  }
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  const auto operation_id = OperationId("operation." + config_.request_owner_epoch.value() + "." +
                                        std::to_string(next_operation_id_++));
  std::shared_ptr<const ResolvedManualProgramHandover> manual_handover;
  if (intent.manual_handover) {
    if (!resolved.value().intrinsic_direct_control) {
      return reject<OperationAcceptedDTO>("only intrinsic manual work can name a held Program",
                                          "direct_control_product_mismatch");
    }
    auto held = resolve_manual_program_handover(intent);
    if (!held.ok()) return Result<OperationAcceptedDTO>::Err(held.error());
    if (!held.value()) {
      return reject<OperationAcceptedDTO>("manual handover has no current Program",
                                          "manual_handover_scope_stale");
    }
    const auto& capture = held.value()->capture;
    const ManualProgramHandoverContextDTO current_context{
        capture.program_runner_owner_epoch, capture.program_run_id, capture.claim_generation,
        held.value()->selection_revision};
    if (current_context != *intent.manual_handover) {
      return reject<OperationAcceptedDTO>("manual handover differs from its current hold",
                                          "manual_handover_scope_stale");
    }
    manual_handover =
        std::make_shared<const ResolvedManualProgramHandover>(std::move(*held.value()));
  }
  if (resolved.value().intrinsic_direct_control) {
    if (intent.participants.front().execution_scope.has_value()) {
      return reject<OperationAcceptedDTO>("manual work cannot carry Program-node authority",
                                          "direct_control_product_mismatch");
    }
    auto claim_resources = resources;
    std::sort(claim_resources.begin(), claim_resources.end());
    using content_identity::CanonicalValue;
    const auto digest = content_identity::sha256_digest(
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"schema", CanonicalValue("manyforge.manual_operation_admission.v1")},
            {"operation_id", CanonicalValue(operation_id.value())},
            {"program_binding_id", CanonicalValue(intent.program_binding_id.value())},
            {"program_run_id", CanonicalValue(intent.program_run_id.value())},
        })));
    std::lock_guard admission_lock(*admission_mutex_);
    const auto current = cell_admission_owner_->GetAuthoringAdmission();
    CellAdmissionClaimIntentDTO claim_intent{
        CellAdmissionClaimKindDTO::kManualOperation,
        CellAdmissionOwnerRoleDTO::kManualOperationOwner,
        config_.request_owner_epoch,
        operation_id.value(),
        intent.request_context.request_id,
        ContentDigest(digest),
        current.admission_generation,
        {std::move(claim_resources),
         {{control.control_domain_id, control.active_control_configuration_id,
           control.active_control_configuration_generation}}},
        std::nullopt,
        std::nullopt};
    if (manual_handover) {
      const auto& context = *intent.manual_handover;
      const auto offer_digest = content_identity::sha256_digest(
          content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
              {"schema", CanonicalValue("manyforge.manual_program_handover.v1")},
              {"operation_digest", CanonicalValue(digest)},
              {"program_runner_owner_epoch",
               CanonicalValue(context.program_runner_owner_epoch.value())},
              {"program_run_id", CanonicalValue(context.program_run_id.value())},
              {"claim_generation",
               CanonicalValue(static_cast<double>(context.claim_generation.value()))},
              {"selection_revision",
               CanonicalValue(static_cast<double>(context.selection_revision))},
          })));
      claim_intent.intervention_evidence = CellAdmissionInterventionEvidenceDTO{
          CellAdmissionOwnerRoleDTO::kManualOperationOwner, config_.request_owner_epoch,
          operation_id.value(), ContentDigest(offer_digest), context.claim_generation};
    }
    const auto claim =
        manual_handover
            ? cell_admission_owner_->AcquireManualCellAdmissionClaim(
                  claim_intent,
                  [&](const auto&, const auto& parent) -> Result<void> {
                    if (!parent ||
                        parent->claim_generation != manual_handover->capture.claim_generation)
                      return reject<void>("manual parent changed", "manual_handover_scope_stale");
                    std::lock_guard state_lock(state_mutex_);
                    return validate_manual_program_handover_locked(*manual_handover);
                  })
            : cell_admission_owner_->AcquireCellAdmissionClaim(claim_intent);
    if (!claim.ok()) return Result<OperationAcceptedDTO>::Err(claim.error());
    refresh_execution_claims();
    manual_claim_acquired = true;
  }
  const auto participant_id = OperationParticipantId(operation_id.value() + ".participant.1");
  const auto controller_goal_id = ControllerGoalId(operation_id.value() + ".controller-goal.1");
  const ControllerGoalGeneration controller_goal_generation(1U);
  const OperationRevision operation_revision(1U);
  const auto payload_kind =
      std::holds_alternative<MotionParticipantIntentDTO>(intent.participants.front().payload)
          ? OperationParticipantPayloadKind::kMotion
          : OperationParticipantPayloadKind::kCapabilityCommand;
  if (payload_kind == OperationParticipantPayloadKind::kMotion &&
      !resolved.value().participant_policy.has_value()) {
    return reject<OperationAcceptedDTO>("resolved participant has no sealed motion policy",
                                        "runtime_participant_policy_missing",
                                        ErrorCode::kMissingReference);
  }
  if (resolved.value().prepared_motion_engine == nullptr) {
    return reject<OperationAcceptedDTO>("resolved participant has no prepared execution engine",
                                        "runtime_participant_prepared_engine_missing",
                                        ErrorCode::kMissingReference);
  }
  ResourceSlotOwnerDTO slot_owner{operation_id,
                                  operation_revision,
                                  participant_id,
                                  control.control_domain_id,
                                  control.control_domain_revision,
                                  control.control_binding_id,
                                  control.control_binding_revision,
                                  control.controller_topology_id,
                                  control.topology_revision,
                                  control.control_route_id,
                                  control.active_control_configuration_id,
                                  control.active_control_configuration_generation,
                                  control.containment_granularity};
  std::optional<ExecuteRequestDTO> motion_request;
  std::optional<double> requested_tcp_linear_speed_mps;
  std::optional<GripperCommandRequestDTO> command_request;
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    const auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
    requested_tcp_linear_speed_mps =
        motion.speed_profile.max_tcp_linear_speed_mps;
    double initial_runtime_scale = 1.0;
    {
      std::lock_guard state_lock(state_mutex_);
      initial_runtime_scale = runtime_scale_setting_for_binding_locked(binding);
    }
    auto built =
        build_motion_request(intent, intent.participants.front(), motion, binding, operation_id,
                             operation_revision, participant_id, controller_goal_id,
                             controller_goal_generation, resolved.value(), initial_runtime_scale);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    motion_request = std::move(built.value());
    motion_request->trajectory_relative_timeout =
        resolved.value().intrinsic_direct_control;
  } else {
    const auto& command = std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload);
    auto built = build_command_request(command, binding);
    if (!built.ok()) {
      return Result<OperationAcceptedDTO>::Err(built.error());
    }
    command_request = std::move(built.value());
    auto private_validation = engine->validate_gripper_command(*command_request);
    if (!private_validation.ok()) {
      return Result<OperationAcceptedDTO>::Err(private_validation.error());
    }
  }
  const auto accepted_at_ns = clock_.now();
  const OperationAcceptedDTO accepted{intent.request_context.request_id,
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      operation_id,
                                      operation_revision,
                                      {OperationParticipantAssignmentDTO{0U, participant_id}},
                                      accepted_at_ns};
  std::optional<RuntimeScaleStatusDTO> initial_runtime_scale_status;
  if (payload_kind == OperationParticipantPayloadKind::kMotion &&
      control.runtime_scale_qualification.has_value()) {
    const auto initial_runtime_scale = motion_request->initial_runtime_scale->requested_scale;
    initial_runtime_scale_status =
        RuntimeScaleStatusDTO{operation_id,
                              operation_revision,
                              RuntimeScaleGeneration(0U),
                              initial_runtime_scale,
                              initial_runtime_scale,
                              {initial_scale_status(*motion_request->initial_runtime_scale,
                                                    control.provider_runtime_epoch)},
                              RuntimeScaleApplicationState::kPending,
                              accepted_at_ns};
  }
  OperationParticipantStateDTO participant_state{
      participant_id,
      controller_goal_id,
      controller_goal_generation,
      binding.controllable_component_id,
      binding.command_target_id,
      binding.capability_id,
      control.control_domain_id,
      control.control_domain_revision,
      control.control_binding_id,
      control.control_binding_revision,
      binding.capability_provider_path.has_value()
          ? std::optional(binding.capability_provider_path->capability_provider_binding_id)
          : std::nullopt,
      binding.capability_provider_path.has_value()
          ? std::optional(binding.capability_provider_path->capability_provider_binding_revision)
          : std::nullopt,
      payload_kind,
      OperationStateKind::kResolving,
      0.0,
      resources,
      std::nullopt,
      intent.participants.front().execution_scope};
  OperationStateDTO operation_state_dto{operation_id,
                                        operation_revision,
                                        intent.program_binding_id,
                                        intent.program_run_id,
                                        intent.coordination_policy,
                                        OperationStateKind::kResolving,
                                        {std::move(participant_state)},
                                        accepted_at_ns,
                                        std::nullopt,
                                        std::nullopt,
                                        std::nullopt};
  if (initial_runtime_scale_status.has_value()) {
    operation_state_dto.runtime_scale_generation = RuntimeScaleGeneration(0U);
  }

  PreparedParticipantStopPlanContextDTO stop_context{
      operation_id,
      operation_revision,
      participant_id,
      intent.program_id,
      intent.program_revision,
      intent.program_binding_id,
      intent.program_run_id,
      payload_kind == OperationParticipantPayloadKind::kMotion
          ? resolved.value().default_operator_stop_profile
          : std::optional<MotionInterruptStopProfileDTO>{},
      payload_kind == OperationParticipantPayloadKind::kMotion &&
              resolved.value().default_operator_stop_profile.has_value()
          ? ParticipantStopProfilePurposeDTO::kOperator
          : ParticipantStopProfilePurposeDTO::kOrdinary,
      std::nullopt,
      resolved.value().stop_node_category};
  auto ordinary_stop_plan = MakePreparedParticipantStopPlan(
      resolved.value().stop_readiness->value(), std::move(stop_context), binding.resources,
      resolved.value().retained_default_stop_limits);
  if (!ordinary_stop_plan.ok()) {
    return Result<OperationAcceptedDTO>::Err(ordinary_stop_plan.error());
  }
  auto fault_stop_plan = make_runtime_fault_stop_plan(
      *resolved.value().stop_readiness,
      payload_kind == OperationParticipantPayloadKind::kMotion
          ? resolved.value().default_fault_stop_profile
          : std::optional<MotionInterruptStopProfileDTO>{},
      binding, resolved.value().retained_default_stop_limits, resolved.value().stop_node_category,
      operation_id, operation_revision, participant_id, intent.program_id, intent.program_revision,
      intent.program_binding_id, intent.program_run_id);
  if (!fault_stop_plan.ok()) {
    return Result<OperationAcceptedDTO>::Err(fault_stop_plan.error());
  }

  std::optional<PreparedOperationParticipantDTO> prepared_command_product;
  if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
    auto made = build_prepared_capability_command_participant(
        intent, std::get<CapabilityCommandIntentDTO>(intent.participants.front().payload),
        resolved.value().binding, *resolved.value().stop_readiness,
        resolved.value().retained_default_stop_limits, resolved.value().stop_node_category,
        operation_id, operation_revision, participant_id, ordinary_stop_plan.value());
    if (!made.ok()) {
      return Result<OperationAcceptedDTO>::Err(made.error());
    }
    prepared_command_product = std::move(made.value());
  }

  std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_stop_plan;
  std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_fault_stop_plan;
  std::shared_ptr<const PreparedOperationParticipantDTO> retained_command_product;
  std::map<OperationId, std::unique_ptr<ActiveOperation>>::node_type prepared_operation;
  std::shared_ptr<const detail::MotionResumeIntent> continuation;
  ReplayEntry prepared_replay{intent, accepted};
  std::vector<OperationId> protected_operation_ids;
  try {
    retained_stop_plan = std::make_shared<const PreparedParticipantStopPlanDTO>(
        std::move(ordinary_stop_plan.value()));
    if (fault_stop_plan.value().has_value()) {
      retained_fault_stop_plan = std::make_shared<const PreparedParticipantStopPlanDTO>(
          std::move(*fault_stop_plan.value()));
    }
    if (prepared_command_product.has_value()) {
      retained_command_product = std::make_shared<const PreparedOperationParticipantDTO>(
          std::move(*prepared_command_product));
    }
    std::map<OperationId, std::unique_ptr<ActiveOperation>> staging;
    staging.emplace(operation_id,
                    std::make_unique<ActiveOperation>(ActiveOperation{
                        intent,
                        accepted,
                        {ActiveOperationParticipant{resolved.value().binding,
                                                    *resolved.value().stop_readiness,
                                                    resolved.value().retained_default_stop_limits,
                                                    resolved.value().stop_node_category,
                                                    resolved.value().default_operator_stop_profile,
                                                    retained_stop_plan,
                                                    resolved.value().default_fault_stop_profile,
                                                    retained_fault_stop_plan,
                                                    std::nullopt,
                                                    engine,
                                                    resolved.value().prepared_motion_engine,
                                                    resolved.value().stop_execution_port,
                                                    slot_owner,
                                                    resources,
                                                    payload_kind,
                                                    ResourceSlotState::kReserved,
                                                    0U,
                                                    0U,
                                                    0U,
                                                    false,
                                                    false,
                                                    false,
                                                    false,
                                                    false,
                                                    false,
                                                    std::nullopt,
                                                    motion_request}},
                        operation_state_dto,
                        std::nullopt,
                        false,
                        false,
                        true,
                        false,
                        false,
                        std::move(initial_runtime_scale_status)}));
    stamp_operation_capture_locked(*staging.at(operation_id), operation_state_dto);
    staging.at(operation_id)->manual_handover = manual_handover;
    staging.at(operation_id)->manual = resolved.value().intrinsic_direct_control;
    staging.at(operation_id)->state = operation_state_dto;
    prepared_operation = staging.extract(operation_id);
  } catch (...) {
    return reject<OperationAcceptedDTO>(
        "the bounded operation admission state could not be allocated before reservation",
        "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
  }

  {
    // This is the only Execute/Stop ordering section: publish the reserved
    // pending operation before any ordinary journal or provider call. A Stop
    // can now cancel/fence it without waiting for those calls.
    // Acquire durable admission ordering before Kernel state. A pending
    // authoring claim may delay new Execute, but it must not make Execute hold
    // state while waiting and thereby delay recognized Stop.
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<OperationAcceptedDTO>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (const auto scope = validate_program_execution_scope_locked(intent); !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    // Ordinary Run work only; Manual Robot Control is not attention-gated.
    if (!resolved.value().intrinsic_direct_control) {
      if (auto refusal = operator_alarm_attention_refusal_locked(resources)) {
        return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
      }
    }
    if (manual_handover) {
      const auto current_hold = validate_manual_program_handover_locked(*manual_handover);
      if (!current_hold.ok()) return Result<OperationAcceptedDTO>::Err(current_hold.error());
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
    const bool engine_in_use = std::any_of(
        active_operations_.begin(), active_operations_.end(),
        [&](const auto& item) { return item.second->participant_for(engine) != nullptr; });
    const bool request_id_used =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.request_id() == intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    });
    if (request_id_used) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID was used during admission",
                "request_id_conflict"));
    }
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "the resolved single-participant engine became busy during admission",
          "participant_engine_busy");
    }
    if (!make_operation_admission_capacity_locked()) {
      return Result<OperationAcceptedDTO>::Err(
          error("operation_capacity_full", "active operation capacity reached its fixed bound",
                "operation_capacity_full"));
    }
    if (program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
        program_cycle_claim_run_->program_run_id == intent.program_run_id &&
        intent.participants.front().execution_scope) {
      const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
      const auto& scope = *intent.participants.front().execution_scope;
      const auto node =
          std::lower_bound(nodes.begin(), nodes.end(), scope.node_instance_id,
                           [](const auto& row, const auto& id) { return row.instance_id < id; });
      const auto index = static_cast<std::size_t>(node - nodes.begin());
      if (index < program_cycle_claim_run_->motion_continuations.size()) {
        const auto& pending = program_cycle_claim_run_->motion_continuations[index];
        if (pending) {
          if (node->owner_api != "kernel.operation" ||
              pending->origin.node_instance_id != scope.node_instance_id ||
              pending->origin.node_attempt_id == scope.node_attempt_id ||
              payload_kind != OperationParticipantPayloadKind::kMotion ||
              pending->binding.value() != resolved.value().binding.value() || !motion_request ||
              pending->base_frame != motion_request->binding.base_frame ||
              pending->tcp_frame != motion_request->binding.tcp_frame ||
              pending->planner_capability != motion_request->binding.planner_capability_binding ||
              pending->authored_motion_kind != motion_request->motion.index())
            return reject<OperationAcceptedDTO>(
                "new attempt differs from its retained motion continuation",
                "program_motion_continuation_mismatch", ErrorCode::kInvalidRequest);
          continuation = pending->intent;
          auto& participant = prepared_operation.mapped()->participants.front();
          participant.continuation = continuation;
          participant.continuation_node_index = index;
        }
      }
    }
    auto reservation =
        config_.operation_admission_gate != nullptr
            ? config_.operation_admission_gate->reserve(*resource_slots_, slot_owner, resources)
            : resource_slots_->reserve(slot_owner, resources);
    if (!reservation.ok()) {
      return Result<OperationAcceptedDTO>::Err(reservation.error());
    }
    const auto inserted = active_operations_.insert(std::move(prepared_operation));
    if (!inserted.inserted) {
      const auto released = resource_slots_->release(slot_owner, resources);
      if (!released.ok()) {
        return Result<OperationAcceptedDTO>::Err(released.error());
      }
      return reject<OperationAcceptedDTO>("operation identity is already resident",
                                          "operation_id_conflict", ErrorCode::kInvariantViolation);
    }
    protected_operation_ids = durable_operation_retention_ids_locked();
  }

  OperationCommitGuard admission_commit(*this, operation_id);
  auto durable_acceptance = attempt_journal_write([&] {
    return journal_.RecordOperationAccepted(config_.request_owner_epoch, intent, accepted,
                                            operation_state_dto, protected_operation_ids);
  });
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation != nullptr) {
      active_operation->ordinary_acceptance_pending = false;
      if (!active_operation->state.active_stop_id.has_value()) {
        auto& participant = active_operation->participants.front();
        const auto released =
            resource_slots_->release(participant.slot_owner, participant.resources);
        if (!released.ok()) {
          return Result<OperationAcceptedDTO>::Err(released.error());
        }
        active_operations_.erase(operation_id);
      }
    }
    return Result<OperationAcceptedDTO>::Err(durable_acceptance.error());
  }

  bool dispatch_allowed = false;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation == nullptr) {
      return reject<OperationAcceptedDTO>("operation disappeared after durable acceptance",
                                          "active_operation_lost", ErrorCode::kInvariantViolation);
    }
    active_operation->ordinary_acceptance_pending = false;
    active_operation->ordinary_acceptance_durable = true;
    replay_ledger_.push_back(std::move(prepared_replay));
    auto& participant = active_operation->participants.front();
    dispatch_allowed = !active_operation->state.active_stop_id.has_value() &&
                       participant.slot_state == ResourceSlotState::kReserved;
    if (dispatch_allowed) {
      participant.prepare_in_flight = true;
    }
  }
  admission_commit.release();
  if (!dispatch_allowed) {
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }

  Result<void> dispatch_result = Result<void>::Ok();
  if (payload_kind == OperationParticipantPayloadKind::kMotion) {
    ResolvedPreparedMotionExecutionRequestDTO prepare_request{
        *motion_request,
        operation_revision,
        resolved.value().binding,
        *resolved.value().stop_readiness,
        requested_tcp_linear_speed_mps,
        *resolved.value().deployment_maximum_tcp_linear_speed_mps,
        resolved.value().retained_default_stop_limits,
        resolved.value().stop_node_category,
        retained_stop_plan,
        retained_fault_stop_plan,
        continuation};
    auto prepared_execution =
        resolved.value().prepared_motion_engine->PrepareMotion(std::move(prepare_request));
    if (!prepared_execution.ok()) {
      dispatch_result = Result<void>::Err(prepared_execution.error());
    } else {
      bool cancel_preparation = false;
      {
        OperationCommitGuard commit(*this, operation_id);
        std::unique_lock state_lock(state_mutex_);
        auto* active_operation = find_active_operation_locked(operation_id);
        if (active_operation == nullptr) {
          cancel_preparation = true;
          dispatch_result =
              Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                      "active operation disappeared during subordinate admission",
                                      "active_operation_lost"));
        } else {
          cancel_preparation = active_operation->terminal_stop_result().has_value();
          // A timed-out Stop keeps this call's borrow until its late product
          // has been disposed of. Its immutable certificate cannot do cleanup.
          active_operation->participants.front().prepare_in_flight = cancel_preparation;
        }
        auto* participant =
            active_operation == nullptr ? nullptr : &active_operation->participants.front();
        if (participant != nullptr && participant->subordinate_id != 0U &&
            participant->subordinate_id != prepared_execution.value()) {
          dispatch_result =
              Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                      "subordinate event and execute result identities disagree",
                                      "subordinate_execution_id_mismatch"));
        } else if (participant != nullptr) {
          participant->subordinate_id = prepared_execution.value();
          participant->preparation_id = prepared_execution.value();
          if (active_operation->state.state == OperationStateKind::kResolving) {
            auto planning_state = active_operation->state;
            planning_state.state = OperationStateKind::kPlanning;
            planning_state.participants.front().state = OperationStateKind::kPlanning;
            planning_state.updated_at_ns = clock_.now();
            auto durable_state =
                record_operation_state_locked(state_lock, planning_state,
                                              active_operation->runtime_scale_status.has_value()
                                                  ? &*active_operation->runtime_scale_status
                                                  : nullptr,
                                              "subordinate_motion_admitted");
            if (durable_state.ok() && durable_state.value()) {
              active_operation->state = std::move(planning_state);
            } else if (!durable_state.ok()) {
              dispatch_result = Result<void>::Err(durable_state.error());
            }
          }
        }
      }
      if (cancel_preparation) {
        const auto cancelled = resolved.value().prepared_motion_engine->CancelPreparedMotion(
            prepared_execution.value());
        if (!cancelled.ok()) {
          dispatch_result = Result<void>::Err(cancelled.error());
        }
      }
    }
    if (dispatch_result.ok()) {
      dispatch_result = advance_prepared_motion(operation_id, engine);
    }
  } else {
    ResolvedPreparedCapabilityCommandExecutionRequestDTO prepare_request{
        retained_command_product,
        resolved.value().binding,
        *resolved.value().stop_readiness,
        resolved.value().retained_default_stop_limits,
        resolved.value().stop_node_category,
        *command_request};
    auto prepared_execution = resolved.value().prepared_motion_engine->PrepareCapabilityCommand(
        std::move(prepare_request));
    if (!prepared_execution.ok()) {
      dispatch_result = Result<void>::Err(prepared_execution.error());
    } else {
      bool cancel_preparation = false;
      {
        std::lock_guard state_lock(state_mutex_);
        auto* active_operation = find_active_operation_locked(operation_id);
        if (active_operation == nullptr) {
          cancel_preparation = true;
          dispatch_result = Result<void>::Err(error(
              std::string(to_string(ErrorCode::kInvariantViolation)),
              "active command disappeared during subordinate admission", "active_operation_lost"));
        } else {
          auto& participant = active_operation->participants.front();
          cancel_preparation = active_operation->terminal_stop_result().has_value();
          participant.prepare_in_flight = cancel_preparation;
          if (participant.subordinate_id != 0U &&
              participant.subordinate_id != prepared_execution.value()) {
            dispatch_result =
                Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                        "prepared command identity disagrees with resident state",
                                        "prepared_command_identity_mismatch"));
          } else {
            participant.subordinate_id = prepared_execution.value();
            participant.preparation_id = prepared_execution.value();
          }
        }
      }
      if (cancel_preparation) {
        const auto cancelled =
            resolved.value().prepared_motion_engine->CancelPreparedCapabilityCommand(
                prepared_execution.value());
        if (!cancelled.ok()) {
          dispatch_result = Result<void>::Err(cancelled.error());
        }
      }
    }
    if (dispatch_result.ok()) {
      dispatch_result = advance_prepared_capability_command(operation_id, engine);
    }
  }

  {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id)) {
      operation->participants.front().prepare_in_flight = false;
      settle_completed_control_locked(operation_id);
    }
  }
  if (!dispatch_result.ok()) {
    OperationCommitGuard commit(*this, operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(operation_id);
    if (active_operation != nullptr) {
      auto& participant = active_operation->participants.front();
      participant.dispatch_effect_in_flight = false;
      if (active_operation->state.active_stop_id.has_value()) {
        participant.last_subordinate_error = dispatch_result.error();
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      auto failed_state = active_operation->state;
      failed_state.state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().state = OperationStateKind::kInterventionRequired;
      failed_state.participants.front().terminal_error = dispatch_result.error();
      failed_state.updated_at_ns = clock_.now();
      auto durable_failure =
          record_operation_state_locked(state_lock, failed_state,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "subordinate_dispatch_failed_after_acceptance");
      if (durable_failure.ok() && durable_failure.value()) {
        if (participant.slot_state != ResourceSlotState::kTerminal) {
          const auto transition = resource_slots_->transition(
              participant.slot_owner, participant.resources, ResourceSlotState::kTerminal);
          if (transition.ok()) {
            participant.slot_state = ResourceSlotState::kTerminal;
          }
        }
        active_operation->state = std::move(failed_state);
      }
    }
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }
  return Result<OperationAcceptedDTO>::Ok(accepted);
}

Result<OperationAcceptedDTO>
ResourceAddressedExecutionKernel::execute_coordinated_motion_after_preflight(
    const OperationIntentDTO& intent) {
  struct PreparedAdmission final {
    RuntimeParticipantExecutionRegistration registration;
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::optional<ExecuteRequestDTO> motion_request;
    std::optional<double> requested_tcp_linear_speed_mps;
    std::optional<GripperCommandRequestDTO> command_request;
    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_command_product;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> stop_plan;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> fault_stop_plan;
  };

  if (next_operation_id_ == 0U || next_operation_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject<OperationAcceptedDTO>("operation identity space is exhausted",
                                        "operation_id_exhausted", ErrorCode::kInternalError);
  }
  const auto operation_id = OperationId("operation." + config_.request_owner_epoch.value() + "." +
                                        std::to_string(next_operation_id_));
  const OperationRevision operation_revision(1U);
  const auto accepted_at_ns = clock_.now();
  std::vector<PreparedAdmission> admissions;
  std::vector<OperationParticipantAssignmentDTO> assignments;
  std::vector<OperationParticipantStateDTO> participant_states;
  std::vector<ActiveOperationParticipant> active_participants;
  std::vector<ResourceSlotReservationOwnerDTO> reservation_rows;
  std::set<IResolvedParticipantExecutionEngine*> selected_engines;
  std::set<ResourceId> selected_resources;
  double coordinated_initial_runtime_scale = 1.0;
  const bool all_motion = std::all_of(
      intent.participants.begin(), intent.participants.end(), [](const auto& participant) {
        return std::holds_alternative<MotionParticipantIntentDTO>(participant.payload);
      });
  if (all_motion) {
    RuntimeScaleControlSelectorDTO selector{intent.program_binding_id, ComponentRoleId(""),
                                            TargetRoleId("")};
    for (const auto& participant : intent.participants) {
      selector.group_targets.push_back({participant.component_role_id, participant.target_role_id});
    }
    std::lock_guard state_lock(state_mutex_);
    auto scope = resolve_runtime_scale_control_locked(selector);
    if (!scope.ok()) return Result<OperationAcceptedDTO>::Err(scope.error());
    const auto setting =
        std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                     [&](const auto& candidate) {
                       return candidate.resolution.same_control_scope(scope.value());
                     });
    if (setting != runtime_scale_control_settings_.end())
      coordinated_initial_runtime_scale = setting->requested_scale;
  }
  admissions.reserve(intent.participants.size());
  assignments.reserve(intent.participants.size());
  participant_states.reserve(intent.participants.size());
  active_participants.reserve(intent.participants.size());
  reservation_rows.reserve(intent.participants.size());

  for (std::size_t index = 0U; index < intent.participants.size(); ++index) {
    auto resolved = resolve_registration(intent, index);
    if (!resolved.ok()) {
      return Result<OperationAcceptedDTO>::Err(resolved.error());
    }
    auto& registration = resolved.value();
    if (registration.intrinsic_direct_control) {
      return reject<OperationAcceptedDTO>(
          "compiler-direct products admit one manual operation participant",
          "direct_control_coordination_unsupported", ErrorCode::kInvalidRequest);
    }
    if (registration.engine == nullptr || registration.prepared_motion_engine == nullptr ||
        registration.stop_execution_port == nullptr) {
      return reject<OperationAcceptedDTO>(
          "coordinated motion participant lacks its prepared or Stop execution port",
          "coordinated_participant_execution_port_missing", ErrorCode::kMissingReference);
    }
    if (!selected_engines.insert(registration.engine).second) {
      return reject<OperationAcceptedDTO>(
          "coordinated participants resolve to one ambiguous subordinate engine",
          "coordinated_participant_engine_ambiguous", ErrorCode::kInvalidRequest);
    }
    const auto payload_kind =
        std::holds_alternative<MotionParticipantIntentDTO>(intent.participants[index].payload)
            ? OperationParticipantPayloadKind::kMotion
            : OperationParticipantPayloadKind::kCapabilityCommand;
    if (!registration.stop_readiness.has_value() ||
        (payload_kind == OperationParticipantPayloadKind::kMotion &&
         !registration.participant_policy.has_value())) {
      return reject<OperationAcceptedDTO>(
          "coordinated participant lacks sealed readiness or motion policy",
          "coordinated_participant_authority_missing", ErrorCode::kMissingReference);
    }
    const auto& binding = registration.binding.value();
    if (!binding.control_path.has_value()) {
      return reject<OperationAcceptedDTO>("resolved participant has no active control path",
                                          "runtime_participant_control_path_missing",
                                          ErrorCode::kMissingReference);
    }
    const auto& control = *binding.control_path;
    if (runtime_route_configuration_publication_required_.contains(control.control_domain_id)) {
      return reject<OperationAcceptedDTO>(
          "runtime route configuration has not been published after reconfiguration",
          "runtime_route_publication_required", ErrorCode::kInvalidStateTransition);
    }
    const auto route_readback =
        runtime_route_configuration_readbacks_.find(control.control_domain_id);
    if (route_readback != runtime_route_configuration_readbacks_.end() &&
        (!route_readback->second.operation_admission_open ||
         route_readback->second.active_configuration.active_control_configuration_id !=
             control.active_control_configuration_id ||
         route_readback->second.active_configuration.active_control_configuration_generation !=
             control.active_control_configuration_generation)) {
      return reject<OperationAcceptedDTO>("runtime route configuration is unreconciled or stale",
                                          "runtime_route_configuration_not_ready",
                                          ErrorCode::kInvalidStateTransition);
    }
    if (auto refusal = component_command_refusal_locked(registration); refusal.has_value()) {
      return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
    }

    std::vector<ResourceId> resources;
    resources.reserve(binding.resources.size());
    for (const auto& resource : binding.resources) {
      if (!selected_resources.insert(resource.resource_id).second) {
        return reject<OperationAcceptedDTO>("coordinated participant resource scopes overlap",
                                            "coordinated_participant_resource_overlap",
                                            ErrorCode::kInvalidRequest);
      }
      resources.push_back(resource.resource_id);
    }

    const auto ordinal = index + 1U;
    const auto participant_id =
        OperationParticipantId(operation_id.value() + ".participant." + std::to_string(ordinal));
    const auto controller_goal_id =
        ControllerGoalId(operation_id.value() + ".controller-goal." + std::to_string(ordinal));
    const ControllerGoalGeneration controller_goal_generation(1U);
    const auto& participant_intent = intent.participants[index];
    std::optional<ExecuteRequestDTO> motion_request;
    std::optional<double> requested_tcp_linear_speed_mps;
    std::optional<GripperCommandRequestDTO> command_request;
    if (payload_kind == OperationParticipantPayloadKind::kMotion) {
      const auto& motion = std::get<MotionParticipantIntentDTO>(participant_intent.payload);
      requested_tcp_linear_speed_mps =
          motion.speed_profile.max_tcp_linear_speed_mps;
      double initial_runtime_scale = 1.0;
      {
        std::lock_guard state_lock(state_mutex_);
        initial_runtime_scale = all_motion ? coordinated_initial_runtime_scale
                                           : runtime_scale_setting_for_binding_locked(binding);
      }
      auto request =
          build_motion_request(intent, participant_intent, motion, binding, operation_id,
                               operation_revision, participant_id, controller_goal_id,
                               controller_goal_generation, registration, initial_runtime_scale);
      if (!request.ok()) {
        return Result<OperationAcceptedDTO>::Err(request.error());
      }
      motion_request = std::move(request.value());
    } else {
      auto request = build_command_request(
          std::get<CapabilityCommandIntentDTO>(participant_intent.payload), binding);
      if (!request.ok()) {
        return Result<OperationAcceptedDTO>::Err(request.error());
      }
      auto private_validation = registration.engine->validate_gripper_command(request.value());
      if (!private_validation.ok()) {
        return Result<OperationAcceptedDTO>::Err(private_validation.error());
      }
      command_request = std::move(request.value());
    }

    PreparedParticipantStopPlanContextDTO stop_context{
        operation_id,
        operation_revision,
        participant_id,
        intent.program_id,
        intent.program_revision,
        intent.program_binding_id,
        intent.program_run_id,
        payload_kind == OperationParticipantPayloadKind::kMotion
            ? registration.default_operator_stop_profile
            : std::optional<MotionInterruptStopProfileDTO>{},
        payload_kind == OperationParticipantPayloadKind::kMotion &&
                registration.default_operator_stop_profile.has_value()
            ? ParticipantStopProfilePurposeDTO::kOperator
            : ParticipantStopProfilePurposeDTO::kOrdinary,
        std::nullopt,
        registration.stop_node_category};
    auto stop_plan = MakePreparedParticipantStopPlan(registration.stop_readiness->value(),
                                                     std::move(stop_context), binding.resources,
                                                     registration.retained_default_stop_limits);
    if (!stop_plan.ok()) {
      return Result<OperationAcceptedDTO>::Err(stop_plan.error());
    }
    auto fault_stop_plan = make_runtime_fault_stop_plan(
        *registration.stop_readiness,
        payload_kind == OperationParticipantPayloadKind::kMotion
            ? registration.default_fault_stop_profile
            : std::optional<MotionInterruptStopProfileDTO>{},
        binding, registration.retained_default_stop_limits, registration.stop_node_category,
        operation_id, operation_revision, participant_id, intent.program_id,
        intent.program_revision, intent.program_binding_id, intent.program_run_id);
    if (!fault_stop_plan.ok()) {
      return Result<OperationAcceptedDTO>::Err(fault_stop_plan.error());
    }
    std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_stop_plan;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> retained_fault_stop_plan;
    std::shared_ptr<const PreparedOperationParticipantDTO> prepared_command_product;
    try {
      retained_stop_plan =
          std::make_shared<const PreparedParticipantStopPlanDTO>(std::move(stop_plan.value()));
      if (fault_stop_plan.value().has_value()) {
        retained_fault_stop_plan = std::make_shared<const PreparedParticipantStopPlanDTO>(
            std::move(*fault_stop_plan.value()));
      }
      if (payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
        auto prepared = build_prepared_capability_command_participant(
            intent, std::get<CapabilityCommandIntentDTO>(participant_intent.payload),
            registration.binding, *registration.stop_readiness,
            registration.retained_default_stop_limits, registration.stop_node_category,
            operation_id, operation_revision, participant_id, *retained_stop_plan);
        if (!prepared.ok()) {
          return Result<OperationAcceptedDTO>::Err(prepared.error());
        }
        prepared_command_product =
            std::make_shared<const PreparedOperationParticipantDTO>(std::move(prepared.value()));
      }
    } catch (...) {
      return reject<OperationAcceptedDTO>(
          "the coordinated Stop plan could not be retained before reservation",
          "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
    }

    ResourceSlotOwnerDTO slot_owner{operation_id,
                                    operation_revision,
                                    participant_id,
                                    control.control_domain_id,
                                    control.control_domain_revision,
                                    control.control_binding_id,
                                    control.control_binding_revision,
                                    control.controller_topology_id,
                                    control.topology_revision,
                                    control.control_route_id,
                                    control.active_control_configuration_id,
                                    control.active_control_configuration_generation,
                                    control.containment_granularity};
    ResourceSlotReservationOwnerDTO reservation{slot_owner, resources};
    assignments.push_back(OperationParticipantAssignmentDTO{index, participant_id});
    participant_states.push_back(OperationParticipantStateDTO{
        participant_id, controller_goal_id, controller_goal_generation,
        binding.controllable_component_id, binding.command_target_id, binding.capability_id,
        control.control_domain_id, control.control_domain_revision, control.control_binding_id,
        control.control_binding_revision,
        binding.capability_provider_path.has_value()
            ? std::optional(binding.capability_provider_path->capability_provider_binding_id)
            : std::nullopt,
        binding.capability_provider_path.has_value()
            ? std::optional(binding.capability_provider_path->capability_provider_binding_revision)
            : std::nullopt,
        payload_kind, OperationStateKind::kResolving, 0.0, resources, std::nullopt,
        participant_intent.execution_scope});
    active_participants.push_back(
        ActiveOperationParticipant{registration.binding,
                                   *registration.stop_readiness,
                                   registration.retained_default_stop_limits,
                                   registration.stop_node_category,
                                   registration.default_operator_stop_profile,
                                   retained_stop_plan,
                                   registration.default_fault_stop_profile,
                                   retained_fault_stop_plan,
                                   std::nullopt,
                                   registration.engine,
                                   registration.prepared_motion_engine,
                                   registration.stop_execution_port,
                                   slot_owner,
                                   resources,
                                   payload_kind,
                                   ResourceSlotState::kReserved,
                                   0U,
                                   0U,
                                   0U,
                                   false,
                                   false,
                                   false,
                                   false,
                                   false,
                                   false,
                                   std::nullopt,
                                   motion_request});
    reservation_rows.push_back(reservation);
    admissions.push_back(
        PreparedAdmission{std::move(registration), payload_kind, std::move(motion_request),
                          requested_tcp_linear_speed_mps,
                          std::move(command_request), std::move(prepared_command_product),
                          std::move(retained_stop_plan), std::move(retained_fault_stop_plan)});
  }

  std::sort(reservation_rows.begin(), reservation_rows.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.owner.operation_id, left.owner.participant_id) <
                     std::tie(right.owner.operation_id, right.owner.participant_id);
            });
  const OperationAcceptedDTO accepted{intent.request_context.request_id,
                                      RequestReplayStatusDTO::kAcceptedNew,
                                      operation_id,
                                      operation_revision,
                                      assignments,
                                      accepted_at_ns};
  OperationStateDTO operation_state{operation_id,
                                    operation_revision,
                                    intent.program_binding_id,
                                    intent.program_run_id,
                                    intent.coordination_policy,
                                    OperationStateKind::kResolving,
                                    std::move(participant_states),
                                    accepted_at_ns,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt};
  std::map<OperationId, std::unique_ptr<ActiveOperation>>::node_type prepared_operation;
  ReplayEntry prepared_replay{intent, accepted};
  std::vector<OperationId> protected_operation_ids;
  std::shared_ptr<detail::CoordinatedMotionPreparation> common_timing;
  try {
    std::optional<RuntimeScaleStatusDTO> initial_runtime_scale_status;
    const bool all_scalable =
        std::all_of(admissions.begin(), admissions.end(), [](const auto& item) {
          return item.motion_request && item.motion_request->initial_runtime_scale;
        });
    if (all_scalable) {
      const double scale =
          admissions.front().motion_request->initial_runtime_scale->requested_scale;
      initial_runtime_scale_status = RuntimeScaleStatusDTO{operation_id,
                                                           operation_revision,
                                                           RuntimeScaleGeneration(0U),
                                                           scale,
                                                           scale,
                                                           {},
                                                           RuntimeScaleApplicationState::kPending,
                                                           accepted_at_ns};
      for (const auto& admission : admissions)
        initial_runtime_scale_status->participants.push_back(initial_scale_status(
            *admission.motion_request->initial_runtime_scale,
            admission.registration.binding.value().control_path->provider_runtime_epoch));
      operation_state.runtime_scale_generation = RuntimeScaleGeneration(0U);
    }
    std::vector<OperationParticipantId> motion_participants;
    for (const auto& admission : admissions)
      if (admission.motion_request)
        motion_participants.push_back(admission.motion_request->binding.participant_id);
    if (motion_participants.size() > 1U)
      common_timing =
          std::make_shared<detail::CoordinatedMotionPreparation>(std::move(motion_participants));
    std::map<OperationId, std::unique_ptr<ActiveOperation>> staging;
    staging.emplace(operation_id, std::make_unique<ActiveOperation>(ActiveOperation{
                                      intent, accepted, std::move(active_participants),
                                      operation_state, std::nullopt, false, false, true, false,
                                      false, std::move(initial_runtime_scale_status)}));
    stamp_operation_capture_locked(*staging.at(operation_id), operation_state);
    staging.at(operation_id)->state = operation_state;
    staging.at(operation_id)->common_timing = common_timing;
    prepared_operation = staging.extract(operation_id);
  } catch (...) {
    return reject<OperationAcceptedDTO>(
        "the coordinated operation state could not be retained before reservation",
        "operation_admission_capacity_unavailable", ErrorCode::kInternalError);
  }

  {
    // Match single-participant admission: ordering precedes Kernel state so a
    // blocked durable claim cannot convoy recognized Stop.
    std::lock_guard admission_lock(*admission_mutex_);
    std::lock_guard state_lock(state_mutex_);
    if (motion_admission_closed_) {
      return reject<OperationAcceptedDTO>(
          "new motion admission closed while the operation was being prepared",
          "motion_admission_closed", ErrorCode::kInvalidStateTransition);
    }
    if (const auto scope = validate_program_execution_scope_locked(intent); !scope.ok()) {
      return Result<OperationAcceptedDTO>::Err(scope.error());
    }
    // One intersecting participant rejects the complete coordinated admission.
    if (auto refusal = operator_alarm_attention_refusal_locked(
            std::vector<ResourceId>(selected_resources.begin(), selected_resources.end()))) {
      return Result<OperationAcceptedDTO>::Err(std::move(*refusal));
    }
    if (program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
        program_cycle_claim_run_->program_run_id == intent.program_run_id) {
      const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
      for (std::size_t index = 0U; index < intent.participants.size(); ++index) {
        const auto& scope = *intent.participants[index].execution_scope;
        const auto node =
            std::lower_bound(nodes.begin(), nodes.end(), scope.node_instance_id,
                             [](const auto& row, const auto& id) { return row.instance_id < id; });
        const auto node_index = static_cast<std::size_t>(node - nodes.begin());
        if (node_index >= program_cycle_claim_run_->motion_continuations.size()) continue;
        const auto& pending = program_cycle_claim_run_->motion_continuations[node_index];
        if (!pending) continue;
        const auto& admission = admissions[index];
        if (node == nodes.end() || node->owner_api != "kernel.operation" ||
            pending->origin.node_instance_id != scope.node_instance_id ||
            pending->origin.node_attempt_id == scope.node_attempt_id ||
            admission.payload_kind != OperationParticipantPayloadKind::kMotion ||
            pending->binding.value() != admission.registration.binding.value() ||
            !admission.motion_request ||
            pending->base_frame != admission.motion_request->binding.base_frame ||
            pending->tcp_frame != admission.motion_request->binding.tcp_frame ||
            pending->planner_capability !=
                admission.motion_request->binding.planner_capability_binding ||
            pending->authored_motion_kind !=
                std::get<MotionParticipantIntentDTO>(intent.participants[index].payload)
                    .motion.index()) {
          return reject<OperationAcceptedDTO>(
              "new synchronized attempt differs from its retained motion continuation",
              "program_motion_continuation_mismatch", ErrorCode::kInvalidRequest);
        }
        auto& participant = prepared_operation.mapped()->participants[index];
        participant.continuation = pending->intent;
        participant.continuation_node_index = node_index;
      }
    }
    if (emergency_stop_reserve_.find(intent.request_context.request_id.value()).has_value()) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID became retained by emergency Stop",
                "request_id_conflict"));
    }
    const bool engine_in_use =
        std::any_of(active_operations_.begin(), active_operations_.end(), [&](const auto& item) {
          return std::any_of(
              selected_engines.begin(), selected_engines.end(),
              [&](const auto* engine) { return item.second->participant_for(engine) != nullptr; });
        });
    const bool request_id_used =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.request_id() == intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             intent.request_context.request_id;
                    });
    if (request_id_used) {
      return Result<OperationAcceptedDTO>::Err(
          error("request_id_conflict", "operation request ID was used during admission",
                "request_id_conflict"));
    }
    if (engine_in_use) {
      return reject<OperationAcceptedDTO>(
          "one coordinated participant engine became busy during admission",
          "participant_engine_busy");
    }
    if (!make_operation_admission_capacity_locked()) {
      return Result<OperationAcceptedDTO>::Err(
          error("operation_capacity_full", "active operation capacity reached its fixed bound",
                "operation_capacity_full"));
    }
    auto reservation =
        config_.operation_admission_gate != nullptr
            ? config_.operation_admission_gate->reserve_batch(*resource_slots_, reservation_rows)
            : resource_slots_->reserve_batch(reservation_rows);
    if (!reservation.ok()) {
      return Result<OperationAcceptedDTO>::Err(reservation.error());
    }
    const auto inserted = active_operations_.insert(std::move(prepared_operation));
    if (!inserted.inserted) {
      const auto released = resource_slots_->release_batch(reservation_rows);
      if (!released.ok()) {
        return Result<OperationAcceptedDTO>::Err(released.error());
      }
      return reject<OperationAcceptedDTO>("operation identity is already resident",
                                          "operation_id_conflict", ErrorCode::kInvariantViolation);
    }
    ++next_operation_id_;
    protected_operation_ids = durable_operation_retention_ids_locked();
  }

  OperationCommitGuard admission_commit(*this, operation_id);
  auto durable_acceptance = attempt_journal_write([&] {
    return journal_.RecordOperationAccepted(config_.request_owner_epoch, intent, accepted,
                                            operation_state, protected_operation_ids);
  });
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation != nullptr) {
      operation->ordinary_acceptance_pending = false;
      if (!operation->state.active_stop_id.has_value()) {
        const auto released = resource_slots_->release_batch(reservation_rows);
        if (!released.ok()) {
          return Result<OperationAcceptedDTO>::Err(released.error());
        }
        active_operations_.erase(operation_id);
      }
    }
    return Result<OperationAcceptedDTO>::Err(durable_acceptance.error());
  }

  bool prepare_allowed = false;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr) {
      return reject<OperationAcceptedDTO>("operation disappeared after durable acceptance",
                                          "active_operation_lost", ErrorCode::kInvariantViolation);
    }
    operation->ordinary_acceptance_pending = false;
    operation->ordinary_acceptance_durable = true;
    replay_ledger_.push_back(std::move(prepared_replay));
    prepare_allowed = !operation->state.active_stop_id.has_value() &&
                      std::all_of(operation->participants.begin(), operation->participants.end(),
                                  [](const auto& participant) {
                                    return participant.slot_state == ResourceSlotState::kReserved;
                                  });
  }
  admission_commit.release();
  if (!prepare_allowed) {
    return Result<OperationAcceptedDTO>::Ok(accepted);
  }

  for (auto& admission : admissions) {
    std::shared_ptr<const detail::MotionResumeIntent> continuation;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr
                              ? nullptr
                              : operation->participant_for(admission.registration.engine);
      if (participant == nullptr) {
        motion_admission_closed_ = true;
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      if (operation->state.active_stop_id.has_value() || participant->stop_cancelled_pre_dispatch ||
          terminal(operation->state.state)) {
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      participant->prepare_in_flight = true;
      continuation = participant->continuation;
    }
    Result<std::uint64_t> prepared = [&]() {
      if (admission.payload_kind == OperationParticipantPayloadKind::kMotion) {
        return admission.registration.prepared_motion_engine->PrepareMotion(
            ResolvedPreparedMotionExecutionRequestDTO{
                *admission.motion_request, operation_revision, admission.registration.binding,
                *admission.registration.stop_readiness, admission.requested_tcp_linear_speed_mps,
                *admission.registration.deployment_maximum_tcp_linear_speed_mps,
                admission.registration.retained_default_stop_limits,
                admission.registration.stop_node_category, admission.stop_plan,
                admission.fault_stop_plan, std::move(continuation), common_timing});
      }
      return admission.registration.prepared_motion_engine->PrepareCapabilityCommand(
          ResolvedPreparedCapabilityCommandExecutionRequestDTO{
              admission.prepared_command_product, admission.registration.binding,
              *admission.registration.stop_readiness,
              admission.registration.retained_default_stop_limits,
              admission.registration.stop_node_category, *admission.command_request});
    }();
    if (!prepared.ok()) {
      {
        // This exact initial Prepare call has returned without a product.
        // Retire its borrow even when a sibling already owns the abort.
        std::lock_guard state_lock(state_mutex_);
        auto* operation = find_active_operation_locked(operation_id);
        if (auto* participant = operation == nullptr
                                    ? nullptr
                                    : operation->participant_for(admission.registration.engine)) {
          participant->last_subordinate_error = prepared.error();
          participant->prepare_in_flight = false;
          settle_completed_control_locked(operation_id);
        }
      }
      (void)fail_pre_dispatch(operation_id, admission.registration.engine,
                                          prepared.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }

    Result<void> admitted = Result<void>::Ok();
    bool preparation_orphaned = false;
    bool stop_owns_preparation = false;
    bool cancel_late_preparation = false;
    std::optional<StopControlWorkToken> stop_wakeup;
    {
      OperationCommitGuard commit(*this, operation_id);
      std::unique_lock state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr
                              ? nullptr
                              : operation->participant_for(admission.registration.engine);
      if (participant == nullptr) {
        preparation_orphaned = true;
        admitted = reject<void>("coordinated participant disappeared during preparation",
                                "active_operation_lost", ErrorCode::kInvariantViolation);
      } else {
        // An abort captured while this call was still in flight did not own
        // its returned product. Keep the borrow until that exact product has
        // been cancelled, or handed to an actual Stop transaction.
        cancel_late_preparation = participant->stop_cancelled_pre_dispatch &&
                                  (!operation->state.active_stop_id.has_value() ||
                                   operation->terminal_stop_result().has_value());
        participant->prepare_in_flight = cancel_late_preparation;
        if (participant->subordinate_id != 0U && participant->subordinate_id != prepared.value()) {
          admitted =
              reject<void>("prepared result identity changed during coordinated admission",
                           "subordinate_execution_id_mismatch", ErrorCode::kInvariantViolation);
        } else {
          participant->subordinate_id = prepared.value();
          participant->preparation_id = prepared.value();
          const auto state_row =
              std::find_if(operation->state.participants.begin(),
                           operation->state.participants.end(), [&](const auto& row) {
                             return row.participant_id == participant->slot_owner.participant_id;
                           });
          if (state_row == operation->state.participants.end()) {
            admitted =
                reject<void>("coordinated participant state row is missing",
                             "operation_participant_state_missing", ErrorCode::kInvariantViolation);
          } else if (operation->state.active_stop_id.has_value() &&
                     operation->state.active_stop_generation.has_value()) {
            stop_owns_preparation = !cancel_late_preparation;
            const auto transaction = std::find_if(
                stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
                  return candidate != nullptr && candidate->active &&
                         candidate->stop_id == operation->state.active_stop_id &&
                         candidate->stop_generation == *operation->state.active_stop_generation;
                });
            if (transaction == stop_transactions_.end()) {
              motion_admission_closed_ = true;
            } else if (!(*transaction)->terminal) {
              stop_wakeup = (*transaction)->token;
              stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
            }
          } else if (!cancel_late_preparation) {
            auto captured = operation->state;
            captured.participants[std::distance(operation->state.participants.begin(), state_row)]
                .state = admission.payload_kind == OperationParticipantPayloadKind::kMotion
                             ? OperationStateKind::kPlanning
                             : OperationStateKind::kPrepared;
            captured.state = aggregate_participant_state(captured.participants);
            captured.updated_at_ns = clock_.now();
            auto durable = record_operation_state_locked(
                state_lock, captured, nullptr,
                admission.payload_kind == OperationParticipantPayloadKind::kMotion
                    ? "coordinated_participant_motion_admitted"
                    : "coordinated_participant_command_admitted");
            if (!durable.ok()) {
              admitted = Result<void>::Err(durable.error());
            } else if (durable.value()) {
              operation->state = std::move(captured);
            } else {
              stop_owns_preparation = true;
            }
          }
        }
      }
    }
    if (stop_wakeup.has_value()) {
      stop_control_executor_->Publish(*stop_wakeup);
    }
    if (stop_owns_preparation) {
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    if (cancel_late_preparation) {
      const auto cancelled = cancel_prepared_participant(
          *admission.registration.prepared_motion_engine, admission.payload_kind, prepared.value());
      OperationCommitGuard commit(*this, operation_id);
      std::unique_lock state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr
                              ? nullptr
                              : operation->participant_for(admission.registration.engine);
      if (participant == nullptr) {
        motion_admission_closed_ = true;
        return Result<OperationAcceptedDTO>::Ok(accepted);
      }
      participant->prepare_in_flight = false;
      if (!cancelled.ok()) {
        motion_admission_closed_ = true;
        participant->last_subordinate_error = cancelled.error();
        for (auto& row : operation->state.participants) {
          if (row.participant_id == participant->slot_owner.participant_id) {
            row.state = OperationStateKind::kInterventionRequired;
            row.terminal_error = cancelled.error();
          }
        }
        operation->state.state = OperationStateKind::kInterventionRequired;
        operation->state.updated_at_ns = clock_.now();
        stamp_operation_capture_locked(*operation, operation->state);
        if (!operation->state.active_stop_id.has_value()) {
          auto captured = operation->state;
          auto durable = record_operation_state_locked(state_lock, captured, nullptr,
                                                       "coordinated_late_prepare_cancel_failed");
          if (durable.ok() && durable.value()) operation->state = std::move(captured);
        }
      }
      // This path follows an abort or an already terminal Stop. Disposal
      // releases the borrowed product; there is no active Stop to wake.
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    if (preparation_orphaned) {
      const auto cancelled = cancel_prepared_participant(
          *admission.registration.prepared_motion_engine, admission.payload_kind, prepared.value());
      if (!cancelled.ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    if (!admitted.ok()) {
      (void)fail_pre_dispatch(operation_id, admission.registration.engine,
                                          admitted.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
    auto advanced =
        admission.payload_kind == OperationParticipantPayloadKind::kMotion
            ? advance_prepared_motion(operation_id, admission.registration.engine)
            : advance_prepared_capability_command(operation_id, admission.registration.engine);
    if (!advanced.ok()) {
      (void)fail_pre_dispatch(operation_id, admission.registration.engine,
                                          advanced.error());
      return Result<OperationAcceptedDTO>::Ok(accepted);
    }
  }
  return Result<OperationAcceptedDTO>::Ok(accepted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::reset(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kReset);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::acknowledge_completed(
    const OperationControlRequestDTO& request) {
  return apply_control(request, OperationControlKind::kAcknowledgeCompleted);
}

Result<OperationControlAcceptedDTO> ResourceAddressedExecutionKernel::apply_control(
    const OperationControlRequestDTO& request, OperationControlKind control,
    const bool automatic_manual_retirement) {
  struct ControlTarget final {
    OperationParticipantId participant_id;
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t subordinate_id{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::string command_target;
    std::uint64_t subordinate_generation{};
  };
  if (operation_control_request_field_error(request).has_value()) {
    return reject<OperationControlAcceptedDTO>("operation control request is malformed",
                                               "operation_control_request_invalid",
                                               ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<OperationControlAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "operation control names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  OperationCommitGuard commit(*this, request.operation_id);
  std::vector<ControlTarget> targets;
  bool reset_already_terminal = false;
  std::optional<OperationStateDTO> accepted_state;
  OperationControlAcceptedDTO accepted{RequestId("pending"),
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       OperationId("pending"),
                                       OperationRevision(0U),
                                       control,
                                       OperationRevision(0U),
                                       0};
  {
    std::lock_guard state_lock(state_mutex_);
    if (operation_control_reconfiguration_in_progress_) {
      return reject<OperationControlAcceptedDTO>(
          "operation-control lanes are being atomically reconfigured",
          "operation_control_reconfiguration_in_progress", ErrorCode::kInvalidStateTransition);
    }
    if (emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    const auto replay = std::find_if(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                                     [&](const ControlReplayEntry& entry) {
                                       return entry.request.request_context.request_id ==
                                              request.request_context.request_id;
                                     });
    if (replay != control_replay_ledger_.end()) {
      if (replay->request != request || replay->accepted.control != control) {
        return Result<OperationControlAcceptedDTO>::Err(error(
            "request_id_conflict", "operation control request ID was reused with different intent",
            "request_id_conflict"));
      }
      if (!replay->durable) {
        return reject<OperationControlAcceptedDTO>(
            "operation control acceptance is still being durably admitted",
            "operation_control_admission_pending", ErrorCode::kInvalidStateTransition);
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<OperationControlAcceptedDTO>::Ok(std::move(accepted));
    }
    if (std::any_of(replay_ledger_.begin(), replay_ledger_.end(), [&](const ReplayEntry& entry) {
          return entry.intent.request_context.request_id == request.request_context.request_id;
        })) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict", "operation control request ID was already used by Execute",
                "request_id_conflict"));
    }
    if (std::any_of(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                    [&](const StopReplayEntry& entry) {
                      return entry.request_id() == request.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             request.request_context.request_id;
                    }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& entry) {
                      return entry.request.request_context.request_id ==
                             request.request_context.request_id;
                    })) {
      return Result<OperationControlAcceptedDTO>::Err(
          error("request_id_conflict",
                "operation control request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr) {
      return reject<OperationControlAcceptedDTO>("operation control names a non-resident operation",
                                                 "operation_not_resident",
                                                 ErrorCode::kMissingReference);
    }
    if (automatic_manual_retirement &&
        (!manual_operation_claim_.has_value() ||
         manual_operation_claim_->intent.owner_epoch != config_.request_owner_epoch ||
         manual_operation_claim_->intent.owner_transaction_id != request.operation_id.value() ||
         (active_operation->state.state != OperationStateKind::kCompleted &&
          active_operation->state.state != OperationStateKind::kStopped &&
          active_operation->state.state != OperationStateKind::kCancelledPreDispatch) ||
         std::any_of(active_operation->state.participants.begin(),
                     active_operation->state.participants.end(),
                     [](const auto& row) { return row.terminal_error.has_value(); }))) {
      return reject<OperationControlAcceptedDTO>(
          "automatic retirement requires current clean manual settlement",
          "manual_operation_not_clean_terminal");
    }
    const auto current_revision = active_operation->state.operation_revision;
    if (current_revision != request.expected_operation_revision) {
      return reject<OperationControlAcceptedDTO>("operation control expected revision is stale",
                                                 "operation_revision_stale");
    }
    if (!control_admissible(control, active_operation->state.state)) {
      return reject<OperationControlAcceptedDTO>(
          std::string(control_name(control)) + " is invalid for the current operation state",
          "operation_control_state_invalid", ErrorCode::kInvalidStateTransition);
    }
    if (active_operation->pending_control.has_value() ||
        active_operation->recovery_effect_in_flight ||
        active_operation->coordinated_cancellation_in_flight) {
      return reject<OperationControlAcceptedDTO>(
          "another operation control is already being admitted", "operation_control_busy");
    }
    if ((control == OperationControlKind::kAcknowledgeCompleted ||
         control == OperationControlKind::kReset) &&
        (active_operation->dispatch_effect_in_flight() ||
         active_operation->reconciliation_in_flight() ||
         active_operation->runtime_scale_effect_in_flight ||
         active_operation->stop_effect_in_flight ||
         (terminal(active_operation->state.state) &&
          std::any_of(active_operation->participants.begin(), active_operation->participants.end(),
                      [](const auto& participant) {
                        return participant.prepare_in_flight ||
                               participant.prepared_advance_in_flight;
                      })))) {
      return reject<OperationControlAcceptedDTO>(
          "operation settlement cannot overtake an in-flight subordinate effect",
          "operation_effect_in_flight");
    }
    if ((control == OperationControlKind::kAcknowledgeCompleted ||
         control == OperationControlKind::kReset) &&
        active_operation->state.active_stop_id.has_value() &&
        !active_operation->terminal_stop_result().has_value()) {
      // A participant may already be at rest while the containing Stop is
      // still settling another participant. Its exact certificate, not that
      // early local observation, is the existing cleanup authority.
      return reject<OperationControlAcceptedDTO>(
          "operation settlement awaits its exact Stop certificate", "operation_effect_in_flight");
    }
    reset_already_terminal = terminal(active_operation->state.state);
    if (current_revision.value() >= kMaximumKernelJsonInteger) {
      return reject<OperationControlAcceptedDTO>("operation revision space is exhausted",
                                                 "operation_revision_exhausted",
                                                 ErrorCode::kInternalError);
    }
    const OperationRevision next_revision(current_revision.value() + 1U);
    targets.reserve(active_operation->participants.size());
    for (std::size_t index = 0U; index < active_operation->participants.size(); ++index) {
      const auto& participant = active_operation->participants[index];
      const auto& participant_state = active_operation->state.participants[index];
      if (control == OperationControlKind::kAcknowledgeCompleted &&
          participant.payload_kind == OperationParticipantPayloadKind::kMotion) {
        targets.push_back(ControlTarget{participant_state.participant_id,
                                        participant.engine,
                                        participant.subordinate_id,
                                        participant.payload_kind,
                                        participant.binding.value().command_target_id.value(),
                                        participant.subordinate_generation});
      } else if (control == OperationControlKind::kReset && participant.subordinate_id != 0U &&
                 (participant.prepared_dispatch_released ||
                  (participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                   participant.stop_cancelled_pre_dispatch &&
                   active_operation->state.active_stop_id.has_value()))) {
        targets.push_back(ControlTarget{participant_state.participant_id,
                                        participant.engine,
                                        participant.subordinate_id,
                                        participant.payload_kind,
                                        participant.binding.value().command_target_id.value(),
                                        participant.subordinate_generation});
      }
    }
    const auto accepted_at_ns = clock_.now();
    accepted = OperationControlAcceptedDTO{request.request_context.request_id,
                                           RequestReplayStatusDTO::kAcceptedNew,
                                           request.operation_id,
                                           current_revision,
                                           control,
                                           next_revision,
                                           accepted_at_ns};
    accepted_state = active_operation->state;
    accepted_state->operation_revision = next_revision;
    accepted_state->updated_at_ns = accepted_at_ns;
    stamp_operation_capture_locked(*active_operation, *accepted_state);
    active_operation->pending_control = PendingOperationControl{accepted,
                                                                targets.size(),
                                                                0U,
                                                                std::nullopt,
                                                                false,
                                                                false,
                                                                false,
                                                                request.execution_scope};
    std::erase_if(control_replay_ledger_, [&](const ControlReplayEntry& entry) {
      return entry.accepted.operation_id == request.operation_id;
    });
    control_replay_ledger_.push_back(ControlReplayEntry{request, accepted, false});
  }

  auto durable_acceptance = attempt_journal_write([&] {
    return journal_.RecordControlAccepted(config_.request_owner_epoch, request, accepted,
                                          *accepted_state);
  });
  if (!durable_acceptance.ok()) {
    std::lock_guard state_lock(state_mutex_);
    const auto replay =
        std::find_if(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                     [&](const ControlReplayEntry& entry) {
                       return entry.accepted.request_id == accepted.request_id && !entry.durable;
                     });
    if (replay != control_replay_ledger_.end()) {
      control_replay_ledger_.erase(replay);
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation != nullptr && active_operation->pending_control.has_value() &&
        active_operation->pending_control->accepted == accepted) {
      active_operation->pending_control.reset();
    }
    return Result<OperationControlAcceptedDTO>::Err(durable_acceptance.error());
  }

  std::optional<StopIntentDTO> reset_stop;
  bool settle_without_effect = false;
  bool defer_effects_to_stop = false;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                                     [&](const ControlReplayEntry& entry) {
                                       return entry.accepted.request_id == accepted.request_id;
                                     });
    if (replay == control_replay_ledger_.end()) {
      motion_admission_closed_ = true;
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    replay->durable = true;
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr || !active_operation->pending_control.has_value() ||
        active_operation->pending_control->accepted != accepted) {
      motion_admission_closed_ = true;
      return Result<OperationControlAcceptedDTO>::Ok(accepted);
    }
    active_operation->pending_control->durable = true;
    const auto terminal_stop_result = active_operation->terminal_stop_result();
    if (active_operation->state.operation_revision != accepted.accepted_against_revision) {
      if (active_operation->state.active_stop_id.has_value()) {
        active_operation->pending_control->superseded_by_stop = true;
        if (control != OperationControlKind::kReset) {
          active_operation->pending_control.reset();
          defer_effects_to_stop = true;
        } else if (!terminal_stop_result.has_value()) {
          defer_effects_to_stop = true;
        } else if (*terminal_stop_result != StopCertificateResultDTO::kSucceededNominal &&
                   *terminal_stop_result != StopCertificateResultDTO::kSucceededEscalated) {
          active_operation->state.state = OperationStateKind::kInterventionRequired;
          active_operation->state.updated_at_ns = clock_.now();
          active_operation->pending_control.reset();
          motion_admission_closed_ = true;
          defer_effects_to_stop = true;
        } else {
          active_operation->pending_control->superseded_by_stop = false;
          settle_without_effect = targets.empty();
        }
      } else {
        motion_admission_closed_ = true;
        active_operation->pending_control.reset();
        defer_effects_to_stop = true;
      }
    } else {
      auto owner_rows =
          retained_owner_rows(active_operation->participants, resource_slots_->coherent_snapshot());
      if (!owner_rows.ok()) {
        motion_admission_closed_ = true;
        active_operation->pending_control.reset();
        return Result<OperationControlAcceptedDTO>::Ok(accepted);
      }
      auto revised =
          owner_rows.value().empty()
              ? Result<std::vector<ResourceSlotOwnerDTO>>::Ok(std::vector<ResourceSlotOwnerDTO>{})
              : resource_slots_->revise_owner_batch(owner_rows.value(),
                                                    accepted.operation_revision_after_acceptance);
      if (!revised.ok()) {
        motion_admission_closed_ = true;
        active_operation->state.state = OperationStateKind::kInterventionRequired;
        active_operation->state.updated_at_ns = clock_.now();
        active_operation->pending_control.reset();
        return Result<OperationControlAcceptedDTO>::Ok(accepted);
      }
      for (const auto& revised_owner : revised.value()) {
        const auto participant = std::find_if(
            active_operation->participants.begin(), active_operation->participants.end(),
            [&](const auto& candidate) {
              return candidate.slot_owner.participant_id == revised_owner.participant_id;
            });
        if (participant == active_operation->participants.end()) {
          motion_admission_closed_ = true;
          continue;
        }
        participant->slot_owner = revised_owner;
      }
      active_operation->state = *accepted_state;
      if (active_operation->runtime_scale_status.has_value()) {
        active_operation->runtime_scale_status->operation_revision =
            accepted.operation_revision_after_acceptance;
        active_operation->runtime_scale_status->updated_at_ns = accepted.accepted_at_ns;
      }
      if (control == OperationControlKind::kReset) {
        if (reset_already_terminal || terminal_stop_result.has_value()) {
          if (terminal_stop_result.has_value() &&
              *terminal_stop_result != StopCertificateResultDTO::kSucceededNominal &&
              *terminal_stop_result != StopCertificateResultDTO::kSucceededEscalated) {
            active_operation->state.state = OperationStateKind::kInterventionRequired;
            active_operation->state.updated_at_ns = clock_.now();
            active_operation->pending_control.reset();
            motion_admission_closed_ = true;
            defer_effects_to_stop = true;
          } else {
            active_operation->pending_control->superseded_by_stop = false;
            settle_without_effect = targets.empty();
          }
        } else if (active_operation->state.active_stop_id.has_value()) {
          active_operation->pending_control->superseded_by_stop = true;
          defer_effects_to_stop = true;
        } else {
          reset_stop =
              internal_control_stop_intent_locked(*active_operation, accepted, "operation_reset");
        }
      } else if (control == OperationControlKind::kAcknowledgeCompleted && targets.empty()) {
        settle_without_effect = true;
      }
    }
    if (!defer_effects_to_stop && !reset_stop.has_value() && !settle_without_effect) {
      active_operation->pending_control->effects_dispatched = true;
    }
  }

  commit.release();
  if (defer_effects_to_stop) {
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  if (settle_without_effect) {
    std::lock_guard state_lock(state_mutex_);
    auto settled = settle_control_operation_locked(request.operation_id, control);
    if (!settled.ok()) {
      motion_admission_closed_ = true;
    }
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }
  if (reset_stop.has_value()) {
    auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*reset_stop)});
    if (!stopped.ok()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      auto* operation = find_active_operation_locked(request.operation_id);
      if (operation != nullptr) {
        operation->state.state = OperationStateKind::kInterventionRequired;
        operation->state.updated_at_ns = clock_.now();
        operation->pending_control.reset();
      }
    }
    return Result<OperationControlAcceptedDTO>::Ok(accepted);
  }

  for (const auto& target : targets) {
    auto started = operation_control_executor_->TryStart(OperationControlEffectRequest{
        target.engine, request.operation_id, target.participant_id,
        accepted.operation_revision_after_acceptance, control, target.subordinate_id,
        target.payload_kind, target.command_target, target.subordinate_generation});
    if (started.status == OperationControlEffectStartStatus::kStarted) {
      continue;
    }
    {
      std::lock_guard state_lock(state_mutex_);
      // Capacity was checked before durable acceptance. Losing that handoff
      // is an owner invariant failure, not a provider's scoped refusal.
      motion_admission_closed_ = true;
    }
    CompleteOperationControlEffect(OperationControlEffectResult{
        request.operation_id, target.participant_id, accepted.operation_revision_after_acceptance,
        control, false,
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              "prequalified operation-control lane rejected accepted work",
              "operation_control_effect_handoff_failed")});
  }
  return Result<OperationControlAcceptedDTO>::Ok(accepted);
}

StopIntentDTO ResourceAddressedExecutionKernel::internal_control_stop_intent_locked(
    const ActiveOperation& operation, const OperationControlAcceptedDTO& accepted,
    const std::string_view reason) const {
  const auto material = config_.request_owner_epoch.value() + "\n" + accepted.request_id.value() +
                        "\n" + std::string(control_name(accepted.control)) + "\n" +
                        operation.state.operation_id.value();
  const RequestId request_id(manyforge::content_identity::sha256_digest(material));
  return StopIntentDTO{
      PublicRequestContextDTO{request_id, config_.request_owner_epoch, "kernel.operation_control",
                              accepted.request_id.value()},
      std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
          operation.state.operation_id, operation.state.operation_revision,
          operation.state.active_stop_generation.value_or(StopGeneration(0U))}},
      std::string(reason),
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}};
}

void ResourceAddressedExecutionKernel::settle_completed_control_locked(
    const OperationId& operation_id) {
  const auto* operation = find_active_operation_locked(operation_id);
  if (operation == nullptr || !operation->pending_control.has_value()) return;
  const auto& pending = *operation->pending_control;
  if (pending.durable && pending.effects_dispatched &&
      pending.completed_effects == pending.expected_effects && !pending.first_failure.has_value() &&
      (pending.accepted.control == OperationControlKind::kReset ||
       pending.accepted.control == OperationControlKind::kAcknowledgeCompleted)) {
    if (!settle_control_operation_locked(operation_id, pending.accepted.control).ok())
      motion_admission_closed_ = true;
  }
}

OperationStateDTO ResourceAddressedExecutionKernel::capture_control_failure_locked(
    ActiveOperation& operation) {
  operation.state.state = OperationStateKind::kInterventionRequired;
  operation.state.updated_at_ns = clock_.now();
  for (const auto& participant : operation.participants) {
    if (!participant.last_subordinate_error.has_value()) continue;
    for (auto& row : operation.state.participants) {
      if (row.participant_id == participant.slot_owner.participant_id) {
        row.state = OperationStateKind::kInterventionRequired;
        row.terminal_error = participant.last_subordinate_error;
        break;
      }
    }
  }
  stamp_operation_capture_locked(operation, operation.state);
  return operation.state;
}

void ResourceAddressedExecutionKernel::clear_motion_continuation_locked(
    const ActiveOperation& operation) {
  if (!program_cycle_claim_run_ ||
      program_cycle_claim_run_->program_run_id != operation.state.program_run_id)
    return;
  for (const auto& participant : operation.participants) {
    if (!participant.continuation || participant.continuation_node_index >=
                                         program_cycle_claim_run_->motion_continuations.size())
      continue;
    auto& slot =
        program_cycle_claim_run_->motion_continuations[participant.continuation_node_index];
    if (slot && slot->intent == participant.continuation) slot.reset();
  }
}

Result<void> ResourceAddressedExecutionKernel::settle_control_operation_locked(
    const OperationId& operation_id, const OperationControlKind control) {
  auto* operation = find_active_operation_locked(operation_id);
  if (operation == nullptr || !operation->pending_control.has_value() ||
      operation->pending_control->accepted.control != control ||
      !operation->pending_control->durable) {
    return reject<void>("operation control settlement has no durable resident claim",
                        "operation_control_settlement_missing", ErrorCode::kInvariantViolation);
  }
  if (operation->ordinary_commit_in_flight || operation->coordinated_cancellation_in_flight ||
      operation->dispatch_effect_in_flight() || operation->reconciliation_in_flight() ||
      std::any_of(operation->participants.begin(), operation->participants.end(),
                  [](const auto& participant) {
                    return participant.prepare_in_flight || participant.prepared_advance_in_flight;
                  })) {
    return Result<void>::Ok();
  }
  if (control == OperationControlKind::kAcknowledgeCompleted &&
      operation->state.state != OperationStateKind::kCompleted) {
    return reject<void>("completion acknowledgement lost its terminal state",
                        "operation_acknowledgement_state_changed",
                        ErrorCode::kInvalidStateTransition);
  }
  if (control != OperationControlKind::kAcknowledgeCompleted &&
      control != OperationControlKind::kReset) {
    return reject<void>("nonterminal operation control cannot settle an operation",
                        "operation_control_settlement_kind_invalid", ErrorCode::kInvalidRequest);
  }
  if (operation->state.active_stop_id.has_value()) {
    const auto terminal_stop = operation->terminal_stop_result();
    if (!terminal_stop.has_value()) {
      // Terminal provider feedback alone cannot release Stop-owned resources.
      return Result<void>::Ok();
    }
    const auto outcome = *terminal_stop;
    if (outcome != StopCertificateResultDTO::kSucceededNominal &&
        outcome != StopCertificateResultDTO::kSucceededEscalated) {
      // Consume the exact cleanup result, but never use it to release a
      // resource whose newer Stop still requires containment/reconciliation.
      operation->state.state = OperationStateKind::kInterventionRequired;
      operation->pending_control.reset();
      return Result<void>::Ok();
    }
  }
  auto owner_rows =
      retained_owner_rows(operation->participants, resource_slots_->coherent_snapshot());
  if (!owner_rows.ok()) {
    return Result<void>::Err(owner_rows.error());
  }
  if (!owner_rows.value().empty()) {
    auto released = resource_slots_->release_batch(owner_rows.value());
    if (!released.ok()) {
      return released;
    }
  }
  operation->pending_control.reset();
  clear_motion_continuation_locked(*operation);
  retain_historical_operation_locked(operation->state);
  active_operations_.erase(operation_id);
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::OperationControlEffectCompleted(
    BoundedOperationControlExecutor& executor, const OperationControlEffectHandle handle) noexcept {
  try {
    auto taken = executor.TryTake(handle);
    if (taken.status == OperationControlEffectTakeStatus::kCompleted ||
        taken.status == OperationControlEffectTakeStatus::kInfrastructureFailure) {
      // The moved result no longer needs its bounded worker slot. Make that
      // lane reusable before publishing free resources or retry eligibility.
      if (executor.Release(handle) != OperationControlEffectReleaseStatus::kReleased ||
          !taken.result.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      CompleteOperationControlEffect(std::move(*taken.result));
    } else if (taken.status != OperationControlEffectTakeStatus::kPending) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

OperationStateDTO ResourceAddressedExecutionKernel::publish_settlement_fault_locked(
    ActiveOperation& operation) {
  // A cleanup fault is a successor current state, not a replacement of the
  // immutable physical Stop outcome. Reuse the existing operation revision
  // so delayed durable projections have a causal order even at equal clocks.
  auto owners = retained_owner_rows(operation.participants, resource_slots_->coherent_snapshot());
  if (!owners.ok() || operation.state.operation_revision.value() >= kMaximumKernelJsonInteger) {
    motion_admission_closed_ = true;
  } else {
    const OperationRevision next(operation.state.operation_revision.value() + 1U);
    auto revised = owners.value().empty()
                       ? Result<std::vector<ResourceSlotOwnerDTO>>::Ok({})
                       : resource_slots_->revise_owner_batch(owners.value(), next);
    if (!revised.ok()) {
      motion_admission_closed_ = true;
    } else {
      for (const auto& owner : revised.value()) {
        const auto participant = std::find_if(
            operation.participants.begin(), operation.participants.end(),
            [&](const auto& row) { return row.slot_owner.participant_id == owner.participant_id; });
        if (participant == operation.participants.end()) {
          motion_admission_closed_ = true;
        } else {
          participant->slot_owner = owner;
        }
      }
      operation.state.operation_revision = next;
      if (operation.runtime_scale_status.has_value())
        operation.runtime_scale_status->operation_revision = next;
    }
  }
  operation.state.state = OperationStateKind::kInterventionRequired;
  operation.state.updated_at_ns = clock_.now();
  operation.pending_control.reset();
  stamp_operation_capture_locked(operation, operation.state);
  return operation.state;
}

void ResourceAddressedExecutionKernel::CompleteOperationControlEffect(
    OperationControlEffectResult result) noexcept {
  std::optional<OperationStateDTO> settlement_failure;
  try {
    [&] {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(result.operation_id);
      if (operation == nullptr || !operation->pending_control.has_value() ||
          operation->pending_control->accepted.control != result.control ||
          operation->pending_control->accepted.operation_revision_after_acceptance !=
              result.operation_revision) {
        return;
      }
      auto& pending = *operation->pending_control;
      const auto participant = std::find_if(
          operation->participants.begin(), operation->participants.end(), [&](const auto& value) {
            return value.slot_owner.participant_id == result.participant_id;
          });
      if (participant == operation->participants.end() ||
          pending.completed_effects >= pending.expected_effects) {
        motion_admission_closed_ = true;
        return;
      }
      if (!result.accepted) {
        const auto failure = result.error.value_or(
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  std::string("subordinate rejected accepted ") + control_name(result.control),
                  "subordinate_control_rejected"));
        participant->last_subordinate_error = failure;
        const auto public_participant = std::find_if(
            operation->state.participants.begin(), operation->state.participants.end(),
            [&](const auto& row) { return row.participant_id == result.participant_id; });
        if (public_participant != operation->state.participants.end()) {
          public_participant->terminal_error = failure;
          public_participant->state = OperationStateKind::kInterventionRequired;
          stamp_operation_capture_locked(*operation, operation->state);
        }
        if (!pending.first_failure.has_value()) {
          pending.first_failure = failure;
        }
      }
      ++pending.completed_effects;
      const bool superseded_by_stop =
          pending.superseded_by_stop || operation->state.active_stop_id.has_value();
      if (superseded_by_stop && result.control != OperationControlKind::kReset) {
        // Retain the exact control failure until the Stop terminal handoff.
        // No provider call runs under the state lock.
        if (operation->terminal_stop_result().has_value() &&
            pending.completed_effects == pending.expected_effects) {
          operation->pending_control.reset();
        }
      } else if (pending.completed_effects != pending.expected_effects) {
        // Terminal Reset/ACK cleanup waits for every effect.
        return;
      } else if (result.control == OperationControlKind::kReset) {
        if (pending.first_failure.has_value()) {
          if (operation->state.active_stop_id.has_value() &&
              !operation->terminal_stop_result().has_value()) {
            // The exact Stop certificate will publish this completed fault;
            // do not race its same-revision intermediate state with a write.
            return;
          }
          // Persist the failure in the existing owner state. A clean terminal
          // state would let an automatic owner repeatedly retry settlement.
          settlement_failure = publish_settlement_fault_locked(*operation);
        } else {
          auto settled = settle_control_operation_locked(result.operation_id, result.control);
          if (!settled.ok()) {
            motion_admission_closed_ = true;
          }
        }
        return;
      } else if (!pending.first_failure.has_value()) {
        if (!settle_control_operation_locked(result.operation_id, result.control).ok()) {
          motion_admission_closed_ = true;
        }
        return;
      } else if (!superseded_by_stop) {
        // This known operation retains its exact resource reservation until
        // explicit owner reconciliation. Do not turn a scoped settlement
        // failure into the permanent cell-wide invariant-failure latch.
        settlement_failure = publish_settlement_fault_locked(*operation);
      }
    }();
    if (settlement_failure.has_value()) {
      // Fault visibility and scoped fencing must not wait behind ordinary
      // storage, and this write must never hold the emergency capture lock.
      const auto persisted = [&]() -> Result<std::uint64_t> {
        try {
          return journal_.RecordOperationState(
              config_.request_owner_epoch, *settlement_failure, nullptr,
              result.control == OperationControlKind::kReset
                  ? "operation_reset_failed"
                  : "operation_acknowledge_completed_failed");
        } catch (...) {
          return Result<std::uint64_t>::Err(error("operation_settlement_fault_persistence_threw",
                                                  "settlement fault persistence threw",
                                                  "settlement_fault_persistence_failed"));
        }
      }();
      if (!persisted.ok()) {
        std::lock_guard state_lock(state_mutex_);
        auto* operation = find_active_operation_locked(result.operation_id);
        if (operation != nullptr && operation->state == *settlement_failure) {
          for (auto& row : operation->state.participants) {
            if (row.terminal_error.has_value()) {
              row.terminal_error->details["settlement_fault_persistence"] = "failed";
              row.terminal_error->details["persistence_error_code"] = persisted.error().code;
            }
          }
        }
      }
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::stop(const StopIntentDTO& intent) {
  return accept_stop(EmergencyStopProjectedIntentDTO{intent});
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::full_stop(
    const FullStopIntentDTO& intent) {
  return accept_stop(EmergencyStopProjectedIntentDTO{intent});
}

Result<std::optional<ResolvedManualProgramHandover>>
ResourceAddressedExecutionKernel::get_manual_program_handover(const OperationIntentDTO& intent) {
  // Stabilize the physical registration inventory without taking Stop's lock
  // across resolution, allocation or the potentially large captured table.
  std::lock_guard submission_lock(submission_mutex_);
  return resolve_manual_program_handover(intent);
}

Result<ResolvedManualControlAvailability>
ResourceAddressedExecutionKernel::get_manual_control_availability(
    const OperationIntentDTO& intent) {
  using Reply = Result<ResolvedManualControlAvailability>;
  std::lock_guard submission_lock(submission_mutex_);
  const auto valid = validate_operation_intent(intent);
  if (!valid.ok()) return Reply::Err(valid.error());
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch ||
      intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      intent.participants.front().execution_scope)
    return reject<ResolvedManualControlAvailability>(
        "manual availability requires a current direct target", "manual_handover_intent_invalid");
  const auto found = resolve_registration(intent, 0U);
  if (!found.ok()) return Reply::Err(found.error());
  if (!found.value().intrinsic_direct_control)
    return reject<ResolvedManualControlAvailability>(
        "manual availability requires an intrinsic direct target",
        "direct_control_product_mismatch");
  ResolvedManualControlAvailability result;
  const auto unavailable = [&](const ErrorDTO& error) {
    const auto reason = error.details.find("reason");
    result.unavailable_reason = reason == error.details.end() ? error.code : reason->second;
    result.cleanup_required = false;
    return Reply::Ok(std::move(result));
  };
  auto resolved = resolve_manual_program_handover(found.value());
  if (!resolved.ok()) return unavailable(resolved.error());
  result.handover = std::move(resolved.value());
  auto& handover = result.handover;
  std::optional<CellAdmissionClaimGeneration> parent;
  if (handover) parent = handover->capture.claim_generation;
  // Keep the same admission -> state ordering as actual claim acquisition.
  // Stop bypasses this ordinary read just as it bypasses the claim write.
  std::lock_guard admission_lock(*admission_mutex_);
  const auto ready = cell_admission_owner_->CheckManualCellAdmissionReadiness(parent);
  if (!ready.ok()) return unavailable(ready.error());
  const auto& binding = found.value().binding.value();
  if (!binding.control_path || !found.value().stop_readiness)
    return unavailable(
        {"manual_control_not_ready", "target has no current route/Stop readiness", {}});
  const auto& control = *binding.control_path;
  const auto route = runtime_route_configuration_readbacks_.find(control.control_domain_id);
  if (runtime_route_configuration_publication_required_.contains(control.control_domain_id) ||
      (route != runtime_route_configuration_readbacks_.end() &&
       (!route->second.operation_admission_open ||
        route->second.active_configuration.active_control_configuration_id !=
            control.active_control_configuration_id ||
        route->second.active_configuration.active_control_configuration_generation !=
            control.active_control_configuration_generation)))
    return unavailable(
        {"runtime_route_configuration_not_ready", "target route is not current", {}});
  if (auto refusal = component_command_refusal_locked(found.value()); refusal.has_value())
    return unavailable(*refusal);
  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) resources.push_back(resource.resource_id);
  std::sort(resources.begin(), resources.end());
  std::lock_guard state_lock(state_mutex_);
  if (motion_admission_closed_ || motion_admission_unreconciled_)
    return unavailable({"motion_admission_closed", "motion admission is closed", {}});
  if (handover) {
    const auto held = validate_manual_program_handover_locked(*handover);
    if (!held.ok()) return unavailable(held.error());
  } else if (program_cycle_claim_run_) {
    return unavailable({"manual_handover_scope_stale", "a Program acquired the target", {}});
  }
  for (const auto& [id, operation] : active_operations_) {
    (void)id;
    const bool overlaps =
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [&](const auto& participant) {
                      return participant.engine == found.value().engine ||
                             std::any_of(participant.resources.begin(), participant.resources.end(),
                                         [&](const auto& resource) {
                                           return std::binary_search(resources.begin(),
                                                                     resources.end(), resource);
                                         });
                    });
    if (!overlaps) continue;
    const bool settled =
        operation->terminal_stop_result() == StopCertificateResultDTO::kSucceededNominal ||
        operation->state.state == OperationStateKind::kCompleted ||
        operation->state.state == OperationStateKind::kCancelledPreDispatch;
    if (!handover || operation->intent.program_run_id != handover->capture.program_run_id ||
        !settled || operation->dispatch_effect_in_flight() ||
        operation->recovery_effect_in_flight || operation->pending_control ||
        operation->stop_effect_in_flight)
      return unavailable({"manual_control_target_busy", "target operation is not settled", {}});
    result.cleanup_required = true;
  }
  result.available = true;
  return Reply::Ok(std::move(result));
}

Result<std::optional<ResolvedManualProgramHandover>>
ResourceAddressedExecutionKernel::resolve_manual_program_handover(
    const OperationIntentDTO& intent) {
  using Reply = Result<std::optional<ResolvedManualProgramHandover>>;
  const auto valid = validate_operation_intent(intent);
  if (!valid.ok()) return Reply::Err(valid.error());
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch ||
      intent.participants.size() != 1U ||
      intent.coordination_policy != OperationCoordinationPolicy::kIndependent ||
      intent.participants.front().execution_scope) {
    return reject<std::optional<ResolvedManualProgramHandover>>(
        "manual handover requires one current direct-control target",
        "manual_handover_intent_invalid");
  }
  auto manual = resolve_registration(intent, 0U);
  if (!manual.ok()) return Reply::Err(manual.error());
  if (!manual.value().intrinsic_direct_control) {
    return reject<std::optional<ResolvedManualProgramHandover>>(
        "manual handover requires an intrinsic direct-control binding",
        "direct_control_product_mismatch");
  }
  return resolve_manual_program_handover(manual.value());
}

Result<std::optional<ResolvedManualProgramHandover>>
ResourceAddressedExecutionKernel::resolve_manual_program_handover(
    const RuntimeParticipantExecutionRegistration& registration) {
  using Reply = Result<std::optional<ResolvedManualProgramHandover>>;
  std::optional<ResolvedManualProgramHandover> captured;
  {
    std::lock_guard lock(state_mutex_);
    if (motion_admission_closed_ || motion_admission_unreconciled_) {
      return reject<std::optional<ResolvedManualProgramHandover>>(
          "motion admission is closed or unreconciled", "motion_admission_closed");
    }
    if (!program_cycle_claim_run_) return Reply::Ok(std::nullopt);
    const auto& parent = *program_cycle_claim_run_;
    const auto fence = std::find_if(
        program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
        [&](const auto& item) { return item.program_run_id == parent.program_run_id; });
    if (!parent.capture || !parent.capture->binding || fence == program_run_stop_fences_.end()) {
      return reject<std::optional<ResolvedManualProgramHandover>>(
          "Program target mapping or hold is unavailable", "manual_handover_capture_unavailable");
    }
    captured.emplace(ResolvedManualProgramHandover{*parent.capture, fence->selection_revision, {}});
  }
  auto& result = *captured;

  std::map<TargetRoleId, bool> conflicts;
  const auto& targets = result.capture.binding->target_role_bindings;
  const auto& nodes = result.capture.table->nodes;
  const auto& manual_resources = registration.binding.value().resources;
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    bool selected = false;
    for (const auto& effect : nodes[index].target_effects) {
      if (!effect.effectful) continue;
      auto cached = conflicts.find(effect.target_role_id);
      if (cached == conflicts.end()) {
        const auto target = std::lower_bound(
            targets.begin(), targets.end(), effect.target_role_id,
            [](const auto& row, const auto& id) { return row.target_role_id < id; });
        if (target == targets.end() || target->target_role_id != effect.target_role_id) {
          return reject<std::optional<ResolvedManualProgramHandover>>(
              "captured producer has no exact Program target", "manual_handover_target_unresolved");
        }
        bool found = false;
        bool overlap = false;
        for (const auto& registration : registrations_) {
          const auto& binding = registration.binding.value();
          if (binding.controllable_component_id != target->controllable_component_id ||
              binding.command_target_id != target->command_target_id)
            continue;
          found = true;
          overlap =
              overlap ||
              std::any_of(
                  binding.resources.begin(), binding.resources.end(), [&](const auto& resource) {
                    return std::any_of(manual_resources.begin(), manual_resources.end(),
                                       [&](const auto& manual_resource) {
                                         return resource.resource_id == manual_resource.resource_id;
                                       });
                  });
        }
        if (!found) {
          return reject<std::optional<ResolvedManualProgramHandover>>(
              "captured producer has no current physical target",
              "manual_handover_target_unresolved");
        }
        cached = conflicts.emplace(effect.target_role_id, overlap).first;
      }
      selected = selected || cached->second;
    }
    if (selected) result.producer_node_indices.push_back(static_cast<std::uint32_t>(index));
  }

  {
    std::lock_guard lock(state_mutex_);
    const auto valid_hold = validate_manual_program_handover_locked(result);
    if (!valid_hold.ok()) return Reply::Err(valid_hold.error());
  }
  return Reply::Ok(std::move(result));
}

Result<void> ResourceAddressedExecutionKernel::validate_manual_program_handover_locked(
    const ResolvedManualProgramHandover& result) const {
  const auto fence = std::find_if(
      program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
      [&](const auto& item) { return item.program_run_id == result.capture.program_run_id; });
  if (motion_admission_closed_ || motion_admission_unreconciled_ || !program_cycle_claim_run_ ||
      program_cycle_claim_run_->claim_generation != result.capture.claim_generation ||
      !program_cycle_claim_run_->capture ||
      program_cycle_claim_run_->capture->table != result.capture.table ||
      program_cycle_claim_run_->capture->binding != result.capture.binding ||
      fence == program_run_stop_fences_.end() ||
      fence->selection_revision != result.selection_revision ||
      (fence->stop_id && !fence->releasable)) {
    return reject<void>("Program hold changed or is not settled", "manual_handover_scope_stale");
  }
  for (const auto index : result.producer_node_indices) {
    // Ready producers never started, so they are as quiet as Held ones.
    if (index >= fence->node_states.size() ||
        (fence->node_states[index] != ProgramNodeHoldStateDTO::kHeld &&
         fence->node_states[index] != ProgramNodeHoldStateDTO::kReady)) {
      return reject<void>("a conflicting Program producer is running, stopping or faulted",
                          "manual_handover_producer_not_held");
    }
  }
  if (std::any_of(
          fence->pending_effects.begin(), fence->pending_effects.end(), [&](const auto& effect) {
            return std::binary_search(result.producer_node_indices.begin(),
                                      result.producer_node_indices.end(), effect.node_index);
          })) {
    return reject<void>("selected Stop effects have not settled", "manual_handover_stop_unsettled");
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::register_program_run_capture(
    ProgramRunExecutionCaptureDTO capture) {
  const auto bounded = [](const auto& id) {
    return !id.empty() && id.value().size() <= 128U && id.value().find('\0') == std::string::npos;
  };
  if (!bounded(capture.request_owner_epoch) || !bounded(capture.program_runner_owner_epoch) ||
      !bounded(capture.program_run_id) || !bounded(capture.program_id) ||
      !bounded(capture.program_revision) || !bounded(capture.program_binding_id) ||
      !bounded(capture.retained_bundle_id) || !bounded(capture.graph_revision) ||
      !capture.claim_generation.nonzero() || capture.table == nullptr || !capture.binding ||
      capture.table->nodes.empty() || capture.table->nodes.size() > kMaxProgramTreeNodes ||
      capture.table->named_selections.size() > kMaxProgramExecutionGroups) {
    return reject<void>("Program execution capture is incomplete or exceeds its finite bound",
                        "program_execution_capture_invalid", ErrorCode::kInvalidRequest);
  }
  if (capture.binding->program_binding_id != capture.program_binding_id ||
      capture.binding->program_id != capture.program_id ||
      capture.binding->program_revision != capture.program_revision ||
      capture.binding->retained_bundle_id != capture.retained_bundle_id ||
      capture.binding->graph_revision != capture.graph_revision) {
    return reject<void>("Program capture Binding differs from its exact product",
                        "program_execution_capture_binding_mismatch", ErrorCode::kInvalidRequest);
  }
  // This validates a trusted composition input once, outside Stop/admission
  // ordering. Artifact provenance remains the sealed Program/Binding owner's
  // responsibility; a public client never supplies this table.
  std::size_t effect_count = 0U;
  for (std::size_t index = 0U; index < capture.table->nodes.size(); ++index) {
    const auto& node = capture.table->nodes[index];
    if (!bounded(node.instance_id) || node.owner_api.empty() || node.owner_api.size() > 128U ||
        (index > 0U && !(capture.table->nodes[index - 1U].instance_id < node.instance_id)) ||
        node.target_effects.size() > kMaxProgramTargetEffects - effect_count) {
      return reject<void>("Program execution nodes are not canonical and bounded",
                          "program_execution_capture_nodes_invalid", ErrorCode::kInvalidRequest);
    }
    effect_count += node.target_effects.size();
    for (std::size_t effect = 0U; effect < node.target_effects.size(); ++effect) {
      const auto& value = node.target_effects[effect];
      if (!bounded(value.target_role_id) || !bounded(value.capability_id) ||
          (effect > 0U && !(std::tie(node.target_effects[effect - 1U].target_role_id,
                                     node.target_effects[effect - 1U].capability_id) <
                            std::tie(value.target_role_id, value.capability_id)))) {
        return reject<void>("Program execution effects are not canonical",
                            "program_execution_capture_effects_invalid",
                            ErrorCode::kInvalidRequest);
      }
    }
  }
  std::size_t membership_count = 0U;
  std::set<std::uint32_t> synchronized_nodes;
  for (std::size_t index = 0U; index < capture.table->named_selections.size(); ++index) {
    const auto& selection = capture.table->named_selections[index];
    if (!valid_program_execution_group(selection.group_id) || selection.group_id == "all" ||
        (index > 0U &&
         capture.table->named_selections[index - 1U].group_id >= selection.group_id) ||
        selection.node_indices.empty() ||
        selection.node_indices.size() > kMaxProgramExecutionMemberships - membership_count ||
        !std::is_sorted(selection.node_indices.begin(), selection.node_indices.end()) ||
        std::adjacent_find(selection.node_indices.begin(), selection.node_indices.end()) !=
            selection.node_indices.end() ||
        selection.node_indices.back() >= capture.table->nodes.size()) {
      return reject<void>("Program execution selections are not canonical and bounded",
                          "program_execution_capture_selections_invalid",
                          ErrorCode::kInvalidRequest);
    }
    membership_count += selection.node_indices.size();
    if (selection.execution_policy != ProgramExecutionPolicy::kIndependent) {
      if (selection.node_indices.size() < 2U) {
        return reject<void>("coordinated Program selection is too small",
                            "program_execution_capture_synchronization_invalid",
                            ErrorCode::kInvalidRequest);
      }
      for (const auto node_index : selection.node_indices) {
        const auto& node = capture.table->nodes[node_index];
        if (!synchronized_nodes.insert(node_index).second || node.owner_api != "kernel.operation" ||
            !std::any_of(
                node.target_effects.begin(), node.target_effects.end(), [](const auto& effect) {
                  return effect.effectful &&
                         effect.capability_id == CapabilityId("execution.trajectory.execute");
                })) {
          return reject<void>("synchronized selections overlap or contain non-motion",
                              "program_execution_capture_synchronization_invalid",
                              ErrorCode::kInvalidRequest);
        }
      }
    }
  }

  const auto coordinates_current = [&] {
    return capture.request_owner_epoch == config_.request_owner_epoch &&
           capture.graph_revision == stop_graph_revision_ &&
           capture.retained_bundle_id == stop_retained_bundle_id_ &&
           program_cycle_claim_run_.has_value() &&
           program_cycle_claim_run_->claim_generation == capture.claim_generation &&
           program_cycle_claim_run_->program_run_id == capture.program_run_id &&
           program_cycle_claim_run_->program_runner_owner_epoch ==
               capture.program_runner_owner_epoch &&
           program_cycle_claim_run_->program_revision == capture.program_revision;
  };
  std::optional<ProgramRunExecutionCaptureDTO> previous;
  std::vector<ProgramNodeHoldStateDTO> node_states;
  node_states.reserve(capture.table->nodes.size());
  for (const auto& node : capture.table->nodes) {
    const bool effectful = std::any_of(
        node.target_effects.begin(), node.target_effects.end(),
        [](const auto& effect) { return effect.effectful; });
    node_states.push_back(effectful ? ProgramNodeHoldStateDTO::kReady
                                    : ProgramNodeHoldStateDTO::kRunning);
  }
  std::vector<std::shared_ptr<const ProgramCycleClaimRun::MotionContinuation>> continuations(
      capture.table->nodes.size());
  {
    std::lock_guard state_lock(state_mutex_);
    if (!coordinates_current()) {
      return reject<void>(
          "Program execution capture does not match the current owner/claim/product",
          "program_execution_capture_stale", ErrorCode::kInvalidStateTransition);
    }
    auto& current = program_cycle_claim_run_->capture;
    if (!current.has_value()) {
      if (pending_program_run_admission_ == capture.program_run_id ||
          std::any_of(active_operations_.begin(), active_operations_.end(), [&](const auto& item) {
            return item.second->state.program_run_id == capture.program_run_id;
          })) {
        return reject<void>("Program execution capture must precede all effects",
                            "program_execution_capture_late", ErrorCode::kInvalidStateTransition);
      }
      // A Stop overtaking registration still fences every later effect.
      auto fence = std::find_if(
          program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
          [&](const auto& row) { return row.program_run_id == capture.program_run_id; });
      if (fence == program_run_stop_fences_.end()) {
        if (program_run_stop_fences_.size() >= config_.request_replay_capacity) {
          return reject<void>("Program execution scope capacity is exhausted",
                              "program_execution_scope_capacity_exhausted",
                              ErrorCode::kInvalidStateTransition);
        }
        program_run_stop_fences_.push_back(
            ProgramRunStopFence{capture.program_run_id, std::nullopt, StopGeneration(0U)});
        fence = std::prev(program_run_stop_fences_.end());
      }
      // A Stop that overtook registration keeps the initial Ready/Running
      // states: nothing has started, and its stop identity already fences
      // every admission until it is released.
      fence->node_states = std::move(node_states);
      program_cycle_claim_run_->motion_continuations = std::move(continuations);
      current = std::move(capture);
      return Result<void>::Ok();
    }
    previous = *current;
  }
  // Large immutable tables are compared and released outside Stop ordering.
  if (previous->program_id != capture.program_id ||
      previous->program_binding_id != capture.program_binding_id ||
      (previous->binding != capture.binding &&
       (!previous->binding || !capture.binding || *previous->binding != *capture.binding)) ||
      (previous->table != capture.table && *previous->table != *capture.table)) {
    return reject<void>("Program-run identity cannot be rebound to a different capture",
                        "program_execution_capture_rebound", ErrorCode::kInvalidStateTransition);
  }
  std::lock_guard state_lock(state_mutex_);
  if (!coordinates_current() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->capture->table != previous->table) {
    return reject<void>("Program execution capture does not match the current owner/claim/product",
                        "program_execution_capture_stale", ErrorCode::kInvalidStateTransition);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_program_node_claim(
    const ProgramNodeAdmissionEvidenceDTO& evidence) const {
  std::lock_guard state_lock(state_mutex_);
  if (motion_admission_closed_ || motion_admission_unreconciled_ ||
      !program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->program_run_id != evidence.program_run_id ||
      program_cycle_claim_run_->program_runner_owner_epoch != evidence.program_runner_owner_epoch ||
      program_cycle_claim_run_->claim_generation !=
          evidence.parent_program_cycle_claim_generation) {
    return reject<void>("node transaction has no current Program capture",
                        "program_node_parent_not_current", ErrorCode::kInvalidStateTransition);
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  const auto node = std::lower_bound(
      nodes.begin(), nodes.end(), evidence.node_instance_id,
      [](const auto& row, const auto& id) { return row.instance_id.value() < id; });
  if (node == nodes.end() || node->instance_id.value() != evidence.node_instance_id ||
      node->owner_api != evidence.owner_api) {
    return reject<void>("node transaction does not match its sealed owner API",
                        "program_execution_scope_unknown", ErrorCode::kInvalidRequest);
  }
  const auto fence =
      std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                   [&](const auto& row) { return row.program_run_id == evidence.program_run_id; });
  const auto index = static_cast<std::size_t>(node - nodes.begin());
  if (fence == program_run_stop_fences_.end() || index >= fence->node_states.size()) {
    return reject<void>("node transaction has no current execution scope",
                        "program_execution_scope_unknown", ErrorCode::kInvalidStateTransition);
  }
  if (fence->stop_id.has_value() ||
      fence->node_states[index] != ProgramNodeHoldStateDTO::kRunning) {
    return Result<void>::Err({"invalid_state_transition",
                              "node transaction is held by an accepted Program Stop",
                              {{"reason", "program_node_stop_fence_active"},
                               {"selection_revision", std::to_string(fence->selection_revision)}}});
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::validate_program_execution_scope_locked(
    const OperationIntentDTO& intent) const {
  if (std::any_of(
          program_run_stop_fences_.begin(), program_run_stop_fences_.end(), [&](const auto& fence) {
            return fence.program_run_id == intent.program_run_id && fence.stop_id.has_value();
          })) {
    return reject<void>("the Program run is closed by an accepted Stop",
                        "program_run_stop_fence_active", ErrorCode::kInvalidStateTransition);
  }
  const auto* claim = program_cycle_claim_run_.has_value() &&
                              program_cycle_claim_run_->program_run_id == intent.program_run_id
                          ? &*program_cycle_claim_run_
                          : nullptr;
  const bool scoped = std::any_of(intent.participants.begin(), intent.participants.end(),
                                  [](const auto& row) { return row.execution_scope.has_value(); });
  if (!scoped && claim == nullptr) {
    // Intrinsic direct-control admission is still independently verified by
    // the product-backed resolver. A diagnostic label is not an exemption.
    return Result<void>::Ok();
  }
  if (claim == nullptr || !claim->capture.has_value()) {
    return reject<void>("Program execution has no exact registered capture",
                        "program_execution_capture_missing", ErrorCode::kInvalidStateTransition);
  }
  const auto& capture = *claim->capture;
  if (capture.program_id != intent.program_id ||
      capture.program_revision != intent.program_revision ||
      capture.program_binding_id != intent.program_binding_id ||
      capture.graph_revision != intent.expected_graph_revision ||
      capture.retained_bundle_id != intent.expected_retained_bundle_id) {
    return reject<void>("operation differs from its captured Program/Binding",
                        "program_execution_capture_mismatch", ErrorCode::kInvalidRequest);
  }
  std::vector<std::uint32_t> scoped_node_indices;
  scoped_node_indices.reserve(intent.participants.size());
  for (const auto& participant : intent.participants) {
    if (!participant.execution_scope.has_value()) {
      return reject<void>("captured Program effects require their original authored scope",
                          "program_execution_scope_missing", ErrorCode::kInvalidRequest);
    }
    const auto& scope = *participant.execution_scope;
    const auto node = std::lower_bound(
        capture.table->nodes.begin(), capture.table->nodes.end(), scope.node_instance_id,
        [](const auto& row, const auto& id) { return row.instance_id < id; });
    if (node == capture.table->nodes.end() || node->instance_id != scope.node_instance_id ||
        (node->owner_api != "kernel.operation" && node->owner_api != "kernel.command_target")) {
      return reject<void>("operation has no captured effectful node scope",
                          "program_execution_scope_unknown", ErrorCode::kInvalidRequest);
    }
    const auto index = static_cast<std::size_t>(node - capture.table->nodes.begin());
    scoped_node_indices.push_back(static_cast<std::uint32_t>(index));
    const auto fence =
        std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                     [&](const auto& row) { return row.program_run_id == intent.program_run_id; });
    if (fence == program_run_stop_fences_.end() || index >= fence->node_states.size() ||
        fence->node_states[index] != ProgramNodeHoldStateDTO::kRunning) {
      return reject<void>("the authored node is held by an accepted Program Stop",
                          "program_node_stop_fence_active", ErrorCode::kInvalidStateTransition);
    }
    const auto key = std::tie(participant.target_role_id, participant.required_capability_id);
    const auto effect =
        std::lower_bound(node->target_effects.begin(), node->target_effects.end(), key,
                         [](const auto& row, const auto& value) {
                           return std::tie(row.target_role_id, row.capability_id) < value;
                         });
    if (effect == node->target_effects.end() ||
        std::tie(effect->target_role_id, effect->capability_id) != key || !effect->effectful) {
      return reject<void>("operation target/capability is not a captured effectful association",
                          "program_execution_scope_effect_mismatch", ErrorCode::kInvalidRequest);
    }
  }
  std::sort(scoped_node_indices.begin(), scoped_node_indices.end());
  if (std::adjacent_find(scoped_node_indices.begin(), scoped_node_indices.end()) !=
      scoped_node_indices.end()) {
    return reject<void>("operation repeats an authored node scope",
                        "program_execution_scope_duplicate", ErrorCode::kInvalidRequest);
  }
  const auto synchronized =
      std::find_if(capture.table->named_selections.begin(), capture.table->named_selections.end(),
                   [&](const auto& selection) {
                     return selection.execution_policy == ProgramExecutionPolicy::kCommonStart &&
                            selection.node_indices == scoped_node_indices;
                   });
  if (intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll) {
    if (synchronized == capture.table->named_selections.end()) {
      return reject<void>("coordinated operation is not an exact authored synchronized group",
                          "program_execution_synchronization_mismatch", ErrorCode::kInvalidRequest);
    }
  } else if (std::any_of(capture.table->named_selections.begin(),
                         capture.table->named_selections.end(), [&](const auto& selection) {
                           return selection.execution_policy !=
                                      ProgramExecutionPolicy::kIndependent &&
                                  std::any_of(scoped_node_indices.begin(),
                                              scoped_node_indices.end(), [&](const auto index) {
                                                return std::binary_search(
                                                    selection.node_indices.begin(),
                                                    selection.node_indices.end(), index);
                                              });
                         })) {
    return reject<void>("coordinated Program motion cannot be dispatched independently",
                        "program_execution_synchronization_required", ErrorCode::kInvalidRequest);
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::release_program_run_stop_fence(
    const ProgramRunId& program_run_id, const StopId& stop_id,
    const StopGeneration stop_generation) {
  if (program_run_id.empty() || stop_id.empty() || !stop_generation.nonzero()) {
    return reject<void>("Program-run Stop fence identity is invalid",
                        "program_run_stop_fence_identity_invalid", ErrorCode::kInvalidRequest);
  }
  std::unique_lock admission_lock(*admission_mutex_, std::try_to_lock);
  if (!admission_lock.owns_lock()) {
    return reject<void>("ordinary admission is still in flight", "program_run_admission_in_flight",
                        ErrorCode::kInvalidStateTransition);
  }
  const auto parent = cell_admission_owner_->held_claim(CellAdmissionClaimKindDTO::kProgramCycle);
  if (parent.has_value() && parent->intent.owner_transaction_id == program_run_id.value() &&
      !cell_admission_owner_->unsettled_program_child_nodes(parent->claim_generation).empty()) {
    return reject<void>("Program transactions are still unresolved", "program_children_unsettled",
                        ErrorCode::kInvalidStateTransition);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto fence = std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                                  [&](const ProgramRunStopFence& candidate) {
                                    return candidate.program_run_id == program_run_id;
                                  });
  if (fence == program_run_stop_fences_.end()) {
    return Result<void>::Ok();
  }
  if (fence->stop_id != stop_id || fence->stop_generation != stop_generation) {
    return reject<void>("Program-run Stop fence identity changed", "program_run_stop_fence_stale",
                        ErrorCode::kInvalidStateTransition);
  }
  if (fence->selection_revision >= kMaximumKernelJsonInteger ||
      fence->selection_revision != fence->legacy_selection_revision ||
      !fence->pending_effects.empty()) {
    return reject<void>("a newer scoped selection superseded the whole-run Stop",
                        "program_run_stop_fence_stale", ErrorCode::kInvalidStateTransition);
  }
  if (pending_program_run_admission_ == std::optional<ProgramRunId>{program_run_id}) {
    return reject<void>("a pre-Stop Program-run admission is still draining",
                        "program_run_admission_in_flight", ErrorCode::kInvalidStateTransition);
  }
  if (!fence->releasable) {
    return reject<void>("Program-run Stop has not settled successfully",
                        "program_run_stop_fence_not_releasable",
                        ErrorCode::kInvalidStateTransition);
  }
  if (fence->node_states.empty()) {
    program_run_stop_fences_.erase(fence);
  } else {
    // Releasing the whole-run Stop resumes execution only. Motion holds are
    // kept: Ready work still needs Run and contained work still needs Resume.
    fence->stop_id.reset();
    ++fence->selection_revision;
  }
  return Result<void>::Ok();
}

Result<std::vector<std::uint32_t>>
ResourceAddressedExecutionKernel::resolve_program_selection_locked(
    const ProgramExecutionSelectionRequestDTO& request) const {
  using Indices = std::vector<std::uint32_t>;
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch ||
      !program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      !program_cycle_claim_run_->capture->table ||
      request.program_run_id != program_cycle_claim_run_->program_run_id ||
      request.program_runner_owner_epoch != program_cycle_claim_run_->program_runner_owner_epoch ||
      request.claim_generation != program_cycle_claim_run_->claim_generation) {
    return reject<Indices>("Program selection does not name the current captured owner/claim",
                           "program_selection_stale", ErrorCode::kInvalidStateTransition);
  }
  const auto& table = *program_cycle_claim_run_->capture->table;
  if (request.group_ids.empty() || request.group_ids.size() > kMaxProgramExecutionGroups) {
    return reject<Indices>("Program selection must name a bounded nonempty group list",
                           "program_selection_invalid", ErrorCode::kInvalidRequest);
  }
  std::vector<bool> selected(table.nodes.size(), false);
  for (const auto& group : request.group_ids) {
    if (group == "all") {
      std::fill(selected.begin(), selected.end(), true);
      continue;
    }
    const auto found =
        std::lower_bound(table.named_selections.begin(), table.named_selections.end(), group,
                         [](const auto& row, const auto& id) { return row.group_id < id; });
    if (found == table.named_selections.end() || found->group_id != group) {
      return reject<Indices>("Program selection is not present in the captured Program",
                             "program_selection_unknown", ErrorCode::kInvalidRequest);
    }
    for (const auto index : found->node_indices) selected[index] = true;
  }
  Indices indices;
  for (std::size_t index = 0; index < selected.size(); ++index)
    if (selected[index]) indices.push_back(static_cast<std::uint32_t>(index));
  return Result<Indices>::Ok(std::move(indices));
}

bool ResourceAddressedExecutionKernel::program_node_effectful_locked(
    const ProgramRunId& program_run_id, const std::size_t index) const {
  if (!program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->program_run_id != program_run_id ||
      !program_cycle_claim_run_->capture->table ||
      index >= program_cycle_claim_run_->capture->table->nodes.size())
    return true;
  const auto& effects = program_cycle_claim_run_->capture->table->nodes[index].target_effects;
  return std::any_of(effects.begin(), effects.end(),
                     [](const auto& effect) { return effect.effectful; });
}

ProgramExecutionScopeStateDTO ResourceAddressedExecutionKernel::program_execution_scope_locked(
    const ProgramRunStopFence& fence) const {
  const auto& claim = *program_cycle_claim_run_;
  ProgramExecutionScopeStateDTO result{
      config_.request_owner_epoch, claim.program_runner_owner_epoch, claim.claim_generation,
      claim.program_run_id,        fence.selection_revision,         {},
      fence.pending_effects};
  for (std::size_t index = 0U; index < fence.node_states.size(); ++index) {
    if (fence.node_states[index] != ProgramNodeHoldStateDTO::kRunning) {
      result.held_nodes.push_back({static_cast<std::uint32_t>(index), fence.node_states[index]});
    }
  }
  return result;
}

Result<ProgramExecutionScopeStateDTO> ResourceAddressedExecutionKernel::get_program_execution_scope(
    const ProgramRunId& program_run_id) const {
  std::lock_guard state_lock(state_mutex_);
  const auto fence =
      std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                   [&](const auto& row) { return row.program_run_id == program_run_id; });
  if (!program_cycle_claim_run_.has_value() || !program_cycle_claim_run_->capture.has_value() ||
      program_cycle_claim_run_->program_run_id != program_run_id ||
      fence == program_run_stop_fences_.end()) {
    return reject<ProgramExecutionScopeStateDTO>("Program execution scope is not current",
                                                 "program_execution_scope_unavailable",
                                                 ErrorCode::kInvalidStateTransition);
  }
  return Result<ProgramExecutionScopeStateDTO>::Ok(program_execution_scope_locked(*fence));
}

Result<ProgramExecutionSelectionAcceptedDTO>
ResourceAddressedExecutionKernel::stop_program_selection(
    const ProgramExecutionSelectionRequestDTO& request) {
  const auto accepted_at_ns = clock_.now();
  StopWorkHandoff handoff;
  const ScopeExit publish([&] { publish_stop_work(handoff); });
  auto result = [&]() -> Result<ProgramExecutionSelectionAcceptedDTO> {
    std::lock_guard state_lock(state_mutex_);
    auto selection = resolve_program_selection_locked(request);
    if (!selection.ok()) {
      const MalformedStopIngressDTO malformed{
          request.request_context.request_id, config_.request_owner_epoch, request.attribution,
          "/api/execution/stops", "recognized_stop_payload_invalid"};
      auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                        accepted_at_ns, nullptr, &malformed);
      if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
      return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
          {std::nullopt, {}, {}, std::move(stopped.value()), selection.error()});
    }
    auto fence =
        std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                     [&](const auto& row) { return row.program_run_id == request.program_run_id; });
    // A resolved captured selection always has its fence installed. If that
    // invariant is lost, recognized Stop must still contain through the
    // existing conservative path rather than return before taking effect.
    if (fence == program_run_stop_fences_.end()) {
      const MalformedStopIngressDTO malformed{
          request.request_context.request_id, config_.request_owner_epoch, request.attribution,
          "/api/execution/stops", "recognized_stop_payload_invalid"};
      auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                        accepted_at_ns, nullptr, &malformed);
      if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
      return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
          {std::nullopt,
           {},
           {},
           std::move(stopped.value()),
           error("program_selection_owner_missing", "Program selection owner is missing",
                 "program_selection_owner_missing")});
    }
    const auto& table = *program_cycle_claim_run_->capture->table;
    const auto replay = std::find_if(
        stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
        [&](const auto& row) { return row.request_id() == request.request_context.request_id; });
    if (replay != stop_replay_ledger_.end() ||
        ordinary_request_id_retained_locked(request.request_context.request_id) ||
        emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      const auto* prior = replay == stop_replay_ledger_.end()
                              ? nullptr
                              : std::get_if<ProgramExecutionSelectionRequestDTO>(&replay->intent);
      if (prior != nullptr && *prior == request) {
        const auto& receipt = std::get<StopReplayEntry::SelectionReceipt>(replay->accepted);
        return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
            {program_execution_scope_locked(*fence), selection.value(),
             receipt.widened_node_indices, receipt.fresh_containment, std::nullopt,
             RequestReplayStatusDTO::kReplayedExact});
      }
      const MalformedStopIngressDTO malformed{
          request.request_context.request_id, config_.request_owner_epoch, request.attribution,
          "/api/execution/stops", "recognized_stop_payload_invalid"};
      auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                        accepted_at_ns, nullptr, &malformed);
      if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
      return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
          {program_execution_scope_locked(*fence),
           selection.value(),
           {},
           std::move(stopped.value()),
           error("request_id_conflict",
                 "Program Stop identity was reused with a different selection",
                 "request_id_conflict")});
    }
    // Exact retained retries above are read-only even at the counter limit.
    // New Stop cannot be refused for exhausted selection bookkeeping: use
    // existing cell-wide containment, whose admission closure also fences any
    // late Execute. Resume already refuses this exhausted revision.
    if (fence->selection_revision >= kMaximumKernelJsonInteger) {
      const MalformedStopIngressDTO malformed{
          request.request_context.request_id, config_.request_owner_epoch, request.attribution,
          "/api/execution/stops", "recognized_stop_payload_invalid"};
      auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{std::monostate{}}, handoff,
                                        accepted_at_ns, nullptr, &malformed);
      if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
      return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(
          {program_execution_scope_locked(*fence),
           selection.value(),
           {},
           std::move(stopped.value()),
           error("program_selection_revision_exhausted", "Program selection revision is exhausted",
                 "program_selection_revision_exhausted")});
    }
    std::vector<bool> selected(table.nodes.size(), false);
    for (const auto index : selection.value()) selected[index] = true;
    const auto node_index = [&](const ProgramNodeExecutionScopeDTO& scope) {
      const auto node =
          std::lower_bound(table.nodes.begin(), table.nodes.end(), scope.node_instance_id,
                           [](const auto& row, const auto& id) { return row.instance_id < id; });
      return static_cast<std::size_t>(node - table.nodes.begin());
    };
    const auto intersects = [&](const auto& operation) {
      if (operation.manual_handover &&
          operation.manual_handover->capture.program_run_id == request.program_run_id) {
        if (std::find(request.group_ids.begin(), request.group_ids.end(), "all") !=
            request.group_ids.end())
          return true;
        return std::any_of(
            operation.manual_handover->producer_node_indices.begin(),
            operation.manual_handover->producer_node_indices.end(),
            [&](const auto index) { return index < selected.size() && selected[index]; });
      }
      return std::any_of(operation.intent.participants.begin(),
                         operation.intent.participants.end(), [&](const auto& row) {
                           return row.execution_scope.has_value() &&
                                  node_index(*row.execution_scope) < selected.size() &&
                                  selected[node_index(*row.execution_scope)];
                         });
    };
    // Coordinated work is indivisible. A node can own several effects, so
    // retain the closure, not just the first operation encountered.
    bool widened = true;
    while (widened) {
      widened = false;
      for (const auto& [id, operation] : active_operations_) {
        if (operation->state.program_run_id != request.program_run_id ||
            operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll)
          continue;
        const auto& participants = operation->intent.participants;
        if (!intersects(*operation)) continue;
        for (const auto& row : participants) {
          if (!row.execution_scope.has_value()) continue;
          const auto index = node_index(*row.execution_scope);
          if (index < selected.size() && !selected[index]) {
            selected[index] = true;
            widened = true;
          }
        }
      }
    }
    ProgramExecutionSelectionAcceptedDTO accepted{
        std::nullopt, selection.value(), {}, std::nullopt};
    for (std::size_t index = 0U; index < selected.size(); ++index) {
      if (!selected[index]) continue;
      if (!std::binary_search(selection.value().begin(), selection.value().end(), index))
        accepted.widened_node_indices.push_back(static_cast<std::uint32_t>(index));
      if (fence->node_states[index] == ProgramNodeHoldStateDTO::kRunning)
        fence->node_states[index] = ProgramNodeHoldStateDTO::kHeld;
    }
    ++fence->selection_revision;
    // The held state is now authoritative, under the same lock used by final
    // Execute. No late admission can cross the following physical capture.
    std::vector<StopOperationTargetDTO> fresh;
    for (const auto& [id, operation] : active_operations_) {
      if (operation->state.program_run_id != request.program_run_id &&
          (!operation->manual_handover ||
           operation->manual_handover->capture.program_run_id != request.program_run_id))
        continue;
      if (!intersects(*operation) ||
          (terminal(operation->state.state) && !operation->containment_effect_uncertain()))
        continue;
      const bool already_contained = std::any_of(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& transaction) {
            if (transaction == nullptr || !transaction->active || transaction->terminal ||
                transaction->stop_id != operation->state.active_stop_id ||
                transaction->stop_generation != operation->state.active_stop_generation ||
                !std::binary_search(transaction->operations.begin(), transaction->operations.end(),
                                    id))
              return false;
            return std::all_of(operation->participants.begin(), operation->participants.end(),
                               [&](const auto& participant) {
                                 return transaction->participant_ids.empty() ||
                                        std::binary_search(transaction->participant_ids.begin(),
                                                           transaction->participant_ids.end(),
                                                           participant.slot_owner.participant_id) ||
                                        (operation->intent.coordination_policy ==
                                             OperationCoordinationPolicy::kCoordinatedAll &&
                                         !participant.prepared_dispatch_released);
                               });
          });
      if (!already_contained)
        fresh.push_back({id, operation->state.operation_revision,
                         operation->state.active_stop_generation.value_or(StopGeneration(0U))});
    }
    if (!fresh.empty()) {
      auto stopped = accept_stop_locked(EmergencyStopProjectedIntentDTO{StopIntentDTO{
                                            request.request_context, std::move(fresh),
                                            "Program node selection", request.attribution}},
                                        handoff, accepted_at_ns);
      if (!stopped.ok()) return Result<ProgramExecutionSelectionAcceptedDTO>::Err(stopped.error());
      accepted.fresh_containment = std::move(stopped.value());
      if (accepted.fresh_containment->resolution_disposition ==
          StopResolutionDispositionDTO::kPromotedToFull) {
        for (std::size_t index = 0U; index < selected.size(); ++index) {
          if (!selected[index])
            accepted.widened_node_indices.push_back(static_cast<std::uint32_t>(index));
          selected[index] = true;
          if (fence->node_states[index] == ProgramNodeHoldStateDTO::kRunning)
            fence->node_states[index] = ProgramNodeHoldStateDTO::kHeld;
        }
        std::sort(accepted.widened_node_indices.begin(), accepted.widened_node_indices.end());
      }
    }
    // Adopt exact current Stop ownership; never resubmit an already covered
    // operation and never use history absence as evidence of rest.
    for (const auto& [id, operation] : active_operations_) {
      if (operation->state.program_run_id != request.program_run_id ||
          !operation->state.active_stop_id.has_value() ||
          !operation->state.active_stop_generation.has_value())
        continue;
      const auto& stop_id = *operation->state.active_stop_id;
      const auto generation = *operation->state.active_stop_generation;
      const auto resident =
          std::find_if(stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& row) {
            return row != nullptr && row->active && row->stop_id == stop_id &&
                   row->stop_generation == generation;
          });
      const auto terminal_stop = operation->terminal_stop_result();
      for (const auto& participant : operation->state.participants) {
        if (!participant.execution_scope.has_value()) continue;
        {
          const auto* origin = &*participant.execution_scope;
          const auto index = node_index(*origin);
          if (index >= selected.size() || !selected[index]) continue;
          if (terminal_stop.has_value()) {
            const auto disposition = *terminal_stop;
            if (disposition != StopCertificateResultDTO::kSucceededNominal &&
                disposition != StopCertificateResultDTO::kSucceededEscalated)
              fence->node_states[index] = ProgramNodeHoldStateDTO::kFaulted;
            continue;
          }
          const auto live_participant = std::find_if(
              operation->participants.begin(), operation->participants.end(), [&](const auto& row) {
                return row.slot_owner.participant_id == participant.participant_id;
              });
          const bool never_released_peer = live_participant != operation->participants.end() &&
                                           operation->intent.coordination_policy ==
                                               OperationCoordinationPolicy::kCoordinatedAll &&
                                           !live_participant->prepared_dispatch_released;
          const bool covered =
              resident != stop_transactions_.end() && !(*resident)->terminal &&
              (std::binary_search((*resident)->operations.begin(), (*resident)->operations.end(),
                                  id)) &&
              ((*resident)->participant_ids.empty() ||
               std::binary_search((*resident)->participant_ids.begin(),
                                  (*resident)->participant_ids.end(), participant.participant_id) ||
               never_released_peer);
          if (!covered) {
            // Neither exact settlement nor an active containing Stop is known.
            fence->node_states[index] = ProgramNodeHoldStateDTO::kFaulted;
            continue;
          }
          const auto retained = std::find_if(
              fence->pending_effects.begin(), fence->pending_effects.end(), [&](const auto& row) {
                return row.node_index == index && row.operation_id == id &&
                       row.participant_id == participant.participant_id && row.stop_id == stop_id &&
                       row.stop_generation == generation;
              });
          if (retained == fence->pending_effects.end()) {
            fence->pending_effects.push_back({static_cast<std::uint32_t>(index), id,
                                              participant.participant_id, origin->node_attempt_id,
                                              stop_id, generation});
          }
          if (fence->node_states[index] != ProgramNodeHoldStateDTO::kFaulted)
            fence->node_states[index] = ProgramNodeHoldStateDTO::kStopping;
        }
      }
    }
    std::sort(fence->pending_effects.begin(), fence->pending_effects.end(),
              [](const auto& a, const auto& b) {
                return std::tie(a.node_index, a.operation_id, a.participant_id) <
                       std::tie(b.node_index, b.operation_id, b.participant_id);
              });
    // Reuse the same bounded Stop receipt ledger for physical and selection
    // requests; never retain an entire copied scope snapshot per request.
    StopReplayEntry receipt{request,
                            StopReplayEntry::SelectionReceipt{accepted.widened_node_indices,
                                                              accepted.fresh_containment}};
    const auto physical_receipt = std::find_if(
        stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
        [&](const auto& row) { return row.request_id() == request.request_context.request_id; });
    if (physical_receipt != stop_replay_ledger_.end())
      *physical_receipt = std::move(receipt);
    else {
      if (stop_replay_ledger_.size() >= config_.request_replay_capacity)
        stop_replay_ledger_.erase(stop_replay_ledger_.begin());
      stop_replay_ledger_.push_back(std::move(receipt));
    }
    accepted.state = program_execution_scope_locked(*fence);
    return Result<ProgramExecutionSelectionAcceptedDTO>::Ok(std::move(accepted));
  }();
  return result;
}

void ResourceAddressedExecutionKernel::reduce_program_selection_stop_locked(
    const StopCertificateDTO& certificate) {
  const bool succeeded = certificate.result == StopCertificateResultDTO::kSucceededNominal ||
                         certificate.result == StopCertificateResultDTO::kSucceededEscalated;
  const auto matches = [&](const auto& effect) {
    return effect.stop_id == certificate.stop_id &&
           effect.stop_generation == certificate.stop_generation;
  };
  for (auto& fence : program_run_stop_fences_) {
    for (auto first = fence.pending_effects.begin(); first != fence.pending_effects.end();) {
      auto last = std::find_if(first, fence.pending_effects.end(), [&](const auto& row) {
        return row.node_index != first->node_index;
      });
      if (std::any_of(first, last, matches)) {
        auto& state = fence.node_states[first->node_index];
        if (!succeeded)
          state = ProgramNodeHoldStateDTO::kFaulted;
        else if (state != ProgramNodeHoldStateDTO::kFaulted && std::all_of(first, last, matches))
          state = ProgramNodeHoldStateDTO::kHeld;
      }
      first = last;
    }
    std::erase_if(fence.pending_effects, matches);
  }
}

Result<ProgramExecutionScopeStateDTO> ResourceAddressedExecutionKernel::start_program_selection(
    const ProgramExecutionSelectionRequestDTO& request,
    const std::uint64_t expected_selection_revision) {
  // Run is the same release as Resume (see release_program_selection).
  return release_program_selection(request, expected_selection_revision);
}

Result<ProgramExecutionScopeStateDTO> ResourceAddressedExecutionKernel::release_program_selection(
    const ProgramExecutionSelectionRequestDTO& request,
    const std::uint64_t expected_selection_revision) {
  std::unique_lock admission_lock(*admission_mutex_, std::try_to_lock);
  if (!admission_lock.owns_lock()) {
    return reject<ProgramExecutionScopeStateDTO>("ordinary admission is still in flight",
                                                 "program_run_admission_in_flight",
                                                 ErrorCode::kInvalidStateTransition);
  }
  // Snapshot under ordinary ordering, before taking Stop's state lock. No
  // receipt/journal I/O or ledger lock is added to that critical section.
  const auto child_nodes =
      cell_admission_owner_->unsettled_program_child_nodes(request.claim_generation);
  std::lock_guard state_lock(state_mutex_);
  auto selection = resolve_program_selection_locked(request);
  if (!selection.ok()) return Result<ProgramExecutionScopeStateDTO>::Err(selection.error());
  auto fence =
      std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                   [&](const auto& row) { return row.program_run_id == request.program_run_id; });
  if (fence == program_run_stop_fences_.end() ||
      fence->selection_revision != expected_selection_revision ||
      fence->selection_revision >= kMaximumKernelJsonInteger) {
    return reject<ProgramExecutionScopeStateDTO>(
        "Program selection changed after Resume was prepared", "program_selection_revision_stale",
        ErrorCode::kInvalidStateTransition);
  }
  // A whole-run Stop (Program Halt) governs execution; only releasing it
  // (Execute) ends it. Resume releases motion holds and never ends a Halt.
  if (motion_admission_closed_ || motion_admission_unreconciled_ ||
      pending_program_run_admission_ == request.program_run_id || fence->stop_id.has_value()) {
    return reject<ProgramExecutionScopeStateDTO>("Program admission or containment has not settled",
                                                 "program_selection_not_releasable",
                                                 ErrorCode::kInvalidStateTransition);
  }
  // Resume releases only current holds. An overlapping selection can also
  // contain a branch already resumed and executing a fresh operation; that
  // work is neither a release target nor a reason to block the remaining held
  // branches. Resolve the effective subset under the same revision fence.
  std::erase_if(selection.value(), [&](const auto index) {
    return fence->node_states[index] == ProgramNodeHoldStateDTO::kRunning;
  });
  if (selection.value().empty()) {
    return reject<ProgramExecutionScopeStateDTO>("Selected scope is not held",
                                                 "program_selection_not_releasable",
                                                 ErrorCode::kInvalidStateTransition);
  }
  // Run and Resume are one release: never-started (Ready) and stopped (Held)
  // work are both released; Stopping or Faulted work is not.
  for (const auto index : selection.value()) {
    if (fence->node_states[index] != ProgramNodeHoldStateDTO::kHeld &&
        fence->node_states[index] != ProgramNodeHoldStateDTO::kReady) {
      return reject<ProgramExecutionScopeStateDTO>(
          "Run/Resume accepts only Ready work and settled ordinary-Stop holds",
          "program_selection_not_releasable", ErrorCode::kInvalidStateTransition);
    }
  }
  const auto& nodes = program_cycle_claim_run_->capture->table->nodes;
  for (const auto& child : child_nodes) {
    const auto node = std::lower_bound(
        nodes.begin(), nodes.end(), child,
        [](const auto& row, const auto& id) { return row.instance_id.value() < id; });
    if (child.empty() || node == nodes.end() || node->instance_id.value() != child ||
        std::binary_search(selection.value().begin(), selection.value().end(),
                           static_cast<std::uint32_t>(node - nodes.begin()))) {
      return reject<ProgramExecutionScopeStateDTO>(
          "selected Program transactions are still unresolved", "program_children_unsettled",
          ErrorCode::kInvalidStateTransition);
    }
  }
  const auto scope_selected = [&](const ProgramNodeExecutionScopeDTO& scope) {
    const auto node =
        std::lower_bound(nodes.begin(), nodes.end(), scope.node_instance_id,
                         [](const auto& row, const auto& id) { return row.instance_id < id; });
    return std::binary_search(selection.value().begin(), selection.value().end(),
                              node - nodes.begin());
  };
  for (const auto& [id, operation] : active_operations_) {
    if (operation->state.program_run_id != request.program_run_id) continue;
    for (const auto& participant : operation->intent.participants) {
      if (!participant.execution_scope.has_value()) continue;
      if (scope_selected(*participant.execution_scope)) {
        return reject<ProgramExecutionScopeStateDTO>(
            "Selected operation requires exact recovery/retirement before release",
            "program_selection_recovery_required", ErrorCode::kInvalidStateTransition);
      }
    }
  }
  // Ordinary Run/Resume: the released nodes' bound resources are the
  // candidate scope; each later operation is rechecked at its own admission.
  if (auto refusal =
          operator_alarm_attention_refusal_locked(program_selection_resources_locked(selection.value()))) {
    return Result<ProgramExecutionScopeStateDTO>::Err(std::move(*refusal));
  }
  // All selected eligibility was checked before any node is released. An
  // unrelated branch may keep executing; there is no per-group lock to clear.
  for (const auto index : selection.value())
    fence->node_states[index] = ProgramNodeHoldStateDTO::kRunning;
  ++fence->selection_revision;
  return Result<ProgramExecutionScopeStateDTO>::Ok(program_execution_scope_locked(*fence));
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::recognized_malformed_stop(
    const MalformedStopIngressDTO& ingress) {
  return accept_stop(EmergencyStopProjectedIntentDTO{std::monostate{}}, nullptr, &ingress);
}

void ResourceAddressedExecutionKernel::ScheduleStopSurveillance(
    StopCertificateId certificate_id, const std::int64_t deadline_ns) noexcept {
  if (!config_.stop_surveillance.has_value()) {
    return;
  }
  std::uint64_t generation = 0U;
  {
    std::lock_guard state_lock(state_mutex_);
    if (stop_surveillance_shutting_down_) {
      return;
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
        });
    if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
        (*found)->surveillance_state->projection.state !=
            StopPostTerminalSurveillanceStateDTO::kMonitoring ||
        (*found)->surveillance_callback_pending ||
        (*found)->surveillance_schedule_generation == std::numeric_limits<std::uint64_t>::max()) {
      return;
    }
    generation = ++(*found)->surveillance_schedule_generation;
    (*found)->surveillance_callback_pending = true;
    ++stop_surveillance_schedule_publications_;
  }

  std::optional<ScheduleHandle> handle;
  try {
    handle = scheduler_.schedule(
        deadline_ns,
        [this, certificate_id, generation] { RunStopSurveillance(certificate_id, generation); },
        SchedulerPriority::Monitoring);
  } catch (...) {
    // The publication boundary itself is infrastructure, not source truth.
  }

  bool cancel_late_handle = false;
  {
    std::lock_guard state_lock(state_mutex_);
    if (stop_surveillance_schedule_publications_ == 0U) {
      std::terminate();
    }
    --stop_surveillance_schedule_publications_;
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
        });
    if (!handle.has_value() || found == stop_terminal_history_.end() ||
        (*found)->surveillance_schedule_generation != generation ||
        !(*found)->surveillance_callback_pending || stop_surveillance_shutting_down_) {
      cancel_late_handle = handle.has_value();
      if (found != stop_terminal_history_.end() &&
          (*found)->surveillance_schedule_generation == generation) {
        (*found)->surveillance_callback_pending = false;
        (*found)->surveillance_schedule_handle.reset();
      }
      if (!handle.has_value()) {
        motion_admission_closed_ = true;
      }
    } else {
      (*found)->surveillance_schedule_handle = *handle;
    }
    stop_surveillance_schedule_cv_.notify_all();
  }
  if (cancel_late_handle) {
    try {
      scheduler_.cancel(*handle);
    } catch (...) {
      std::terminate();
    }
  }
}

void ResourceAddressedExecutionKernel::RunStopSurveillance(
    StopCertificateId certificate_id, const std::uint64_t schedule_generation) noexcept {
  try {
    std::int64_t next_deadline_ns = 0;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() ||
          (*found)->surveillance_schedule_generation != schedule_generation ||
          !(*found)->surveillance_callback_pending) {
        return;
      }
      (*found)->surveillance_callback_pending = false;
      (*found)->surveillance_schedule_handle.reset();
      if (stop_surveillance_shutting_down_ || (*found)->surveillance_state == nullptr ||
          (*found)->surveillance_state->projection.state !=
              StopPostTerminalSurveillanceStateDTO::kMonitoring) {
        return;
      }
    }

    const auto now = clock_.now();
    if (now < 0) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return;
    }
    std::size_t source_count = 0U;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr) {
        return;
      }
      source_count = (*found)->surveillance_state->source_plans.size();
    }
    for (std::size_t source_index = 0U; source_index < source_count; ++source_index) {
      std::optional<StopSurveillanceObservationQueryDTO> query;
      {
        std::lock_guard state_lock(state_mutex_);
        const auto found = std::find_if(
            stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
              return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
            });
        if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
            (*found)->surveillance_state->projection.state !=
                StopPostTerminalSurveillanceStateDTO::kMonitoring) {
          break;
        }
        auto made =
            MakeStopSurveillanceObservationQuery(*(*found)->surveillance_state, source_index, now);
        if (!made.ok()) {
          motion_admission_closed_ = true;
          continue;
        }
        query = std::move(made.value());
      }
      auto sample = config_.stop_surveillance->observer->Observe(*query);
      {
        std::lock_guard state_lock(state_mutex_);
        const auto found = std::find_if(
            stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
              return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
            });
        if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr ||
            (*found)->surveillance_state->projection.state !=
                StopPostTerminalSurveillanceStateDTO::kMonitoring) {
          break;
        }
        auto reduced =
            ReduceStopSurveillanceObservation(*(*found)->surveillance_state, *query, sample);
        if (!reduced.ok()) {
          motion_admission_closed_ = true;
          continue;
        }
        *(*found)->surveillance_state = std::move(reduced.value());
        (*found)->status.surveillance = (*found)->surveillance_state->projection;
        if ((*found)->fault_latch.has_value()) {
          (*found)->status.fault_latch = (*found)->fault_latch;
        }
      }
    }

    std::optional<StopControlWorkToken> checkpoint_token;
    bool reschedule = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto found = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
          });
      if (found == stop_terminal_history_.end() || (*found)->surveillance_state == nullptr) {
        return;
      }
      auto& record = **found;
      const auto& policy = record.surveillance_state->projection.policy;
      if (now >= record.next_surveillance_checkpoint_ns && record.fault_latch.has_value() &&
          !record.surveillance_projection_token.has_value()) {
        auto checkpoint = MakeStopSurveillanceCheckpoint(*record.surveillance_state);
        if (checkpoint.ok()) {
          checkpoint_token = allocate_stop_projection_job_locked();
          if (checkpoint_token.has_value()) {
            auto& projection = *stop_projection_jobs_[checkpoint_token->slot];
            projection.surveillance_checkpoint = std::move(checkpoint.value());
            projection.surveillance_fault_latch = *record.fault_latch;
            record.surveillance_projection_token = checkpoint_token;
            if (now <= std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
              record.next_surveillance_checkpoint_ns = now + policy.checkpoint_interval_ns;
            }
          } else {
            motion_admission_closed_ = true;
          }
        }
      }
      reschedule = record.surveillance_state->projection.state ==
                   StopPostTerminalSurveillanceStateDTO::kMonitoring;
      if (reschedule) {
        if (now > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns) {
          motion_admission_closed_ = true;
          reschedule = false;
        } else {
          const auto elapsed = std::max<std::int64_t>(0, now - record.next_surveillance_sample_ns);
          const auto skipped = elapsed / policy.sample_cadence_ns + 1;
          if (skipped >
              (std::numeric_limits<std::int64_t>::max() - record.next_surveillance_sample_ns) /
                  policy.sample_cadence_ns) {
            motion_admission_closed_ = true;
            reschedule = false;
          } else {
            record.next_surveillance_sample_ns += skipped * policy.sample_cadence_ns;
            next_deadline_ns = record.next_surveillance_sample_ns;
          }
        }
      }
    }
    if (checkpoint_token.has_value()) {
      stop_projection_worker_->Publish(*checkpoint_token);
    }
    if (reschedule) {
      ScheduleStopSurveillance(std::move(certificate_id), next_deadline_ns);
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

Result<StopStatusDTO> ResourceAddressedExecutionKernel::get_stop_status(
    const StopId& stop_id) const {
  if (stop_id.empty()) {
    return reject<StopStatusDTO>("Stop status identity is empty", "stop_id_invalid",
                                 ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto terminal = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->certificate.stop_id == stop_id;
      });
  if (terminal != stop_terminal_history_.end()) {
    return Result<StopStatusDTO>::Ok((*terminal)->status);
  }
  const auto active = std::find_if(
      stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& transaction) {
        return transaction != nullptr && transaction->active && transaction->stop_id == stop_id &&
               transaction->accepted.has_value();
      });
  if (active == stop_transactions_.end()) {
    return reject<StopStatusDTO>("Stop status is not resident", "stop_status_not_resident",
                                 ErrorCode::kMissingReference);
  }
  const auto& transaction = **active;
  std::vector<StopScopeEntryDTO> entries;
  std::vector<StopAttemptedStageResultDTO> attempts;
  entries.reserve(transaction.participants.size() + transaction.newly_captured_resources.size());
  for (const auto& participant : transaction.participants) {
    entries.push_back(participant_scope_entry(
        participant, *transaction.stop_id, transaction.stop_generation,
        transaction.accepted->accepted_at_ns, transaction.accepted->remaining_outer_duration_ns));
    attempts.insert(attempts.end(), participant.retained_results.begin(),
                    participant.retained_results.end());
  }
  for (const auto& resource : transaction.newly_captured_resources) {
    entries.push_back(StopScopeEntryDTO{
        StopScopeEntryKeyDTO{*transaction.stop_id, transaction.stop_generation, resource},
        resource,
        StopScopeEntryDispositionDTO::kNotStoppable,
        StopScopeDispatchStateDTO::kEffectUncertain,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        transaction.accepted->accepted_at_ns,
        transaction.accepted->remaining_outer_duration_ns,
        {}});
  }
  const auto projection = [&] {
    switch (transaction.acceptance_projection_state) {
      case EmergencyStopProjectionStateDTO::kPending:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                              std::nullopt};
      case EmergencyStopProjectionStateDTO::kRecorded:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                              std::nullopt, std::nullopt};
      case EmergencyStopProjectionStateDTO::kFailed:
        return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                              ErrorDTO{"operation_journal_stop_projection_failed",
                                                       "Stop acceptance forensic projection failed",
                                                       {}},
                                              1U};
    }
    return StopDurableProjectionStatusDTO{
        StopDurableProjectionStateDTO::kFailed,
        ErrorDTO{"stop_projection_state_invalid", "Stop projection state is invalid", {}}, 1U};
  }();
  StopStatusDTO status{*transaction.accepted,
                       transaction.fresh_scope_complete ? StopStatusStateDTO::kCertifying
                                                        : StopStatusStateDTO::kStopping,
                       std::move(entries),
                       {},
                       std::move(attempts),
                       {},
                       {},
                       {},
                       projection,
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                      std::nullopt, std::nullopt},
                       StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                                      std::nullopt, std::nullopt},
                       std::nullopt,
                       transaction.fault_latch,
                       std::nullopt};
  return Result<StopStatusDTO>::Ok(std::move(status));
}

CellStopActionProjectionDTO ResourceAddressedExecutionKernel::cell_stop_action_projection() const {
  std::lock_guard state_lock(state_mutex_);
  CellStopActionProjectionDTO projection{cell_stop_generation_, std::nullopt, std::nullopt};
  if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
    const auto& latch = *active_stop_fault_latch_;
    projection.fault = CellStopFaultSummaryDTO{
        latch.latch_id,        latch.latch_generation, latch.state,           latch.trigger,
        latch.recovery_status, latch.reason_code,      latch.stop_certificate};
  }
  if (stop_terminal_history_.empty() || stop_terminal_history_.back() == nullptr) {
    return projection;
  }
  const auto& certificate = stop_terminal_history_.back()->certificate;
  CellStopTerminalSummaryDTO terminal{
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation},
      certificate.result,
      certificate.result == StopCertificateResultDTO::kSucceededEscalated,
      false,
      {}};
  terminal.executed_stages.reserve(certificate.attempted_stage_results.size());
  for (const auto& attempted : certificate.attempted_stage_results) {
    if (attempted.authority != StopStageResultAuthorityDTO::kAuthoritative ||
        attempted.result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched) {
      continue;
    }
    const auto& request = attempted.result.request;
    const auto stage_index = request.stage_index;
    if (stage_index >= request.participant_stop_plan.stage_commands.size()) {
      continue;
    }
    const auto& stage = request.participant_stop_plan.stage_commands[stage_index].resolved_stage;
    const auto action_class = stage.command_qualification.stop_action_class;
    terminal.escalated = terminal.escalated || stage_index > 0U;
    terminal.hard_stop_floor_used =
        terminal.hard_stop_floor_used || action_class != StopActionClass::kControlled;
    terminal.executed_stages.push_back(CellStopStageSummaryDTO{
        request.operation_id, request.participant_id, stage.stop_escalation_stage_id, stage_index,
        action_class, attempted.result.outcome, attempted.result.actual_provider_contract_mode,
        attempted.result.recoverable_substitution_reason});
  }
  projection.latest_terminal = std::move(terminal);
  return projection;
}

Result<StopCertificateDTO> ResourceAddressedExecutionKernel::get_stop_certificate(
    const StopCertificateId& certificate_id) const {
  if (certificate_id.empty()) {
    return reject<StopCertificateDTO>("Stop certificate identity is empty",
                                      "stop_certificate_id_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->certificate.stop_certificate_id == certificate_id;
      });
  if (found == stop_terminal_history_.end()) {
    return reject<StopCertificateDTO>("Stop certificate is not resident",
                                      "stop_certificate_not_resident",
                                      ErrorCode::kMissingReference);
  }
  return Result<StopCertificateDTO>::Ok((*found)->certificate);
}

std::optional<StopFaultLatchDTO> ResourceAddressedExecutionKernel::get_stop_fault_latch() const {
  std::lock_guard state_lock(state_mutex_);
  return active_stop_fault_latch_;
}

StopFaultLatchOriginDTO ResourceAddressedExecutionKernel::get_stop_fault_latch_origin(
    const StopFaultLatchDTO& latch) const {
  std::lock_guard state_lock(state_mutex_);
  for (const auto& stop : attributed_containment_stops_) {
    if (stop.stop_id == latch.stop_id && stop.stop_generation == latch.stop_generation) {
      return stop.origin;
    }
  }
  for (const auto& record : stop_terminal_history_) {
    if (record != nullptr && record->certificate.stop_id == latch.stop_id &&
        record->certificate.stop_generation == latch.stop_generation) {
      return stop_fault_latch_certificate_origin(record->certificate);
    }
  }
  return StopFaultLatchOriginDTO::kUnattributed;
}

Result<std::optional<StopFaultRecoveryOfferDTO>>
ResourceAddressedExecutionKernel::get_stop_fault_recovery_offer() const {
  std::lock_guard state_lock(state_mutex_);
  if (!active_stop_fault_latch_.has_value() ||
      active_stop_fault_latch_->state == StopFaultLatchStateDTO::kCleared) {
    return Result<std::optional<StopFaultRecoveryOfferDTO>>::Ok(std::nullopt);
  }
  if (!active_stop_fault_latch_->stop_certificate.has_value()) {
    // Publish the real fault immediately, but no actionable recovery context
    // before terminal containment. The current response already carries its
    // waiting status; an empty expectation table cannot authorize recovery.
    return Result<std::optional<StopFaultRecoveryOfferDTO>>::Ok(
        StopFaultRecoveryOfferDTO{*active_stop_fault_latch_, {}, std::nullopt});
  }
  const auto terminal = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr &&
               record->certificate.stop_id == active_stop_fault_latch_->stop_id &&
               record->certificate.stop_generation == active_stop_fault_latch_->stop_generation &&
               record->status.fault_latch.has_value() &&
               *record->status.fault_latch == *active_stop_fault_latch_;
      });
  if (terminal == stop_terminal_history_.end()) {
    return reject<std::optional<StopFaultRecoveryOfferDTO>>(
        "active Stop fault latch has no exact resident terminal status",
        "stop_fault_recovery_offer_terminal_missing", ErrorCode::kInvariantViolation);
  }

  // One shared owner derives this table and the native facts table. A
  // cell-wide Stop that captured no participant configuration (for example
  // a recognized-malformed Stop promoted to Full while idle) is offered the
  // installed control domains' active configurations. When no such proof
  // exists the latch is still published, explicitly unavailable.
  std::vector<const RuntimeParticipantBinding*> installed_bindings;
  installed_bindings.reserve(registrations_.size());
  for (const auto& registration : registrations_) {
    installed_bindings.push_back(&registration.binding);
  }
  auto expectations =
      StopFaultRecoveryConfigurationExpectations((*terminal)->status, installed_bindings);
  if (!expectations.ok() &&
      expectations.error().code == kStopFaultRecoveryTerminalConfigurationConflict) {
    return reject<std::optional<StopFaultRecoveryOfferDTO>>(
        "Stop terminal status contains conflicting control-domain configuration truth",
        "stop_fault_recovery_offer_configuration_conflict", ErrorCode::kInvariantViolation);
  }
  StopFaultRecoveryOfferDTO offer{*active_stop_fault_latch_, {}, std::nullopt};
  if (expectations.ok()) {
    offer.configuration_expectations = std::move(expectations.value());
  } else {
    offer.recovery_unavailable_reason = expectations.error().code;
  }
  if (stop_fault_recovery_offer_field_error(offer).has_value()) {
    return reject<std::optional<StopFaultRecoveryOfferDTO>>(
        "Stop fault-recovery offer is not a complete canonical owner projection",
        "stop_fault_recovery_offer_invalid", ErrorCode::kInvariantViolation);
  }
  return Result<std::optional<StopFaultRecoveryOfferDTO>>::Ok(std::move(offer));
}

Result<void> ResourceAddressedExecutionKernel::publish_stop_fault_assessment_locked(
    const StopStatusDTO& expected, const StopFaultLatchDTO& assessed) {
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(),
      [&](const auto& record) { return record != nullptr && record->status == expected; });
  if (found == stop_terminal_history_.end() || active_stop_fault_latch_ != expected.fault_latch) {
    return Result<void>::Err(error("stop_fault_recovery_assessment_conflict",
                                   "Stop fault truth changed during assessment",
                                   "stop_fault_recovery_assessment_conflict"));
  }
  (*found)->fault_latch = assessed;
  (*found)->status.fault_latch = assessed;
  active_stop_fault_latch_ = assessed;
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::refresh_stop_fault_recovery_assessment() {
  std::lock_guard recovery_lock(stop_fault_recovery_mutex_);
  const auto admission = cell_admission_owner_->RefreshStopFaultAdmission();
  if (!admission.ok()) return admission;
  std::optional<StopStatusDTO> status;
  {
    std::lock_guard state_lock(state_mutex_);
    if (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
        !active_stop_fault_latch_->stop_certificate.has_value()) {
      return Result<void>::Ok();
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->fault_latch == active_stop_fault_latch_;
        });
    if (found == stop_terminal_history_.end()) {
      return Result<void>::Err(error("stop_fault_recovery_terminal_missing",
                                     "Stop terminal truth is not resident",
                                     "stop_fault_recovery_terminal_missing"));
    }
    status = (*found)->status;
  }
  if (!config_.stop_fault_recovery_facts_resolver) {
    return Result<void>::Err(error("stop_fault_recovery_facts_unavailable",
                                   "Stop recovery facts owner is not installed",
                                   "stop_fault_recovery_facts_unavailable"));
  }
  auto facts = config_.stop_fault_recovery_facts_resolver(*status, clock_.now());
  if (!facts.ok()) return Result<void>::Err(facts.error());
  auto assessed = AssessStopFaultRecovery(*status, facts.value());
  if (!assessed.ok()) return Result<void>::Err(assessed.error());
  std::lock_guard state_lock(state_mutex_);
  return publish_stop_fault_assessment_locked(*status, assessed.value());
}

Result<StopFaultRecoveryResultDTO> ResourceAddressedExecutionKernel::recover_stop_fault(
    const StopFaultRecoveryIntentDTO& intent) {
  if (stop_fault_recovery_intent_field_error(intent).has_value()) {
    return reject<StopFaultRecoveryResultDTO>("Stop fault-recovery intent is invalid",
                                              "stop_fault_recovery_intent_invalid",
                                              ErrorCode::kInvalidRequest);
  }
  if (intent.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<StopFaultRecoveryResultDTO>::Err(
        error("stale_request_owner_epoch", "Stop fault recovery names a stale owner epoch",
              "stale_request_owner_epoch"));
  }

  // Recovery is an ordinary, durable control path. Serialize it independently
  // from emergency Stop so two clear attempts cannot both evaluate one latch
  // and race their post-commit publication.
  std::lock_guard recovery_lock(stop_fault_recovery_mutex_);

  std::optional<StopStatusDTO> status;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        stop_fault_recovery_replay_ledger_.begin(), stop_fault_recovery_replay_ledger_.end(),
        [&](const auto& row) {
          return row.intent.request_context.request_id == intent.request_context.request_id;
        });
    if (replay != stop_fault_recovery_replay_ledger_.end()) {
      if (replay->intent != intent) {
        return Result<StopFaultRecoveryResultDTO>::Err(
            error("request_id_conflict", "Stop fault-recovery request ID conflicts",
                  "request_id_conflict"));
      }
      auto result = replay->result;
      result.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<StopFaultRecoveryResultDTO>::Ok(std::move(result));
    }
    const auto request_id = intent.request_context.request_id;
    const bool request_id_used_elsewhere =
        emergency_stop_reserve_.find(request_id.value()).has_value() ||
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& row) {
                      return row.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& row) {
                      return row.request.request_context.request_id == request_id;
                    });
    if (request_id_used_elsewhere) {
      return Result<StopFaultRecoveryResultDTO>::Err(
          error("request_id_conflict", "Stop fault-recovery request ID is already retained",
                "request_id_conflict"));
    }
    if (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
        active_stop_fault_latch_->latch_id != intent.expected_latch_id ||
        active_stop_fault_latch_->latch_generation != intent.expected_latch_generation ||
        !active_stop_fault_latch_->stop_certificate.has_value()) {
      return reject<StopFaultRecoveryResultDTO>("Stop fault latch is not current",
                                                "stop_fault_recovery_latch_stale",
                                                ErrorCode::kInvalidStateTransition);
    }
    const auto found = std::find_if(
        stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
          return record != nullptr && record->fault_latch.has_value() &&
                 record->fault_latch->latch_id == intent.expected_latch_id &&
                 record->fault_latch->latch_generation == intent.expected_latch_generation;
        });
    if (found == stop_terminal_history_.end()) {
      return reject<StopFaultRecoveryResultDTO>("Stop terminal truth is not resident",
                                                "stop_fault_recovery_terminal_missing",
                                                ErrorCode::kMissingReference);
    }
    status = (*found)->status;
  }
  // Preserve exact recovery replay even if admission-generation publication is
  // temporarily unavailable. New recovery work still requires the current
  // Stop restriction to be durably reflected before evaluation proceeds.
  const auto admission = cell_admission_owner_->RefreshStopFaultAdmission();
  if (!admission.ok()) return Result<StopFaultRecoveryResultDTO>::Err(admission.error());
  if (!config_.stop_fault_recovery_facts_resolver) {
    return reject<StopFaultRecoveryResultDTO>("Stop recovery facts owner is not installed",
                                              "stop_fault_recovery_facts_unavailable",
                                              ErrorCode::kMissingReference);
  }
  auto facts = config_.stop_fault_recovery_facts_resolver(*status, clock_.now());
  if (!facts.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(facts.error());
  }
  auto evaluated = EvaluateStopFaultRecovery(intent, *status, facts.value());
  if (!evaluated.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(evaluated.error());
  }
  if (!evaluated.value().result.accepted) {
    std::lock_guard state_lock(state_mutex_);
    // A current-facts assessment is owner truth even when the caller's
    // revision is stale. Publish the newest non-cleared latch so the next
    // exact request can name it; this performs no clear and no external
    // effect.
    auto published = publish_stop_fault_assessment_locked(*status, evaluated.value().result.latch);
    if (!published.ok()) return Result<StopFaultRecoveryResultDTO>::Err(published.error());
    stop_fault_recovery_replay_ledger_.clear();
    stop_fault_recovery_replay_ledger_.push_back(
        StopFaultRecoveryReplayEntry{intent, evaluated.value().result});
    return Result<StopFaultRecoveryResultDTO>::Ok(std::move(evaluated.value().result));
  }
  const auto persisted = journal_.RecordStopFaultCleared(
      config_.request_owner_epoch, intent, evaluated.value().result, evaluated.value().result.latch,
      *evaluated.value().resulting_stop_status);
  if (!persisted.ok()) {
    return Result<StopFaultRecoveryResultDTO>::Err(persisted.error());
  }
  std::unique_lock state_lock(state_mutex_);
  const auto found = std::find_if(
      stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
        return record != nullptr && record->fault_latch.has_value() &&
               record->fault_latch->latch_id == intent.expected_latch_id &&
               record->fault_latch->latch_generation == intent.expected_latch_generation;
      });
  if (found == stop_terminal_history_.end()) {
    motion_admission_closed_ = true;
    return reject<StopFaultRecoveryResultDTO>("Stop fault truth changed after durable clear",
                                              "stop_fault_recovery_postcommit_conflict",
                                              ErrorCode::kInvariantViolation);
  }
  (*found)->fault_latch = evaluated.value().result.latch;
  (*found)->status = *evaluated.value().resulting_stop_status;
  (*found)->surveillance_state.reset();
  if (active_stop_fault_latch_.has_value() &&
      active_stop_fault_latch_->latch_id == intent.expected_latch_id &&
      active_stop_fault_latch_->latch_generation == intent.expected_latch_generation) {
    active_stop_fault_latch_ = evaluated.value().result.latch;
  }
  refresh_stop_fault_latch_locked();
  stop_fault_recovery_replay_ledger_.clear();
  stop_fault_recovery_replay_ledger_.push_back(
      StopFaultRecoveryReplayEntry{intent, evaluated.value().result});
  // Orphan fences are resource-scoped and enforced by the slot table. A
  // successful durable fault clear removes the cell-wide Stop latch without
  // weakening any remaining orphan resource.
  motion_admission_closed_ =
      manager_liveness_motion_inhibited_ || is_active_stop_fault_latch(active_stop_fault_latch_);
  state_lock.unlock();
  // The clear result is already durable. A failed generation publication keeps
  // authoring closed and is retried by the existing reconciliation lane.
  static_cast<void>(cell_admission_owner_->RefreshStopFaultAdmission());
  return Result<StopFaultRecoveryResultDTO>::Ok(std::move(evaluated.value().result));
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::accept_stop(
    EmergencyStopProjectedIntentDTO projected_intent,
    const PartialDispatchStopScope* partial_dispatch_scope,
    const MalformedStopIngressDTO* malformed_ingress,
    const std::optional<StopFaultTriggerDTO> required_fault_latch) {
  const auto accepted_at_ns = clock_.now();
  StopWorkHandoff handoff;
  const ScopeExit publish([&] { publish_stop_work(handoff); });
  auto result = [&] {
    // Neither ordinary admission nor its durable journal is on this lane.
    std::lock_guard state_lock(state_mutex_);
    return accept_stop_locked(std::move(projected_intent), handoff, accepted_at_ns,
                              partial_dispatch_scope, malformed_ingress, required_fault_latch);
  }();
  return result;
}

void ResourceAddressedExecutionKernel::publish_stop_work(const StopWorkHandoff& handoff) noexcept {
  if (handoff.control.has_value()) {
    stop_control_executor_->Publish(*handoff.control);
  }
  if (handoff.projection.has_value()) {
    stop_projection_worker_->Publish(*handoff.projection);
  }
}

bool ResourceAddressedExecutionKernel::ordinary_request_id_retained_locked(
    const RequestId& request_id) const {
  return std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                     [&](const auto& row) {
                       return row.intent.request_context.request_id == request_id;
                     }) ||
         std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                     [&](const auto& row) {
                       return row.request.request_context.request_id == request_id;
                     }) ||
         std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                     [&](const auto& row) {
                       return row.request.request_context.request_id == request_id;
                     }) ||
         std::any_of(runtime_scale_control_replay_ledger_.begin(),
                     runtime_scale_control_replay_ledger_.end(), [&](const auto& row) {
                       return row.request.request_context.request_id == request_id;
                     });
}

Result<StopAcceptedDTO> ResourceAddressedExecutionKernel::accept_stop_locked(
    EmergencyStopProjectedIntentDTO projected_intent, StopWorkHandoff& handoff,
    const TimePoint accepted_at_ns, const PartialDispatchStopScope* partial_dispatch_scope,
    const MalformedStopIngressDTO* malformed_ingress,
    const std::optional<StopFaultTriggerDTO> required_fault_latch) {
  struct PostReserveInvariantGuard final {
    bool armed{};
    ~PostReserveInvariantGuard() {
      if (armed) {
        // Once the emergency reserve has accepted a recognized Stop, returning
        // a normal rejection or unwinding would lose its already-linearized
        // containment authority. Every remaining failure is an owner bug and
        // terminates fail-closed rather than pretending the Stop was rejected.
        std::terminate();
      }
    }
  } post_reserve_guard;
  const auto* ordinary = std::get_if<StopIntentDTO>(&projected_intent);
  const auto* full = std::get_if<FullStopIntentDTO>(&projected_intent);
  const auto* program_run_targets =
      ordinary == nullptr ? nullptr
                          : std::get_if<std::vector<StopProgramRunTargetDTO>>(&ordinary->selector);
  const auto normalized_alternatives = static_cast<unsigned>(ordinary != nullptr) +
                                       static_cast<unsigned>(full != nullptr) +
                                       static_cast<unsigned>(malformed_ingress != nullptr);
  if (normalized_alternatives != 1U ||
      (malformed_ingress != nullptr && !std::holds_alternative<std::monostate>(projected_intent))) {
    return reject<StopAcceptedDTO>("Stop ingress has no normalized request identity",
                                   "stop_ingress_not_normalized", ErrorCode::kInvalidRequest);
  }
  std::optional<FullStopIntentDTO> malformed_full;
  if (malformed_ingress != nullptr) {
    if (malformed_stop_ingress_field_error(*malformed_ingress).has_value()) {
      return reject<StopAcceptedDTO>("recognized malformed Stop ingress is invalid",
                                     "malformed_stop_ingress_invalid", ErrorCode::kInvalidRequest);
    }
    if (malformed_ingress->receiver_owner_epoch != config_.request_owner_epoch) {
      return reject<StopAcceptedDTO>("recognized malformed Stop ingress names a stale owner",
                                     "stale_request_owner_epoch", ErrorCode::kInvalidRequest);
    }
    malformed_full.emplace(PublicRequestContextDTO{malformed_ingress->request_id,
                                                   malformed_ingress->receiver_owner_epoch,
                                                   "recognized_stop_ingress", std::nullopt},
                           StopGeneration(0U), malformed_ingress->attribution);
    full = &*malformed_full;
  }
  const auto& context = ordinary != nullptr ? ordinary->request_context : full->request_context;
  const bool runtime_collision_containment =
      ordinary != nullptr &&
      ordinary->reason == std::optional<std::string>{"runtime_collision_fault"} &&
      ordinary->request_context.source_label == "kernel.runtime_collision";
  const auto& attribution = ordinary != nullptr ? ordinary->attribution : full->attribution;
  if (!attribution.locally_valid()) {
    return reject<StopAcceptedDTO>("Stop ingress attribution is invalid",
                                   "stop_request_attribution_invalid", ErrorCode::kInvalidRequest);
  }
  std::optional<ContentDigest> normalized_digest;
  if (malformed_ingress == nullptr) {
    const auto computed = ordinary != nullptr ? ComputeNormalizedStopIntentDigest(*ordinary)
                                              : ComputeNormalizedStopIntentDigest(*full);
    if (!computed.ok()) {
      return Result<StopAcceptedDTO>::Err(computed.error());
    }
    normalized_digest = computed.value();
  }
  const auto requested_selector_digest =
      malformed_ingress != nullptr
          ? ComputeRequestedStopSelectorDigest(*malformed_ingress)
          : (ordinary != nullptr ? ComputeRequestedStopSelectorDigest(*ordinary)
                                 : ComputeRequestedStopSelectorDigest(*full));
  if (!requested_selector_digest.ok()) {
    return Result<StopAcceptedDTO>::Err(requested_selector_digest.error());
  }

  std::int64_t outer_deadline_ns{};

  EmergencyStopAcceptanceOutcome outcome;
  std::optional<StopAcceptedDTO> accepted;
  StopControlWorkToken work_token;
  std::optional<StopControlWorkToken> projection_token;
  bool handoff_work = false;
  {
    // The caller retains state_mutex_ through selection and acceptance.
    if (!cell_stop_deadline_budget_.has_value()) {
      return reject<StopAcceptedDTO>("Stop deadline referent is unavailable",
                                     "stop_deadline_budget_unavailable",
                                     ErrorCode::kInvariantViolation);
    }
    if (ordinary != nullptr && malformed_ingress == nullptr) {
      const auto replay =
          std::find_if(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                       [&](const StopReplayEntry& entry) { return entry.matches(*ordinary); });
      if (replay != stop_replay_ledger_.end()) {
        auto replayed = std::get<StopAcceptedDTO>(replay->accepted);
        replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
        return Result<StopAcceptedDTO>::Ok(std::move(replayed));
      }
    }
    if (program_run_targets != nullptr) {
      std::size_t missing_fences = 0U;
      for (const auto& target : *program_run_targets) {
        const auto retained =
            std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                         [&](const ProgramRunStopFence& fence) {
                           return fence.program_run_id == target.program_run_id;
                         });
        missing_fences += retained == program_run_stop_fences_.end() ? 1U : 0U;
      }
      if (missing_fences >
          config_.request_replay_capacity -
              std::min(config_.request_replay_capacity, program_run_stop_fences_.size())) {
        return reject<StopAcceptedDTO>("Program-run Stop fence capacity is exhausted",
                                       "program_run_stop_fence_capacity_exhausted",
                                       ErrorCode::kInternalError);
      }
    }
    outer_deadline_ns =
        checked_deadline(accepted_at_ns, cell_stop_deadline_budget_->outer_duration_ns);
    if (outer_deadline_ns < 0) {
      return reject<StopAcceptedDTO>("Stop deadline cannot be represented monotonically",
                                     "stop_deadline_overflow", ErrorCode::kInternalError);
    }
    const auto proposed_number = next_stop_id_;
    if (proposed_number == 0U || proposed_number > kMaximumKernelJsonInteger ||
        cell_stop_generation_.value() >= kMaximumKernelJsonInteger) {
      return reject<StopAcceptedDTO>("Stop identity or generation space is exhausted",
                                     "stop_counter_exhausted", ErrorCode::kInternalError);
    }
    const auto proposed_stop_id = StopId("stop." + config_.request_owner_epoch.value() + "." +
                                         std::to_string(proposed_number));
    const auto proposed_generation = StopGeneration(cell_stop_generation_.value() + 1U);
    const auto slot_snapshot = resource_slots_->coherent_snapshot();
    auto resolver_snapshot = capture_stop_scope_locked(slot_snapshot);
    auto resolution = ordinary != nullptr ? resolve_stop_scope(*ordinary, resolver_snapshot)
                                          : resolve_full_stop_scope(*full, resolver_snapshot);
    if (!resolution.ok()) {
      return Result<StopAcceptedDTO>::Err(resolution.error());
    }
    if (malformed_ingress != nullptr) {
      resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
      resolution.value().reason = StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent;
      resolution.value().full_stop = true;
      resolution.value().stale_diagnostics.clear();
    }
    if (partial_dispatch_scope != nullptr) {
      if (ordinary == nullptr || partial_dispatch_scope->operation_id.empty() ||
          !partial_dispatch_scope->operation_revision.nonzero() ||
          partial_dispatch_scope->participant_ids.empty() ||
          !std::is_sorted(partial_dispatch_scope->participant_ids.begin(),
                          partial_dispatch_scope->participant_ids.end()) ||
          std::adjacent_find(partial_dispatch_scope->participant_ids.begin(),
                             partial_dispatch_scope->participant_ids.end()) !=
              partial_dispatch_scope->participant_ids.end()) {
        return reject<StopAcceptedDTO>("partial-dispatch containment scope is invalid",
                                       "partial_dispatch_scope_invalid",
                                       ErrorCode::kInvariantViolation);
      }
      auto* operation = find_active_operation_locked(partial_dispatch_scope->operation_id);
      if (operation == nullptr ||
          operation->state.operation_revision != partial_dispatch_scope->operation_revision ||
          operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll) {
        return reject<StopAcceptedDTO>("partial-dispatch operation authority changed",
                                       "partial_dispatch_operation_mismatch",
                                       ErrorCode::kInvariantViolation);
      }
      StopScopeResolutionDTO exact_partial{StopResolutionDispositionDTO::kExact,
                                           StopScopeResolutionReasonDTO::kExactCurrentScope,
                                           false,
                                           {partial_dispatch_scope->operation_id},
                                           {},
                                           {},
                                           {}};
      for (const auto& participant_id : partial_dispatch_scope->participant_ids) {
        const auto participant =
            std::find_if(operation->participants.begin(), operation->participants.end(),
                         [&](const auto& candidate) {
                           return candidate.slot_owner.participant_id == participant_id;
                         });
        if (participant == operation->participants.end() ||
            !participant->prepared_dispatch_released ||
            participant->slot_state != ResourceSlotState::kDispatched) {
          return reject<StopAcceptedDTO>(
              "partial-dispatch scope names work not released to a backend",
              "partial_dispatch_participant_not_released", ErrorCode::kInvariantViolation);
        }
        exact_partial.controllable_components.push_back(
            participant->binding.value().controllable_component_id);
        exact_partial.resources.insert(exact_partial.resources.end(),
                                       participant->resources.begin(),
                                       participant->resources.end());
      }
      std::sort(exact_partial.controllable_components.begin(),
                exact_partial.controllable_components.end());
      exact_partial.controllable_components.erase(
          std::unique(exact_partial.controllable_components.begin(),
                      exact_partial.controllable_components.end()),
          exact_partial.controllable_components.end());
      std::sort(exact_partial.resources.begin(), exact_partial.resources.end());
      exact_partial.resources.erase(
          std::unique(exact_partial.resources.begin(), exact_partial.resources.end()),
          exact_partial.resources.end());
      resolution = Result<StopScopeResolutionDTO>::Ok(std::move(exact_partial));
    }
    auto full_resolution =
        ordinary != nullptr
            ? resolve_full_stop_scope(
                  FullStopIntentDTO{context, cell_stop_generation_, attribution}, resolver_snapshot)
            : resolution;
    if (!full_resolution.ok()) {
      return Result<StopAcceptedDTO>::Err(full_resolution.error());
    }
    const bool request_id_used_by_ordinary_command =
        ordinary_request_id_retained_locked(context.request_id);
    const bool request_id_used_by_prior_stop =
        ordinary != nullptr && malformed_ingress == nullptr &&
        std::any_of(
            stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
            [&](const StopReplayEntry& entry) { return entry.request_id() == context.request_id; });
    if (request_id_used_by_ordinary_command || request_id_used_by_prior_stop) {
      auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
      resolution = full_resolution;
      resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
      resolution.value().reason = StopScopeResolutionReasonDTO::kRequestIdentityConflict;
      resolution.value().stale_diagnostics = std::move(retained_diagnostics);
    }
    const bool request_id_retained_by_stop =
        request_id_used_by_prior_stop ||
        emergency_stop_reserve_.find(context.request_id.value()).has_value();
    auto captured_digest = ComputeCapturedStopScopeDigest(resolution.value());
    auto full_digest = ComputeCapturedStopScopeDigest(full_resolution.value());
    if (!captured_digest.ok()) {
      return Result<StopAcceptedDTO>::Err(captured_digest.error());
    }
    if (!full_digest.ok()) {
      return Result<StopAcceptedDTO>::Err(full_digest.error());
    }

    const auto transaction_intersects = [](const StopTransaction& transaction,
                                           const StopScopeResolutionDTO& scope) {
      const bool operation_overlap =
          transaction.accepted.has_value() &&
          std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                      transaction.accepted->affected_operation_revisions.end(),
                      [&](const auto& affected) {
                        return std::binary_search(scope.operations.begin(), scope.operations.end(),
                                                  affected.operation_id);
                      });
      const bool resource_overlap = std::any_of(
          transaction.resources.begin(), transaction.resources.end(), [&](const auto& resource) {
            return std::binary_search(scope.resources.begin(), scope.resources.end(), resource);
          });
      return operation_overlap || resource_overlap;
    };
    const auto transaction_covers = [](const StopTransaction& transaction,
                                       const StopScopeResolutionDTO& scope) {
      if (!transaction.accepted.has_value()) {
        return false;
      }
      return std::all_of(scope.operations.begin(), scope.operations.end(),
                         [&](const auto& operation_id) {
                           const auto found = std::lower_bound(
                               transaction.accepted->affected_operation_revisions.begin(),
                               transaction.accepted->affected_operation_revisions.end(),
                               operation_id, [](const auto& affected, const OperationId& id) {
                                 return affected.operation_id < id;
                               });
                           return found !=
                                      transaction.accepted->affected_operation_revisions.end() &&
                                  found->operation_id == operation_id;
                         }) &&
             std::all_of(scope.resources.begin(), scope.resources.end(), [&](const auto& resource) {
               return std::binary_search(transaction.resources.begin(), transaction.resources.end(),
                                         resource);
             });
    };
    const auto resident_transactions = [&](const StopScopeResolutionDTO& scope) {
      std::vector<StopTransaction*> result;
      result.reserve(stop_transactions_.size());
      for (const auto& candidate : stop_transactions_) {
        if (candidate != nullptr && candidate->active && !candidate->terminal &&
            candidate->stop_id.has_value() && candidate->accepted.has_value() &&
            transaction_intersects(*candidate, scope)) {
          result.push_back(candidate.get());
        }
      }
      std::sort(result.begin(), result.end(), [](const auto* left, const auto* right) {
        return *left->stop_id < *right->stop_id;
      });
      return result;
    };
    const auto transaction_matches_selector = [&](const StopTransaction& transaction) {
      if (ordinary == nullptr || !transaction.accepted.has_value()) {
        return false;
      }
      return std::visit(
          [&](const auto& targets) {
            using Targets = std::decay_t<decltype(targets)>;
            if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                                   transaction.accepted->affected_operation_revisions.end(),
                                   [&](const auto& affected) {
                                     return affected.operation_id == target.operation_id;
                                   });
              });
            } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::binary_search(transaction.resources.begin(),
                                          transaction.resources.end(), target.resource_id);
              });
            } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(
                    transaction.accepted->affected_operation_revisions.begin(),
                    transaction.accepted->affected_operation_revisions.end(),
                    [&](const auto& affected) {
                      const auto found = std::lower_bound(
                          resolver_snapshot.operations.begin(), resolver_snapshot.operations.end(),
                          affected.operation_id, [](const auto& operation, const OperationId& id) {
                            return operation.operation_id < id;
                          });
                      return found != resolver_snapshot.operations.end() &&
                             found->operation_id == affected.operation_id &&
                             std::binary_search(found->controllable_components.begin(),
                                                found->controllable_components.end(),
                                                target.controllable_component_id);
                    });
              });
            } else {
              return std::all_of(targets.begin(), targets.end(), [&](const auto& target) {
                return std::any_of(transaction.accepted->affected_operation_revisions.begin(),
                                   transaction.accepted->affected_operation_revisions.end(),
                                   [&](const auto& affected) {
                                     const auto found = std::lower_bound(
                                         resolver_snapshot.operations.begin(),
                                         resolver_snapshot.operations.end(), affected.operation_id,
                                         [](const auto& operation, const OperationId& id) {
                                           return operation.operation_id < id;
                                         });
                                     return found != resolver_snapshot.operations.end() &&
                                            found->operation_id == affected.operation_id &&
                                            found->program_run_id == target.program_run_id;
                                   });
              });
            }
          },
          ordinary->selector);
    };

    std::optional<std::string_view> overlapping_stop_id;
    if (ordinary != nullptr && !resolution.value().full_stop) {
      auto resident = resident_transactions(resolution.value());
      if (resident.empty() && resolution.value().disposition ==
                                  StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
        for (const auto& candidate : stop_transactions_) {
          if (candidate != nullptr && candidate->active && !candidate->terminal &&
              candidate->stop_id.has_value() && transaction_matches_selector(*candidate)) {
            resident.push_back(candidate.get());
          }
        }
        std::sort(resident.begin(), resident.end(), [](const auto* left, const auto* right) {
          return *left->stop_id < *right->stop_id;
        });
      }
      const auto covering_full =
          std::find_if(resident.begin(), resident.end(), [&](const auto* tx) {
            return tx->full_stop && transaction_covers(*tx, resolution.value());
          });
      if (covering_full != resident.end()) {
        overlapping_stop_id = (*covering_full)->stop_id->value();
      } else if (resident.size() == 1U &&
                 transaction_covers(*resident.front(), resolution.value())) {
        overlapping_stop_id = resident.front()->stop_id->value();
      } else if (!resident.empty()) {
        auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
        resolution = full_resolution;
        resolution.value().disposition = StopResolutionDispositionDTO::kPromotedToFull;
        resolution.value().reason = StopScopeResolutionReasonDTO::kOverlappingStopTransactions;
        resolution.value().stale_diagnostics = std::move(retained_diagnostics);
        captured_digest = ComputeCapturedStopScopeDigest(resolution.value());
        if (!captured_digest.ok()) {
          return Result<StopAcceptedDTO>::Err(captured_digest.error());
        }
      }
    }
    const auto full_predecessors = resident_transactions(full_resolution.value());
    auto full_outer_deadline_ns = outer_deadline_ns;
    std::vector<StopAcceptedPredecessorInput> full_predecessor_inputs;
    std::vector<StopControlWorkToken> full_predecessor_tokens;
    std::vector<StopCertificateDTO> full_predecessor_certificates;
    full_predecessor_inputs.reserve(full_predecessors.size());
    full_predecessor_tokens.reserve(full_predecessors.size());
    full_predecessor_certificates.reserve(full_predecessors.size());
    for (const auto* predecessor : full_predecessors) {
      full_outer_deadline_ns = std::min(full_outer_deadline_ns, predecessor->outer_deadline_ns);
      const auto remaining = std::min(predecessor->accepted->remaining_outer_duration_ns,
                                      predecessor->outer_deadline_ns <= accepted_at_ns
                                          ? 0
                                          : predecessor->outer_deadline_ns - accepted_at_ns);
      if (remaining <= 0) {
        motion_admission_closed_ = true;
        return reject<StopAcceptedDTO>("resident predecessor exhausted before aggregation",
                                       "stop_predecessor_deadline_exhausted",
                                       ErrorCode::kInvariantViolation);
      }
      full_predecessor_inputs.push_back({*predecessor->accepted, remaining});
      if (predecessor->certificate.has_value()) {
        full_predecessor_certificates.push_back(*predecessor->certificate);
      } else {
        full_predecessor_tokens.push_back(predecessor->token);
      }
    }

    const EmergencyStopAcceptanceCandidate candidate{
        context.request_id.value(),
        normalized_digest.has_value() ? std::optional<std::string_view>(normalized_digest->value())
                                      : std::nullopt,
        attribution.transport,
        attribution.assurance,
        attribution.principal_id.has_value()
            ? std::optional<std::string_view>(*attribution.principal_id)
            : std::nullopt,
        malformed_ingress != nullptr ? std::optional<std::string_view>(malformed_ingress->reason)
                                     : std::nullopt,
        proposed_stop_id.value(),
        proposed_generation,
        accepted_at_ns,
        outer_deadline_ns,
        full_outer_deadline_ns,
        resolution.value().disposition,
        captured_digest.value().value(),
        full_digest.value().value(),
        overlapping_stop_id,
        resolution.value().full_stop};
    outcome = emergency_stop_reserve_.reserve_or_join(candidate);
    if (!outcome.accepted() || outcome.work_token.slot >= stop_transactions_.size() ||
        outcome.correlation_slot >= stop_correlations_.size()) {
      return reject<StopAcceptedDTO>("emergency Stop reserve rejected its prepared candidate",
                                     "emergency_stop_reserve_invariant",
                                     ErrorCode::kInvariantViolation);
    }
    post_reserve_guard.armed = true;

    std::optional<EmergencyStopAffectedStateSnapshotDTO> projection_state;
    if (outcome.replay_status == RequestReplayStatusDTO::kReplayedExact) {
      const auto& retained = stop_correlations_[outcome.correlation_slot];
      if (!retained.has_value()) {
        return reject<StopAcceptedDTO>("Stop reserve replay has no retained result",
                                       "stop_replay_result_missing",
                                       ErrorCode::kInvariantViolation);
      }
      accepted = *retained;
      accepted->replay_status = RequestReplayStatusDTO::kReplayedExact;
      work_token = outcome.work_token;
    } else if (outcome.acceptance_path == EmergencyStopAcceptancePathDTO::kJoinedResident ||
               (stop_transactions_[outcome.work_token.slot]->active &&
                stop_transactions_[outcome.work_token.slot]->token == outcome.work_token)) {
      auto& transaction = *stop_transactions_[outcome.work_token.slot];
      if (!transaction.active || transaction.token != outcome.work_token ||
          !transaction.accepted.has_value() || !transaction.stop_id.has_value()) {
        return reject<StopAcceptedDTO>("joined Stop transaction is not resident",
                                       "stop_join_transaction_missing",
                                       ErrorCode::kInvariantViolation);
      }
      accepted = *transaction.accepted;
      accepted->request_id = context.request_id;
      accepted->replay_status = RequestReplayStatusDTO::kAcceptedNew;
      accepted->requested_selector_digest = requested_selector_digest.value();
      // A containment that joins a resident Stop still latches its outcome.
      if (required_fault_latch.has_value() && !transaction.certificate.has_value()) {
        latch_stop_fault_locked(transaction, *required_fault_latch);
      }
      if (malformed_ingress != nullptr) {
        accepted->resolution_disposition = StopResolutionDispositionDTO::kPromotedToFull;
        accepted->resolution_reason = resolution.value().reason;
        accepted->stale_diagnostics.clear();
      } else if (full != nullptr) {
        // The physical transaction is shared; freshness diagnostics describe
        // this caller's selector, not the request that originally created it.
        accepted->resolution_disposition = resolution.value().disposition;
        accepted->resolution_reason = resolution.value().reason;
        accepted->stale_diagnostics = std::move(resolution.value().stale_diagnostics);
      } else if (!transaction.full_stop) {
        if (resolution.value().disposition ==
            StopResolutionDispositionDTO::kAlreadyTerminalNoEffect) {
          accepted->resolution_disposition =
              resolution.value().stale_diagnostics.empty()
                  ? StopResolutionDispositionDTO::kExact
                  : StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
          accepted->resolution_reason =
              resolution.value().stale_diagnostics.empty()
                  ? StopScopeResolutionReasonDTO::kExactCurrentScope
                  : StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope;
        } else {
          accepted->resolution_disposition = resolution.value().disposition;
          accepted->resolution_reason = resolution.value().reason;
        }
        accepted->stale_diagnostics = std::move(resolution.value().stale_diagnostics);
      }
      accepted->remaining_outer_duration_ns =
          std::max<std::int64_t>(0, transaction.outer_deadline_ns - accepted_at_ns);
      accepted->accepted_at_ns = accepted_at_ns;
      for (auto& participant : accepted->participant_budget_allocations) {
        const auto elapsed =
            accepted->fixed_outer_duration_ns - accepted->remaining_outer_duration_ns;
        for (auto& phase : participant.phases) {
          phase.remaining_deadline_offset_ns =
              accepted->remaining_outer_duration_ns <= accepted->fixed_kernel_completion_reserve_ns
                  ? 0
                  : std::max<std::int64_t>(
                        0, std::min(phase.fixed_deadline_offset_ns - elapsed,
                                    accepted->remaining_outer_duration_ns -
                                        accepted->fixed_kernel_completion_reserve_ns));
        }
      }
      if (outcome.correlation_retained) {
        stop_correlations_[outcome.correlation_slot] = *accepted;
      }
      projection_state.emplace();
      projection_state->operations.reserve(accepted->affected_operation_revisions.size());
      for (const auto& affected : accepted->affected_operation_revisions) {
        const auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation != nullptr) {
          projection_state->operations.push_back(operation->state);
        }
      }
      projection_state->resources = transaction.resources;
      work_token = outcome.work_token;
    } else {
      if (outcome.full_stop && !resolution.value().full_stop) {
        auto retained_diagnostics = std::move(resolution.value().stale_diagnostics);
        resolution = std::move(full_resolution);
        resolution.value().disposition = outcome.disposition;
        resolution.value().reason = request_id_retained_by_stop
                                        ? StopScopeResolutionReasonDTO::kRequestIdentityConflict
                                        : StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced;
        resolution.value().stale_diagnostics = std::move(retained_diagnostics);
      }
      auto acceptance_resolution = resolution.value();
      const bool aggregate = acceptance_resolution.full_stop && !full_predecessor_inputs.empty();
      if (aggregate) {
        for (const auto& predecessor : full_predecessor_inputs) {
          for (const auto& affected : predecessor.acceptance.affected_operation_revisions) {
            acceptance_resolution.operations.push_back(affected.operation_id);
          }
        }
        for (const auto* predecessor : full_predecessors) {
          acceptance_resolution.resources.insert(acceptance_resolution.resources.end(),
                                                 predecessor->resources.begin(),
                                                 predecessor->resources.end());
        }
        std::sort(acceptance_resolution.operations.begin(), acceptance_resolution.operations.end());
        acceptance_resolution.operations.erase(std::unique(acceptance_resolution.operations.begin(),
                                                           acceptance_resolution.operations.end()),
                                               acceptance_resolution.operations.end());
        std::sort(acceptance_resolution.resources.begin(), acceptance_resolution.resources.end());
        acceptance_resolution.resources.erase(std::unique(acceptance_resolution.resources.begin(),
                                                          acceptance_resolution.resources.end()),
                                              acceptance_resolution.resources.end());
      }
      std::vector<StopAffectedOperationRevisionDTO> affected_revisions;
      std::vector<PreparedParticipantStopPlanDTO> plans;
      std::vector<ResourceSlotStopFenceOwnerDTO> owner_rows;
      std::vector<OperationId> newly_fenced_operations;
      EmergencyStopAffectedStateSnapshotDTO affected_state;
      const bool exact_partial_capture =
          partial_dispatch_scope != nullptr && !acceptance_resolution.full_stop;
      const auto participant_selected = [&](const OperationId& operation_id,
                                            const OperationParticipantId& participant_id) {
        return !exact_partial_capture ||
               (operation_id == partial_dispatch_scope->operation_id &&
                std::binary_search(partial_dispatch_scope->participant_ids.begin(),
                                   partial_dispatch_scope->participant_ids.end(), participant_id));
      };
      std::size_t participant_capacity = 0U;
      for (const auto& operation_id : resolution.value().operations) {
        const auto* operation = find_active_operation_locked(operation_id);
        if (operation != nullptr) {
          participant_capacity += static_cast<std::size_t>(std::count_if(
              operation->participants.begin(), operation->participants.end(),
              [&](const auto& participant) {
                return participant_selected(operation_id, participant.slot_owner.participant_id);
              }));
        }
      }
      affected_revisions.reserve(resolution.value().operations.size());
      plans.reserve(participant_capacity);
      owner_rows.reserve(participant_capacity);
      newly_fenced_operations.reserve(resolution.value().operations.size());
      affected_state.operations.reserve(resolution.value().operations.size());
      for (const auto& operation_id : resolution.value().operations) {
        const bool inherited =
            aggregate &&
            std::any_of(full_predecessor_inputs.begin(), full_predecessor_inputs.end(),
                        [&](const auto& predecessor) {
                          const auto found = std::lower_bound(
                              predecessor.acceptance.affected_operation_revisions.begin(),
                              predecessor.acceptance.affected_operation_revisions.end(),
                              operation_id, [](const auto& affected, const OperationId& id) {
                                return affected.operation_id < id;
                              });
                          return found !=
                                     predecessor.acceptance.affected_operation_revisions.end() &&
                                 found->operation_id == operation_id;
                        });
        if (inherited) {
          continue;
        }
        auto* operation = find_active_operation_locked(operation_id);
        if (operation == nullptr) {
          return reject<StopAcceptedDTO>("resolved Stop operation is not resident or exhausted",
                                         "stop_operation_not_fenceable",
                                         ErrorCode::kInvariantViolation);
        }
        auto revision_before_stop = operation->state.operation_revision;
        if (operation->pending_control.has_value() &&
            operation->pending_control->accepted.operation_revision_after_acceptance >
                revision_before_stop) {
          revision_before_stop =
              operation->pending_control->accepted.operation_revision_after_acceptance;
        }
        if (revision_before_stop.value() >= kMaximumKernelJsonInteger) {
          return reject<StopAcceptedDTO>("resolved Stop operation revision is exhausted",
                                         "stop_operation_not_fenceable",
                                         ErrorCode::kInvariantViolation);
        }
        const OperationRevision next_revision(revision_before_stop.value() + 1U);
        affected_revisions.push_back({operation_id, next_revision});
        newly_fenced_operations.push_back(operation_id);
        for (const auto& participant : operation->participants) {
          if (!participant_selected(operation_id, participant.slot_owner.participant_id)) {
            continue;
          }
          const auto& selected_stop_plan = runtime_collision_containment
                                               ? participant.fault_stop_plan
                                               : participant.ordinary_stop_plan;
          if (selected_stop_plan == nullptr) {
            return reject<StopAcceptedDTO>("resolved Stop participant has no prepared plan",
                                           "stop_participant_plan_missing",
                                           ErrorCode::kInvariantViolation);
          }
          plans.push_back(*selected_stop_plan);
          owner_rows.push_back(ResourceSlotStopFenceOwnerDTO{participant.slot_owner,
                                                             participant.resources, next_revision});
        }
      }
      std::sort(owner_rows.begin(), owner_rows.end(), [](const auto& left, const auto& right) {
        return std::tie(left.expected_owner.operation_id, left.expected_owner.participant_id) <
               std::tie(right.expected_owner.operation_id, right.expected_owner.participant_id);
      });
      std::vector<ResourceId> orphan_resources;
      for (const auto& resource : resolution.value().resources) {
        if (aggregate && std::any_of(full_predecessors.begin(), full_predecessors.end(),
                                     [&](const auto* predecessor) {
                                       return std::binary_search(predecessor->resources.begin(),
                                                                 predecessor->resources.end(),
                                                                 resource);
                                     })) {
          continue;
        }
        const auto found = std::lower_bound(
            slot_snapshot.slots.begin(), slot_snapshot.slots.end(), resource,
            [](const auto& slot, const ResourceId& id) { return slot.resource_id < id; });
        if (found != slot_snapshot.slots.end() && found->resource_id == resource &&
            !found->owner.has_value() &&
            found->state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
          orphan_resources.push_back(resource);
        }
      }
      const auto remaining_outer_duration_ns =
          std::max<std::int64_t>(0, outcome.outer_deadline_ns - accepted_at_ns);
      auto made =
          aggregate
              ? (ordinary != nullptr
                     ? MakeAggregatedStopAccepted(*ordinary, StopId(std::string(outcome.stop_id())),
                                                  outcome.stop_generation, acceptance_resolution,
                                                  affected_revisions, *cell_stop_deadline_budget_,
                                                  remaining_outer_duration_ns, accepted_at_ns,
                                                  plans, full_predecessor_inputs)
                     : MakeAggregatedStopAccepted(*full, StopId(std::string(outcome.stop_id())),
                                                  outcome.stop_generation, acceptance_resolution,
                                                  affected_revisions, *cell_stop_deadline_budget_,
                                                  remaining_outer_duration_ns, accepted_at_ns,
                                                  plans, full_predecessor_inputs))
              : (ordinary != nullptr
                     ? MakeStopAccepted(*ordinary, StopId(std::string(outcome.stop_id())),
                                        outcome.stop_generation, acceptance_resolution,
                                        affected_revisions, *cell_stop_deadline_budget_,
                                        remaining_outer_duration_ns, accepted_at_ns, plans)
                     : MakeStopAccepted(*full, StopId(std::string(outcome.stop_id())),
                                        outcome.stop_generation, acceptance_resolution,
                                        affected_revisions, *cell_stop_deadline_budget_,
                                        remaining_outer_duration_ns, accepted_at_ns, plans));
      if (!made.ok()) {
        return Result<StopAcceptedDTO>::Err(made.error());
      }
      accepted = std::move(made.value());
      if (malformed_ingress != nullptr) {
        accepted->requested_selector_digest = requested_selector_digest.value();
      }
      auto fenced =
          owner_rows.empty() && orphan_resources.empty()
              ? Result<ResourceSlotStopFenceResultDTO>::Ok(ResourceSlotStopFenceResultDTO{
                    slot_snapshot.operation_resource_generation, outcome.stop_generation, {}, {}})
              : resource_slots_->stop_fence_batch(ResourceSlotStopFenceRequestDTO{
                    slot_snapshot.operation_resource_generation, outcome.stop_generation,
                    std::move(owner_rows), orphan_resources});
      if (!fenced.ok()) {
        motion_admission_closed_ = true;
        return Result<StopAcceptedDTO>::Err(fenced.error());
      }
      // The resource/operation Stop fence is now linearized. Install the
      // matching process-local participant fence before publishing any
      // physical Stop stage so late work from the replaced trajectory cannot
      // win a competing inner terminal. This seam is non-blocking and has no
      // physical effect; containment still belongs exclusively to StopControl.
      for (const auto& affected : affected_revisions) {
        auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation == nullptr) {
          continue;
        }
        for (auto& participant : operation->participants) {
          if (!participant_selected(affected.operation_id, participant.slot_owner.participant_id) ||
              participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
              participant.engine == nullptr || participant.preparation_id == 0U) {
            continue;
          }
          const auto fence =
              participant.engine->fence_addressed_participant_stop(participant.preparation_id);
          participant.stop_cancelled_pre_dispatch =
              participant.stop_cancelled_pre_dispatch ||
              fence == AddressedParticipantStopFenceDisposition::kEffectPreventedBeforeDispatch;
        }
      }
      for (const auto& affected : affected_revisions) {
        auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation == nullptr) {
          continue;
        }
        if (operation->pending_control.has_value()) {
          operation->pending_control->superseded_by_stop = true;
        }
        operation->state.operation_revision = affected.operation_revision_after_acceptance;
        if (operation->runtime_scale_status.has_value()) {
          operation->runtime_scale_status->operation_revision =
              affected.operation_revision_after_acceptance;
          operation->runtime_scale_status->updated_at_ns = accepted_at_ns;
        }
        for (auto& participant : operation->participants) {
          if (!participant_selected(affected.operation_id, participant.slot_owner.participant_id)) {
            continue;
          }
          participant.stop_cancelled_pre_dispatch =
              participant.stop_cancelled_pre_dispatch ||
              (participant.payload_kind == OperationParticipantPayloadKind::kMotion
                   ? !participant.prepared_dispatch_released
                   : (!participant.prepared_dispatch_released &&
                      participant.slot_state == ResourceSlotState::kReserved));
        }
        // Stop owns the public terminal from acceptance through aggregate
        // settlement. A participant that never crossed the dispatch barrier
        // is recorded below, but the operation remains Stopping until that
        // participant owner has committed the same no-effect outcome.
        operation->state.state = OperationStateKind::kStopping;
        operation->state.active_stop_id = StopId(std::string(outcome.stop_id()));
        operation->state.active_stop_generation = outcome.stop_generation;
        operation->state.updated_at_ns = accepted_at_ns;
        for (auto& participant_state : operation->state.participants) {
          if (participant_selected(affected.operation_id, participant_state.participant_id)) {
            const auto participant = std::find_if(
                operation->participants.begin(), operation->participants.end(),
                [&](const auto& candidate) {
                  return candidate.slot_owner.participant_id == participant_state.participant_id;
                });
            participant_state.state = participant != operation->participants.end() &&
                                              participant->stop_cancelled_pre_dispatch
                                          ? OperationStateKind::kCancelledPreDispatch
                                          : OperationStateKind::kStopping;
            // Containment does not acknowledge or repair a retained fault.
          }
        }
        for (auto& participant : operation->participants) {
          if (!participant_selected(affected.operation_id, participant.slot_owner.participant_id)) {
            continue;
          }
          participant.slot_owner.operation_revision = affected.operation_revision_after_acceptance;
          participant.slot_state = ResourceSlotState::kStopping;
        }
        operation->stop_effect_in_flight = true;
        stamp_operation_capture_locked(*operation, operation->state);
        affected_state.operations.push_back(operation->state);
      }
      affected_state.operations.clear();
      affected_state.operations.reserve(accepted->affected_operation_revisions.size());
      for (const auto& affected : accepted->affected_operation_revisions) {
        const auto* operation = find_active_operation_locked(affected.operation_id);
        if (operation != nullptr) {
          affected_state.operations.push_back(operation->state);
        }
      }
      affected_state.resources = acceptance_resolution.resources;

      auto& transaction = *stop_transactions_[outcome.work_token.slot];
      transaction = StopTransaction{
          true,
          outcome.work_token,
          StopId(std::string(outcome.stop_id())),
          outcome.stop_generation,
          outcome.outer_deadline_ns,
          outcome.full_stop,
          false,
          EmergencyStopProjectionStateDTO::kPending,
          false,
          false,
          false,
          *accepted,
          projected_intent,
          affected_revisions,
          fenced.value().orphan_resources,
          std::move(newly_fenced_operations),
          exact_partial_capture ? partial_dispatch_scope->participant_ids
                                : std::vector<OperationParticipantId>{},
          acceptance_resolution.resources,
          aggregate ? full_predecessor_tokens : std::vector<StopControlWorkToken>{},
          aggregate ? full_predecessor_certificates : std::vector<StopCertificateDTO>{},
          false,
          {},
          std::nullopt,
          std::nullopt,
          malformed_ingress != nullptr ? std::optional<std::string>(malformed_ingress->reason)
                                       : std::nullopt,
          std::nullopt};
      if (accepted->resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull) {
        latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kScopePromotion);
      }
      // Incident containment latches even a nominal Stop of an idle cell:
      // physical rest after an unexpected loss is not yet proven.
      if (required_fault_latch.has_value()) {
        latch_stop_fault_locked(transaction, *required_fault_latch);
      }
      if (outcome.correlation_retained) {
        stop_correlations_[outcome.correlation_slot] = *accepted;
      }
      projection_state = std::move(affected_state);
      work_token = outcome.work_token;
      handoff_work = true;
      // All deadline mutation for one physical Stop slot is serialized by
      // state_mutex_. A predecessor may terminalize as soon as this aggregate
      // becomes resident and rearm it at `now`; publishing the outer watchdog
      // later must never overwrite that wakeup.
      stop_control_executor_->ArmDeadline(work_token, outcome.outer_deadline_ns);
      next_stop_id_ = proposed_number + 1U;
      cell_stop_generation_ = std::max(cell_stop_generation_, outcome.stop_generation);
      if (outcome.full_stop ||
          accepted->resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull) {
        motion_admission_closed_ = true;
      }
    }

    if (outcome.replay_status == RequestReplayStatusDTO::kAcceptedNew &&
        outcome.correlation_retained && accepted.has_value() && projection_state.has_value()) {
      projection_token = allocate_stop_projection_job_locked();
      if (!projection_token.has_value()) {
        // The containment effect is already linearized and handed to the
        // prequalified StopControl owner. Forensic projection capacity may
        // therefore degrade admission, but it must never reject, widen, pin,
        // or terminate an accepted Stop.
        auto& transaction = *stop_transactions_[outcome.work_token.slot];
        transaction.persistence_degraded = true;
        transaction.acceptance_projection_state = EmergencyStopProjectionStateDTO::kFailed;
        motion_admission_closed_ = true;
      } else {
        auto& projection = *stop_projection_jobs_[projection_token->slot];
        projection.intent = projected_intent;
        projection.affected_state = std::move(*projection_state);
        projection.acceptance = EmergencyStopAcceptanceRecordDTO{
            config_.request_owner_epoch,
            context.request_id,
            normalized_digest,
            malformed_ingress != nullptr ? std::optional<std::string>(malformed_ingress->reason)
                                         : std::nullopt,
            attribution,
            accepted->replay_status,
            accepted->stop_id,
            accepted->stop_generation,
            accepted_at_ns,
            outcome.outer_deadline_ns,
            accepted->resolution_disposition,
            accepted->captured_scope_digest,
            EmergencyStopEntryStateDTO::kActive,
            outcome.acceptance_path,
            EmergencyStopProjectionStateDTO::kPending};
        projection.accepted = *accepted;
      }
    }
    if (program_run_targets != nullptr && accepted.has_value() && !outcome.full_stop &&
        accepted->resolution_disposition != StopResolutionDispositionDTO::kPromotedToFull) {
      for (const auto& target : *program_run_targets) {
        const auto retained =
            std::find_if(program_run_stop_fences_.begin(), program_run_stop_fences_.end(),
                         [&](const ProgramRunStopFence& fence) {
                           return fence.program_run_id == target.program_run_id;
                         });
        ProgramRunStopFence* installed = nullptr;
        if (retained == program_run_stop_fences_.end()) {
          program_run_stop_fences_.push_back(ProgramRunStopFence{
              target.program_run_id, accepted->stop_id, accepted->stop_generation, false});
          installed = &program_run_stop_fences_.back();
        } else {
          retained->stop_id = accepted->stop_id;
          retained->stop_generation = accepted->stop_generation;
          retained->releasable = false;
          installed = &*retained;
        }
        auto& fence = *installed;
        if (fence.selection_revision < kMaximumKernelJsonInteger) ++fence.selection_revision;
        fence.legacy_selection_revision = fence.selection_revision;
        // The whole-run Stop closes the run through its stop identity. Only
        // effectful work that may be moving is contained here; Ready (never
        // started), Held and non-effectful nodes keep their motion state, so
        // releasing the Stop later does not grant motion.
        for (std::size_t index = 0U; index < fence.node_states.size(); ++index) {
          auto& state = fence.node_states[index];
          if (state == ProgramNodeHoldStateDTO::kRunning &&
              program_node_effectful_locked(fence.program_run_id, index))
            state = ProgramNodeHoldStateDTO::kStopping;
        }
      }
    }
    if (ordinary != nullptr && malformed_ingress == nullptr && accepted.has_value()) {
      const auto retained =
          std::find_if(stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
                       [&](const StopReplayEntry& entry) { return entry.matches(*ordinary); });
      if (retained == stop_replay_ledger_.end()) {
        if (stop_replay_ledger_.size() >= config_.request_replay_capacity) {
          stop_replay_ledger_.erase(stop_replay_ledger_.begin());
        }
        stop_replay_ledger_.push_back(StopReplayEntry{*ordinary, *accepted});
      }
    }
  }

  if (handoff_work) {
    handoff.control = work_token;
  }
  if (projection_token.has_value()) {
    handoff.projection = projection_token;
  }
  if (!accepted.has_value()) {
    return reject<StopAcceptedDTO>("Stop acceptance result was not retained",
                                   "stop_acceptance_result_missing",
                                   ErrorCode::kInvariantViolation);
  }
  post_reserve_guard.armed = false;
  return Result<StopAcceptedDTO>::Ok(std::move(*accepted));
}

void ResourceAddressedExecutionKernel::ProjectStop(const StopControlWorkToken token) noexcept {
  StopProjectionJob job;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      if (!token.locally_valid() || token.slot >= stop_projection_jobs_.size()) {
        motion_admission_closed_ = true;
        return;
      }
      auto& retained = *stop_projection_jobs_[token.slot];
      if (!retained.active || retained.token != token) {
        return;
      }
      job = retained;
      retained.active = false;
    }

    if (job.terminal_status.has_value()) {
      if (!job.terminal_certificate.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      std::optional<ErrorDTO> operation_state_projection_error;
      for (const auto& operation_state : job.terminal_operation_states) {
        Result<std::uint64_t> recorded = Result<std::uint64_t>::Err(
            ErrorDTO{"operation_journal_stop_terminal_state_projection_failed",
                     "Stop terminal operation-state projection did not return a result",
                     {}});
        try {
          recorded = journal_.RecordOperationState(config_.request_owner_epoch, operation_state,
                                                   nullptr, "stop_terminal_committed");
        } catch (...) {
        }
        if (!recorded.ok() && !operation_state_projection_error.has_value()) {
          operation_state_projection_error = recorded.error();
        }
      }
      if (!job.terminal_fault_latch.has_value() &&
          (operation_state_projection_error.has_value() ||
           job.terminal_status->acceptance_projection.state ==
               StopDurableProjectionStateDTO::kFailed)) {
        std::lock_guard state_lock(state_mutex_);
        if (!stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value() ||
            cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger) {
          motion_admission_closed_ = true;
          return;
        }
        cell_stop_fault_latch_generation_ =
            StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
        job.terminal_fault_latch = persistence_fault_latch(
            config_.cell_id, config_.request_owner_epoch, *stop_retained_bundle_id_,
            *stop_graph_revision_, *job.terminal_certificate, cell_stop_fault_latch_generation_,
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                           std::nullopt});
        job.terminal_status->fault_latch = job.terminal_fault_latch;
        active_stop_fault_latch_ = job.terminal_fault_latch;
        motion_admission_closed_ = true;
      }
      auto projected =
          journal_.RecordStopTerminal(config_.request_owner_epoch, *job.terminal_status,
                                      *job.terminal_certificate, job.terminal_fault_latch);
      if (operation_state_projection_error.has_value() &&
          projected.state == StopForensicProjectionStateDTO::kRecorded) {
        projected = StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kFailed,
                                                    projected.journal_sequence,
                                                    operation_state_projection_error};
      }
      std::lock_guard state_lock(state_mutex_);
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.terminal_certificate->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.terminal_certificate->stop_generation;
          });
      if (record == stop_terminal_history_.end()) {
        motion_admission_closed_ = true;
        return;
      }
      if ((*record)->terminal_projection_token == token) {
        (*record)->terminal_projection_token.reset();
      }
      if (job.terminal_fault_latch.has_value() && !(*record)->fault_latch.has_value()) {
        (*record)->fault_latch = job.terminal_fault_latch;
        (*record)->status.fault_latch = job.terminal_fault_latch;
      }
      if (projected.state == StopForensicProjectionStateDTO::kRecorded) {
        (*record)->status.terminal_projection = StopDurableProjectionStatusDTO{
            StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
        if ((*record)->fault_latch.has_value()) {
          (*record)->fault_latch->terminal_projection = StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
          (*record)->status.fault_latch = (*record)->fault_latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == (*record)->fault_latch->latch_id) {
            active_stop_fault_latch_ = (*record)->fault_latch;
          }
        }
      } else {
        motion_admission_closed_ = true;
        const auto failure =
            projected.error.value_or(ErrorDTO{"operation_journal_stop_terminal_projection_failed",
                                              "Stop terminal forensic projection failed",
                                              {}});
        (*record)->status.terminal_projection = StopDurableProjectionStatusDTO{
            StopDurableProjectionStateDTO::kFailed, failure, kStopJournalProjectionAttempt};
        if (!(*record)->fault_latch.has_value() && stop_retained_bundle_id_.has_value() &&
            stop_graph_revision_.has_value() &&
            cell_stop_fault_latch_generation_.value() < kMaximumKernelJsonInteger) {
          cell_stop_fault_latch_generation_ =
              StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
          (*record)->fault_latch = persistence_fault_latch(
              config_.cell_id, config_.request_owner_epoch, *stop_retained_bundle_id_,
              *stop_graph_revision_, (*record)->certificate, cell_stop_fault_latch_generation_,
              StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure,
                                             kStopJournalProjectionAttempt});
          (*record)->status.fault_latch = (*record)->fault_latch;
          active_stop_fault_latch_ = (*record)->fault_latch;
        }
        if ((*record)->fault_latch.has_value()) {
          (*record)->fault_latch->terminal_projection = StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kFailed, failure, kStopJournalProjectionAttempt};
          (*record)->status.fault_latch = (*record)->fault_latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == (*record)->fault_latch->latch_id) {
            active_stop_fault_latch_ = (*record)->fault_latch;
          }
        }
      }
      return;
    }

    if (job.surveillance_checkpoint.has_value()) {
      if (!job.surveillance_fault_latch.has_value()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
        return;
      }
      const auto projected_at_ns = clock_.now();
      const auto projected = journal_.RecordStopSurveillanceCheckpoint(
          config_.request_owner_epoch, *job.surveillance_checkpoint, *job.surveillance_fault_latch,
          projected_at_ns);
      std::lock_guard state_lock(state_mutex_);
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.surveillance_checkpoint->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.surveillance_checkpoint->stop_generation;
          });
      if (record == stop_terminal_history_.end()) {
        motion_admission_closed_ = true;
        return;
      }
      if ((*record)->surveillance_projection_token == token) {
        (*record)->surveillance_projection_token.reset();
      }
      if ((*record)->surveillance_state == nullptr) {
        motion_admission_closed_ = true;
        return;
      }
      auto checkpoint_status = StopDurableProjectionStatusDTO{
          StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
      if (projected.state != StopForensicProjectionStateDTO::kRecorded) {
        const auto failure = projected.error.value_or(
            ErrorDTO{"operation_journal_stop_surveillance_projection_failed",
                     "Stop surveillance checkpoint projection failed",
                     {}});
        checkpoint_status = StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                                           failure, kStopJournalProjectionAttempt};
        motion_admission_closed_ = true;
        if ((*record)->fault_latch.has_value()) {
          auto& latch = *(*record)->fault_latch;
          const bool assessment_changed =
              !latch.persistence_degraded ||
              latch.recovery_status != StopFaultRecoveryStatusDTO::kWaitingForDurableRecord ||
              latch.reason_code != "stop_fault_recovery_waiting_for_durable_record";
          latch.persistence_degraded = true;
          latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
          latch.reason_code = "stop_fault_recovery_waiting_for_durable_record";
          if (assessment_changed &&
              latch.recovery_assessment_revision < std::numeric_limits<std::uint64_t>::max()) {
            ++latch.recovery_assessment_revision;
          }
          (*record)->status.fault_latch = latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == latch.latch_id &&
              active_stop_fault_latch_->latch_generation == latch.latch_generation) {
            active_stop_fault_latch_ = latch;
          }
        }
      }
      auto& current = (*record)->surveillance_state->projection;
      if (current.surveillance_generation == job.surveillance_checkpoint->surveillance_generation &&
          current.checkpoint_generation == job.surveillance_checkpoint->checkpoint_generation) {
        current.checkpoint_projection = checkpoint_status;
      }
      (*record)->status.surveillance = current;
      (*record)->status.surveillance_projection = current.checkpoint_projection;
      if ((*record)->fault_latch.has_value()) {
        (*record)->status.fault_latch = (*record)->fault_latch;
      }
      return;
    }

    if (!job.acceptance.has_value() || !job.intent.has_value() || !job.accepted.has_value() ||
        !job.affected_state.has_value()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return;
    }

    const auto projected =
        journal_.RecordStopAccepted(config_.request_owner_epoch, *job.acceptance, *job.intent,
                                    *job.accepted, *job.affected_state);
    const auto state = projected.projection_state == EmergencyStopProjectionStateDTO::kRecorded
                           ? EmergencyStopProjectionStateDTO::kRecorded
                           : EmergencyStopProjectionStateDTO::kFailed;
    const auto durable_status =
        state == EmergencyStopProjectionStateDTO::kRecorded
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{
                  StopDurableProjectionStateDTO::kFailed,
                  projected.error.value_or(ErrorDTO{"operation_journal_stop_projection_failed",
                                                    "Stop acceptance forensic projection failed",
                                                    {}}),
                  kStopJournalProjectionAttempt};
    // The Stop's acceptance projection is its creating request's recovery
    // projection. A joined request's durable receipt can never upgrade that
    // status; its failure still degrades the Stop fail-closed.
    const bool publishes_acceptance_status =
        job.acceptance->acceptance_path != EmergencyStopAcceptancePathDTO::kJoinedResident ||
        state == EmergencyStopProjectionStateDTO::kFailed;
    const bool marked =
        !publishes_acceptance_status ||
        emergency_stop_reserve_.mark_projection(job.acceptance->stop_id.value(), state);
    std::optional<StopControlWorkToken> terminal_projection;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
                   *candidate->stop_id == job.acceptance->stop_id;
          });
      if (transaction != stop_transactions_.end() && publishes_acceptance_status) {
        (*transaction)->acceptance_projection_state = state;
        if ((*transaction)->terminal_projection_token.has_value()) {
          auto& terminal_job =
              *stop_projection_jobs_[(*transaction)->terminal_projection_token->slot];
          if (terminal_job.terminal_status.has_value()) {
            terminal_job.terminal_status->acceptance_projection = durable_status;
          }
        }
        if ((*transaction)->terminal && (*transaction)->terminal_projection_token.has_value()) {
          terminal_projection = (*transaction)->terminal_projection_token;
        }
      }
      const auto terminal_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_id == job.acceptance->stop_id &&
                   record->certificate.stop_generation == job.acceptance->stop_generation;
          });
      if (terminal_record != stop_terminal_history_.end() && publishes_acceptance_status) {
        (*terminal_record)->status.acceptance_projection = durable_status;
        if ((*terminal_record)->terminal_projection_token.has_value()) {
          const auto retained_token = *(*terminal_record)->terminal_projection_token;
          if (retained_token.slot < stop_projection_jobs_.size()) {
            auto& terminal_job = *stop_projection_jobs_[retained_token.slot];
            if (terminal_job.active && terminal_job.token == retained_token &&
                terminal_job.terminal_status.has_value()) {
              terminal_job.terminal_status->acceptance_projection = durable_status;
              terminal_projection = retained_token;
            }
          }
        }
      }
      if (state == EmergencyStopProjectionStateDTO::kFailed ||
          (!marked && transaction == stop_transactions_.end() &&
           terminal_record == stop_terminal_history_.end())) {
        motion_admission_closed_ = true;
        if (transaction != stop_transactions_.end()) {
          (*transaction)->persistence_degraded = true;
        }
      }
    }
    if (terminal_projection.has_value()) {
      stop_projection_worker_->Publish(*terminal_projection);
    }
  } catch (...) {
    std::optional<StopControlWorkToken> terminal_projection;
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    if (job.surveillance_checkpoint.has_value()) {
      const auto record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& candidate) {
            return candidate != nullptr &&
                   candidate->certificate.stop_id == job.surveillance_checkpoint->stop_id &&
                   candidate->certificate.stop_generation ==
                       job.surveillance_checkpoint->stop_generation;
          });
      if (record != stop_terminal_history_.end()) {
        if ((*record)->surveillance_projection_token == token) {
          (*record)->surveillance_projection_token.reset();
        }
        const ErrorDTO failure{"operation_journal_stop_surveillance_projection_failed",
                               "Stop surveillance checkpoint projection failed",
                               {}};
        (*record)->status.surveillance_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if ((*record)->surveillance_state != nullptr) {
          (*record)->surveillance_state->projection.checkpoint_projection =
              (*record)->status.surveillance_projection;
          (*record)->status.surveillance = (*record)->surveillance_state->projection;
        }
        if ((*record)->fault_latch.has_value()) {
          auto& latch = *(*record)->fault_latch;
          const bool assessment_changed =
              !latch.persistence_degraded ||
              latch.recovery_status != StopFaultRecoveryStatusDTO::kWaitingForDurableRecord ||
              latch.reason_code != "stop_fault_recovery_waiting_for_durable_record";
          latch.persistence_degraded = true;
          latch.recovery_status = StopFaultRecoveryStatusDTO::kWaitingForDurableRecord;
          latch.reason_code = "stop_fault_recovery_waiting_for_durable_record";
          if (assessment_changed &&
              latch.recovery_assessment_revision < std::numeric_limits<std::uint64_t>::max()) {
            ++latch.recovery_assessment_revision;
          }
          (*record)->status.fault_latch = latch;
          if (active_stop_fault_latch_.has_value() &&
              active_stop_fault_latch_->latch_id == latch.latch_id &&
              active_stop_fault_latch_->latch_generation == latch.latch_generation) {
            active_stop_fault_latch_ = latch;
          }
        }
      }
      return;
    }
    if (job.acceptance.has_value()) {
      static_cast<void>(emergency_stop_reserve_.mark_projection(
          job.acceptance->stop_id.value(), EmergencyStopProjectionStateDTO::kFailed));
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && candidate->stop_id.has_value() &&
                   *candidate->stop_id == job.acceptance->stop_id;
          });
      if (transaction != stop_transactions_.end()) {
        (*transaction)->acceptance_projection_state = EmergencyStopProjectionStateDTO::kFailed;
        (*transaction)->persistence_degraded = true;
        if ((*transaction)->terminal && (*transaction)->terminal_projection_token.has_value()) {
          terminal_projection = (*transaction)->terminal_projection_token;
        }
      }
      const auto terminal_record = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr && record->certificate.stop_id == job.acceptance->stop_id &&
                   record->certificate.stop_generation == job.acceptance->stop_generation;
          });
      if (terminal_record != stop_terminal_history_.end()) {
        const ErrorDTO failure{"operation_journal_stop_projection_failed",
                               "Stop acceptance forensic projection failed",
                               {}};
        (*terminal_record)->status.acceptance_projection =
            StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed, failure, 1U};
        if ((*terminal_record)->terminal_projection_token.has_value()) {
          const auto retained_token = *(*terminal_record)->terminal_projection_token;
          if (retained_token.slot < stop_projection_jobs_.size()) {
            auto& terminal_job = *stop_projection_jobs_[retained_token.slot];
            if (terminal_job.active && terminal_job.token == retained_token &&
                terminal_job.terminal_status.has_value()) {
              terminal_job.terminal_status->acceptance_projection =
                  (*terminal_record)->status.acceptance_projection;
              terminal_projection = retained_token;
            }
          }
        }
      }
    }
    if (terminal_projection.has_value()) {
      stop_projection_worker_->Publish(*terminal_projection);
    }
  }
}

void ResourceAddressedExecutionKernel::StageAttemptCompleted(
    const StopStageAttemptHandle handle) noexcept {
  std::optional<StopControlWorkToken> owner_token;
  try {
    {
      std::lock_guard state_lock(state_mutex_);
      for (const auto& transaction : stop_transactions_) {
        if (transaction == nullptr || !transaction->active || transaction->terminal) {
          continue;
        }
        const bool owns = std::any_of(
            transaction->participants.begin(), transaction->participants.end(),
            [&](const auto& participant) {
              return std::any_of(participant.attempts.begin(), participant.attempts.end(),
                                 [&](const auto& attempt) {
                                   return attempt.handle.has_value() && *attempt.handle == handle &&
                                          !attempt.result_consumed;
                                 });
            });
        if (owns) {
          owner_token = transaction->token;
          // Deadline mutation for one physical Stop slot is serialized by
          // state_mutex_. This cannot race with ProgressStop rearming the
          // participant phase watchdog after it publishes an attempt.
          stop_control_executor_->ArmDeadline(*owner_token, clock_.now());
          break;
        }
      }
    }
    if (owner_token.has_value()) {
      // Publish coalesces an already-delivered generation. Rearming its fixed
      // deadline at the current instant under the Kernel state lock is the
      // allocation-free wakeup path for an asynchronously completed attempt.
      stop_control_executor_->Publish(*owner_token);
      return;
    }
    // A terminal transaction may already have handed off its immutable
    // certificate. Consume and release a late isolated result so one hung
    // predecessor can never exhaust future Stop-stage capacity. Later P5d
    // projection owns the bounded late-audit sink.
    const auto late = stop_stage_attempt_executor_->TryTake(handle);
    if (late.status != StopStageAttemptTakeStatus::kPending) {
      static_cast<void>(stop_stage_attempt_executor_->Release(handle));
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
  }
}

std::optional<StopControlWorkToken>
ResourceAddressedExecutionKernel::allocate_stop_projection_job_locked() noexcept {
  for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
    auto& job = *stop_projection_jobs_[index];
    if (job.active) {
      continue;
    }
    auto& generation = stop_projection_generations_[index];
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
      continue;
    }
    ++generation;
    const StopControlWorkToken token{static_cast<StopControlSlotIndex>(index), generation};
    job = StopProjectionJob{};
    job.token = token;
    job.active = true;
    return token;
  }
  return std::nullopt;
}

void ResourceAddressedExecutionKernel::latch_stop_fault_locked(StopTransaction& transaction,
                                                               const StopFaultTriggerDTO trigger) {
  motion_admission_closed_ = true;
  if (transaction.fault_latch.has_value()) {
    auto& latch = *transaction.fault_latch;
    if (fault_trigger_priority(trigger) > fault_trigger_priority(latch.trigger)) {
      latch.trigger = trigger;
      latch.reason_code = fault_trigger_reason(trigger);
      ++latch.recovery_assessment_revision;
    }
    latch.persistence_degraded = latch.persistence_degraded || transaction.persistence_degraded;
    if (active_stop_fault_latch_.has_value() &&
        active_stop_fault_latch_->latch_id == latch.latch_id)
      active_stop_fault_latch_ = latch;
    return;
  }
  if (!transaction.accepted.has_value() || !transaction.stop_id.has_value() ||
      !stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value() ||
      cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger)
    return;
  const auto generation = StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
  std::vector<OperationId> operations;
  operations.reserve(transaction.accepted->affected_operation_revisions.size());
  for (const auto& affected : transaction.accepted->affected_operation_revisions)
    operations.push_back(affected.operation_id);
  StopFaultLatchDTO latch{
      config_.cell_id,
      *transaction.stop_id,
      transaction.stop_generation,
      std::nullopt,
      StopFaultLatchId("stop-fault." + config_.request_owner_epoch.value() + "." +
                       std::to_string(generation.value())),
      generation,
      trigger,
      transaction.persistence_degraded,
      std::move(operations),
      transaction.resources,
      *stop_retained_bundle_id_,
      *stop_graph_revision_,
      {StopDurableProjectionStateDTO::kPending, std::nullopt, std::nullopt},
      {StopDurableProjectionStateDTO::kNotApplicable, std::nullopt, std::nullopt},
      StopFaultLatchStateDTO::kLatched,
      1U,
      StopFaultRecoveryStatusDTO::kWaitingForObservation,
      fault_trigger_reason(trigger),
      {}};
  if (stop_fault_latch_field_error(latch).has_value()) return;
  cell_stop_fault_latch_generation_ = generation;
  transaction.fault_latch = latch;
  active_stop_fault_latch_ = std::move(latch);
}

void ResourceAddressedExecutionKernel::refresh_stop_fault_latch_locked() {
  // Recovery of one exact fault must not clear another. Reuse the bounded
  // resident transaction/history tables; no additional ledger or timer.
  const auto retain_newest = [&](const auto& latch) {
    if (is_active_stop_fault_latch(latch) &&
        (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
         active_stop_fault_latch_->latch_generation <= latch->latch_generation))
      active_stop_fault_latch_ = latch;
  };
  for (const auto& transaction : stop_transactions_)
    if (transaction != nullptr && transaction->active && !transaction->terminal)
      retain_newest(transaction->fault_latch);
  for (const auto& record : stop_terminal_history_)
    if (record != nullptr) retain_newest(record->fault_latch);
}

bool ResourceAddressedExecutionKernel::commit_stop_terminal_locked(
    StopTransaction& transaction, const std::int64_t completed_at_ns) noexcept {
  try {
    if (!transaction.accepted.has_value() || !transaction.stop_id.has_value() ||
        transaction.certificate.has_value()) {
      return false;
    }
    std::vector<StopCertificateDTO> predecessors = transaction.predecessor_certificates;
    predecessors.reserve(predecessors.size() + transaction.predecessor_tokens.size());
    for (const auto& predecessor_token : transaction.predecessor_tokens) {
      if (predecessor_token.slot >= stop_transactions_.size()) {
        return false;
      }
      const auto& predecessor = *stop_transactions_[predecessor_token.slot];
      if (!predecessor.active || predecessor.token != predecessor_token ||
          !predecessor.certificate.has_value()) {
        return false;
      }
      predecessors.push_back(*predecessor.certificate);
    }
    std::sort(predecessors.begin(), predecessors.end(),
              [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; });
    if (std::adjacent_find(predecessors.begin(), predecessors.end(),
                           [](const auto& left, const auto& right) {
                             return left.stop_id == right.stop_id;
                           }) != predecessors.end() ||
        predecessors.size() != transaction.accepted->joined_predecessor_stops.size()) {
      return false;
    }

    std::vector<StopScopeEntryDTO> entries;
    std::vector<StopAttemptedStageResultDTO> attempts;
    entries.reserve(transaction.participants.size() + transaction.newly_captured_resources.size());
    for (const auto& participant : transaction.participants) {
      entries.push_back(participant_scope_entry(
          participant, *transaction.stop_id, transaction.stop_generation,
          transaction.accepted->accepted_at_ns, transaction.accepted->remaining_outer_duration_ns));
      attempts.insert(attempts.end(), participant.retained_results.begin(),
                      participant.retained_results.end());
    }
    for (const auto& resource : transaction.newly_captured_resources) {
      entries.push_back(StopScopeEntryDTO{
          StopScopeEntryKeyDTO{*transaction.stop_id, transaction.stop_generation, resource},
          resource,
          StopScopeEntryDispositionDTO::kNotStoppable,
          StopScopeDispatchStateDTO::kEffectUncertain,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          transaction.accepted->accepted_at_ns,
          transaction.accepted->remaining_outer_duration_ns,
          {}});
    }
    // Fresh operation IDs and orphan resources are already canonical. The key
    // variant orders participant subjects before resource-only subjects.
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
      const auto& left_key = left.key;
      const auto& right_key = right.key;
      if (left_key.origin_stop_id != right_key.origin_stop_id) {
        return left_key.origin_stop_id < right_key.origin_stop_id;
      }
      if (left_key.origin_stop_generation != right_key.origin_stop_generation) {
        return left_key.origin_stop_generation < right_key.origin_stop_generation;
      }
      if (left_key.subject.index() != right_key.subject.index()) {
        return left_key.subject.index() < right_key.subject.index();
      }
      if (const auto* left_participant =
              std::get_if<StopParticipantScopeKeyDTO>(&left_key.subject)) {
        const auto& right_participant = std::get<StopParticipantScopeKeyDTO>(right_key.subject);
        return std::tie(left_participant->operation_id, left_participant->participant_id) <
               std::tie(right_participant.operation_id, right_participant.participant_id);
      }
      return std::get<ResourceId>(left_key.subject) < std::get<ResourceId>(right_key.subject);
    });
    std::sort(attempts.begin(), attempts.end(), [](const auto& left, const auto& right) {
      const auto key = [](const auto& attempt) {
        const auto& participant = std::get<StopParticipantScopeKeyDTO>(attempt.scope_key.subject);
        return std::tie(attempt.scope_key.origin_stop_id, attempt.scope_key.origin_stop_generation,
                        participant.operation_id, participant.participant_id,
                        attempt.result.request.stage_index, attempt.result_sequence);
      };
      return key(left) < key(right);
    });

    std::optional<ErrorDTO> terminal_error;
    for (const auto& participant : transaction.participants) {
      if (participant.participant_settlement_error.has_value()) {
        terminal_error = participant.participant_settlement_error;
        break;
      }
      if (!participant.contained && participant.terminal_error.has_value()) {
        terminal_error = participant.terminal_error;
        break;
      }
    }
    const bool known_latch_required =
        transaction.fault_latch.has_value() || transaction.persistence_degraded ||
        transaction.accepted->resolution_disposition ==
            StopResolutionDispositionDTO::kPromotedToFull ||
        std::any_of(attempts.begin(), attempts.end(),
                    [](const auto& attempt) {
                      return attempt.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                             (stage_kind(attempt.result.request.participant_stop_plan,
                                         attempt.result.request.stage_index) !=
                                  StopEscalationStageKind::kNominal ||
                              attempt.result.outcome != ParticipantStopStageOutcomeDTO::kSucceeded);
                    }) ||
        !transaction.newly_captured_resources.empty();
    std::optional<StopFaultLatchId> latch_id =
        transaction.fault_latch.has_value() ? std::optional(transaction.fault_latch->latch_id)
                                            : std::nullopt;
    std::optional<StopFaultLatchGeneration> latch_generation =
        transaction.fault_latch.has_value()
            ? std::optional(transaction.fault_latch->latch_generation)
            : std::nullopt;
    const auto assign_latch_identity = [&]() {
      if (latch_id.has_value()) {
        return true;
      }
      if (cell_stop_fault_latch_generation_.value() >= kMaximumKernelJsonInteger) {
        return false;
      }
      latch_generation = StopFaultLatchGeneration(cell_stop_fault_latch_generation_.value() + 1U);
      latch_id = StopFaultLatchId("stop-fault." + config_.request_owner_epoch.value() + "." +
                                  std::to_string(latch_generation->value()));
      return true;
    };
    if (known_latch_required && !assign_latch_identity()) {
      return false;
    }
    StopTerminalReductionInputDTO reduction{
        StopCertificateId("stop-certificate." + transaction.stop_id->value()),
        StopCertificateRevision(1U),
        *transaction.stop_id,
        transaction.stop_generation,
        requested_selector_snapshot(transaction.projected_intent,
                                    transaction.accepted->requested_selector_digest,
                                    transaction.malformed_ingress_reason),
        transaction.accepted->resolution_disposition,
        transaction.accepted->resolution_reason,
        transaction.accepted->stale_diagnostics,
        {StopScopeCaptureHistoryDTO{
            1U, transaction.accepted->resolution_disposition,
            transaction.accepted->resolution_reason, transaction.accepted->captured_scope_digest,
            transaction.accepted->joined_predecessor_stops,
            transaction.newly_fenced_operation_revisions, transaction.newly_captured_resources}},
        std::move(predecessors),
        std::move(entries),
        std::move(attempts),
        std::nullopt,
        latch_id,
        latch_generation,
        completed_at_ns,
        terminal_error};
    auto reduced = ReduceStopTerminal(reduction);
    // The reducer owns the authoritative nominal/escalated/partial outcome.
    // Pre-classification above avoids a second reduction in the common case,
    // while this exact retry closes outcomes (for example evidence-derived
    // partial containment) that cannot be guessed before reduction.
    if (!reduced.ok() && reduced.error().code == "stop_terminal_fault_latch_required" &&
        !latch_id.has_value() && assign_latch_identity()) {
      reduction.fault_latch_id = latch_id;
      reduction.fault_latch_generation = latch_generation;
      reduced = ReduceStopTerminal(reduction);
    }
    if (!reduced.ok()) {
      return false;
    }
    auto certificate = std::move(reduced.value());
    bool projection_capacity_available = false;
    for (std::size_t index = 0U; index < stop_projection_jobs_.size(); ++index) {
      if (!stop_projection_jobs_[index]->active &&
          stop_projection_generations_[index] != std::numeric_limits<std::uint64_t>::max()) {
        projection_capacity_available = true;
        break;
      }
    }
    std::optional<ErrorDTO> projection_capacity_error;
    if (!projection_capacity_available) {
      projection_capacity_error =
          ErrorDTO{"operation_journal_stop_terminal_projection_capacity_exhausted",
                   "Stop terminal forensic projection capacity is exhausted",
                   {}};
      transaction.persistence_degraded = true;
      motion_admission_closed_ = true;
      if (!assign_latch_identity()) {
        return false;
      }
    }
    const auto terminal_projection_status =
        projection_capacity_error.has_value()
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kFailed,
                                             projection_capacity_error, 1U}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                             std::nullopt};
    std::optional<StopFaultLatchDTO> latch;
    if (transaction.fault_latch.has_value() ||
        terminal_fault_required(certificate, transaction.persistence_degraded)) {
      if (!latch_id.has_value() || !latch_generation.has_value() ||
          !stop_retained_bundle_id_.has_value() || !stop_graph_revision_.has_value()) {
        return false;
      }
      const StopCertificateRefDTO reference{certificate.stop_certificate_id,
                                            certificate.stop_certificate_revision,
                                            certificate.stop_id, certificate.stop_generation};
      auto [operations, resources] = fault_latch_capture(certificate);
      auto trigger = primary_fault_trigger(certificate, transaction.persistence_degraded);
      if (transaction.fault_latch.has_value() &&
          fault_trigger_priority(transaction.fault_latch->trigger) >
              fault_trigger_priority(trigger))
        trigger = transaction.fault_latch->trigger;
      latch = StopFaultLatchDTO{
          config_.cell_id,
          certificate.stop_id,
          certificate.stop_generation,
          reference,
          *latch_id,
          *latch_generation,
          trigger,
          transaction.persistence_degraded,
          std::move(operations),
          std::move(resources),
          *stop_retained_bundle_id_,
          *stop_graph_revision_,
          terminal_projection_status,
          StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                         std::nullopt, std::nullopt},
          StopFaultLatchStateDTO::kLatched,
          transaction.fault_latch.has_value()
              ? transaction.fault_latch->recovery_assessment_revision + 1U
              : 1U,
          StopFaultRecoveryStatusDTO::kWaitingForObservation,
          fault_trigger_reason(trigger),
          {}};
      if (stop_fault_latch_field_error(*latch).has_value()) {
        return false;
      }
    }
    std::unique_ptr<StopSurveillanceReducerState> surveillance_state;
    if (latch.has_value() && stop_requires_surveillance(certificate, *latch)) {
      if (!config_.stop_surveillance.has_value()) {
        motion_admission_closed_ = true;
      } else {
        auto source_plans = stop_surveillance_source_plans(certificate);
        if (!source_plans.ok()) {
          motion_admission_closed_ = true;
        } else {
          auto seeded = MakeStopSurveillanceReducerState(
              config_.request_owner_epoch, 1U, certificate, *latch,
              config_.stop_surveillance->policy, std::move(source_plans.value()));
          if (!seeded.ok()) {
            return false;
          }
          surveillance_state =
              std::make_unique<StopSurveillanceReducerState>(std::move(seeded.value()));
        }
      }
    }
    const auto terminal_state = [&] {
      switch (certificate.result) {
        case StopCertificateResultDTO::kSucceededNominal:
        case StopCertificateResultDTO::kSucceededEscalated:
          return StopStatusStateDTO::kSucceeded;
        case StopCertificateResultDTO::kPartial:
          return StopStatusStateDTO::kPartial;
        case StopCertificateResultDTO::kFailedUncontained:
          return StopStatusStateDTO::kFailed;
      }
      return StopStatusStateDTO::kFailed;
    }();
    const auto acceptance_projection = [&] {
      switch (transaction.acceptance_projection_state) {
        case EmergencyStopProjectionStateDTO::kPending:
          return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending,
                                                std::nullopt, std::nullopt};
        case EmergencyStopProjectionStateDTO::kRecorded:
          return StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded,
                                                std::nullopt, std::nullopt};
        case EmergencyStopProjectionStateDTO::kFailed:
          return StopDurableProjectionStatusDTO{
              StopDurableProjectionStateDTO::kFailed,
              ErrorDTO{"operation_journal_stop_projection_failed",
                       "Stop acceptance forensic projection failed",
                       {}},
              1U};
      }
      return StopDurableProjectionStatusDTO{
          StopDurableProjectionStateDTO::kFailed,
          ErrorDTO{"stop_projection_state_invalid", "Stop projection state is invalid", {}}, 1U};
    }();
    const auto surveillance_projection =
        surveillance_state != nullptr
            ? StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                             std::nullopt}
            : StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kNotApplicable,
                                             std::nullopt, std::nullopt};
    StopStatusDTO status{*transaction.accepted,
                         terminal_state,
                         certificate.scope_entries,
                         {},
                         certificate.attempted_stage_results,
                         certificate.evidence_items,
                         certificate.unsettled,
                         certificate.failed,
                         acceptance_projection,
                         terminal_projection_status,
                         surveillance_projection,
                         StopCertificateRefDTO{certificate.stop_certificate_id,
                                               certificate.stop_certificate_revision,
                                               certificate.stop_id, certificate.stop_generation},
                         latch,
                         surveillance_state != nullptr
                             ? std::optional(surveillance_state->projection)
                             : std::nullopt};
    if (stop_terminal_projection_field_error(status, certificate, latch).has_value()) {
      return false;
    }
    if (stop_terminal_history_.size() == kMaximumResidentStopTerminalRecords) {
      const auto referenced_by_live_aggregate = [&](const StopTerminalRecord& record) {
        return std::any_of(
            stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
              return candidate != nullptr && candidate->active && !candidate->terminal &&
                     candidate->accepted.has_value() &&
                     std::any_of(candidate->accepted->joined_predecessor_stops.begin(),
                                 candidate->accepted->joined_predecessor_stops.end(),
                                 [&](const auto& predecessor) {
                                   return predecessor.predecessor_stop_id ==
                                              record.certificate.stop_id &&
                                          predecessor.predecessor_stop_generation ==
                                              record.certificate.stop_generation;
                                 });
            });
      };
      const auto compactable = std::find_if(
          stop_terminal_history_.begin(), stop_terminal_history_.end(), [&](const auto& record) {
            return record != nullptr &&
                   record->status.terminal_projection.state ==
                       StopDurableProjectionStateDTO::kRecorded &&
                   (!record->fault_latch.has_value() ||
                    record->fault_latch->state == StopFaultLatchStateDTO::kCleared) &&
                   (!record->status.surveillance.has_value() ||
                    record->status.surveillance->state ==
                        StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved) &&
                   !referenced_by_live_aggregate(*record);
          });
      if (compactable == stop_terminal_history_.end()) {
        return false;
      }
      stop_terminal_history_.erase(compactable);
    }
    std::optional<StopControlWorkToken> projection_token;
    if (projection_capacity_available) {
      projection_token = allocate_stop_projection_job_locked();
      if (!projection_token.has_value()) {
        return false;
      }
      auto& projection = *stop_projection_jobs_[projection_token->slot];
      projection.terminal_status = status;
      projection.terminal_certificate = certificate;
      projection.terminal_fault_latch = latch;
      projection.terminal_operation_states.reserve(transaction.operations.size());
      for (const auto& operation_id : transaction.operations) {
        auto* operation = find_active_operation_locked(operation_id);
        if (operation == nullptr || !terminal(operation->state.state)) {
          return false;
        }
        stamp_operation_capture_locked(*operation, operation->state);
        projection.terminal_operation_states.push_back(operation->state);
      }
    }
    std::int64_t first_surveillance_sample_ns = 0;
    std::int64_t first_surveillance_checkpoint_ns = 0;
    if (surveillance_state != nullptr) {
      const auto& policy = config_.stop_surveillance->policy;
      if (completed_at_ns > std::numeric_limits<std::int64_t>::max() - policy.sample_cadence_ns ||
          completed_at_ns >
              std::numeric_limits<std::int64_t>::max() - policy.checkpoint_interval_ns) {
        return false;
      }
      first_surveillance_sample_ns = completed_at_ns + policy.sample_cadence_ns;
      first_surveillance_checkpoint_ns = completed_at_ns + policy.checkpoint_interval_ns;
    }
    auto record = std::make_unique<StopTerminalRecord>(StopTerminalRecord{
        status, certificate, latch, projection_token, std::move(surveillance_state), std::nullopt,
        0U, false, std::nullopt, first_surveillance_sample_ns, first_surveillance_checkpoint_ns});
    transaction.certificate = certificate;
    transaction.fault_latch = latch;
    transaction.terminal_projection_token = projection_token;
    stop_terminal_history_.push_back(std::move(record));
    for (const auto& operation_id : transaction.operations) {
      auto* operation = find_active_operation_locked(operation_id);
      if (operation != nullptr && operation->state.active_stop_id == certificate.stop_id &&
          operation->state.active_stop_generation == certificate.stop_generation) {
        operation->settled_stop = ActiveOperation::SettledStop{
            certificate.stop_id, certificate.stop_generation, certificate.result};
      }
    }
    const bool program_run_fence_releasable =
        certificate.result == StopCertificateResultDTO::kSucceededNominal ||
        certificate.result == StopCertificateResultDTO::kSucceededEscalated;
    for (auto& fence : program_run_stop_fences_) {
      if (fence.stop_id == certificate.stop_id &&
          fence.stop_generation == certificate.stop_generation) {
        fence.releasable = program_run_fence_releasable;
        for (auto& state : fence.node_states) {
          if (!program_run_fence_releasable) {
            if (state != ProgramNodeHoldStateDTO::kFaulted) state = ProgramNodeHoldStateDTO::kFaulted;
          } else if (state == ProgramNodeHoldStateDTO::kStopping) {
            // Contained work waits for Resume; every other state is kept.
            state = ProgramNodeHoldStateDTO::kHeld;
          }
        }
      }
    }
    reduce_program_selection_stop_locked(certificate);
    if (latch.has_value()) {
      cell_stop_fault_latch_generation_ =
          std::max(cell_stop_fault_latch_generation_, *latch_generation);
      if (!is_active_stop_fault_latch(active_stop_fault_latch_) ||
          active_stop_fault_latch_->latch_generation <= *latch_generation)
        active_stop_fault_latch_ = *latch;
      motion_admission_closed_ = true;
    }
    static_cast<void>(
        emergency_stop_reserve_.release_after_terminal_handoff(transaction.stop_id->value()));
    return true;
  } catch (...) {
    return false;
  }
}

void ResourceAddressedExecutionKernel::ProgressStop(const StopControlWorkToken token) noexcept {
  try {
    struct PreparedCancellation final {
      OperationId operation_id;
      OperationParticipantId participant_id;
      OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
      IResolvedPreparedParticipantExecutionEngine* engine{};
      IResolvedParticipantExecutionEngine* execution_engine{};
      std::uint64_t execution_id{};
    };
    struct AddressedParticipantStopSettlement final {
      OperationId operation_id;
      OperationParticipantId participant_id;
      IResolvedParticipantExecutionEngine* engine{};
      AddressedParticipantStopSettlementRequestDTO request;
    };
    std::optional<std::int64_t> next_deadline;
    std::vector<StopControlWorkToken> dependents;
    std::vector<PreparedCancellation> prepared_cancellations;
    std::vector<PreparedCancellation> prepared_classifications;
    std::vector<AddressedParticipantStopSettlement> participant_settlements;
    std::vector<OperationControlEffectRequest> reset_effects;
    std::optional<StopControlWorkToken> terminal_projection;
    std::optional<std::pair<StopCertificateId, std::int64_t>> surveillance_schedule;
    bool transaction_became_terminal = false;
    {
      std::lock_guard state_lock(state_mutex_);
      if (!token.locally_valid() || token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        return;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token || transaction.terminal ||
          transaction.progress_in_flight || !transaction.stop_id.has_value()) {
        return;
      }
      transaction.progress_in_flight = true;
      const auto stop_id = *transaction.stop_id;
      const auto stop_generation = transaction.stop_generation;
      const auto now = clock_.now();
      const auto* ordinary_stop = std::get_if<StopIntentDTO>(&transaction.projected_intent);
      const bool runtime_collision_containment =
          ordinary_stop != nullptr &&
          ordinary_stop->reason == std::optional<std::string>{"runtime_collision_fault"} &&
          ordinary_stop->request_context.source_label == "kernel.runtime_collision";

      if (!transaction.participant_work_initialized) {
        transaction.participants.reserve(transaction.operations.size());
        for (const auto& operation_id : transaction.operations) {
          const auto* operation = find_active_operation_locked(operation_id);
          if (operation == nullptr ||
              operation->state.participants.size() != operation->participants.size()) {
            motion_admission_closed_ = true;
            continue;
          }
          const auto accepted_revision =
              std::lower_bound(transaction.accepted->affected_operation_revisions.begin(),
                               transaction.accepted->affected_operation_revisions.end(),
                               operation_id, [](const auto& affected, const OperationId& id) {
                                 return affected.operation_id < id;
                               });
          if (accepted_revision == transaction.accepted->affected_operation_revisions.end() ||
              accepted_revision->operation_id != operation_id) {
            motion_admission_closed_ = true;
            continue;
          }
          for (std::size_t index = 0U; index < operation->participants.size(); ++index) {
            const auto& active_participant = operation->participants[index];
            const auto& participant_state = operation->state.participants[index];
            if (!transaction.participant_ids.empty() &&
                !std::binary_search(transaction.participant_ids.begin(),
                                    transaction.participant_ids.end(),
                                    participant_state.participant_id)) {
              continue;
            }
            const auto& selected_active_stop_plan = runtime_collision_containment
                                                        ? active_participant.fault_stop_plan
                                                        : active_participant.ordinary_stop_plan;
            if (selected_active_stop_plan == nullptr ||
                active_participant.slot_owner.participant_id != participant_state.participant_id) {
              motion_admission_closed_ = true;
              continue;
            }
            const auto& stop_plan = *selected_active_stop_plan;
            const auto& stop_goal_id = participant_state.controller_goal_id;
            const auto stop_goal_generation = participant_state.controller_goal_generation;
            StopParticipantWork work{operation_id,
                                     accepted_revision->operation_revision_after_acceptance,
                                     active_participant.payload_kind,
                                     stop_goal_id,
                                     stop_goal_generation,
                                     active_participant.binding,
                                     active_participant.stop_readiness,
                                     active_participant.retained_default_stop_limits,
                                     active_participant.stop_node_category,
                                     stop_plan,
                                     active_participant.resources,
                                     active_participant.stop_execution_port,
                                     active_participant.prepared_motion_engine,
                                     active_participant.engine,
                                     active_participant.preparation_id,
                                     active_participant.stop_cancelled_pre_dispatch,
                                     false,
                                     false,
                                     false,
                                     false,
                                     false,
                                     0U,
                                     1U,
                                     std::nullopt,
                                     {},
                                     {},
                                     false,
                                     false,
                                     std::nullopt,
                                     false,
                                     false,
                                     std::nullopt,
                                     false};
            work.retained_results.reserve(kMaxStopEscalationStages * 2U);
            transaction.participants.push_back(std::move(work));
          }
        }
        transaction.participant_work_initialized = true;
      }

      for (auto& item : transaction.participants) {
        const auto retain_first_error = [&](ErrorDTO failure) {
          if (!item.terminal_error.has_value()) {
            item.terminal_error = std::move(failure);
          }
        };
        const auto advance_to_strongest = [&] {
          // The sealed chain is strength-ordered. Nominal failure/expiry goes
          // straight to its strongest authorized action, never another Soft
          // or Spring attempt. The original transaction budget is unchanged.
          const auto count = item.plan.stage_commands.size();
          item.current_stage_index = item.current_stage_index + 1U < count ? count - 1U : count;
        };
        if (item.terminal && item.terminal_projection_complete) {
          continue;
        }
        if (item.cancelled_pre_dispatch && !item.terminal) {
          bool preparation_in_flight = false;
          if (item.prepared_execution_id == 0U) {
            auto* operation = find_active_operation_locked(item.operation_id);
            ActiveOperationParticipant* participant = nullptr;
            if (operation != nullptr) {
              const auto found = std::find_if(
                  operation->participants.begin(), operation->participants.end(),
                  [&](const auto& candidate) {
                    return candidate.slot_owner.participant_id == item.plan.participant_id;
                  });
              if (found != operation->participants.end()) {
                participant = &*found;
              }
            }
            if (participant != nullptr) {
              preparation_in_flight =
                  participant->prepare_in_flight || participant->prepared_advance_in_flight;
              if (!preparation_in_flight) item.prepared_execution_id = participant->preparation_id;
              item.prepared_motion_engine = participant->prepared_motion_engine;
              item.execution_engine = participant->engine;
              if (participant->last_subordinate_error.has_value()) {
                retain_first_error(*participant->last_subordinate_error);
              }
            }
          }
          const bool needs_cancellation =
              item.prepared_motion_engine != nullptr && item.prepared_execution_id != 0U;
          if (needs_cancellation && !item.prepared_cancel_requested) {
            item.prepared_cancel_requested = true;
            prepared_cancellations.push_back({item.operation_id, item.plan.participant_id,
                                              item.payload_kind, item.prepared_motion_engine,
                                              item.execution_engine, item.prepared_execution_id});
          }
          const bool waiting =
              preparation_in_flight ||
              (needs_cancellation &&
               (!item.prepared_cancel_complete ||
                (item.payload_kind == OperationParticipantPayloadKind::kMotion &&
                 !item.prepared_cancel_terminal_observed &&
                 !item.unclassified_prepared_terminal.has_value())));
          if ((needs_cancellation || preparation_in_flight) &&
              now >= transaction.outer_deadline_ns) {
            // No effect crossed the fence, but cleanup did not settle in the
            // accepted budget. Keep its outstanding borrows; a late result
            // cannot rewrite the partial certificate or clear the fault.
            fail_prepared_cancellation(
                item, error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                            "Prepared cancellation did not settle before the Stop deadline",
                            "prepared_cancellation_deadline_expired"));
            latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kPartial);
          } else if (waiting) {
            next_deadline = !next_deadline.has_value()
                                ? transaction.outer_deadline_ns
                                : std::min(*next_deadline, transaction.outer_deadline_ns);
            continue;
          } else if (item.unclassified_prepared_terminal.has_value()) {
            if (item.prepared_motion_engine == nullptr || item.prepared_execution_id == 0U) {
              fail_prepared_cancellation(item, *item.unclassified_prepared_terminal);
              motion_admission_closed_ = true;
            } else {
              if (!item.prepared_classification_requested) {
                item.prepared_classification_requested = true;
                prepared_classifications.push_back(
                    {item.operation_id, item.plan.participant_id, item.payload_kind,
                     item.prepared_motion_engine, item.execution_engine,
                     item.prepared_execution_id});
              }
              next_deadline = !next_deadline.has_value()
                                  ? transaction.outer_deadline_ns
                                  : std::min(*next_deadline, transaction.outer_deadline_ns);
              continue;
            }
          } else {
            item.contained = true;
            item.terminal = true;
          }
        }

        const auto expire_attempt = [&](auto& attempt, const TimePoint observed_at) {
          if (!attempt.started || attempt.result_consumed || attempt.superseded ||
              observed_at < attempt.stage_deadline_ns) {
            return false;
          }
          attempt.superseded = true;
          if (attempt.handle.has_value()) {
            static_cast<void>(stop_stage_attempt_executor_->RequestCancel(*attempt.handle));
          }
          retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                   "Stop stage did not prove containment before its phase deadline",
                                   "participant_stop_stage_deadline_expired"));
          latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
          advance_to_strongest();
          return true;
        };
        for (auto& attempt : item.attempts) {
          if (!attempt.started || attempt.result_consumed || !attempt.handle.has_value()) {
            continue;
          }
          auto result = stop_stage_attempt_executor_->TryTake(*attempt.handle);
          const auto observed_at = clock_.now();
          // Deadline precedence applies even when a result is already queued.
          // Provider-reported elapsed time cannot backdate Kernel receipt.
          static_cast<void>(expire_attempt(attempt, observed_at));
          if (result.status == StopStageAttemptTakeStatus::kPending) {
            continue;
          }
          attempt.result_consumed = true;
          const auto released = stop_stage_attempt_executor_->Release(*attempt.handle);
          if (released != StopStageAttemptReleaseStatus::kReleased) {
            motion_admission_closed_ = true;
          }
          if (result.status == StopStageAttemptTakeStatus::kCompleted &&
              result.result.has_value()) {
            auto valid = ValidateParticipantStopStageResult(
                *result.result, item.readiness.value().resolved_stop_base_plan, item.binding,
                item.retained_default_stop_limits, item.node_category, item.resources);
            if (valid.ok()) {
              item.retained_results.push_back(StopAttemptedStageResultDTO{
                  StopScopeEntryKeyDTO{
                      stop_id, stop_generation,
                      StopParticipantScopeKeyDTO{item.operation_id, item.plan.participant_id}},
                  item.next_result_sequence++, observed_at,
                  attempt.superseded ? StopStageResultAuthorityDTO::kSupersededLate
                                     : StopStageResultAuthorityDTO::kAuthoritative,
                  std::move(*result.result)});
              if (!attempt.superseded) {
                const auto& retained = item.retained_results.back().result;
                if (retained.error.has_value()) {
                  retain_first_error(*retained.error);
                }
                if (retained.outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
                  item.contained = true;
                  item.terminal = true;
                  item.terminal_error.reset();
                } else {
                  latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
                  advance_to_strongest();
                }
              }
            } else {
              motion_admission_closed_ = true;
              if (!attempt.superseded) {
                retain_first_error(valid.error());
                latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
                advance_to_strongest();
              }
            }
          } else {
            motion_admission_closed_ = true;
            if (!attempt.superseded) {
              retain_first_error(error(std::string(to_string(ErrorCode::kInternalError)),
                                       "Stop-stage execution lane failed without a result",
                                       "stop_stage_attempt_infrastructure_failure"));
              latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
              advance_to_strongest();
            }
          }
        }

        while (!item.terminal) {
          if (item.current_stage_index >= item.plan.stage_commands.size()) {
            latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kFailedUncontained);
            item.terminal = true;
            item.contained = false;
            if (!item.terminal_error.has_value()) {
              item.terminal_error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                          "Stop cascade exhausted without proof of containment",
                                          "participant_stop_cascade_exhausted");
            }
            break;
          }
          auto& attempt = item.attempts[item.current_stage_index];
          if (attempt.started && !attempt.result_consumed) {
            if (expire_attempt(attempt, clock_.now())) continue;
            next_deadline = !next_deadline.has_value()
                                ? std::optional(attempt.stage_deadline_ns)
                                : std::min(*next_deadline, attempt.stage_deadline_ns);
            break;
          }
          if (attempt.started) {
            advance_to_strongest();
            continue;
          }
          const auto dispatch_now = clock_.now();
          auto budget = remaining_stop_budget(item.plan, stop_id, stop_generation, dispatch_now,
                                              transaction.outer_deadline_ns);
          const auto watchdog = stage_watchdog_deadline(
              item.plan.stage_commands[item.current_stage_index], budget, dispatch_now);
          if (!watchdog.has_value() || budget.remaining_outer_duration_ns <= 0) {
            retain_first_error(error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                                     "Stop stage command phase expired before dispatch",
                                     "participant_stop_stage_command_phase_expired"));
            latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
            advance_to_strongest();
            continue;
          }
          if (item.port == nullptr) {
            motion_admission_closed_ = true;
            retain_first_error(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                     "prepared Stop participant has no execution port",
                                     "participant_stop_execution_port_missing"));
            latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
            advance_to_strongest();
            continue;
          }
          auto stage = resolve_participant_stop_stage(item, stop_id, stop_generation,
                                                      item.current_stage_index, budget);
          if (!stage.ok()) {
            motion_admission_closed_ = true;
            retain_first_error(stage.error());
            latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kEscalated);
            advance_to_strongest();
            continue;
          }
          attempt.stage =
              std::make_shared<const ResolvedParticipantStopStageDTO>(std::move(stage.value()));
          attempt.budget = std::make_shared<const RemainingStopBudgetDTO>(std::move(budget));
          const auto started = stop_stage_attempt_executor_->TryStart(
              StopStageAttemptRequest{item.port, attempt.stage, attempt.budget});
          if (started.status != StopStageAttemptStartStatus::kStarted ||
              !started.handle.has_value()) {
            latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kFailedUncontained);
            motion_admission_closed_ = true;
            item.terminal = true;
            item.contained = false;
            item.terminal_error =
                error(std::string(to_string(ErrorCode::kInvariantViolation)),
                      "prequalified Stop-stage isolation could not accept prepared work",
                      "stop_stage_attempt_prequalification_failed");
            break;
          }
          attempt.started = true;
          attempt.handle = *started.handle;
          attempt.stage_deadline_ns = *watchdog;
          next_deadline = !next_deadline.has_value()
                              ? watchdog
                              : std::optional(std::min(*next_deadline, *watchdog));
          break;
        }

        if (!item.terminal) {
          continue;
        }
        auto* operation = find_active_operation_locked(item.operation_id);
        if (operation == nullptr || operation->state.active_stop_id != stop_id ||
            operation->state.active_stop_generation != stop_generation) {
          motion_admission_closed_ = true;
          continue;
        }
        const auto participant_state =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& candidate) {
                           return candidate.participant_id == item.plan.participant_id;
                         });
        const auto active_participant =
            std::find_if(operation->participants.begin(), operation->participants.end(),
                         [&](const auto& candidate) {
                           return candidate.slot_owner.participant_id == item.plan.participant_id;
                         });
        if (participant_state == operation->state.participants.end() ||
            active_participant == operation->participants.end()) {
          motion_admission_closed_ = true;
          continue;
        }
        const bool participant_settlement_required = !item.settlement_not_required();
        if (item.contained && participant_settlement_required &&
            !item.participant_settlement_complete) {
          if (item.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand &&
              active_participant->dispatch_effect_in_flight) {
            // Physical containment is already independent of ordinary dispatch.
            // Local cleanup needs the returned command identity, but must not
            // keep this Stop transaction alive beyond its existing deadline.
            // Retain the unresolved dispatch borrow and containment evidence.
            if (now < transaction.outer_deadline_ns) {
              next_deadline = !next_deadline.has_value()
                                  ? transaction.outer_deadline_ns
                                  : std::min(*next_deadline, transaction.outer_deadline_ns);
              continue;
            }
            item.participant_settlement_complete = true;
            item.participant_settlement_error =
                error(std::string(to_string(ErrorCode::kExecutionTimeout)),
                      "Contained command did not return before the Stop settlement deadline",
                      "participant_stop_dispatch_settlement_deadline_expired");
            motion_admission_closed_ = true;
          }
          if (!item.participant_settlement_complete && !item.participant_settlement_requested) {
            item.participant_settlement_requested = true;
            if (active_participant->engine == nullptr || item.prepared_execution_id == 0U) {
              item.participant_settlement_complete = true;
              item.participant_settlement_error =
                  error(std::string(to_string(ErrorCode::kInvariantViolation)),
                        "Stopped participant lost its local execution owner",
                        "participant_stop_settlement_authority_missing");
              motion_admission_closed_ = true;
            } else {
              std::optional<ParticipantStopStageResultDTO> settlement_evidence;
              if (item.cancelled_pre_dispatch) {
                auto budget = remaining_stop_budget(item.plan, stop_id, stop_generation, now,
                                                    transaction.outer_deadline_ns);
                auto stage =
                    resolve_participant_stop_stage(item, stop_id, stop_generation, 0U, budget);
                if (stage.ok()) {
                  settlement_evidence =
                      ParticipantStopStageResultDTO{std::move(stage.value()),
                                                    StopStageAttemptDispositionDTO::kNotDispatched,
                                                    ParticipantStopStageOutcomeDTO::kSucceeded,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    0,
                                                    {},
                                                    std::nullopt};
                } else {
                  item.participant_settlement_complete = true;
                  item.participant_settlement_error = stage.error();
                  motion_admission_closed_ = true;
                }
              } else {
                const auto containment = std::find_if(
                    item.retained_results.rbegin(), item.retained_results.rend(),
                    [](const auto& candidate) {
                      return candidate.authority == StopStageResultAuthorityDTO::kAuthoritative &&
                             candidate.result.outcome ==
                                 ParticipantStopStageOutcomeDTO::kSucceeded &&
                             !candidate.result.error.has_value();
                    });
                if (containment != item.retained_results.rend()) {
                  settlement_evidence = containment->result;
                }
              }
              if (!item.participant_settlement_complete && !settlement_evidence.has_value()) {
                item.participant_settlement_complete = true;
                item.participant_settlement_error =
                    error(std::string(to_string(ErrorCode::kInvariantViolation)),
                          "Contained participant has no exact containment result",
                          "participant_stop_containment_result_missing");
                motion_admission_closed_ = true;
              } else if (settlement_evidence.has_value()) {
                participant_settlements.push_back(AddressedParticipantStopSettlement{
                    item.operation_id, item.plan.participant_id, active_participant->engine,
                    AddressedParticipantStopSettlementRequestDTO{
                        item.prepared_execution_id,
                        StopCertificateRefDTO{
                            StopCertificateId("stop-certificate." + stop_id.value()),
                            StopCertificateRevision(1U), stop_id, stop_generation},
                        std::move(*settlement_evidence), item.payload_kind,
                        item.cancelled_pre_dispatch}});
              }
            }
          }
          if (!item.participant_settlement_complete) {
            continue;
          }
        }
        // A failed preparation keeps its owner's failure terminal and recovery
        // authority; containment had nothing physical to stop.
        const bool participant_settled =
            item.contained && !item.preparation_failed &&
            (!participant_settlement_required || (item.participant_settlement_complete &&
                                                  !item.participant_settlement_error.has_value()));
        participant_state->state = participant_settled ? OperationStateKind::kStopped
                                                       : OperationStateKind::kInterventionRequired;
        if (!participant_settled && !participant_state->terminal_error.has_value()) {
          participant_state->terminal_error = item.participant_settlement_error.has_value()
                                                  ? item.participant_settlement_error
                                                  : item.terminal_error;
        } else if (runtime_collision_containment &&
                   !participant_state->terminal_error.has_value()) {
          participant_state->terminal_error = active_participant->last_subordinate_error;
        }
        const bool all_participants_terminal =
            std::all_of(operation->state.participants.begin(), operation->state.participants.end(),
                        [](const auto& candidate) { return terminal(candidate.state); });
        if (all_participants_terminal) {
          operation->state.state =
              std::all_of(operation->state.participants.begin(),
                          operation->state.participants.end(),
                          [](const auto& candidate) {
                            return candidate.state == OperationStateKind::kStopped;
                          })
                  ? OperationStateKind::kStopped
                  : OperationStateKind::kInterventionRequired;
        }
        operation->state.updated_at_ns = now;
        // A production Stop backend can publish its terminal state before the
        // isolated Stop-stage result is consumed.  That callback already
        // terminalizes the same resource slots under state_mutex_.  Treat the
        // subsequent certificate commit as the same terminal fact instead of
        // attempting the illegal Terminal -> Terminal transition and masking
        // the original Stop outcome as an intervention.
        if (active_participant->slot_state != ResourceSlotState::kTerminal) {
          const auto transitioned = resource_slots_->transition(active_participant->slot_owner,
                                                                active_participant->resources,
                                                                ResourceSlotState::kTerminal);
          if (transitioned.ok()) {
            active_participant->slot_state = ResourceSlotState::kTerminal;
          } else {
            motion_admission_closed_ = true;
            operation->state.state = OperationStateKind::kInterventionRequired;
            participant_state->state = OperationStateKind::kInterventionRequired;
            participant_state->terminal_error = transitioned.error();
          }
        }
        item.terminal_projection_complete = true;
      }

      for (const auto& operation_id : transaction.operations) {
        auto* operation = find_active_operation_locked(operation_id);
        if (operation != nullptr &&
            std::all_of(transaction.participants.begin(), transaction.participants.end(),
                        [&](const auto& participant) {
                          return participant.operation_id != operation_id ||
                                 (participant.terminal &&
                                  participant.terminal_projection_complete &&
                                  (!participant.contained ||
                                   participant.settlement_not_required() ||
                                   participant.participant_settlement_complete));
                        })) {
          operation->stop_effect_in_flight = false;
        }
      }

      transaction.fresh_scope_complete =
          std::all_of(transaction.participants.begin(), transaction.participants.end(),
                      [](const auto& participant) {
                        return participant.terminal && participant.terminal_projection_complete &&
                               (!participant.contained ||
                                participant.settlement_not_required() ||
                                participant.participant_settlement_complete);
                      });
      const auto expected_participant_count = static_cast<std::size_t>(std::count_if(
          transaction.accepted->participant_budget_allocations.begin(),
          transaction.accepted->participant_budget_allocations.end(), [&](const auto& budget) {
            return budget.origin_stop_id == stop_id &&
                   budget.origin_stop_generation == stop_generation;
          }));
      if (transaction.participants.size() != expected_participant_count) {
        transaction.fresh_scope_complete = false;
        motion_admission_closed_ = true;
      }
      if (transaction.fresh_scope_complete) {
        const bool predecessors_terminal = std::all_of(
            transaction.predecessor_tokens.begin(), transaction.predecessor_tokens.end(),
            [&](const auto& predecessor_token) {
              return predecessor_token.slot < stop_transactions_.size() &&
                     stop_transactions_[predecessor_token.slot]->active &&
                     stop_transactions_[predecessor_token.slot]->token == predecessor_token &&
                     stop_transactions_[predecessor_token.slot]->certificate.has_value();
            });
        transaction_became_terminal = transaction.fresh_scope_complete && predecessors_terminal;
        if (transaction_became_terminal) {
          // The immutable local certificate is the terminal handoff boundary.
          // Do not expose terminal state, release the emergency reserve slot,
          // or wake dependent aggregates until the complete predecessor and
          // fresh-scope truth has been reduced and retained locally.
          if (!commit_stop_terminal_locked(transaction, now)) {
            transaction.progress_in_flight = false;
            motion_admission_closed_ = true;
            return;
          }
          transaction.terminal = true;
          const auto retain_terminal_capture = [&](const OperationStateDTO& capture) {
            if (!transaction.terminal_projection_token.has_value()) return;
            auto& states = stop_projection_jobs_[transaction.terminal_projection_token->slot]
                               ->terminal_operation_states;
            const auto row = std::find_if(states.begin(), states.end(), [&](const auto& item) {
              return item.operation_id == capture.operation_id;
            });
            if (row != states.end()) *row = capture;
          };
          const bool containment_succeeded =
              transaction.certificate->result == StopCertificateResultDTO::kSucceededNominal ||
              transaction.certificate->result == StopCertificateResultDTO::kSucceededEscalated;
          for (const auto& operation_id : transaction.operations) {
            auto* operation = find_active_operation_locked(operation_id);
            if (operation == nullptr || !operation->pending_control.has_value() ||
                !operation->pending_control->durable) {
              continue;
            }
            auto& pending = *operation->pending_control;
            if (pending.accepted.control != OperationControlKind::kReset) {
              if (pending.first_failure.has_value()) {
                retain_terminal_capture(capture_control_failure_locked(*operation));
              }
              if (pending.completed_effects != pending.expected_effects) {
                // Retain the exact control claim until the bounded control
                // lane reports completion.
                continue;
              }
              operation->pending_control.reset();
              continue;
            }
            if (pending.effects_dispatched) {
              // A Reset already executing when this Stop arrived remains one
              // effect. Either its callback or this certificate handoff is
              // last; only the latter may release Stop-owned resources.
              if (pending.completed_effects == pending.expected_effects) {
                if (pending.first_failure.has_value()) {
                  const auto fault = publish_settlement_fault_locked(*operation);
                  // Reuse this Stop's already-reserved durable projection;
                  // never perform ordinary fault I/O on the Stop worker.
                  retain_terminal_capture(fault);
                } else if (!settle_control_operation_locked(operation_id,
                                                            OperationControlKind::kReset)
                                .ok()) {
                  motion_admission_closed_ = true;
                }
              }
              continue;
            }
            if (!containment_succeeded) {
              operation->state.state = OperationStateKind::kInterventionRequired;
              operation->state.updated_at_ns = now;
              operation->pending_control.reset();
              motion_admission_closed_ = true;
              continue;
            }
            auto requests =
                reset_effect_requests(operation_id, operation->participants,
                                      pending.accepted.operation_revision_after_acceptance);
            if (!requests.ok() || pending.completed_effects != 0U) {
              operation->state.state = OperationStateKind::kInterventionRequired;
              operation->state.updated_at_ns = now;
              operation->pending_control.reset();
              motion_admission_closed_ = true;
              continue;
            }
            pending.superseded_by_stop = false;
            // Stop may have cancelled a motion prepared after Reset admission.
            // Its inner execution now needs cleanup too. Freeze this bounded
            // effect set at dispatch, not before containment knows the set.
            pending.expected_effects = requests.value().size();
            pending.effects_dispatched = true;
            if (requests.value().empty()) {
              if (!settle_control_operation_locked(operation_id, OperationControlKind::kReset)
                       .ok()) {
                motion_admission_closed_ = true;
              }
              continue;
            }
            reset_effects.insert(reset_effects.end(),
                                 std::make_move_iterator(requests.value().begin()),
                                 std::make_move_iterator(requests.value().end()));
          }
          if (transaction.acceptance_projection_state !=
              EmergencyStopProjectionStateDTO::kPending) {
            terminal_projection = transaction.terminal_projection_token;
          }
          const auto terminal_record = std::find_if(
              stop_terminal_history_.begin(), stop_terminal_history_.end(),
              [&](const auto& record) {
                return record != nullptr &&
                       record->certificate.stop_id == transaction.certificate->stop_id &&
                       record->certificate.stop_generation ==
                           transaction.certificate->stop_generation;
              });
          if (terminal_record != stop_terminal_history_.end() &&
              (*terminal_record)->surveillance_state != nullptr) {
            surveillance_schedule = std::pair((*terminal_record)->certificate.stop_certificate_id,
                                              (*terminal_record)->next_surveillance_sample_ns);
          }
          for (const auto& candidate : stop_transactions_) {
            if (candidate == nullptr || !candidate->active || candidate->terminal) {
              continue;
            }
            const auto predecessor = std::find(candidate->predecessor_tokens.begin(),
                                               candidate->predecessor_tokens.end(), token);
            if (predecessor != candidate->predecessor_tokens.end()) {
              candidate->predecessor_certificates.push_back(*transaction.certificate);
              std::sort(
                  candidate->predecessor_certificates.begin(),
                  candidate->predecessor_certificates.end(),
                  [](const auto& left, const auto& right) { return left.stop_id < right.stop_id; });
              candidate->predecessor_tokens.erase(predecessor);
              stop_control_executor_->ArmDeadline(candidate->token, now);
              dependents.push_back(candidate->token);
            }
          }
        }
      }
      transaction.progress_in_flight = false;
      if (transaction_became_terminal) {
        stop_control_executor_->CancelDeadline(token);
      } else if (next_deadline.has_value()) {
        stop_control_executor_->ArmDeadline(token, *next_deadline);
      }
    }
    for (const auto& handoff : participant_settlements) {
      Result<AddressedParticipantStopSettlementDTO> settled =
          Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInternalError)),
                    "addressed motion Stop settlement did not return a result",
                    "participant_stop_settlement_failed"));
      try {
        settled = handoff.engine->settle_addressed_participant_stop(handoff.request);
      } catch (const std::exception& exception) {
        settled = Result<AddressedParticipantStopSettlementDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  std::string("addressed motion Stop settlement threw: ") + exception.what(),
                  "participant_stop_settlement_threw"));
      } catch (...) {
        settled = Result<AddressedParticipantStopSettlementDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "addressed motion Stop settlement threw", "participant_stop_settlement_threw"));
      }
      if (settled.ok()) {
        const auto& value = settled.value();
        const auto& request = handoff.request;
        const auto& stage = request.containment.request;
        const auto& recovery = value.recovery_assessment;
        const bool exact = value.execution_id == request.execution_id &&
                           value.aggregate_stop_certificate == request.aggregate_stop_certificate &&
                           value.operation_id == stage.operation_id &&
                           value.participant_id == stage.participant_id &&
                           value.controller_goal_id == stage.controller_goal_id &&
                           value.controller_goal_generation == stage.controller_goal_generation &&
                           value.terminal_state == ExecutionState::kStopped &&
                           recovery.execution_id == request.execution_id &&
                           recovery.execution_state == ExecutionState::kStopped &&
                           recovery.recovery_class == RecoveryClass::kOrdinaryOperatorStop &&
                           recovery.status == RecoveryAssessmentStatus::kRetryReady &&
                           recovery.retry_admissible && recovery.backend_stop_confirmed &&
                           recovery.stop_owner_generation != 0U &&
                           recovery.stop_certificate_id ==
                               request.aggregate_stop_certificate.stop_certificate_id.value() &&
                           recovery.stop_certificate_revision ==
                               request.aggregate_stop_certificate.stop_certificate_revision.value();
        if (!exact) {
          settled = Result<AddressedParticipantStopSettlementDTO>::Err(
              error(std::string(to_string(ErrorCode::kInvariantViolation)),
                    "participant returned a mismatched addressed Stop settlement",
                    "participant_stop_settlement_mismatch"));
        }
      }
      std::lock_guard state_lock(state_mutex_);
      if (token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        continue;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token) {
        continue;
      }
      const auto item =
          std::find_if(transaction.participants.begin(), transaction.participants.end(),
                       [&](const auto& candidate) {
                         return candidate.operation_id == handoff.operation_id &&
                                candidate.plan.participant_id == handoff.participant_id;
                       });
      if (item == transaction.participants.end() ||
          item->prepared_execution_id != handoff.request.execution_id ||
          !item->participant_settlement_requested) {
        motion_admission_closed_ = true;
        continue;
      }
      item->participant_settlement_complete = true;
      if (!settled.ok()) {
        item->participant_settlement_error = settled.error();
        motion_admission_closed_ = true;
      }
    }
    for (const auto& cancellation : prepared_cancellations) {
      std::optional<ExecutionStatus> current;
      Result<void> cancelled = Result<void>::Err(error(
          std::string(to_string(ErrorCode::kInternalError)),
          "prepared motion cancellation did not return a result", "prepared_motion_cancel_failed"));
      try {
        cancelled =
            cancellation.payload_kind == OperationParticipantPayloadKind::kCapabilityCommand
                ? cancellation.engine->CancelPreparedCapabilityCommand(cancellation.execution_id)
                : cancellation.engine->CancelPreparedMotion(cancellation.execution_id);
        if (cancelled.ok() &&
            cancellation.payload_kind == OperationParticipantPayloadKind::kMotion) {
          // Acceptance can fence and settle the worker before participant
          // work exists. Idempotent cancellation need not repeat that event.
          // Read the existing local status outside the owner lock; later
          // terminals still use the subscribed event wakeup. No provider I/O.
          current = cancellation.execution_engine->get_status();
        }
      } catch (const std::exception& exception) {
        cancelled = Result<void>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  std::string("prepared motion cancellation threw: ") + exception.what(),
                  "prepared_motion_cancel_threw"));
      } catch (...) {
        cancelled = Result<void>::Err(error(std::string(to_string(ErrorCode::kInvariantViolation)),
                                            "prepared motion cancellation threw",
                                            "prepared_motion_cancel_threw"));
      }
      {
        std::lock_guard state_lock(state_mutex_);
        if (token.slot >= stop_transactions_.size()) {
          motion_admission_closed_ = true;
          continue;
        }
        auto& transaction = *stop_transactions_[token.slot];
        if (!transaction.active || transaction.token != token) {
          continue;
        }
        const auto item =
            std::find_if(transaction.participants.begin(), transaction.participants.end(),
                         [&](const auto& candidate) {
                           return candidate.operation_id == cancellation.operation_id &&
                                  candidate.plan.participant_id == cancellation.participant_id;
                         });
        if (item == transaction.participants.end() ||
            item->prepared_execution_id != cancellation.execution_id) {
          motion_admission_closed_ = true;
          continue;
        }
        item->prepared_cancel_complete = true;
        if (!cancelled.ok()) {
          fail_prepared_cancellation(*item, cancelled.error());
          latch_stop_fault_locked(transaction, StopFaultTriggerDTO::kPartial);
        } else if (item->payload_kind == OperationParticipantPayloadKind::kCapabilityCommand) {
          // Capability preparation has no asynchronous worker terminal: the
          // successful cancellation call is its complete local no-effect
          // transition. Motion publishes the corresponding engine event.
          item->prepared_cancel_terminal_observed = true;
        } else if (current.has_value()) {
          observe_prepared_cancellation_terminal(*item, current->execution_id,
                                                 operation_state(current->state),
                                                 status_error(current->last_error));
          if (item->terminal_error.has_value()) motion_admission_closed_ = true;
        }
      }
    }
    for (const auto& classification : prepared_classifications) {
      // Read only after a failure terminal was observed. The prepared owner's
      // exact snapshot proves an already-failed, never-dispatched preparation;
      // anything else keeps the failed-cancellation classification.
      std::optional<ResolvedPreparedMotionExecutionSnapshotDTO> prepared;
      try {
        auto snapshot = classification.engine->ObservePreparedMotion(classification.execution_id);
        if (snapshot.ok()) prepared = std::move(snapshot.value());
      } catch (...) {
        prepared.reset();
      }
      std::lock_guard state_lock(state_mutex_);
      if (token.slot >= stop_transactions_.size()) {
        motion_admission_closed_ = true;
        continue;
      }
      auto& transaction = *stop_transactions_[token.slot];
      if (!transaction.active || transaction.token != token) continue;
      const auto item =
          std::find_if(transaction.participants.begin(), transaction.participants.end(),
                       [&](const auto& candidate) {
                         return candidate.operation_id == classification.operation_id &&
                                candidate.plan.participant_id == classification.participant_id;
                       });
      if (item == transaction.participants.end() ||
          item->prepared_execution_id != classification.execution_id ||
          !item->unclassified_prepared_terminal.has_value()) {
        motion_admission_closed_ = true;
        continue;
      }
      if (prepared.has_value() && prepared->execution_id == classification.execution_id &&
          prepared->state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed &&
          !prepared->last_dispatched_participant) {
        settle_failed_preparation(
            *item, prepared->error.value_or(*item->unclassified_prepared_terminal));
      } else {
        fail_prepared_cancellation(*item, *item->unclassified_prepared_terminal);
        motion_admission_closed_ = true;
      }
    }
    if (!prepared_cancellations.empty() || !prepared_classifications.empty() ||
        !participant_settlements.empty()) {
      // The executor coalesces identical token publications. Finish the
      // bounded, effect-free cancellation pass directly after releasing the
      // state lock so this same callback can commit terminal no-effect truth
      // without waiting for an artificial watchdog generation.
      ProgressStop(token);
      return;
    }
    for (auto& effect : reset_effects) {
      const auto identity = effect;
      auto started = operation_control_executor_->TryStart(std::move(effect));
      if (started.status == OperationControlEffectStartStatus::kStarted) {
        continue;
      }
      {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
      CompleteOperationControlEffect(OperationControlEffectResult{
          identity.operation_id, identity.participant_id, identity.operation_revision,
          identity.control, false,
          error(std::string(to_string(ErrorCode::kInvariantViolation)),
                "prequalified operation-control lane rejected certified Reset work",
                "operation_control_effect_handoff_failed")});
    }
    if (transaction_became_terminal) {
      if (terminal_projection.has_value()) {
        stop_projection_worker_->Publish(*terminal_projection);
      }
      for (const auto& dependent : dependents) {
        stop_control_executor_->Publish(dependent);
      }
      if (surveillance_schedule.has_value()) {
        ScheduleStopSurveillance(std::move(surveillance_schedule->first),
                                 surveillance_schedule->second);
      }
    }
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    motion_admission_closed_ = true;
    if (token.locally_valid() && token.slot < stop_transactions_.size()) {
      auto& transaction = *stop_transactions_[token.slot];
      if (transaction.active && transaction.token == token) {
        transaction.progress_in_flight = false;
      }
    }
  }
}

Result<RuntimeScaleAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale(
    const RuntimeScaleRequestDTO& request) {
  return set_runtime_scale_impl(request, true);
}

Result<RuntimeScaleAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale_impl(
    const RuntimeScaleRequestDTO& request, const bool retain_public_replay) {
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.operation_id.empty() ||
      !request.expected_operation_revision.nonzero() || !std::isfinite(request.scale) ||
      request.scale < 0.0 || request.scale > 1.0 || !request.attribution.locally_valid()) {
    return reject<RuntimeScaleAcceptedDTO>("runtime-scale request is malformed",
                                           "runtime_scale_request_invalid",
                                           ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<RuntimeScaleAcceptedDTO>::Err(
        error("stale_request_owner_epoch", "runtime-scale request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  struct ScaleEffectTarget final {
    IResolvedParticipantExecutionEngine* engine{};
    ResolvedRuntimeScaleCommandDTO command;
  };
  std::vector<ScaleEffectTarget> effect_targets;
  std::shared_ptr<detail::CoordinatedExecutionClock> common_clock;
  const auto shared_participants_ready = [&](const ActiveOperation& operation) {
    const auto now = clock_.now();
    const auto& rows = operation.runtime_scale_status->participants;
    const auto expected_state = operation.state.state;
    if (expected_state != OperationStateKind::kExecuting) return false;
    if (rows.size() != operation.participants.size()) return false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
      const auto& row = rows[index];
      const auto freshness = operation.participants[index]
                                 .binding.value()
                                 .control_path->runtime_scale_qualification->freshness_budget_ns;
      if (operation.state.participants[index].state != expected_state || !row.supported ||
          row.error || row.acknowledgement_sequence == 0U || row.acknowledgement_timestamp_ns < 0 ||
          row.acknowledgement_timestamp_ns > now ||
          now - row.acknowledgement_timestamp_ns > freshness)
        return false;
    }
    return true;
  };
  RuntimeScaleAcceptedDTO accepted{request.request_context.request_id,
                                   RequestReplayStatusDTO::kAcceptedNew,
                                   request.operation_id,
                                   request.expected_operation_revision,
                                   request.scale,
                                   RuntimeScaleGeneration(0U),
                                   0};
  {
    OperationCommitGuard commit(*this, request.operation_id);
    std::unique_lock state_lock(state_mutex_);
    if (emergency_stop_reserve_.find(request.request_context.request_id.value()).has_value()) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict", "runtime-scale request ID is retained by emergency Stop",
                "request_id_conflict"));
    }
    if (retain_public_replay) {
      const auto replay = std::find_if(
          runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
          [&](const RuntimeScaleReplayEntry& entry) {
            return entry.request.request_context.request_id == request.request_context.request_id;
          });
      if (replay != runtime_scale_replay_ledger_.end()) {
        if (replay->request != request) {
          return Result<RuntimeScaleAcceptedDTO>::Err(error(
              "request_id_conflict", "runtime-scale request ID was reused with different intent",
              "request_id_conflict"));
        }
        auto replayed = replay->accepted;
        replayed.replay_status = RequestReplayStatusDTO::kReplayedExact;
        return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(replayed));
      }
    }
    const auto request_id = request.request_context.request_id;
    const bool used_elsewhere =
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(
            stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
            [&](const StopReplayEntry& entry) { return entry.request_id() == request_id; }) ||
        std::any_of(runtime_scale_control_replay_ledger_.begin(),
                    runtime_scale_control_replay_ledger_.end(),
                    [&](const RuntimeScaleControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    });
    if (used_elsewhere) {
      return Result<RuntimeScaleAcceptedDTO>::Err(
          error("request_id_conflict",
                "runtime-scale request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale names a non-resident operation",
                                             "operation_not_resident",
                                             ErrorCode::kMissingReference);
    }
    if (active_operation->state.operation_revision != request.expected_operation_revision) {
      return reject<RuntimeScaleAcceptedDTO>("runtime scale expected operation revision is stale",
                                             "operation_revision_stale");
    }
    if (active_operation->participants.size() != 1U) {
      if (active_operation->common_timing)
        common_clock = active_operation->common_timing->execution_clock();
      if (!common_clock || !common_clock->sample_now() ||
          active_operation->state.state != OperationStateKind::kExecuting)
        return reject<RuntimeScaleAcceptedDTO>(
            "coordinated runtime scaling requires a released shared execution clock",
            "coordinated_runtime_scale_not_qualified", ErrorCode::kUnsupportedCapability);
    }
    if (!active_operation->runtime_scale_status.has_value() ||
        std::any_of(active_operation->participants.begin(), active_operation->participants.end(),
                    [](const auto& participant) {
                      return participant.payload_kind != OperationParticipantPayloadKind::kMotion ||
                             !participant.binding.value().control_path ||
                             !participant.binding.value().control_path->runtime_scale_qualification;
                    })) {
      return reject<RuntimeScaleAcceptedDTO>(
          "the exact operation binding has no qualified runtime scaling",
          "runtime_scale_unsupported", ErrorCode::kUnsupportedCapability);
    }
    if (common_clock && !shared_participants_ready(*active_operation))
      return reject<RuntimeScaleAcceptedDTO>("a coordinated participant is not ready for scaling",
                                             "coordinated_runtime_scale_not_ready");
    if (active_operation->pending_control.has_value() ||
        active_operation->runtime_scale_effect_in_flight ||
        active_operation->recovery_effect_in_flight) {
      return reject<RuntimeScaleAcceptedDTO>("another operation effect is already being admitted",
                                             "operation_control_busy");
    }
    const auto current_generation =
        active_operation->runtime_scale_status->runtime_scale_generation;
    if (current_generation != request.expected_runtime_scale_generation) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation is stale",
                                             "runtime_scale_generation_stale");
    }
    const auto state = active_operation->state.state;
    if (state != OperationStateKind::kPlanning && state != OperationStateKind::kValidating &&
        state != OperationStateKind::kPrepared && state != OperationStateKind::kDispatching &&
        state != OperationStateKind::kExecuting) {
      return reject<RuntimeScaleAcceptedDTO>(
          "runtime scale is invalid for the current operation state", "runtime_scale_state_invalid",
          ErrorCode::kInvalidStateTransition);
    }
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleAcceptedDTO>("runtime-scale generation space is exhausted",
                                             "runtime_scale_generation_exhausted",
                                             ErrorCode::kInternalError);
    }

    const RuntimeScaleGeneration next_generation(current_generation.value() + 1U);
    accepted = RuntimeScaleAcceptedDTO{request.request_context.request_id,
                                       RequestReplayStatusDTO::kAcceptedNew,
                                       request.operation_id,
                                       request.expected_operation_revision,
                                       request.scale,
                                       next_generation,
                                       clock_.now()};
    auto accepted_status = *active_operation->runtime_scale_status;
    accepted_status.runtime_scale_generation = next_generation;
    accepted_status.requested_scale = request.scale;
    accepted_status.state = RuntimeScaleApplicationState::kPending;
    accepted_status.updated_at_ns = accepted.accepted_at_ns;
    for (auto& participant : accepted_status.participants) {
      participant.accepted_generation = next_generation;
      participant.commanded_scale = request.scale;
      participant.state = RuntimeScaleApplicationState::kPending;
      participant.error.reset();
    }
    auto accepted_state = active_operation->state;
    const auto prior_state = active_operation->state;
    accepted_state.runtime_scale_generation = next_generation;
    accepted_state.updated_at_ns = accepted.accepted_at_ns;
    stamp_operation_capture_locked(*active_operation, accepted_state);
    state_lock.unlock();
    auto durable_acceptance = attempt_journal_write([&] {
      return journal_.RecordRuntimeScaleAccepted(config_.request_owner_epoch, request, accepted,
                                                 accepted_state, accepted_status);
    });
    state_lock.lock();
    if (!durable_acceptance.ok()) {
      return Result<RuntimeScaleAcceptedDTO>::Err(durable_acceptance.error());
    }
    if (retain_public_replay) {
      std::erase_if(runtime_scale_replay_ledger_, [&](const RuntimeScaleReplayEntry& entry) {
        return entry.accepted.operation_id == request.operation_id;
      });
      runtime_scale_replay_ledger_.push_back(RuntimeScaleReplayEntry{request, accepted});
    }
    active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr ||
        active_operation->state.journal_capture_order != prior_state.journal_capture_order ||
        active_operation->state.operation_revision != prior_state.operation_revision ||
        active_operation->state.active_stop_id != prior_state.active_stop_id)
      return Result<RuntimeScaleAcceptedDTO>::Ok(accepted);
    const bool shared_ready = !common_clock || shared_participants_ready(*active_operation);
    active_operation->state = std::move(accepted_state);
    active_operation->runtime_scale_status = std::move(accepted_status);
    if (common_clock && active_operation->state.state == OperationStateKind::kExecuting) {
      // One immutable-law publication, not a loop over effectful participant
      // setters. Stop can freeze this occurrence; it is never reopened here.
      if (!shared_ready ||
          !common_clock->retarget(
              request.scale,
              CoordinatedRuntimeScaleCommandDTO{
                  request.operation_id, request.expected_operation_revision, next_generation})) {
        auto failed = *active_operation->runtime_scale_status;
        failed.state = RuntimeScaleApplicationState::kFailed;
        for (auto& participant : failed.participants) {
          participant.state = RuntimeScaleApplicationState::kFailed;
          participant.error =
              error(std::string(to_string(ErrorCode::kExecutionRejected)),
                    shared_ready ? "shared runtime-scale law rejected the accepted target"
                                 : "shared scaling evidence changed or expired before publication",
                    shared_ready ? "coordinated_runtime_scale_effect_rejected"
                                 : "coordinated_runtime_scale_not_ready");
        }
        (void)retain_runtime_scale_observation_locked(
            state_lock, *active_operation, std::move(failed), "runtime_scale_effect_rejected");
      }
      // Existing participant observation supplies applied evidence; publication
      // of a common target alone must never claim that either robot applied it.
      return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(accepted));
    }
    effect_targets.reserve(active_operation->participants.size());
    for (std::size_t index = 0U; index < active_operation->participants.size(); ++index) {
      const auto& current_participant = active_operation->participants[index];
      const auto& binding = current_participant.binding.value();
      const auto& control = *binding.control_path;
      const auto& qualification = *control.runtime_scale_qualification;
      const auto& operation_participant = active_operation->state.participants[index];
      effect_targets.push_back(ScaleEffectTarget{
          current_participant.engine,
          ResolvedRuntimeScaleCommandDTO{request.operation_id,
                                         request.expected_operation_revision,
                                         operation_participant.participant_id,
                                         binding.controllable_component_id,
                                         binding.command_target_id,
                                         binding.capability_id,
                                         control.control_domain_id,
                                         control.control_domain_revision,
                                         control.control_binding_id,
                                         control.control_binding_revision,
                                         operation_participant.controller_goal_id,
                                         operation_participant.controller_goal_generation,
                                         control.controller_topology_id,
                                         control.topology_revision,
                                         control.control_route_id,
                                         qualification.scaling_domain_id,
                                         qualification.coordinated_barrier_id,
                                         current_participant.resources,
                                         control.active_control_configuration_id,
                                         control.active_control_configuration_generation,
                                         current_generation,
                                         next_generation,
                                         request.scale}});
      if (!common_clock) break;
    }
    active_operation->runtime_scale_effect_in_flight = true;
  }

  const auto mark_failed = [&](IResolvedParticipantExecutionEngine* engine, ErrorDTO failure,
                               const char* reason) {
    OperationCommitGuard commit(*this, request.operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation == nullptr || !active_operation->runtime_scale_status.has_value() ||
        active_operation->state.operation_revision != request.expected_operation_revision ||
        active_operation->runtime_scale_status->runtime_scale_generation !=
            accepted.runtime_scale_generation)
      return;
    auto status = *active_operation->runtime_scale_status;
    status.state = RuntimeScaleApplicationState::kFailed;
    status.updated_at_ns = clock_.now();
    const auto participant = active_operation->participant_for(engine);
    const auto row = std::find_if(
        status.participants.begin(), status.participants.end(), [&](const auto& candidate) {
          return participant != nullptr &&
                 candidate.participant_id == participant->slot_owner.participant_id;
        });
    if (row != status.participants.end()) {
      row->state = RuntimeScaleApplicationState::kFailed;
      row->error = std::move(failure);
    }
    (void)retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                  reason);
  };
  for (const auto& target : effect_targets) {
    bool outcome_unknown = false;
    const auto commanded = [&]() -> Result<ResolvedRuntimeScaleAcceptedDTO> {
      try {
        return target.engine->set_runtime_scale(target.command);
      } catch (...) {
        outcome_unknown = true;
        return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
            error(std::string(to_string(ErrorCode::kInvariantViolation)),
                  "participant runtime-scale outcome is unknown",
                  "runtime_scale_effect_outcome_unknown"));
      }
    }();
    if (!commanded.ok()) {
      if (outcome_unknown) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
      mark_failed(target.engine, commanded.error(), "runtime_scale_effect_rejected");
      continue;
    }
    const auto& exact = commanded.value();
    const auto& resolved_command = target.command;
    const bool acceptance_matches =
        exact.operation_id == resolved_command.operation_id &&
        exact.operation_revision == resolved_command.operation_revision &&
        exact.participant_id == resolved_command.participant_id &&
        exact.controllable_component_id == resolved_command.controllable_component_id &&
        exact.command_target_id == resolved_command.command_target_id &&
        exact.capability_id == resolved_command.capability_id &&
        exact.control_domain_id == resolved_command.control_domain_id &&
        exact.control_domain_revision == resolved_command.control_domain_revision &&
        exact.control_binding_id == resolved_command.control_binding_id &&
        exact.control_binding_revision == resolved_command.control_binding_revision &&
        exact.controller_goal_id == resolved_command.controller_goal_id &&
        exact.controller_goal_generation == resolved_command.controller_goal_generation &&
        exact.controller_topology_id == resolved_command.controller_topology_id &&
        exact.topology_revision == resolved_command.topology_revision &&
        exact.control_route_id == resolved_command.control_route_id &&
        exact.scaling_domain_id == resolved_command.scaling_domain_id &&
        exact.coordinated_barrier_id == resolved_command.coordinated_barrier_id &&
        exact.resources == resolved_command.resources &&
        exact.active_control_configuration_id == resolved_command.active_control_configuration_id &&
        exact.active_control_configuration_generation ==
            resolved_command.active_control_configuration_generation &&
        exact.accepted_generation == resolved_command.requested_runtime_scale_generation &&
        std::abs(exact.commanded_scale - resolved_command.requested_scale) <= 1e-12;
    if (!acceptance_matches) {
      mark_failed(target.engine,
                  error(std::string(to_string(ErrorCode::kInvariantViolation)),
                        "participant engine returned mismatched runtime-scale acceptance",
                        "runtime_scale_acceptance_mismatch"),
                  "runtime_scale_acceptance_mismatch");
      continue;
    }
    const auto observed = target.engine->runtime_scale_status(resolved_command.operation_id,
                                                              resolved_command.control_binding_id);
    if (!observed.ok()) {
      mark_failed(target.engine, observed.error(), "runtime_scale_status_unavailable");
      continue;
    }
    (void)apply_runtime_scale_status(target.engine, observed.value());
  }
  {
    std::lock_guard state_lock(state_mutex_);
    auto* active_operation = find_active_operation_locked(request.operation_id);
    if (active_operation != nullptr) {
      active_operation->runtime_scale_effect_in_flight = false;
    }
  }
  return Result<RuntimeScaleAcceptedDTO>::Ok(std::move(accepted));
}

Result<RuntimeScaleControlAcceptedDTO> ResourceAddressedExecutionKernel::set_runtime_scale_control(
    const RuntimeScaleControlRequestDTO& request) {
  if (request.request_context.request_id.empty() ||
      request.request_context.request_owner_epoch.empty() ||
      request.request_context.source_label.empty() || request.selector.program_binding_id.empty() ||
      (request.selector.group_targets.empty() &&
       (request.selector.component_role_id.empty() || request.selector.target_role_id.empty())) ||
      !std::isfinite(request.scale) || request.scale < 0.0 || request.scale > 1.0 ||
      !request.attribution.locally_valid()) {
    return reject<RuntimeScaleControlAcceptedDTO>("runtime-scale control request is malformed",
                                                  "runtime_scale_control_request_invalid",
                                                  ErrorCode::kInvalidRequest);
  }
  if (request.request_context.request_owner_epoch != config_.request_owner_epoch) {
    return Result<RuntimeScaleControlAcceptedDTO>::Err(
        error("stale_request_owner_epoch",
              "runtime-scale control request names a stale Kernel owner epoch",
              "stale_request_owner_epoch"));
  }

  // Linearize the persistent setting with new operation admission. Emergency
  // Stop never takes this mutex and therefore never waits for this path.
  std::lock_guard submit_lock(submission_mutex_);
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        runtime_scale_control_replay_ledger_.begin(), runtime_scale_control_replay_ledger_.end(),
        [&](const RuntimeScaleControlReplayEntry& entry) {
          return entry.request.request_context.request_id == request.request_context.request_id;
        });
    if (replay != runtime_scale_control_replay_ledger_.end()) {
      if (replay->request != request) {
        return Result<RuntimeScaleControlAcceptedDTO>::Err(
            error("request_id_conflict",
                  "runtime-scale control request ID was reused with different intent",
                  "request_id_conflict"));
      }
      auto accepted = replay->accepted;
      accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
      return Result<RuntimeScaleControlAcceptedDTO>::Ok(std::move(accepted));
    }
    const auto request_id = request.request_context.request_id;
    const bool used_elsewhere =
        emergency_stop_reserve_.find(request_id.value()).has_value() ||
        std::any_of(replay_ledger_.begin(), replay_ledger_.end(),
                    [&](const ReplayEntry& entry) {
                      return entry.intent.request_context.request_id == request_id;
                    }) ||
        std::any_of(control_replay_ledger_.begin(), control_replay_ledger_.end(),
                    [&](const ControlReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    }) ||
        std::any_of(
            stop_replay_ledger_.begin(), stop_replay_ledger_.end(),
            [&](const StopReplayEntry& entry) { return entry.request_id() == request_id; }) ||
        std::any_of(runtime_scale_replay_ledger_.begin(), runtime_scale_replay_ledger_.end(),
                    [&](const RuntimeScaleReplayEntry& entry) {
                      return entry.request.request_context.request_id == request_id;
                    });
    if (used_elsewhere) {
      return Result<RuntimeScaleControlAcceptedDTO>::Err(
          error("request_id_conflict",
                "runtime-scale control request ID was already used by another Kernel command",
                "request_id_conflict"));
    }
    const auto resolved = resolve_runtime_scale_control_locked(request.selector);
    if (!resolved.ok()) {
      return Result<RuntimeScaleControlAcceptedDTO>::Err(resolved.error());
    }
    const auto setting =
        std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                     [&](const RuntimeScaleControlSetting& candidate) {
                       return candidate.resolution.same_control_scope(resolved.value());
                     });
    const auto current_generation = setting == runtime_scale_control_settings_.end()
                                        ? RuntimeScaleControlGeneration(0U)
                                        : setting->generation;
    if (current_generation != request.expected_control_generation) {
      return reject<RuntimeScaleControlAcceptedDTO>("runtime-scale control generation is stale",
                                                    "runtime_scale_control_generation_stale");
    }
    if (current_generation.value() == std::numeric_limits<std::uint64_t>::max()) {
      return reject<RuntimeScaleControlAcceptedDTO>(
          "runtime-scale control generation space is exhausted",
          "runtime_scale_control_generation_exhausted", ErrorCode::kInternalError);
    }

    // Validate the exact scope setting plus the per-member effective settings
    // as one bounded atomic update. Aggregate selectors retain their own
    // generation for replay while writing through to the singular settings
    // used to seed later independent motions.
    std::vector<RuntimeScaleControlScope> target_settings;
    target_settings.reserve(resolved.value().members.size() + 1U);
    target_settings.push_back(resolved.value());
    if (resolved.value().members.size() > 1U) {
      for (const auto& member : resolved.value().members) {
        target_settings.push_back(RuntimeScaleControlScope{member.selector, {member}});
      }
    }
    auto projected_setting_count = runtime_scale_control_settings_.size();
    auto projected_memberships = std::accumulate(
        runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
        std::size_t{0U}, [](const std::size_t total, const RuntimeScaleControlSetting& retained) {
          return total + retained.resolution.members.size();
        });
    for (const auto& target : target_settings) {
      const auto same_scope = std::find_if(
          runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
          [&](const RuntimeScaleControlSetting& retained) {
            return retained.resolution.same_control_scope(target);
          });
      if (same_scope != runtime_scale_control_settings_.end()) {
        if (target.members.size() == 1U &&
            same_scope->generation.value() == std::numeric_limits<std::uint64_t>::max()) {
          return reject<RuntimeScaleControlAcceptedDTO>(
              "runtime-scale member generation space is exhausted",
              "runtime_scale_control_generation_exhausted", ErrorCode::kInternalError);
        }
        continue;
      }
      const auto same_selector = std::find_if(
          runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
          [&](const RuntimeScaleControlSetting& retained) {
            return retained.resolution.selector == target.selector;
          });
      if (same_selector == runtime_scale_control_settings_.end()) {
        ++projected_setting_count;
      } else {
        projected_memberships -= same_selector->resolution.members.size();
      }
      projected_memberships += target.members.size();
    }
    if (projected_setting_count > registrations_.size() + kMaxProgramExecutionGroups ||
        projected_memberships > kMaxProgramExecutionMemberships) {
      return reject<RuntimeScaleControlAcceptedDTO>(
          "runtime-scale control setting capacity is exhausted",
          "runtime_scale_control_capacity_exhausted", ErrorCode::kInvalidStateTransition);
    }
  }

  const auto projected = get_runtime_scale_control(request.selector);
  if (!projected.ok()) {
    return Result<RuntimeScaleControlAcceptedDTO>::Err(projected.error());
  }
  if (!projected.value().supported || projected.value().frozen) {
    return Result<RuntimeScaleControlAcceptedDTO>::Err(projected.value().reason.value_or(error(
        std::string(to_string(ErrorCode::kInvalidStateTransition)),
        "runtime-scale control is not currently available", "runtime_scale_control_unavailable")));
  }

  RuntimeScaleControlAcceptedDTO accepted{
      request.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      request.selector,
      request.scale,
      RuntimeScaleControlGeneration(request.expected_control_generation.value() + 1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      clock_.now()};
  std::vector<RuntimeScaleRequestDTO> live_requests;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto resolved = resolve_runtime_scale_control_locked(request.selector);
    if (!resolved.ok()) {
      return Result<RuntimeScaleControlAcceptedDTO>::Err(resolved.error());
    }
    auto setting =
        std::find_if(runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
                     [&](const RuntimeScaleControlSetting& candidate) {
                       return candidate.resolution.same_control_scope(resolved.value());
                     });
    const auto current_generation = setting == runtime_scale_control_settings_.end()
                                        ? RuntimeScaleControlGeneration(0U)
                                        : setting->generation;
    if (current_generation != request.expected_control_generation) {
      return reject<RuntimeScaleControlAcceptedDTO>(
          "runtime-scale control changed while its live effect was applied",
          "runtime_scale_control_generation_stale");
    }
    const auto upsert_setting = [&](RuntimeScaleControlScope resolution,
                                    const RuntimeScaleControlGeneration generation) {
      auto retained = std::find_if(
          runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
          [&](const RuntimeScaleControlSetting& candidate) {
            return candidate.resolution.same_control_scope(resolution);
          });
      if (retained == runtime_scale_control_settings_.end()) {
        // A portable selector replaces a stale route resolution instead of
        // accumulating aliases as control configurations change.
        std::erase_if(runtime_scale_control_settings_, [&](const auto& candidate) {
          return candidate.resolution.selector == resolution.selector;
        });
        runtime_scale_control_settings_.push_back(
            RuntimeScaleControlSetting{std::move(resolution), generation, request.scale});
      } else {
        retained->resolution = std::move(resolution);
        retained->generation = generation;
        retained->requested_scale = request.scale;
      }
    };
    upsert_setting(resolved.value(), accepted.control_generation);
    if (resolved.value().members.size() > 1U) {
      for (const auto& member : resolved.value().members) {
        RuntimeScaleControlScope singular{member.selector, {member}};
        const auto retained = std::find_if(
            runtime_scale_control_settings_.begin(), runtime_scale_control_settings_.end(),
            [&](const RuntimeScaleControlSetting& candidate) {
              return candidate.resolution.same_control_scope(singular);
            });
        const auto generation = retained == runtime_scale_control_settings_.end()
                                    ? RuntimeScaleControlGeneration(1U)
                                    : RuntimeScaleControlGeneration(retained->generation.value() + 1U);
        upsert_setting(std::move(singular), generation);
      }
    }
    const auto matches_scope = [&](const RuntimeParticipantBindingDTO& binding) {
      if (!binding.control_path || !binding.control_path->runtime_scale_qualification) return false;
      const auto& control = *binding.control_path;
      const auto& qualification = *control.runtime_scale_qualification;
      return std::any_of(
          resolved.value().members.begin(), resolved.value().members.end(),
          [&](const auto& member) {
            return control.control_domain_id == member.control_domain_id &&
                   control.control_domain_revision == member.control_domain_revision &&
                   control.control_binding_id == member.control_binding_id &&
                   control.control_binding_revision == member.control_binding_revision &&
                   qualification.scaling_domain_id == member.scaling_domain_id &&
                   control.active_control_configuration_id ==
                       member.active_control_configuration_id &&
                   control.active_control_configuration_generation ==
                       member.active_control_configuration_generation;
          });
    };
    for (const auto& [operation_id, operation] : active_operations_) {
      if (terminal(operation->state.state) || !operation->runtime_scale_status) continue;
      const bool selected = std::any_of(
          operation->participants.begin(), operation->participants.end(),
          [&](const auto& participant) {
            return participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
                   matches_scope(participant.binding.value());
          });
      if (!selected) continue;
      auto context = request.request_context;
      std::string member_request_material("manyforge.runtime-scale-group-member.v1");
      member_request_material.push_back('\0');
      member_request_material.append(request.request_context.request_id.value());
      member_request_material.push_back('\0');
      member_request_material.append(operation_id.value());
      context.request_id =
          RequestId(manyforge::content_identity::sha256_digest(member_request_material));
      live_requests.push_back(RuntimeScaleRequestDTO{
          std::move(context), operation_id, operation->state.operation_revision,
          operation->runtime_scale_status->runtime_scale_generation, request.scale,
          request.attribution});
    }
    std::sort(live_requests.begin(), live_requests.end(), [](const auto& left, const auto& right) {
      return left.operation_id < right.operation_id;
    });
    std::erase_if(runtime_scale_control_replay_ledger_,
                  [&](const RuntimeScaleControlReplayEntry& entry) {
                    return entry.accepted.selector == request.selector ||
                           entry.scope.same_control_scope(resolved.value());
                  });
    runtime_scale_control_replay_ledger_.push_back(
        RuntimeScaleControlReplayEntry{request, accepted, resolved.value()});
  }

  // Independent group members keep their ordinary operation ownership. The
  // group command simply applies the accepted setting through each existing
  // exact operation-scale path. A concurrent Stop may make a member no longer
  // applicable; the retained setting is still authoritative.
  std::optional<RuntimeScaleAcceptedDTO> only_live_acceptance;
  for (auto& exact : live_requests) {
    auto applied = set_runtime_scale_impl(exact, false);
    if (applied.ok() && live_requests.size() == 1U)
      only_live_acceptance = std::move(applied.value());
  }
  if (only_live_acceptance) {
    accepted.active_operation_id = only_live_acceptance->operation_id;
    accepted.active_operation_revision = only_live_acceptance->operation_revision;
    accepted.active_operation_scale_generation = only_live_acceptance->runtime_scale_generation;
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(
        runtime_scale_control_replay_ledger_.begin(), runtime_scale_control_replay_ledger_.end(),
        [&](const auto& entry) {
          return entry.request.request_context.request_id == request.request_context.request_id;
        });
    if (replay != runtime_scale_control_replay_ledger_.end()) replay->accepted = accepted;
  }
  return Result<RuntimeScaleControlAcceptedDTO>::Ok(std::move(accepted));
}

Result<void> ResourceAddressedExecutionKernel::retain_runtime_scale_observation_locked(
    std::unique_lock<std::mutex>& state_lock, ActiveOperation& operation,
    RuntimeScaleStatusDTO status, const char* reason) {
  const auto& prior = *operation.runtime_scale_status;
  // Freshness and virtual time are live observations, not journal events.
  // Keep every current sample but persist only a change of scale/application
  // or fault. Exact identity/generation checks precede this owner-local call.
  const bool unchanged =
      prior.operation_id == status.operation_id &&
      prior.operation_revision == status.operation_revision &&
      prior.runtime_scale_generation == status.runtime_scale_generation &&
      prior.requested_scale == status.requested_scale &&
      prior.effective_scale == status.effective_scale && prior.state == status.state &&
      std::equal(prior.participants.begin(), prior.participants.end(), status.participants.begin(),
                 status.participants.end(), [](const auto& left, const auto& right) {
                   return left.accepted_generation == right.accepted_generation &&
                          left.commanded_scale == right.commanded_scale &&
                          left.applied_scale == right.applied_scale &&
                          left.supported == right.supported && left.state == right.state &&
                          left.error == right.error;
                 });
  if (!unchanged) {
    auto captured = operation.state;
    auto durable = record_operation_state_locked(state_lock, captured, &status, reason);
    if (!durable.ok()) {
      return Result<void>::Err(durable.error());
    }
    if (!durable.value()) return Result<void>::Ok();
    operation.state = std::move(captured);
  }
  operation.runtime_scale_status = std::move(status);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::apply_runtime_scale_status(
    IResolvedParticipantExecutionEngine* engine,
    const ResolvedRuntimeScaleStatusDTO& resolved_status) {
  OperationCommitGuard commit(*this, resolved_status.operation_id);
  std::unique_lock state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(resolved_status.operation_id);
  if (active_operation == nullptr || active_operation->recovery_effect_in_flight ||
      !active_operation->runtime_scale_status.has_value()) {
    return Result<void>::Ok();
  }
  const auto* active_participant = active_operation->participant_for(engine);
  if (active_participant == nullptr ||
      resolved_status.operation_revision < active_operation->state.operation_revision) {
    return Result<void>::Ok();
  }
  auto status = *active_operation->runtime_scale_status;
  const auto row =
      std::find_if(status.participants.begin(), status.participants.end(), [&](const auto& item) {
        return item.participant_id == active_participant->slot_owner.participant_id;
      });
  if (row == status.participants.end()) return Result<void>::Ok();
  auto& participant = *row;
  const auto& binding = active_participant->binding.value();
  const auto& control = *binding.control_path;
  const auto& qualification = *control.runtime_scale_qualification;
  const bool exact_target_matches =
      resolved_status.operation_id == status.operation_id &&
      resolved_status.operation_revision == status.operation_revision &&
      resolved_status.participant_id == participant.participant_id &&
      resolved_status.controllable_component_id == participant.controllable_component_id &&
      resolved_status.command_target_id == participant.command_target_id &&
      resolved_status.capability_id == participant.capability_id &&
      resolved_status.control_domain_id == participant.control_domain_id &&
      resolved_status.control_domain_revision == participant.control_domain_revision &&
      resolved_status.control_binding_id == participant.control_binding_id &&
      resolved_status.control_binding_revision == participant.control_binding_revision &&
      resolved_status.controller_goal_id == participant.controller_goal_id &&
      resolved_status.controller_goal_generation == participant.controller_goal_generation &&
      resolved_status.controller_topology_id == participant.controller_topology_id &&
      resolved_status.topology_revision == participant.topology_revision &&
      resolved_status.control_route_id == participant.control_route_id &&
      resolved_status.scaling_domain_id == participant.scaling_domain_id &&
      resolved_status.coordinated_barrier_id == participant.coordinated_barrier_id &&
      resolved_status.resources == participant.resources &&
      resolved_status.active_control_configuration_id ==
          participant.active_control_configuration_id &&
      resolved_status.active_control_configuration_generation ==
          participant.active_control_configuration_generation &&
      resolved_status.acknowledgement_source_epoch == control.provider_runtime_epoch;
  if (!exact_target_matches) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error =
        error(std::string(to_string(ErrorCode::kInvariantViolation)),
              "participant engine returned runtime-scale status for another binding",
              "runtime_scale_status_identity_mismatch");
    status.updated_at_ns = clock_.now();
    return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                   "runtime_scale_status_identity_mismatch");
  }
  if (resolved_status.accepted_generation < status.runtime_scale_generation) {
    return Result<void>::Ok();
  }
  if (resolved_status.accepted_generation != status.runtime_scale_generation) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                              "participant engine advanced an unrequested runtime-scale generation",
                              "runtime_scale_status_generation_mismatch");
    status.updated_at_ns = clock_.now();
    return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                   "runtime_scale_status_generation_mismatch");
  }
  const auto valid_scale = [](const double scale) {
    return std::isfinite(scale) && scale >= 0.0 && scale <= 1.0;
  };
  const auto observed_state = resolved_status.state;
  if (resolved_status.acknowledgement_timestamp_ns < 0 ||
      resolved_status.acknowledgement_sequence > 9'007'199'254'740'991ULL ||
      !std::isfinite(resolved_status.virtual_trajectory_time_s) ||
      resolved_status.virtual_trajectory_time_s < 0.0 ||
      !valid_scale(resolved_status.commanded_scale) ||
      !valid_scale(resolved_status.applied_scale) ||
      (observed_state != RuntimeScaleApplicationState::kPending &&
       observed_state != RuntimeScaleApplicationState::kApplied &&
       observed_state != RuntimeScaleApplicationState::kRejected &&
       observed_state != RuntimeScaleApplicationState::kFailed)) {
    // Unchanged observations skip serialization, not validation. Never put
    // invalid provider scalars into the live owner projection.
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    participant.error = error(std::string(to_string(ErrorCode::kInvariantViolation)),
                              "participant engine returned invalid runtime-scale values",
                              "runtime_scale_status_invalid");
    status.updated_at_ns = clock_.now();
    return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                   "runtime_scale_status_invalid");
  }
  participant.commanded_scale = resolved_status.commanded_scale;
  participant.applied_scale = resolved_status.applied_scale;
  participant.virtual_trajectory_time_s = resolved_status.virtual_trajectory_time_s;
  participant.acknowledgement_source_epoch = resolved_status.acknowledgement_source_epoch;
  participant.acknowledgement_sequence = resolved_status.acknowledgement_sequence;
  participant.acknowledgement_timestamp_ns = resolved_status.acknowledgement_timestamp_ns;
  participant.supported = resolved_status.state != RuntimeScaleApplicationState::kRejected;
  participant.error = resolved_status.error;
  status.updated_at_ns = clock_.now();

  const bool stale = resolved_status.acknowledgement_timestamp_ns > 0 &&
                     status.updated_at_ns > resolved_status.acknowledgement_timestamp_ns &&
                     status.updated_at_ns - resolved_status.acknowledgement_timestamp_ns >
                         qualification.freshness_budget_ns;
  if (resolved_status.error.has_value() || stale) {
    participant.state = RuntimeScaleApplicationState::kFailed;
    status.state = RuntimeScaleApplicationState::kFailed;
    if (stale && !participant.error.has_value()) {
      participant.error = error(std::string(to_string(ErrorCode::kExecutionRejected)),
                                "runtime-scale applied-state acknowledgement is stale",
                                "runtime_scale_acknowledgement_stale");
    }
  } else {
    participant.state = resolved_status.state;
  }
  status.effective_scale = std::min_element(status.participants.begin(), status.participants.end(),
                                            [](const auto& left, const auto& right) {
                                              return left.applied_scale < right.applied_scale;
                                            })
                               ->applied_scale;
  status.state = RuntimeScaleApplicationState::kApplied;
  for (const auto& item : status.participants) {
    if (item.state == RuntimeScaleApplicationState::kFailed ||
        item.state == RuntimeScaleApplicationState::kRejected) {
      status.state = item.state;
      break;
    }
    if (item.state != RuntimeScaleApplicationState::kApplied ||
        std::abs(item.commanded_scale - status.requested_scale) > 1e-12 ||
        std::abs(item.applied_scale - status.requested_scale) > 1e-12)
      status.state = RuntimeScaleApplicationState::kPending;
  }
  return retain_runtime_scale_observation_locked(state_lock, *active_operation, std::move(status),
                                                 "runtime_scale_status_observed");
}

Result<RuntimeScaleStatusDTO> ResourceAddressedExecutionKernel::get_runtime_scale(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<RuntimeScaleStatusDTO>("operation identity is empty", "operation_id_empty",
                                         ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation == nullptr) {
    return reject<RuntimeScaleStatusDTO>("operation is not resident", "operation_not_resident",
                                         ErrorCode::kMissingReference);
  }
  if (!active_operation->runtime_scale_status.has_value()) {
    return reject<RuntimeScaleStatusDTO>("operation binding has no qualified runtime scaling",
                                         "runtime_scale_unsupported",
                                         ErrorCode::kUnsupportedCapability);
  }
  return Result<RuntimeScaleStatusDTO>::Ok(*active_operation->runtime_scale_status);
}

Result<OperationStateDTO> ResourceAddressedExecutionKernel::get_operation_state(
    const OperationId& operation_id) const {
  auto projection = get_operation_projection(operation_id);
  if (!projection.ok()) {
    return Result<OperationStateDTO>::Err(std::move(projection.error()));
  }
  return Result<OperationStateDTO>::Ok(std::move(projection.value().operation));
}

OperationReadProjectionDTO ResourceAddressedExecutionKernel::operation_projection_locked(
    const OperationStateDTO& state, const bool active, const bool control_pending) const {
  // Residency is the Kernel's authority over the operation, not table
  // membership. A retired manual operation still holds its cell-admission
  // claim until the owner releases it (advancing the admission generation);
  // publishing non-resident earlier lets a reader capture a generation that
  // the pending release is about to invalidate.
  const bool resident =
      active || (manual_operation_claim_.has_value() &&
                 manual_operation_claim_->intent.owner_transaction_id == state.operation_id.value());
  const auto control = std::find_if(
      control_replay_ledger_.begin(), control_replay_ledger_.end(), [&](const auto& entry) {
        return entry.durable && entry.accepted.operation_id == state.operation_id;
      });
  return {state, resident, config_.request_owner_epoch,
          control == control_replay_ledger_.end() ? std::nullopt : std::optional(control->accepted),
          control_pending};
}

Result<OperationReadProjectionDTO> ResourceAddressedExecutionKernel::get_operation_projection(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<OperationReadProjectionDTO>("operation identity is empty", "operation_id_empty",
                                              ErrorCode::kInvalidRequest);
  }
  std::lock_guard state_lock(state_mutex_);
  const auto* active_operation = find_active_operation_locked(operation_id);
  if (active_operation != nullptr) {
    return Result<OperationReadProjectionDTO>::Ok(operation_projection_locked(
        active_operation->state, true, active_operation->pending_control.has_value()));
  }
  const auto historical = std::find_if(
      historical_operations_.begin(), historical_operations_.end(),
      [&](const OperationStateDTO& state) { return state.operation_id == operation_id; });
  if (historical != historical_operations_.end()) {
    return Result<OperationReadProjectionDTO>::Ok(
        operation_projection_locked(*historical, false, false));
  }
  return reject<OperationReadProjectionDTO>("operation is not resident or retained",
                                            "operation_not_resident", ErrorCode::kMissingReference);
}

std::vector<OperationStateDTO> ResourceAddressedExecutionKernel::get_resident_operations(
    const std::vector<ProgramRunId>& program_run_ids) const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<OperationStateDTO> operations;
  operations.reserve(active_operations_.size());
  for (const auto& [operation_id, operation] : active_operations_) {
    static_cast<void>(operation_id);
    if (std::find(program_run_ids.begin(), program_run_ids.end(),
                  operation->state.program_run_id) != program_run_ids.end()) {
      operations.push_back(operation->state);
    }
  }
  return operations;
}

std::vector<OperationReadProjectionDTO> ResourceAddressedExecutionKernel::get_latest_run_operations(
    const std::vector<ProgramRunId>& program_run_ids) const {
  std::lock_guard state_lock(state_mutex_);
  std::vector<OperationReadProjectionDTO> result;
  for (const auto& run_id : program_run_ids) {
    const auto before = result.size();
    for (const auto& [id, operation] : active_operations_) {
      static_cast<void>(id);
      if (operation->state.program_run_id == run_id)
        result.push_back(operation_projection_locked(operation->state, true,
                                                     operation->pending_control.has_value()));
    }
    if (result.size() != before) continue;
    const auto latest =
        std::find_if(historical_operations_.rbegin(), historical_operations_.rend(),
                     [&](const auto& operation) { return operation.program_run_id == run_id; });
    if (latest != historical_operations_.rend())
      result.push_back(operation_projection_locked(*latest, false, false));
  }
  return result;
}

Result<OperationRecoveryProjectionDTO>
ResourceAddressedExecutionKernel::get_operation_recovery_assessment(
    const OperationId& operation_id) const {
  if (operation_id.empty()) {
    return reject<OperationRecoveryProjectionDTO>("operation identity is empty",
                                                  "operation_id_empty", ErrorCode::kInvalidRequest);
  }
  std::vector<IResolvedParticipantExecutionEngine*> engines;
  OperationRevision revision(0U);
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  std::vector<OperationRecoveryParticipantDTO> participants;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr || operation->participants.empty() ||
        operation->stop_effect_in_flight ||
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& participant) { return participant.engine == nullptr; })) {
      return reject<OperationRecoveryProjectionDTO>("operation has no resident recovery owner",
                                                    "operation_recovery_unavailable",
                                                    ErrorCode::kInvalidStateTransition);
    }
    engines.reserve(operation->participants.size());
    revision = operation->state.operation_revision;
    stop_id = operation->state.active_stop_id;
    stop_generation = operation->state.active_stop_generation;
    participants.reserve(operation->participants.size());
    for (const auto& participant : operation->participants) {
      engines.push_back(participant.engine);
      const auto& binding = participant.binding.value();
      std::optional<CapabilityProviderBindingId> provider;
      if (binding.capability_provider_path.has_value()) {
        provider = binding.capability_provider_path->capability_provider_binding_id;
      }
      participants.push_back(OperationRecoveryParticipantDTO{
          participant.slot_owner.participant_id, binding.controllable_component_id,
          binding.command_target_id, binding.capability_id,
          participant.slot_owner.control_domain_id, participant.slot_owner.control_binding_id,
          std::move(provider), participant.slot_owner.control_route_id,
          participant.slot_owner.active_control_configuration_id,
          participant.slot_owner.active_control_configuration_generation, participant.resources});
    }
  }
  std::vector<KernelRecoveryAssessmentDTO> assessments;
  assessments.reserve(engines.size());
  try {
    for (auto* engine : engines) assessments.push_back(engine->get_recovery_assessment());
  } catch (...) {
    return reject<OperationRecoveryProjectionDTO>("participant recovery assessment is unavailable",
                                                  "operation_recovery_unavailable",
                                                  ErrorCode::kInternalError);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr || operation->state.operation_revision != revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || operation->participants.size() != engines.size() ||
        !std::equal(operation->participants.begin(), operation->participants.end(), engines.begin(),
                    [](const auto& participant, const auto* engine) {
                      return participant.engine == engine;
                    })) {
      return reject<OperationRecoveryProjectionDTO>(
          "operation changed while its recovery assessment was read", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
    KernelRecoveryAssessmentDTO assessment;
    if (assessments.size() == 1U) {
      assessment = assessments.front();
    } else {
      // Only a semantic participant change is a new coordinated assessment.
      // A newer equivalent observation sample (for example an extended
      // freshness deadline) refreshes the retained inputs in place; the
      // composed assessment publishes no observation deadline of its own, so
      // its revision stays nameable by a recovery request across polls.
      const auto& retained = operation->recovery_assessment_inputs;
      bool semantic_change = retained.size() != assessments.size();
      for (std::size_t index = 0U; !semantic_change && index < assessments.size(); ++index) {
        semantic_change =
            retained[index].assessment_revision != assessments[index].assessment_revision ||
            recovery_assessment_semantically_differs(retained[index], assessments[index]);
      }
      if (semantic_change &&
          operation->recovery_assessment_revision >= kMaximumKernelJsonInteger) {
        return reject<OperationRecoveryProjectionDTO>(
            "coordinated recovery assessment revision is exhausted",
            "operation_recovery_unavailable", ErrorCode::kInternalError);
      }
      operation->recovery_assessment_inputs = assessments;
      if (semantic_change) ++operation->recovery_assessment_revision;
      assessment.schema_version = "manyforge.kernel_recovery_assessment.v1";
      assessment.assessment_revision = operation->recovery_assessment_revision;
      assessment.assessed_at_ns =
          std::max_element(assessments.begin(), assessments.end(),
                           [](const auto& left, const auto& right) {
                             return left.assessed_at_ns < right.assessed_at_ns;
                           })
              ->assessed_at_ns;
      assessment.execution_state = assessments.front().execution_state;
      assessment.recovery_class = assessments.front().recovery_class;
      if (std::any_of(assessments.begin(), assessments.end(), [&](const auto& row) {
            return row.execution_state != assessment.execution_state;
          }))
        assessment.execution_state = ExecutionState::kInterrupted;
      if (std::any_of(assessments.begin(), assessments.end(), [&](const auto& row) {
            return row.recovery_class != assessment.recovery_class;
          }))
        assessment.recovery_class = RecoveryClass::kUnknownFault;
      assessment.stop_owner_active =
          std::any_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.stop_owner_active; });
      assessment.backend_stop_confirmed =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.backend_stop_confirmed; });
      assessment.observed_state_available =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_available; });
      assessment.observed_state_fresh =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_fresh; });
      assessment.observed_state_at_rest =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_at_rest; });
      assessment.observed_state_measured =
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.observed_state_measured; });
      assessment.stronger_authority_active =
          std::any_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.stronger_authority_active; });
      assessment.queued_latch_present =
          std::any_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.queued_latch_present; });
      assessment.allowed_actions = assessments.front().allowed_actions;
      std::erase_if(assessment.allowed_actions, [&](const auto action) {
        return std::any_of(assessments.begin() + 1, assessments.end(), [&](const auto& row) {
          return std::find(row.allowed_actions.begin(), row.allowed_actions.end(), action) ==
                 row.allowed_actions.end();
        });
      });
      assessment.retry_admissible =
          !assessment.allowed_actions.empty() &&
          std::all_of(assessments.begin(), assessments.end(),
                      [](const auto& row) { return row.retry_admissible; });
      if (assessment.retry_admissible) {
        assessment.status = RecoveryAssessmentStatus::kRetryReady;
        assessment.reason_code = "coordinated_operation_recovery_ready";
      } else if (std::any_of(assessments.begin(), assessments.end(), [](const auto& row) {
                   return row.status == RecoveryAssessmentStatus::kResetRequired;
                 })) {
        assessment.status = RecoveryAssessmentStatus::kResetRequired;
        assessment.reason_code = "coordinated_operation_reset_required";
      } else {
        assessment.status = RecoveryAssessmentStatus::kWaiting;
        assessment.reason_code = "coordinated_operation_recovery_waiting";
      }
    }
    if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
      const auto& latch = *active_stop_fault_latch_;
      // A participant may be physically stopped and locally retry-ready while
      // the addressed cell owner has retained a stronger Stop-fault latch.
      // Publish the composed truth: motion is settled, but only Program Reset
      // may clear the exact outer latch.  Never offer Resume that the same
      // owner would subsequently reject at motion admission.
      ++assessment.assessment_revision;
      assessment.recovery_class = RecoveryClass::kStopFaultIntervention;
      assessment.status = RecoveryAssessmentStatus::kResetRequired;
      assessment.latch_id = latch.latch_id.value();
      assessment.stop_owner_generation = latch.stop_generation.value();
      assessment.retry_admissible = false;
      assessment.allowed_actions = {RecoveryAction::kResetCycle};
      assessment.reason_code = "active_stop_fault_requires_reset";
      if (latch.stop_certificate.has_value()) {
        assessment.stop_certificate_id = latch.stop_certificate->stop_certificate_id.value();
        assessment.stop_certificate_revision =
            latch.stop_certificate->stop_certificate_revision.value();
      }
    }
    return Result<OperationRecoveryProjectionDTO>::Ok(
        OperationRecoveryProjectionDTO{revision, std::move(stop_id), std::move(stop_generation),
                                       std::move(assessment), std::move(participants)});
  }
  return reject<OperationRecoveryProjectionDTO>("operation recovery assessment changed",
                                                "operation_recovery_stale",
                                                ErrorCode::kStaleGeneration);
}

Result<KernelRecoveryActionResultDTO>
ResourceAddressedExecutionKernel::apply_operation_recovery_action(
    const OperationId& operation_id, const OperationRevision expected_operation_revision,
    const KernelRecoveryActionRequestDTO& request) {
  bool coordinated_recovery = false;
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (operation != nullptr && operation->participants.size() > 1U) {
      if (operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll) {
        return reject<KernelRecoveryActionResultDTO>(
            "multi-participant recovery requires one coordinated operation",
            "operation_recovery_invalid", ErrorCode::kInvalidRequest);
      }
      coordinated_recovery = true;
    }
  }
  // The helper takes the same operation-level recovery mutex and performs all
  // effect calls outside Kernel state authority.
  if (coordinated_recovery)
    return apply_coordinated_operation_recovery_action(operation_id, expected_operation_revision,
                                                       request);
  const auto requests_match = [](const KernelRecoveryActionRequestDTO& left,
                                 const KernelRecoveryActionRequestDTO& right) {
    return left.request_id == right.request_id && left.action == right.action &&
           left.principal_id == right.principal_id &&
           left.expected_assessment_revision == right.expected_assessment_revision &&
           left.expected_control_command_epoch == right.expected_control_command_epoch &&
           left.expected_execution_id == right.expected_execution_id &&
           left.expected_latch_id == right.expected_latch_id &&
           left.expected_stop_certificate_id == right.expected_stop_certificate_id &&
           left.expected_stop_owner_generation == right.expected_stop_owner_generation &&
           left.reason == right.reason;
  };
  const auto retain_current_recovery = [&](OperationRecoveryReplayEntry entry) {
    std::erase_if(operation_recovery_replay_ledger_,
                  [&](const OperationRecoveryReplayEntry& current) {
                    return current.operation_id == entry.operation_id;
                  });
    operation_recovery_replay_ledger_.push_back(std::move(entry));
  };
  if (operation_id.empty() || expected_operation_revision.value() == 0U ||
      request.request_id.empty()) {
    return reject<KernelRecoveryActionResultDTO>("operation recovery request is incomplete",
                                                 "operation_recovery_invalid",
                                                 ErrorCode::kInvalidRequest);
  }
  std::lock_guard recovery_lock(operation_recovery_mutex_);
  IResolvedParticipantExecutionEngine* engine = nullptr;
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  std::optional<ProgramRunExecutionCaptureDTO> continuation_capture;
  std::size_t continuation_index{};
  std::shared_ptr<ProgramCycleClaimRun::MotionContinuation> continuation;
  IResolvedPreparedParticipantExecutionEngine* prepared_engine = nullptr;
  std::uint64_t preparation_id{};
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(operation_recovery_replay_ledger_.begin(),
                                     operation_recovery_replay_ledger_.end(),
                                     [&](const OperationRecoveryReplayEntry& entry) {
                                       return entry.request.request_id == request.request_id;
                                     });
    if (replay != operation_recovery_replay_ledger_.end()) {
      if (replay->operation_id != operation_id ||
          replay->operation_revision != expected_operation_revision ||
          !requests_match(replay->request, request)) {
        return reject<KernelRecoveryActionResultDTO>(
            "operation recovery request identity was reused", "request_id_conflict",
            ErrorCode::kInvalidRequest);
      }
      if (replay->outcome_unresolved) {
        return reject<KernelRecoveryActionResultDTO>(
            "participant recovery outcome remains unresolved",
            "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
      }
      return Result<KernelRecoveryActionResultDTO>::Ok(replay->result);
    }
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->participants.size() != 1U || operation->participants.front().engine == nullptr ||
        operation->stop_effect_in_flight) {
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery fence is stale or unsupported", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
    if (operation->ordinary_commit_in_flight || operation->ordinary_acceptance_pending ||
        operation->pending_control.has_value() || operation->runtime_scale_effect_in_flight ||
        operation->recovery_effect_in_flight || operation->coordinated_cancellation_in_flight ||
        operation->dispatch_effect_in_flight() || operation->reconciliation_in_flight() ||
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& participant) {
                      return participant.prepare_in_flight ||
                             participant.prepared_advance_in_flight;
                    }))
      return reject<KernelRecoveryActionResultDTO>("operation work is still in flight",
                                                   "operation_recovery_busy",
                                                   ErrorCode::kInvalidStateTransition);
    if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
      return Result<KernelRecoveryActionResultDTO>::Err(
          ErrorDTO{"operation_recovery_blocked_by_stop_fault",
                   "Program Reset is required before motion can resume",
                   {{"reason", "active_stop_fault_requires_reset"},
                    {"latch_id", active_stop_fault_latch_->latch_id.value()}}});
    }
    if (motion_producing_recovery(request.action) &&
        !operation->manual) {
      if (auto refusal =
              operator_alarm_attention_refusal_locked(operation_resources(operation->participants))) {
        return Result<KernelRecoveryActionResultDTO>::Err(std::move(*refusal));
      }
    }
    if (!operation->stop_allows_retirement()) {
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery awaits its exact successful Stop certificate",
          "operation_recovery_busy", ErrorCode::kInvalidStateTransition);
    }
    engine = operation->participants.front().engine;
    stop_id = operation->state.active_stop_id;
    stop_generation = operation->state.active_stop_generation;
    const auto& participant = operation->participants.front();
    const auto& public_participant = operation->state.participants.front();
    if ((request.action == RecoveryAction::kRetryMotionFromCurrentState ||
         request.action == RecoveryAction::kReplanFromStoppedState ||
         request.action == RecoveryAction::kAcknowledgeAndContinue ||
         request.action == RecoveryAction::kRetryAfterSceneRepair) &&
        participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
        participant.prepared_dispatch_released && participant.prepared_participant &&
        public_participant.execution_scope && program_cycle_claim_run_ &&
        program_cycle_claim_run_->program_run_id == operation->state.program_run_id &&
        program_cycle_claim_run_->capture) {
      const auto& capture = *program_cycle_claim_run_->capture;
      const auto& scope = *public_participant.execution_scope;
      const auto node = std::lower_bound(
          capture.table->nodes.begin(), capture.table->nodes.end(), scope.node_instance_id,
          [](const auto& row, const auto& id) { return row.instance_id < id; });
      // An internal pick/place phase has the same motion payload shape, but
      // its continuation belongs to the existing sequence owner, not this slot.
      if (node != capture.table->nodes.end() && node->instance_id == scope.node_instance_id &&
          node->owner_api == "kernel.operation") {
        continuation_capture = capture;
        continuation_index = static_cast<std::size_t>(node - capture.table->nodes.begin());
        const auto& source =
            std::get<PreparedMotionExecutionDTO>(participant.prepared_participant->payload)
                .plan_request;
        continuation = std::make_shared<ProgramCycleClaimRun::MotionContinuation>(
            ProgramCycleClaimRun::MotionContinuation{
                scope,
                operation_id,
                public_participant.participant_id,
                participant.prepared_participant->prepared_contract_digest,
                participant.binding,
                {},
                source.base_frame,
                source.tcp_frame,
                source.planner_capability_binding});
        prepared_engine = participant.prepared_motion_engine;
        preparation_id = participant.subordinate_id;
        continuation->authored_motion_kind =
            std::get<MotionParticipantIntentDTO>(operation->intent.participants.front().payload)
                .motion.index();
      }
    }
    operation->recovery_effect_in_flight = true;
  }

  const ScopeExit release_recovery_borrow([&] {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id))
      operation->recovery_effect_in_flight = false;
  });
  // Copy the semantic destination before recovery clears the subordinate.
  // This observer never grants rest: the existing exact Stop certificate is
  // checked both above and at publication below. Stop can overtake this read.
  const auto capture_rejected = [](ErrorDTO error) {
    error.details["continuation_error_code"] = error.code;
    error.code = "operation_recovery_invalid";
    return Result<KernelRecoveryActionResultDTO>::Err(std::move(error));
  };
  try {
    if (continuation) {
      if (!prepared_engine)
        return capture_rejected(error("motion_continuation_unavailable",
                                      "motion continuation has no prepared owner",
                                      "motion_continuation_unavailable"));
      auto snapshot = prepared_engine->ObservePreparedMotion(preparation_id);
      if (!snapshot.ok()) return capture_rejected(snapshot.error());
      const auto& observed = snapshot.value();
      if (observed.execution_id != preparation_id)
        return capture_rejected(error("motion_continuation_identity_mismatch",
                                      "motion continuation changed preparation identity",
                                      "motion_continuation_identity_mismatch"));
      const auto& source = observed.last_dispatched_participant;
      if (!source &&
          (observed.state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch ||
           observed.state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed)) {
        // Only a never-dispatched attempt can leave the prior slot unchanged.
        // A dispatched attempt retains its actual prior dispatch below.
        continuation.reset();
      } else if (!source) {
        return capture_rejected(error("motion_continuation_dispatch_unsettled",
                                      "motion dispatch outcome is not settled",
                                      "motion_continuation_dispatch_unsettled"));
      } else {
        if (source->prepared_contract_digest != continuation->prepared_contract_digest ||
            source->operation_id != operation_id ||
            source->participant_id != continuation->participant_id ||
            !std::holds_alternative<PreparedMotionExecutionDTO>(source->payload))
          return capture_rejected(error("motion_continuation_identity_mismatch",
                                        "motion continuation differs from the retired participant",
                                        "motion_continuation_identity_mismatch"));
        const auto& motion = std::get<PreparedMotionExecutionDTO>(source->payload);
        continuation->base_frame = motion.plan_request.base_frame;
        continuation->tcp_frame = motion.plan_request.tcp_frame;
        continuation->planner_capability = motion.plan_request.planner_capability_binding;
        auto captured = detail::capture_motion_resume_intent(
            motion.plan_request.motion, motion.trajectory, observed.stopped_trajectory_time_s);
        if (!captured.ok()) return capture_rejected(captured.error());
        continuation->intent =
            std::make_shared<const detail::MotionResumeIntent>(std::move(captured.value()));
      }
    }
  } catch (...) {
    return capture_rejected(error("motion_continuation_capture_failed",
                                  "motion continuation could not be captured before recovery",
                                  "motion_continuation_capture_failed"));
  }
  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    if (!operation || operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || !operation->stop_allows_retirement() ||
        operation->participants.size() != 1U || operation->participants.front().engine != engine ||
        (continuation_capture &&
         (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
          program_cycle_claim_run_->capture->table != continuation_capture->table ||
          program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
          program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id)))
      return Result<KernelRecoveryActionResultDTO>::Err(
          error("operation_recovery_stale", "recovery was overtaken before its effect",
                "operation_recovery_stale"));
  }

  KernelRecoveryActionResultDTO result;
  try {
    result = engine->apply_recovery_action(request);
  } catch (...) {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id))
      operation->recovery_effect_in_flight = false;
    motion_admission_closed_ = true;
    retain_current_recovery(OperationRecoveryReplayEntry{
        operation_id, expected_operation_revision, request,
        KernelRecoveryActionResultDTO{
            request.request_id,
            request.action,
            false,
            {},
            ErrorDTO{"operation_recovery_outcome_unresolved",
                     "participant recovery outcome is unknown",
                     {{"reason", "operation_recovery_outcome_unresolved"}}}},
        true});
    return reject<KernelRecoveryActionResultDTO>("participant recovery outcome is unknown",
                                                 "operation_recovery_outcome_unresolved",
                                                 ErrorCode::kInvariantViolation);
  }
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation != nullptr) operation->recovery_effect_in_flight = false;
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || operation->participants.size() != 1U ||
        operation->participants.front().engine != engine || !operation->stop_allows_retirement() ||
        result.request_id != request.request_id || result.action != request.action) {
      motion_admission_closed_ = true;
      retain_current_recovery(OperationRecoveryReplayEntry{
          operation_id, expected_operation_revision, request, result, true});
      return reject<KernelRecoveryActionResultDTO>(
          "operation or participant recovery result changed during the effect",
          "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
    }
    if (!result.accepted) {
      retain_current_recovery(
          OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
      return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
    }
    if (continuation &&
        (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
         program_cycle_claim_run_->capture->table != continuation_capture->table ||
         program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
         program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id ||
         continuation_index >= program_cycle_claim_run_->motion_continuations.size())) {
      motion_admission_closed_ = true;
      retain_current_recovery(OperationRecoveryReplayEntry{
          operation_id, expected_operation_revision, request, result, true});
      return reject<KernelRecoveryActionResultDTO>(
          "Program capture changed during motion retirement",
          "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
    }
    auto owner_rows =
        retained_owner_rows(operation->participants, resource_slots_->coherent_snapshot());
    if (!owner_rows.ok()) {
      motion_admission_closed_ = true;
      retain_current_recovery(OperationRecoveryReplayEntry{
          operation_id, expected_operation_revision, request, result, true});
      return Result<KernelRecoveryActionResultDTO>::Err(owner_rows.error());
    }
    if (!owner_rows.value().empty()) {
      auto released = resource_slots_->release_batch(owner_rows.value());
      if (!released.ok()) {
        motion_admission_closed_ = true;
        retain_current_recovery(OperationRecoveryReplayEntry{
            operation_id, expected_operation_revision, request, result, true});
        return Result<KernelRecoveryActionResultDTO>::Err(released.error());
      }
    }
    retain_current_recovery(
        OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
    if (continuation)
      program_cycle_claim_run_->motion_continuations[continuation_index] = std::move(continuation);
    retain_historical_operation_locked(operation->state);
    active_operations_.erase(operation_id);
  }
  return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
}

Result<KernelRecoveryActionResultDTO>
ResourceAddressedExecutionKernel::apply_coordinated_operation_recovery_action(
    const OperationId& operation_id, const OperationRevision expected_operation_revision,
    const KernelRecoveryActionRequestDTO& request) {
  const auto requests_match = [](const KernelRecoveryActionRequestDTO& left,
                                 const KernelRecoveryActionRequestDTO& right) {
    return left.request_id == right.request_id && left.action == right.action &&
           left.principal_id == right.principal_id &&
           left.expected_assessment_revision == right.expected_assessment_revision &&
           left.expected_control_command_epoch == right.expected_control_command_epoch &&
           left.expected_execution_id == right.expected_execution_id &&
           left.expected_latch_id == right.expected_latch_id &&
           left.expected_stop_certificate_id == right.expected_stop_certificate_id &&
           left.expected_stop_owner_generation == right.expected_stop_owner_generation &&
           left.reason == right.reason;
  };
  const auto retain_current = [&](OperationRecoveryReplayEntry entry) {
    std::erase_if(operation_recovery_replay_ledger_,
                  [&](const OperationRecoveryReplayEntry& current) {
                    return current.operation_id == entry.operation_id;
                  });
    operation_recovery_replay_ledger_.push_back(std::move(entry));
  };
  if (operation_id.empty() || expected_operation_revision.value() == 0U ||
      request.request_id.empty()) {
    return reject<KernelRecoveryActionResultDTO>("operation recovery request is incomplete",
                                                 "operation_recovery_invalid",
                                                 ErrorCode::kInvalidRequest);
  }

  std::lock_guard recovery_lock(operation_recovery_mutex_);
  {
    std::lock_guard state_lock(state_mutex_);
    const auto replay = std::find_if(operation_recovery_replay_ledger_.begin(),
                                     operation_recovery_replay_ledger_.end(),
                                     [&](const OperationRecoveryReplayEntry& entry) {
                                       return entry.request.request_id == request.request_id;
                                     });
    if (replay != operation_recovery_replay_ledger_.end()) {
      if (replay->operation_id != operation_id ||
          replay->operation_revision != expected_operation_revision ||
          !requests_match(replay->request, request)) {
        return reject<KernelRecoveryActionResultDTO>(
            "operation recovery request identity was reused", "request_id_conflict",
            ErrorCode::kInvalidRequest);
      }
      if (replay->outcome_unresolved) {
        return reject<KernelRecoveryActionResultDTO>(
            "coordinated recovery outcome remains unresolved",
            "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
      }
      return Result<KernelRecoveryActionResultDTO>::Ok(replay->result);
    }
  }

  auto projection = get_operation_recovery_assessment(operation_id);
  if (!projection.ok()) {
    return Result<KernelRecoveryActionResultDTO>::Err(projection.error());
  }
  const auto& assessment = projection.value().assessment;
  const bool exact_assessment =
      projection.value().operation_revision == expected_operation_revision &&
      request.expected_assessment_revision == assessment.assessment_revision &&
      request.expected_control_command_epoch == assessment.control_command_epoch &&
      request.expected_execution_id == assessment.execution_id &&
      request.expected_latch_id == assessment.latch_id &&
      request.expected_stop_certificate_id == assessment.stop_certificate_id &&
      request.expected_stop_owner_generation == assessment.stop_owner_generation;
  const bool action_allowed =
      assessment.retry_admissible &&
      std::find(assessment.allowed_actions.begin(), assessment.allowed_actions.end(),
                request.action) != assessment.allowed_actions.end() &&
      request.action != RecoveryAction::kResetCycle;
  if (!exact_assessment || !action_allowed) {
    KernelRecoveryActionResultDTO rejected{
        request.request_id, request.action, false, assessment,
        ErrorDTO{exact_assessment ? "operation_recovery_invalid" : "operation_recovery_stale",
                 exact_assessment ? "recovery action is not offered to every participant"
                                  : "coordinated recovery assessment changed",
                 {{"reason", exact_assessment ? "recovery_action_not_offered"
                                              : "recovery_assessment_changed"}}}};
    {
      std::lock_guard state_lock(state_mutex_);
      retain_current(OperationRecoveryReplayEntry{operation_id, expected_operation_revision,
                                                  request, rejected});
    }
    return Result<KernelRecoveryActionResultDTO>::Ok(std::move(rejected));
  }

  struct Target final {
    IResolvedParticipantExecutionEngine* engine{};
    KernelRecoveryAssessmentDTO assessment;
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    std::uint64_t preparation_id{};
    std::size_t continuation_index{};
    std::shared_ptr<ProgramCycleClaimRun::MotionContinuation> continuation;
  };
  std::vector<Target> targets;
  std::optional<StopId> stop_id;
  std::optional<StopGeneration> stop_generation;
  std::optional<ProgramRunExecutionCaptureDTO> continuation_capture;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll ||
        operation->participants.size() < 2U ||
        operation->participants.size() != operation->recovery_assessment_inputs.size() ||
        operation->stop_effect_in_flight || operation->ordinary_commit_in_flight ||
        operation->ordinary_acceptance_pending || operation->pending_control.has_value() ||
        operation->runtime_scale_effect_in_flight || operation->recovery_effect_in_flight ||
        operation->coordinated_cancellation_in_flight || operation->dispatch_effect_in_flight() ||
        operation->reconciliation_in_flight() ||
        std::any_of(operation->participants.begin(), operation->participants.end(),
                    [](const auto& participant) {
                      return participant.engine == nullptr || participant.prepare_in_flight ||
                             participant.prepared_advance_in_flight;
                    })) {
      return reject<KernelRecoveryActionResultDTO>(
          "coordinated operation recovery fence is stale or busy", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
    if (is_active_stop_fault_latch(active_stop_fault_latch_)) {
      return Result<KernelRecoveryActionResultDTO>::Err(
          ErrorDTO{"operation_recovery_blocked_by_stop_fault",
                   "Program Reset is required before motion can resume",
                   {{"reason", "active_stop_fault_requires_reset"},
                    {"latch_id", active_stop_fault_latch_->latch_id.value()}}});
    }
    if (motion_producing_recovery(request.action) &&
        !operation->manual) {
      if (auto refusal =
              operator_alarm_attention_refusal_locked(operation_resources(operation->participants))) {
        return Result<KernelRecoveryActionResultDTO>::Err(std::move(*refusal));
      }
    }
    if (!operation->stop_allows_retirement()) {
      return reject<KernelRecoveryActionResultDTO>(
          "operation recovery awaits its exact successful Stop certificate",
          "operation_recovery_busy", ErrorCode::kInvalidStateTransition);
    }
    stop_id = operation->state.active_stop_id;
    stop_generation = operation->state.active_stop_generation;
    if (program_cycle_claim_run_ && program_cycle_claim_run_->capture &&
        program_cycle_claim_run_->program_run_id == operation->state.program_run_id) {
      continuation_capture = *program_cycle_claim_run_->capture;
    }
    targets.reserve(operation->participants.size());
    const bool capture_continuation =
        request.action == RecoveryAction::kRetryMotionFromCurrentState ||
        request.action == RecoveryAction::kReplanFromStoppedState ||
        request.action == RecoveryAction::kAcknowledgeAndContinue ||
        request.action == RecoveryAction::kRetryAfterSceneRepair;
    for (std::size_t index = 0U; index < operation->participants.size(); ++index) {
      auto& participant = operation->participants[index];
      const auto& public_participant = operation->state.participants[index];
      Target target{
          participant.engine, operation->recovery_assessment_inputs[index], nullptr, 0U, 0U, {}};
      if (capture_continuation &&
          participant.payload_kind == OperationParticipantPayloadKind::kMotion &&
          participant.prepared_dispatch_released && participant.prepared_participant &&
          public_participant.execution_scope && continuation_capture) {
        const auto& scope = *public_participant.execution_scope;
        const auto node =
            std::lower_bound(continuation_capture->table->nodes.begin(),
                             continuation_capture->table->nodes.end(), scope.node_instance_id,
                             [](const auto& row, const auto& id) { return row.instance_id < id; });
        if (node != continuation_capture->table->nodes.end() &&
            node->instance_id == scope.node_instance_id && node->owner_api == "kernel.operation") {
          target.continuation_index =
              static_cast<std::size_t>(node - continuation_capture->table->nodes.begin());
          const auto& source =
              std::get<PreparedMotionExecutionDTO>(participant.prepared_participant->payload)
                  .plan_request;
          target.continuation = std::make_shared<ProgramCycleClaimRun::MotionContinuation>(
              ProgramCycleClaimRun::MotionContinuation{
                  scope,
                  operation_id,
                  public_participant.participant_id,
                  participant.prepared_participant->prepared_contract_digest,
                  participant.binding,
                  {},
                  source.base_frame,
                  source.tcp_frame,
                  source.planner_capability_binding,
                  std::get<MotionParticipantIntentDTO>(
                      operation->intent.participants[index].payload)
                      .motion.index()});
          target.prepared_engine = participant.prepared_motion_engine;
          target.preparation_id = participant.subordinate_id;
        }
      }
      targets.push_back(std::move(target));
    }
    operation->recovery_effect_in_flight = true;
  }

  const ScopeExit release_recovery_borrow([&] {
    std::lock_guard state_lock(state_mutex_);
    if (auto* operation = find_active_operation_locked(operation_id))
      operation->recovery_effect_in_flight = false;
  });
  const auto unresolved_locked = [&](std::string message) {
    motion_admission_closed_ = true;
    KernelRecoveryActionResultDTO result{
        request.request_id, request.action, false, assessment,
        ErrorDTO{"operation_recovery_outcome_unresolved",
                 std::move(message),
                 {{"reason", "operation_recovery_outcome_unresolved"}}}};
    retain_current(OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request,
                                                result, true});
    return reject<KernelRecoveryActionResultDTO>(
        "coordinated participant recovery outcome is unknown",
        "operation_recovery_outcome_unresolved", ErrorCode::kInvariantViolation);
  };
  const auto unresolved = [&](std::string message) {
    std::lock_guard state_lock(state_mutex_);
    return unresolved_locked(std::move(message));
  };

  try {
    for (auto& target : targets) {
      if (!target.continuation) continue;
      if (target.prepared_engine == nullptr) {
        return reject<KernelRecoveryActionResultDTO>("motion continuation has no prepared owner",
                                                     "operation_recovery_invalid",
                                                     ErrorCode::kInvalidRequest);
      }
      auto snapshot = target.prepared_engine->ObservePreparedMotion(target.preparation_id);
      if (!snapshot.ok()) return Result<KernelRecoveryActionResultDTO>::Err(snapshot.error());
      const auto& observed = snapshot.value();
      if (observed.execution_id != target.preparation_id) {
        return reject<KernelRecoveryActionResultDTO>(
            "motion continuation changed preparation identity", "operation_recovery_invalid",
            ErrorCode::kInvalidRequest);
      }
      const auto& source = observed.last_dispatched_participant;
      if (!source &&
          (observed.state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch ||
           observed.state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed)) {
        target.continuation.reset();
        continue;
      }
      if (!source ||
          source->prepared_contract_digest != target.continuation->prepared_contract_digest ||
          source->operation_id != operation_id ||
          source->participant_id != target.continuation->participant_id ||
          !std::holds_alternative<PreparedMotionExecutionDTO>(source->payload)) {
        return reject<KernelRecoveryActionResultDTO>(
            "motion continuation dispatch identity is unsettled", "operation_recovery_invalid",
            ErrorCode::kInvalidRequest);
      }
      const auto& motion = std::get<PreparedMotionExecutionDTO>(source->payload);
      target.continuation->base_frame = motion.plan_request.base_frame;
      target.continuation->tcp_frame = motion.plan_request.tcp_frame;
      target.continuation->planner_capability = motion.plan_request.planner_capability_binding;
      auto captured = detail::capture_motion_resume_intent(
          motion.plan_request.motion, motion.trajectory, observed.stopped_trajectory_time_s);
      if (!captured.ok()) return Result<KernelRecoveryActionResultDTO>::Err(captured.error());
      target.continuation->intent =
          std::make_shared<const detail::MotionResumeIntent>(std::move(captured.value()));
    }
  } catch (...) {
    return reject<KernelRecoveryActionResultDTO>(
        "motion continuation could not be captured before coordinated recovery",
        "operation_recovery_invalid", ErrorCode::kInternalError);
  }

  {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    bool participant_changed =
        operation == nullptr || operation->participants.size() != targets.size();
    for (std::size_t index = 0U; !participant_changed && index < targets.size(); ++index) {
      participant_changed = operation->participants[index].engine != targets[index].engine;
    }
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->stop_effect_in_flight || !operation->stop_allows_retirement() ||
        participant_changed ||
        (continuation_capture &&
         (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
          program_cycle_claim_run_->capture->table != continuation_capture->table ||
          program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
          program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id))) {
      return reject<KernelRecoveryActionResultDTO>(
          "coordinated operation changed before recovery effects", "operation_recovery_stale",
          ErrorCode::kStaleGeneration);
    }
  }

  std::size_t accepted_count = 0U;
  std::optional<KernelRecoveryActionResultDTO> rejected;
  for (auto& target : targets) {
    auto participant_request = request;
    participant_request.expected_assessment_revision = target.assessment.assessment_revision;
    participant_request.expected_control_command_epoch = target.assessment.control_command_epoch;
    participant_request.expected_execution_id = target.assessment.execution_id;
    participant_request.expected_latch_id = target.assessment.latch_id;
    participant_request.expected_stop_certificate_id = target.assessment.stop_certificate_id;
    participant_request.expected_stop_owner_generation = target.assessment.stop_owner_generation;
    try {
      auto result = target.engine->apply_recovery_action(participant_request);
      if (result.request_id != request.request_id || result.action != request.action) {
        return unresolved("participant recovery returned a different command identity");
      }
      if (!result.accepted) {
        rejected = std::move(result);
        break;
      }
      ++accepted_count;
    } catch (...) {
      return unresolved("participant recovery threw after coordinated acceptance began");
    }
  }
  if (rejected.has_value()) {
    if (accepted_count != 0U)
      return unresolved("coordinated participants disagreed after a recovery effect");
    KernelRecoveryActionResultDTO result{request.request_id, request.action, false, assessment,
                                         rejected->error};
    {
      std::lock_guard state_lock(state_mutex_);
      retain_current(
          OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
    }
    return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
  }

  KernelRecoveryActionResultDTO result{request.request_id, request.action, true, {}, std::nullopt};
  result.resulting_assessment.schema_version = "manyforge.kernel_recovery_assessment.v1";
  result.resulting_assessment.assessment_revision = assessment.assessment_revision;
  result.resulting_assessment.reason_code = "coordinated_operation_recovered";
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (operation == nullptr ||
        operation->state.operation_revision != expected_operation_revision ||
        operation->state.active_stop_id != stop_id ||
        operation->state.active_stop_generation != stop_generation ||
        operation->participants.size() != targets.size() || !operation->stop_allows_retirement()) {
      return unresolved_locked("coordinated operation changed while recovery results returned");
    }
    if (continuation_capture &&
        (!program_cycle_claim_run_ || !program_cycle_claim_run_->capture ||
         program_cycle_claim_run_->capture->table != continuation_capture->table ||
         program_cycle_claim_run_->claim_generation != continuation_capture->claim_generation ||
         program_cycle_claim_run_->program_run_id != continuation_capture->program_run_id)) {
      return unresolved_locked("Program capture changed during coordinated motion retirement");
    }
    if (program_cycle_claim_run_ &&
        std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
          return target.continuation &&
                 target.continuation_index >= program_cycle_claim_run_->motion_continuations.size();
        })) {
      return unresolved_locked("motion continuation index changed during retirement");
    }
    auto owner_rows =
        retained_owner_rows(operation->participants, resource_slots_->coherent_snapshot());
    if (!owner_rows.ok())
      return unresolved_locked("coordinated resource ownership changed during retirement");
    if (!owner_rows.value().empty()) {
      auto released = resource_slots_->release_batch(owner_rows.value());
      if (!released.ok())
        return unresolved_locked("coordinated resource claims could not be retired");
    }
    if (program_cycle_claim_run_) {
      for (auto& target : targets) {
        if (target.continuation) {
          program_cycle_claim_run_->motion_continuations[target.continuation_index] =
              std::move(target.continuation);
        }
      }
    }
    retain_current(
        OperationRecoveryReplayEntry{operation_id, expected_operation_revision, request, result});
    retain_historical_operation_locked(operation->state);
    active_operations_.erase(operation_id);
  }
  return Result<KernelRecoveryActionResultDTO>::Ok(std::move(result));
}

Result<void> ResourceAddressedExecutionKernel::fail_pre_dispatch(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* failing_engine,
    ErrorDTO failure) {
  struct Cancellation final {
    IResolvedPreparedParticipantExecutionEngine* engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t preparation_id{};
  };
  std::vector<Cancellation> cancellations;
  std::vector<ResourceSlotReservationOwnerDTO> reservation_rows;
  const bool admission_refusal = failing_engine == nullptr;
  const auto stop_owns_failure_locked = [&](ActiveOperation* operation) {
    if (operation == nullptr ||
        (!admission_refusal &&
         operation->intent.coordination_policy != OperationCoordinationPolicy::kCoordinatedAll)) {
      return true;
    }
    if (operation->state.active_stop_id.has_value()) {
      if (auto* participant = operation->participant_for(failing_engine); participant != nullptr) {
        participant->last_subordinate_error = std::move(failure);
      }
      if (operation->state.active_stop_generation.has_value()) {
        const auto transaction = std::find_if(
            stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
              return candidate != nullptr && candidate->active &&
                     candidate->stop_id == operation->state.active_stop_id &&
                     candidate->stop_generation == *operation->state.active_stop_generation;
            });
        if (transaction != stop_transactions_.end()) {
          stop_control_executor_->ArmDeadline((*transaction)->token, clock_.now());
          stop_control_executor_->Publish((*transaction)->token);
        } else {
          motion_admission_closed_ = true;
        }
      }
      return true;
    }
    return false;
  };
  {
    // A Stop-owned cancellation callback must not wait for ordinary storage.
    std::lock_guard state_lock(state_mutex_);
    if (stop_owns_failure_locked(find_active_operation_locked(operation_id)))
      return Result<void>::Ok();
  }
  {
    OperationCommitGuard commit(*this, operation_id);
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    if (stop_owns_failure_locked(operation)) return Result<void>::Ok();
    if (std::all_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.stop_cancelled_pre_dispatch; })) {
      // This operation's abort already owns cancellation and final cleanup.
      return Result<void>::Ok();
    }
    if (operation->coordinated_dispatch_barrier_released ||
        std::any_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.prepared_dispatch_released; })) {
      motion_admission_closed_ = true;
      return reject<void>("pre-dispatch failure raced the coordinated dispatch barrier",
                          "coordinated_pre_dispatch_barrier_race", ErrorCode::kInvariantViolation);
    }

    cancellations.reserve(operation->participants.size());
    reservation_rows.reserve(operation->participants.size());
    operation->coordinated_cancellation_in_flight = true;
    for (auto& participant : operation->participants) {
      participant.stop_cancelled_pre_dispatch = true;
      if (admission_refusal || participant.engine == failing_engine) {
        participant.last_subordinate_error = failure;
      }
      if (participant.prepared_motion_engine != nullptr && participant.subordinate_id != 0U) {
        cancellations.push_back(Cancellation{participant.prepared_motion_engine,
                                             participant.payload_kind, participant.subordinate_id});
      }
      reservation_rows.push_back({participant.slot_owner, participant.resources});
    }
    for (auto& row : operation->state.participants) {
      const auto participant = std::find_if(
          operation->participants.begin(), operation->participants.end(), [&](const auto& current) {
            return current.slot_owner.participant_id == row.participant_id;
          });
      const bool failed =
          participant != operation->participants.end() && participant->engine == failing_engine;
      // An admission refusal applies to every participant alike and takes
      // the same no-effect failure outcome as a failed preparation.
      row.state = failed || admission_refusal ? OperationStateKind::kInterventionRequired
                                              : OperationStateKind::kCancelledPreDispatch;
      row.progress = 0.0;
      row.terminal_error =
          failed || admission_refusal ? std::optional(failure) : std::nullopt;
    }
    operation->state.state = OperationStateKind::kInterventionRequired;
    operation->state.updated_at_ns = clock_.now();
    stamp_operation_capture_locked(*operation, operation->state);
  }

  std::optional<ErrorDTO> cancellation_failure;
  for (const auto& cancellation : cancellations) {
    auto cancelled = cancel_prepared_participant(*cancellation.engine, cancellation.payload_kind,
                                                 cancellation.preparation_id);
    if (!cancelled.ok() && !cancellation_failure.has_value()) {
      cancellation_failure = cancelled.error();
    }
  }

  OperationCommitGuard commit(*this, operation_id);
  std::unique_lock state_lock(state_mutex_);
  auto* operation = find_active_operation_locked(operation_id);
  if (operation != nullptr) {
    operation->coordinated_cancellation_in_flight = false;
  }
  if (operation == nullptr || operation->state.active_stop_id.has_value()) {
    return Result<void>::Ok();
  }
  if (cancellation_failure.has_value()) {
    motion_admission_closed_ = true;
    for (auto& row : operation->state.participants) {
      if (row.state == OperationStateKind::kCancelledPreDispatch) {
        row.state = OperationStateKind::kInterventionRequired;
        row.terminal_error = cancellation_failure;
      }
    }
  }
  auto captured = operation->state;
  auto durable = record_operation_state_locked(
      state_lock, captured, nullptr,
      cancellation_failure.has_value() ? "coordinated_prepare_cancel_failed"
      : admission_refusal              ? "admission_refused_before_dispatch"
                                       : "coordinated_prepare_failed_before_dispatch");
  if (!durable.ok()) {
    motion_admission_closed_ = true;
    return Result<void>::Err(durable.error());
  }
  if (!durable.value()) return Result<void>::Ok();
  operation->state = std::move(captured);
  if (operation->dispatch_effect_in_flight() || operation->reconciliation_in_flight() ||
      std::any_of(operation->participants.begin(), operation->participants.end(),
                  [](const auto& participant) {
                    return participant.prepare_in_flight || participant.prepared_advance_in_flight;
                  })) {
    // A sibling may still hold a borrowed engine from before the abort. Keep
    // the fault resident; existing explicit Reset can retire it after drain.
    return Result<void>::Ok();
  }
  std::sort(reservation_rows.begin(), reservation_rows.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.owner.operation_id, left.owner.participant_id) <
                     std::tie(right.owner.operation_id, right.owner.participant_id);
            });
  const auto released = resource_slots_->release_batch(reservation_rows);
  if (!released.ok()) {
    motion_admission_closed_ = true;
    return released;
  }
  for (auto& participant : operation->participants) {
    participant.slot_state = ResourceSlotState::kTerminal;
  }
  retain_historical_operation_locked(operation->state);
  active_operations_.erase(operation_id);
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_prepared_motion(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine) {
  struct Target final {
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t subordinate_id{};
    RuntimeParticipantBinding binding;
    RuntimeParticipantStopReadiness readiness;
    std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
    MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
    std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
    std::shared_ptr<const PreparedParticipantStopPlanDTO> fault_stop_plan;
  };
  std::optional<Target> target;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant == nullptr ||
        participant->payload_kind != OperationParticipantPayloadKind::kMotion ||
        operation->recovery_effect_in_flight || participant->prepared_motion_engine == nullptr ||
        participant->subordinate_id == 0U || participant->prepared_dispatch_released ||
        participant->prepared_advance_in_flight || participant->prepared_participant.has_value() ||
        operation->coordinated_dispatch_barrier_released ||
        (terminal(operation->state.state) && !operation->state.active_stop_id.has_value())) {
      return Result<void>::Ok();
    }
    participant->prepared_advance_in_flight = true;
    target.emplace(Target{
        participant->prepared_motion_engine, participant->engine, participant->subordinate_id,
        participant->binding, participant->stop_readiness,
        participant->retained_default_stop_limits, participant->stop_node_category,
        participant->ordinary_stop_plan, participant->fault_stop_plan});
  }

  const auto release_claim = [&]() {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(target->engine);
    if (participant != nullptr && participant->subordinate_id == target->subordinate_id) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
    }
  };

  auto snapshot = target->prepared_engine->ObservePreparedMotion(target->subordinate_id);
  if (!snapshot.ok()) {
    release_claim();
    return Result<void>::Err(snapshot.error());
  }
  if (snapshot.value().execution_id != target->subordinate_id) {
    release_claim();
    return reject<void>("prepared execution observation changed identity",
                        "prepared_motion_observation_identity_mismatch",
                        ErrorCode::kInvariantViolation);
  }
  if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparing) {
    release_claim();
    return Result<void>::Ok();
  }

  if (snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kPreparationFailed ||
      snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch) {
    const bool cancelled =
        snapshot.value().state == ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
    const auto failure = snapshot.value().error.value_or(error(
        std::string(to_string(cancelled ? ErrorCode::kInvalidStateTransition
                                        : ErrorCode::kExecutionRejected)),
        cancelled ? "prepared execution was cancelled before dispatch"
                  : "prepared execution failed before dispatch",
        cancelled ? "prepared_motion_cancelled_pre_dispatch" : "prepared_motion_prepare_failed"));
    bool stop_owns_terminal = false;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(target->engine);
      if (participant != nullptr) {
        participant->prepared_advance_in_flight = false;
        participant->last_subordinate_error = failure;
        stop_owns_terminal = operation->state.active_stop_id.has_value();
        if (stop_owns_terminal) settle_completed_control_locked(operation_id);
      }
    }
    if (stop_owns_terminal) {
      return Result<void>::Ok();
    }
    bool coordinated = false;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto* operation = find_active_operation_locked(operation_id);
      coordinated = operation != nullptr && operation->intent.coordination_policy ==
                                                OperationCoordinationPolicy::kCoordinatedAll;
    }
    if (!coordinated) {
      return apply_subordinate_state(
          operation_id, target->engine, target->subordinate_id,
          cancelled ? OperationStateKind::kStopped : OperationStateKind::kInterventionRequired, 0.0,
          cancelled ? std::optional<ErrorDTO>{} : std::optional(failure));
    }
    return fail_pre_dispatch(operation_id, target->engine, failure);
  }

  if (!snapshot.value().participant || !snapshot.value().backend_prepare_result ||
      snapshot.value().state != ResolvedPreparedMotionExecutionStateDTO::kPrepared) {
    release_claim();
    return reject<void>("prepared execution observation has an invalid state shape",
                        "prepared_motion_observation_invalid", ErrorCode::kInvariantViolation);
  }
  const auto* prepared_motion =
      std::get_if<PreparedMotionExecutionDTO>(&snapshot.value().participant->payload);
  bool common_time_law_missing = false;
  if (prepared_motion && prepared_motion->trajectory.runtime_scale_bounds) {
    std::lock_guard state_lock(state_mutex_);
    const auto* operation = find_active_operation_locked(operation_id);
    const bool common_time_law_required =
        operation != nullptr &&
        operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
        std::count_if(operation->participants.begin(), operation->participants.end(),
                      [](const auto& row) {
                        return row.payload_kind == OperationParticipantPayloadKind::kMotion;
                      }) > 1;
    if (common_time_law_required) {
      const auto& bounds = *prepared_motion->trajectory.runtime_scale_bounds;
      common_time_law_missing =
          !operation->common_timing || !operation->common_timing->accepts_prepared(
                                           snapshot.value().participant->participant_id, bounds);
    }
  }
  if (common_time_law_missing) {
    release_claim();
    return fail_pre_dispatch(
        operation_id, target->engine,
        error(std::string(to_string(ErrorCode::kUnsupportedCapability)),
              "Coordinated trajectories require a qualified common time-law preparation",
              "coordinated_time_law_preparation_not_qualified"));
  }
  const auto verified = VerifyPreparedOperationParticipant(
      *snapshot.value().participant, target->binding, target->readiness,
      target->retained_default_stop_limits, target->stop_node_category);
  if (!verified.ok() ||
      snapshot.value().participant->ordinary_stop_plan != *target->ordinary_stop_plan ||
      !prepared_fault_stop_plan_matches(snapshot.value().participant->fault_stop_plan,
                                        target->fault_stop_plan) ||
      prepare_participant_result_field_error(*snapshot.value().backend_prepare_result,
                                             *snapshot.value().participant)
          .has_value()) {
    release_claim();
    return verified.ok()
               ? reject<void>("prepared participant differs from owner authority",
                              "prepared_motion_product_mismatch", ErrorCode::kInvariantViolation)
               : Result<void>::Err(verified.error());
  }

  return release_prepared_dispatch_barrier(operation_id, target->engine, target->subordinate_id,
                                           *snapshot.value().participant);
}

Result<void> ResourceAddressedExecutionKernel::release_prepared_dispatch_barrier(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
    std::uint64_t preparation_id, const PreparedOperationParticipantDTO& participant_product) {
  struct DispatchTarget final {
    OperationParticipantId participant_id;
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t preparation_id{};
    ContentDigest contract_digest;
    ControllerGoalId controller_goal_id;
    ControllerGoalGeneration controller_goal_generation{0U};
  };
  struct CancellationTarget final {
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t preparation_id{};
  };

  bool cancel_instead = false;
  std::optional<CancellationTarget> cancellation_target;
  std::optional<StopControlWorkToken> stop_wakeup;
  std::vector<DispatchTarget> dispatch_targets;
  const ScopeExit release_advance([&] {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant != nullptr && participant->preparation_id == preparation_id) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
    }
  });
  {
    OperationCommitGuard commit(*this, operation_id);
    std::unique_lock state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant == nullptr || participant->subordinate_id != preparation_id) {
      return Result<void>::Ok();
    }
    if (operation->state.active_stop_id.has_value() &&
        operation->state.active_stop_generation.has_value()) {
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active &&
                   candidate->stop_id == operation->state.active_stop_id &&
                   candidate->stop_generation == *operation->state.active_stop_generation;
          });
      if (transaction == stop_transactions_.end()) {
        motion_admission_closed_ = true;
      } else {
        stop_wakeup = (*transaction)->token;
        stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
      }
    } else if (participant->stop_cancelled_pre_dispatch &&
               operation->intent.coordination_policy ==
                   OperationCoordinationPolicy::kCoordinatedAll &&
               terminal(operation->state.state)) {
      // The coordinated abort already owns cancellation. A late prepared
      // read is not permission to cancel or dispatch the participant again.
      return Result<void>::Ok();
    } else if (participant->slot_state != ResourceSlotState::kReserved ||
               participant->stop_cancelled_pre_dispatch || terminal(operation->state.state)) {
      cancel_instead = true;
      // The read's borrow continues through disposal of its prepared product.
      participant->prepared_advance_in_flight = true;
      cancellation_target =
          CancellationTarget{participant->prepared_motion_engine, participant->payload_kind,
                             participant->subordinate_id};
    } else {
      participant->prepared_participant = participant_product;
      auto prepared_state = operation->state;
      const auto state_row =
          std::find_if(prepared_state.participants.begin(), prepared_state.participants.end(),
                       [&](const auto& row) {
                         return row.participant_id == participant->slot_owner.participant_id;
                       });
      if (state_row == prepared_state.participants.end()) {
        return reject<void>("prepared participant has no public operation-state row",
                            "operation_participant_state_missing", ErrorCode::kInvariantViolation);
      }
      state_row->state = OperationStateKind::kPrepared;
      prepared_state.state = aggregate_participant_state(prepared_state.participants);
      prepared_state.updated_at_ns = clock_.now();

      const bool all_prepared = std::all_of(
          operation->participants.begin(), operation->participants.end(),
          [](const auto& candidate) { return candidate.prepared_participant.has_value(); });
      if (!all_prepared) {
        auto durable = record_operation_state_locked(state_lock, prepared_state,
                                                     operation->runtime_scale_status.has_value()
                                                         ? &*operation->runtime_scale_status
                                                         : nullptr,
                                                     "coordinated_participant_prepared_waiting");
        if (!durable.ok()) {
          return Result<void>::Err(durable.error());
        }
        if (!durable.value()) return Result<void>::Ok();
        operation->state = std::move(prepared_state);
      } else {
        prepared_state.state = OperationStateKind::kPrepared;
        for (auto& row : prepared_state.participants) {
          row.state = OperationStateKind::kPrepared;
        }
        prepared_state.updated_at_ns = clock_.now();
        std::vector<PreparedMotionDiagnosticsDTO> motion_diagnostics;
        motion_diagnostics.reserve(operation->participants.size());
        for (const auto& candidate : operation->participants) {
          if (candidate.payload_kind != OperationParticipantPayloadKind::kMotion) {
            continue;
          }
          if (!candidate.prepared_participant.has_value()) {
            return reject<void>("prepared barrier lost one motion participant product",
                                "prepared_motion_product_missing", ErrorCode::kInvariantViolation);
          }
          auto diagnostic = prepared_motion_diagnostics(*candidate.prepared_participant);
          if (!diagnostic.ok()) {
            return Result<void>::Err(diagnostic.error());
          }
          motion_diagnostics.push_back(std::move(diagnostic.value()));
        }
        std::sort(motion_diagnostics.begin(), motion_diagnostics.end(),
                  [](const auto& left, const auto& right) {
                    return left.participant_id < right.participant_id;
                  });
        auto durable = record_operation_state_locked(
            state_lock, prepared_state,
            operation->runtime_scale_status.has_value() ? &*operation->runtime_scale_status
                                                        : nullptr,
            operation->participants.size() == 1U ? (operation->participants.front().payload_kind ==
                                                            OperationParticipantPayloadKind::kMotion
                                                        ? "subordinate_motion_prepared"
                                                        : "subordinate_command_prepared")
                                                 : "coordinated_prepare_barrier_complete",
            motion_diagnostics);
        if (!durable.ok()) {
          return Result<void>::Err(durable.error());
        }
        if (!durable.value()) return Result<void>::Ok();
        // Final pre-dispatch recheck (spec 450 §5.2, spec 530 §11). The
        // prepared-state write above released state_mutex_, so attention is
        // evaluated only here, in the same lock hold that transitions the
        // slots and releases dispatch: an alarm published or a projection
        // aged out at any earlier instant is observed. Dispatch none; the
        // shared pre-dispatch path cancels every prepared participant.
        if (!operation->manual) {
          if (auto refusal = operator_alarm_attention_refusal_locked(
                  operation_resources(operation->participants))) {
            operation->state = std::move(prepared_state);
            participant = operation->participant_for(engine);
            if (participant != nullptr) participant->prepared_advance_in_flight = false;
            state_lock.unlock();
            commit.release();
            return fail_pre_dispatch(operation_id, nullptr, std::move(*refusal));
          }
        }

        std::vector<ResourceSlotReservationOwnerDTO> rows;
        rows.reserve(operation->participants.size());
        for (const auto& candidate : operation->participants) {
          rows.push_back({candidate.slot_owner, candidate.resources});
        }
        std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
          return std::tie(left.owner.operation_id, left.owner.participant_id) <
                 std::tie(right.owner.operation_id, right.owner.participant_id);
        });
        auto transitioned =
            operation->participants.size() == 1U
                ? resource_slots_->transition(operation->participants.front().slot_owner,
                                              operation->participants.front().resources,
                                              ResourceSlotState::kDispatched)
                : resource_slots_->transition_batch(rows, ResourceSlotState::kDispatched);
        if (!transitioned.ok()) {
          return transitioned;
        }
        operation->state = std::move(prepared_state);
        operation->coordinated_dispatch_barrier_released = true;
        dispatch_targets.reserve(operation->participants.size());
        for (auto& candidate : operation->participants) {
          if (!candidate.prepared_participant.has_value() ||
              candidate.prepared_motion_engine == nullptr || candidate.subordinate_id == 0U) {
            motion_admission_closed_ = true;
            return reject<void>("complete prepared barrier lost one participant product",
                                "coordinated_prepared_product_missing",
                                ErrorCode::kInvariantViolation);
          }
          const auto public_row =
              std::find_if(operation->state.participants.begin(),
                           operation->state.participants.end(), [&](const auto& row) {
                             return row.participant_id == candidate.slot_owner.participant_id;
                           });
          if (public_row == operation->state.participants.end()) {
            motion_admission_closed_ = true;
            return reject<void>("complete prepared barrier lost one public participant row",
                                "operation_participant_state_missing",
                                ErrorCode::kInvariantViolation);
          }
          candidate.slot_state = ResourceSlotState::kDispatched;
          dispatch_targets.push_back(DispatchTarget{
              candidate.slot_owner.participant_id, candidate.payload_kind,
              candidate.prepared_motion_engine, candidate.engine, candidate.subordinate_id,
              candidate.prepared_participant->prepared_contract_digest,
              public_row->controller_goal_id, public_row->controller_goal_generation});
        }
        std::sort(dispatch_targets.begin(), dispatch_targets.end(),
                  [](const auto& left, const auto& right) {
                    return left.participant_id < right.participant_id;
                  });
      }
    }
  }

  if (stop_wakeup.has_value()) {
    stop_control_executor_->Publish(*stop_wakeup);
    return Result<void>::Ok();
  }
  if (cancel_instead) {
    const auto cancelled =
        cancellation_target.has_value() && cancellation_target->prepared_engine != nullptr
            ? cancel_prepared_participant(*cancellation_target->prepared_engine,
                                          cancellation_target->payload_kind,
                                          cancellation_target->preparation_id)
            : reject<void>("prepared cancellation target is unavailable",
                           "prepared_cancellation_target_missing", ErrorCode::kInvariantViolation);
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
      if (participant != nullptr && participant->subordinate_id == preparation_id) {
        participant->prepared_advance_in_flight = false;
        if (cancelled.ok()) {
          settle_completed_control_locked(operation_id);
        } else {
          participant->last_subordinate_error = cancelled.error();
          motion_admission_closed_ = true;
        }
      }
    }
    return cancelled.ok() ? Result<void>::Ok() : Result<void>::Err(cancelled.error());
  }

  std::vector<OperationParticipantId> attempted_participants;
  for (const auto& dispatch_target : dispatch_targets) {
    std::optional<StopIntentDTO> admission_stop;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(dispatch_target.engine);
      if (participant == nullptr || operation->state.active_stop_id.has_value() ||
          participant->subordinate_id != dispatch_target.preparation_id) {
        break;
      }
      const bool handover_stale =
          operation->manual_handover &&
          !validate_manual_program_handover_locked(*operation->manual_handover).ok();
      if (motion_admission_closed_ || handover_stale) {
        // Reuse exact Stop settlement for an admitted operation whose final
        // dispatch lost to a cell fault. Unreleased products are cancelled;
        // any already-released coordinated participant is physically stopped.
        admission_stop = StopIntentDTO{
            PublicRequestContextDTO{RequestId("stop.admission-closed." + operation_id.value()),
                                    config_.request_owner_epoch, "kernel.admission", std::nullopt},
            std::vector<StopOperationTargetDTO>{
                {operation_id, operation->state.operation_revision, StopGeneration(0U)}},
            std::string(handover_stale ? "manual_handover_scope_stale"
                                       : "motion_admission_closed")};
      } else {
        participant->prepared_dispatch_released = true;
        participant->dispatch_effect_in_flight = true;
        attempted_participants.push_back(dispatch_target.participant_id);
      }
    }

    if (admission_stop.has_value()) {
      const auto contained = stop(*admission_stop);
      return contained.ok() ? Result<void>::Ok() : Result<void>::Err(contained.error());
    }

    Result<void> dispatched = Result<void>::Ok();
    std::optional<GripperCommandResultDTO> command_result;
    if (dispatch_target.payload_kind == OperationParticipantPayloadKind::kMotion) {
      dispatched = dispatch_target.prepared_engine->DispatchPreparedMotion(
          dispatch_target.preparation_id, dispatch_target.contract_digest);
    } else {
      auto command = dispatch_target.prepared_engine->DispatchPreparedCapabilityCommand(
          dispatch_target.preparation_id, dispatch_target.contract_digest,
          dispatch_target.controller_goal_id, dispatch_target.controller_goal_generation);
      if (!command.ok()) {
        dispatched = Result<void>::Err(command.error());
      } else if (!command.value().accepted || command.value().error.has_value() ||
                 command.value().command_id == 0U || command.value().generation == 0U) {
        dispatched = Result<void>::Err(command.value().error.value_or(
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  "prepared capability command returned an invalid acceptance",
                  "subordinate_command_result_invalid")));
      } else {
        command_result = command.value();
      }
    }
    {
      // Returning from the provider ends the dispatch borrow, independently of
      // ordinary journal publication. Stop must be able to settle even when a
      // sibling participant's ordinary capture is blocked in storage.
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(dispatch_target.engine);
      if (participant != nullptr && participant->subordinate_id == dispatch_target.preparation_id) {
        participant->dispatch_effect_in_flight = false;
        if (!dispatched.ok()) {
          participant->last_subordinate_error = dispatched.error();
        } else if (command_result.has_value()) {
          participant->subordinate_id = command_result->command_id;
          participant->subordinate_generation = command_result->generation;
        }
        if (operation->state.active_stop_id.has_value()) {
          const auto transaction = std::find_if(
              stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
                return candidate && candidate->active &&
                       candidate->stop_id == operation->state.active_stop_id &&
                       candidate->stop_generation == operation->state.active_stop_generation;
              });
          if (transaction != stop_transactions_.end()) {
            stop_wakeup = (*transaction)->token;
            stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
          }
        }
      }
    }
    if (stop_wakeup.has_value()) {
      stop_control_executor_->Publish(*stop_wakeup);
    }
    if (command_result.has_value() && !stop_wakeup.has_value()) {
      OperationCommitGuard commit(*this, operation_id);
      std::unique_lock state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(dispatch_target.engine);
      if (participant != nullptr && !operation->state.active_stop_id.has_value() &&
          participant->preparation_id == dispatch_target.preparation_id &&
          participant->subordinate_id == command_result->command_id &&
          participant->subordinate_generation == command_result->generation) {
        auto executing_state = operation->state;
        const auto public_row = std::find_if(
            executing_state.participants.begin(), executing_state.participants.end(),
            [&](const auto& row) { return row.participant_id == dispatch_target.participant_id; });
        if (public_row == executing_state.participants.end()) {
          dispatched =
              reject<void>("dispatched command lost its public participant row",
                           "operation_participant_state_missing", ErrorCode::kInvariantViolation);
        } else if (public_row->state == OperationStateKind::kPrepared ||
                   public_row->state == OperationStateKind::kDispatching) {
          // Reconciliation or Stop may already have advanced this participant
          // while we waited for the ordinary gate. Never restore an older state.
          public_row->state = OperationStateKind::kExecuting;
          executing_state.state = aggregate_participant_state(executing_state.participants);
          executing_state.updated_at_ns = clock_.now();
          auto durable = record_operation_state_locked(state_lock, executing_state,
                                                       operation->runtime_scale_status.has_value()
                                                           ? &*operation->runtime_scale_status
                                                           : nullptr,
                                                       "subordinate_command_admitted");
          if (!durable.ok()) {
            dispatched = Result<void>::Err(durable.error());
          } else if (durable.value()) {
            operation->state = std::move(executing_state);
          }
        }
        if (!dispatched.ok()) participant->last_subordinate_error = dispatched.error();
      }
    }
    if (dispatched.ok()) {
      continue;
    }
    if (dispatch_targets.size() == 1U) {
      return apply_subordinate_state(
          operation_id, dispatch_target.engine, dispatch_target.preparation_id,
          OperationStateKind::kInterventionRequired, 0.0, dispatched.error());
    }

    const ErrorDTO partial_failure{
        std::string(to_string(ErrorCode::kExecutionRejected)),
        "coordinated dispatch failed after the barrier released physical work",
        {{"reason", "operation_partial_dispatch"},
         {"backend_reason", dispatched.error().details.contains("reason")
                                ? dispatched.error().details.at("reason")
                                : dispatched.error().code}}};
    std::vector<CancellationTarget> cancellations;
    std::vector<ResourceSlotReservationOwnerDTO> undispatched_rows;
    OperationRevision current_revision(0U);
    std::optional<OperationStateDTO> partial_state;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      if (operation == nullptr || operation->state.active_stop_id.has_value()) {
        return Result<void>::Ok();
      }
      current_revision = operation->state.operation_revision;
      for (auto& candidate : operation->participants) {
        const bool attempted =
            std::find(attempted_participants.begin(), attempted_participants.end(),
                      candidate.slot_owner.participant_id) != attempted_participants.end();
        auto state_row =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& row) {
                           return row.participant_id == candidate.slot_owner.participant_id;
                         });
        if (state_row == operation->state.participants.end()) {
          motion_admission_closed_ = true;
          continue;
        }
        if (attempted) {
          state_row->state = OperationStateKind::kDispatching;
          if (candidate.slot_owner.participant_id == dispatch_target.participant_id) {
            state_row->terminal_error = partial_failure;
          }
          continue;
        }
        candidate.stop_cancelled_pre_dispatch = true;
        state_row->state = OperationStateKind::kCancelledPreDispatch;
        state_row->terminal_error.reset();
        undispatched_rows.push_back({candidate.slot_owner, candidate.resources});
        if (candidate.prepared_motion_engine != nullptr && candidate.subordinate_id != 0U) {
          cancellations.push_back(CancellationTarget{
              candidate.prepared_motion_engine, candidate.payload_kind, candidate.subordinate_id});
        }
      }
      operation->state.state = OperationStateKind::kDispatching;
      operation->state.updated_at_ns = clock_.now();
      operation->coordinated_cancellation_in_flight = !cancellations.empty();
      partial_state = operation->state;
    }
    std::sort(attempted_participants.begin(), attempted_participants.end());
    const PartialDispatchStopScope partial_scope{operation_id, current_revision,
                                                 attempted_participants};
    StopIntentDTO containment{
        PublicRequestContextDTO{RequestId("stop.partial-dispatch." + operation_id.value()),
                                config_.request_owner_epoch, "kernel.partial_dispatch",
                                std::nullopt},
        std::vector<StopOperationTargetDTO>{
            StopOperationTargetDTO{operation_id, current_revision, StopGeneration(0U)}},
        std::string("operation_partial_dispatch")};
    auto stopped =
        accept_stop(EmergencyStopProjectedIntentDTO{std::move(containment)}, &partial_scope);
    if (!stopped.ok()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
    }

    std::sort(undispatched_rows.begin(), undispatched_rows.end(),
              [](const auto& left, const auto& right) {
                return std::tie(left.owner.operation_id, left.owner.participant_id) <
                       std::tie(right.owner.operation_id, right.owner.participant_id);
              });
    if (!undispatched_rows.empty()) {
      std::lock_guard state_lock(state_mutex_);
      auto terminalized =
          resource_slots_->transition_batch(undispatched_rows, ResourceSlotState::kTerminal);
      auto released = terminalized.ok() ? resource_slots_->release_batch(undispatched_rows)
                                        : Result<void>::Err(terminalized.error());
      if (!released.ok()) {
        motion_admission_closed_ = true;
      } else if (auto* operation = find_active_operation_locked(operation_id);
                 operation != nullptr) {
        for (auto& candidate : operation->participants) {
          if (std::find(attempted_participants.begin(), attempted_participants.end(),
                        candidate.slot_owner.participant_id) == attempted_participants.end()) {
            candidate.slot_state = ResourceSlotState::kTerminal;
          }
        }
      }
    }
    for (const auto& cancellation : cancellations) {
      if (!cancel_prepared_participant(*cancellation.prepared_engine, cancellation.payload_kind,
                                       cancellation.preparation_id)
               .ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    {
      std::lock_guard state_lock(state_mutex_);
      if (auto* operation = find_active_operation_locked(operation_id); operation != nullptr) {
        stamp_operation_capture_locked(*operation, operation->state);
        partial_state = operation->state;
      }
    }
    if (!partial_state.has_value()) {
      std::lock_guard state_lock(state_mutex_);
      motion_admission_closed_ = true;
      return Result<void>::Ok();
    }
    const auto durable = attempt_journal_write([&] {
      return journal_.RecordOperationState(config_.request_owner_epoch, *partial_state, nullptr,
                                           "operation_partial_dispatch");
    });
    {
      std::lock_guard state_lock(state_mutex_);
      if (!durable.ok()) motion_admission_closed_ = true;
      if (auto* operation = find_active_operation_locked(operation_id); operation != nullptr) {
        operation->coordinated_cancellation_in_flight = false;
        settle_completed_control_locked(operation_id);
      }
    }
    return Result<void>::Ok();
  }
  return Result<void>::Ok();
}

Result<void> ResourceAddressedExecutionKernel::advance_prepared_capability_command(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine) {
  struct Target final {
    IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
    IResolvedParticipantExecutionEngine* engine{};
    std::uint64_t preparation_id{};
    RuntimeParticipantBinding binding;
    RuntimeParticipantStopReadiness readiness;
    std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
    MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
    std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
  };

  std::optional<Target> target;
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant == nullptr ||
        participant->payload_kind != OperationParticipantPayloadKind::kCapabilityCommand ||
        operation->recovery_effect_in_flight || participant->prepared_motion_engine == nullptr ||
        participant->subordinate_id == 0U || participant->prepared_dispatch_released ||
        participant->prepared_advance_in_flight || participant->prepared_participant.has_value() ||
        operation->coordinated_dispatch_barrier_released ||
        (terminal(operation->state.state) && !operation->state.active_stop_id.has_value())) {
      return Result<void>::Ok();
    }
    const auto state_row =
        std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                     [&](const auto& row) {
                       return row.participant_id == participant->slot_owner.participant_id;
                     });
    if (state_row == operation->state.participants.end()) {
      return reject<void>("prepared command has no public participant row",
                          "operation_participant_state_missing", ErrorCode::kInvariantViolation);
    }
    participant->prepared_advance_in_flight = true;
    target.emplace(Target{participant->prepared_motion_engine, participant->engine,
                          participant->subordinate_id, participant->binding,
                          participant->stop_readiness, participant->retained_default_stop_limits,
                          participant->stop_node_category, participant->ordinary_stop_plan});
  }

  const auto release_claim = [&]() {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(target->engine);
    if (participant != nullptr && participant->subordinate_id == target->preparation_id) {
      participant->prepared_advance_in_flight = false;
      settle_completed_control_locked(operation_id);
    }
  };

  auto snapshot = target->prepared_engine->ObservePreparedCapabilityCommand(target->preparation_id);
  if (!snapshot.ok()) {
    release_claim();
    return Result<void>::Err(snapshot.error());
  }
  if (snapshot.value().preparation_id != target->preparation_id) {
    release_claim();
    return reject<void>("prepared command observation changed identity",
                        "prepared_command_observation_identity_mismatch",
                        ErrorCode::kInvariantViolation);
  }
  if (snapshot.value().state == ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed ||
      snapshot.value().state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch) {
    const bool cancelled =
        snapshot.value().state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
    const auto failure = snapshot.value().error.value_or(error(
        std::string(to_string(cancelled ? ErrorCode::kInvalidStateTransition
                                        : ErrorCode::kExecutionRejected)),
        cancelled ? "prepared command was cancelled before dispatch"
                  : "prepared command failed before dispatch",
        cancelled ? "prepared_command_cancelled_pre_dispatch" : "prepared_command_prepare_failed"));
    bool stop_owns_terminal = false;
    bool coordinated = false;
    {
      std::lock_guard state_lock(state_mutex_);
      auto* operation = find_active_operation_locked(operation_id);
      auto* participant =
          operation == nullptr ? nullptr : operation->participant_for(target->engine);
      if (participant != nullptr) {
        participant->prepared_advance_in_flight = false;
        participant->last_subordinate_error = failure;
        stop_owns_terminal = operation->state.active_stop_id.has_value();
        coordinated =
            operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll;
        if (stop_owns_terminal) settle_completed_control_locked(operation_id);
      }
    }
    if (stop_owns_terminal) {
      return Result<void>::Ok();
    }
    if (coordinated) {
      return fail_pre_dispatch(operation_id, target->engine, failure);
    }
    return apply_subordinate_state(
        operation_id, target->engine, target->preparation_id,
        cancelled ? OperationStateKind::kStopped : OperationStateKind::kInterventionRequired, 0.0,
        cancelled ? std::optional<ErrorDTO>{} : std::optional(failure));
  }
  if (snapshot.value().state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared ||
      !snapshot.value().participant || !snapshot.value().backend_prepare_result) {
    release_claim();
    return reject<void>("prepared command observation has an invalid shape",
                        "prepared_command_observation_invalid", ErrorCode::kInvariantViolation);
  }
  const auto verified = VerifyPreparedOperationParticipant(
      *snapshot.value().participant, target->binding, target->readiness,
      target->retained_default_stop_limits, target->stop_node_category);
  if (!verified.ok() ||
      snapshot.value().participant->ordinary_stop_plan != *target->ordinary_stop_plan ||
      prepare_participant_result_field_error(*snapshot.value().backend_prepare_result,
                                             *snapshot.value().participant)
          .has_value()) {
    release_claim();
    return verified.ok()
               ? reject<void>("prepared command differs from owner authority",
                              "prepared_command_product_mismatch", ErrorCode::kInvariantViolation)
               : Result<void>::Err(verified.error());
  }

  return release_prepared_dispatch_barrier(operation_id, target->engine, target->preparation_id,
                                           *snapshot.value().participant);
}

Result<void> ResourceAddressedExecutionKernel::apply_subordinate_state(
    const OperationId& operation_id, IResolvedParticipantExecutionEngine* engine,
    std::uint64_t subordinate_id, OperationStateKind state, double progress,
    std::optional<ErrorDTO> terminal_error, const bool observe_current_motion) {
  {
    std::lock_guard state_lock(state_mutex_);
    auto* operation = find_active_operation_locked(operation_id);
    auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
    if (participant != nullptr && operation->state.active_stop_id.has_value() &&
        (participant->slot_state == ResourceSlotState::kStopping ||
         participant->slot_state == ResourceSlotState::kTerminal)) {
      // The exact Stop certificate owns this participant's terminal. A
      // synchronous Stop callback must never wait behind ordinary storage.
      // Unaffected siblings of a partial Stop still follow the ordinary path.
      if (terminal_error.has_value() && participant->subordinate_id == subordinate_id)
        participant->last_subordinate_error = terminal_error;
      return Result<void>::Ok();
    }
  }
  OperationCommitGuard commit(*this, operation_id);
  if (observe_current_motion) {
    // Callbacks are wakeups, not ordered state delivery. Sample only after
    // acquiring the existing per-operation writer gate so a delayed callback
    // or concurrent reconciliation cannot publish an older sampled state.
    // get_status is an observer-safe local read; no provider I/O or Stop lock.
    const auto current = engine->get_status();
    if (current.execution_id != subordinate_id) return Result<void>::Ok();
    const auto observed = operation_state(current.state);
    if (observed != state) terminal_error.reset();
    state = observed;
    progress = current.progress;
    if (state != OperationStateKind::kFailed && state != OperationStateKind::kStopped &&
        state != OperationStateKind::kInterventionRequired &&
        state != OperationStateKind::kInterrupted) {
      terminal_error.reset();
    } else if (!terminal_error) {
      terminal_error = status_error(current.last_error);
    }
  }
  std::unique_lock state_lock(state_mutex_);
  auto* active_operation = find_active_operation_locked(operation_id);
  auto* active_participant =
      active_operation == nullptr ? nullptr : active_operation->participant_for(engine);
  if (active_participant == nullptr || active_operation->recovery_effect_in_flight ||
      active_operation->coordinated_cancellation_in_flight) {
    return Result<void>::Ok();
  }
  // Stop may have won while this ordinary callback waited for its writer
  // gate or sampled the subordinate. Its certificate still owns settlement.
  if (active_operation->state.active_stop_id &&
      (active_participant->slot_state == ResourceSlotState::kStopping ||
       active_participant->slot_state == ResourceSlotState::kTerminal)) {
    return Result<void>::Ok();
  }
  if (state == OperationStateKind::kPrepared && active_operation->pending_control.has_value() &&
      (active_operation->pending_control->accepted.control == OperationControlKind::kReset ||
       active_operation->pending_control->accepted.control ==
           OperationControlKind::kAcknowledgeCompleted)) {
    // The subordinate Idle transition confirms the settling effect. The
    // durable owner retains the terminal state named by the accepted control;
    // it does not republish that private FSM reset as a new active operation.
    return Result<void>::Ok();
  }
  if (terminal(active_operation->state.state) && state != active_operation->state.state) {
    // A settled participant cannot be revived by late subordinate feedback.
    return Result<void>::Ok();
  }
  if (active_participant->payload_kind == OperationParticipantPayloadKind::kMotion) {
    if (subordinate_id == 0U) {
      return reject<void>("subordinate execution identity is empty",
                          "subordinate_execution_id_empty", ErrorCode::kInvariantViolation);
    }
    // The execute result establishes event ownership. Events observed before
    // that result are ignored rather than guessed from an engine-global slot;
    // reconcile_subordinate_state() closes the resulting observation window.
    if (active_participant->subordinate_id == 0U ||
        active_participant->subordinate_id != subordinate_id) {
      return Result<void>::Ok();
    }
  }

  const auto desired_slot = slot_state_for(state);
  const auto desired_slot_value = desired_slot.value_or(active_participant->slot_state);
  const bool slot_changed =
      desired_slot.has_value() && active_participant->slot_state != desired_slot_value;
  const auto participant_iterator = std::find_if(
      active_operation->state.participants.begin(), active_operation->state.participants.end(),
      [&](const auto& participant) {
        return participant.participant_id == active_participant->slot_owner.participant_id;
      });
  if (participant_iterator == active_operation->state.participants.end()) {
    return reject<void>("active participant has no public operation-state row",
                        "operation_participant_state_missing", ErrorCode::kInvariantViolation);
  }
  const auto& participant = *participant_iterator;
  if (!std::isfinite(progress)) {
    return reject<void>("subordinate progress is not finite", "subordinate_progress_invalid",
                        ErrorCode::kInvariantViolation);
  }
  const double bounded_progress = std::clamp(progress, 0.0, 1.0);
  const bool failure_terminal = state == OperationStateKind::kFailed ||
                                state == OperationStateKind::kStopped ||
                                state == OperationStateKind::kInterrupted ||
                                state == OperationStateKind::kInterventionRequired;
  if (failure_terminal) {
    const bool generic_subordinate_error =
        terminal_error.has_value() && terminal_error->details.contains("reason") &&
        terminal_error->details.at("reason") == "subordinate_terminal_error";
    if (active_operation->state.active_stop_id.has_value() &&
        participant.terminal_error.has_value()) {
      // Stop feedback cannot erase a cleanup fault already owned by this
      // participant. Its containment evidence has a separate certificate.
      terminal_error = participant.terminal_error;
    } else if ((!terminal_error.has_value() || generic_subordinate_error) &&
               active_participant->last_subordinate_error.has_value()) {
      terminal_error = active_participant->last_subordinate_error;
    }
  }
  const bool same_terminal_error =
      participant.terminal_error.has_value() == terminal_error.has_value() &&
      (!terminal_error.has_value() ||
       (participant.terminal_error->code == terminal_error->code &&
        participant.terminal_error->message == terminal_error->message &&
        participant.terminal_error->details == terminal_error->details));
  if (!slot_changed && participant.state == state && (!terminal(state) || same_terminal_error)) {
    // Progress is current presentation, not a durable lifecycle transition.
    // Retain the latest scalar without writing a full operation snapshot on
    // every sample. A later transition records its complete current state.
    if (participant.progress != bounded_progress) {
      participant_iterator->progress = bounded_progress;
      active_operation->state.updated_at_ns = clock_.now();
    }
    return Result<void>::Ok();
  }

  auto observed_state = active_operation->state;
  auto& observed_participant = *std::find_if(
      observed_state.participants.begin(), observed_state.participants.end(),
      [&](const auto& candidate) {
        return candidate.participant_id == active_participant->slot_owner.participant_id;
      });
  observed_participant.state = state;
  observed_participant.progress = bounded_progress;
  if (terminal(state)) {
    observed_participant.terminal_error = std::move(terminal_error);
  }
  observed_state.state = aggregate_participant_state(observed_state.participants);
  observed_state.updated_at_ns = clock_.now();
  auto durable_state = record_operation_state_locked(
      state_lock, observed_state,
      active_operation->runtime_scale_status.has_value() ? &*active_operation->runtime_scale_status
                                                         : nullptr,
      "subordinate_state_observed");
  if (!durable_state.ok()) {
    return Result<void>::Err(durable_state.error());
  }
  if (!durable_state.value()) return Result<void>::Ok();
  if (slot_changed) {
    auto transition = resource_slots_->transition(
        active_participant->slot_owner, active_participant->resources, desired_slot_value);
    if (!transition.ok()) {
      auto intervention = observed_state;
      intervention.state = OperationStateKind::kInterventionRequired;
      auto& failed_participant = *std::find_if(
          intervention.participants.begin(), intervention.participants.end(),
          [&](const auto& candidate) {
            return candidate.participant_id == active_participant->slot_owner.participant_id;
          });
      failed_participant.state = OperationStateKind::kInterventionRequired;
      failed_participant.terminal_error = transition.error();
      intervention.updated_at_ns = clock_.now();
      auto durable_failure =
          record_operation_state_locked(state_lock, intervention,
                                        active_operation->runtime_scale_status.has_value()
                                            ? &*active_operation->runtime_scale_status
                                            : nullptr,
                                        "resource_transition_failed_after_state_observation");
      if (durable_failure.ok() && durable_failure.value()) {
        active_operation->state = std::move(intervention);
      }
      return transition;
    }
    active_participant->slot_state = desired_slot_value;
  }
  active_operation->state = std::move(observed_state);
  return Result<void>::Ok();
}

void ResourceAddressedExecutionKernel::on_engine_event(IResolvedParticipantExecutionEngine* engine,
                                                       const KernelEvent& event) {
  if (event.type == KernelEventType::kCollisionDetected) {
    std::optional<StopIntentDTO> containment;
    {
      std::lock_guard state_lock(state_mutex_);
      const auto operation_id = find_event_operation_locked(engine, event.execution_id);
      auto* operation =
          operation_id.has_value() ? find_active_operation_locked(*operation_id) : nullptr;
      auto* participant = operation == nullptr ? nullptr : operation->participant_for(engine);
      if (operation != nullptr && participant != nullptr && !terminal(operation->state.state) &&
          !operation->state.active_stop_id.has_value()) {
        auto collision_details = event.details;
        collision_details["reason"] = "runtime_collision_fault";
        participant->last_subordinate_error = ErrorDTO{
            std::string(to_string(ErrorCode::kExecutionAborted)),
            "Runtime collision containment stopped the operation", std::move(collision_details)};
        const auto material = config_.request_owner_epoch.value() + "\nruntime-collision\n" +
                              operation->state.operation_id.value() + "\n" +
                              std::to_string(operation->state.operation_revision.value()) + "\n" +
                              participant->slot_owner.participant_id.value();
        containment = StopIntentDTO{
            PublicRequestContextDTO{RequestId(manyforge::content_identity::sha256_digest(material)),
                                    config_.request_owner_epoch, "kernel.runtime_collision",
                                    participant->slot_owner.participant_id.value()},
            std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
                operation->state.operation_id, operation->state.operation_revision,
                operation->state.active_stop_generation.value_or(StopGeneration(0U))}},
            std::string("runtime_collision_fault"),
            RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                                  std::nullopt}};
      }
    }
    if (containment.has_value()) {
      auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*containment)});
      if (!stopped.ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    return;
  }
  if (event.type == KernelEventType::kParticipantPrepared) {
    std::optional<OperationId> operation_id;
    {
      std::lock_guard state_lock(state_mutex_);
      operation_id = find_event_operation_locked(engine, event.execution_id);
    }
    if (operation_id.has_value()) {
      (void)advance_prepared_motion(*operation_id, engine);
    }
    return;
  }
  if (event.type == KernelEventType::kErrorRaised) {
    if (!event.error.has_value()) {
      return;
    }
    std::optional<OperationId> operation_id;
    std::optional<StopIntentDTO> containment;
    {
      std::lock_guard state_lock(state_mutex_);
      operation_id = find_event_operation_locked(engine, event.execution_id);
      if (operation_id.has_value()) {
        auto* active_operation = find_active_operation_locked(*operation_id);
        if (active_operation != nullptr) {
          auto* participant = active_operation->participant_for(engine);
          if (participant != nullptr) {
            participant->last_subordinate_error = event.error;
            const auto reason = event.error->details.find("reason");
            const bool invalid_prepared_dispatch =
                reason != event.error->details.end() &&
                reason->second.starts_with("prepared_execution_handle_");
            if (invalid_prepared_dispatch && participant->prepared_dispatch_released &&
                !active_operation->state.active_stop_id.has_value()) {
              const auto material =
                  config_.request_owner_epoch.value() + "\nprepared-dispatch-handle\n" +
                  active_operation->state.operation_id.value() + "\n" +
                  std::to_string(active_operation->state.operation_revision.value());
              containment = StopIntentDTO{
                  PublicRequestContextDTO{
                      RequestId(manyforge::content_identity::sha256_digest(material)),
                      config_.request_owner_epoch, "kernel.prepared_dispatch_validation",
                      std::nullopt},
                  std::vector<StopOperationTargetDTO>{StopOperationTargetDTO{
                      active_operation->state.operation_id,
                      active_operation->state.operation_revision,
                      active_operation->state.active_stop_generation.value_or(StopGeneration(0U))}},
                  std::string("prepared_dispatch_membership_invalid"),
                  RequestAttributionDTO{RequestTransportDTO::kInternal,
                                        RequestAssuranceDTO::kUnverified, std::nullopt}};
            }
          }
        }
      }
    }
    if (containment.has_value()) {
      auto stopped = accept_stop(EmergencyStopProjectedIntentDTO{std::move(*containment)});
      if (!stopped.ok()) {
        std::lock_guard state_lock(state_mutex_);
        motion_admission_closed_ = true;
      }
    }
    if (operation_id.has_value()) {
      (void)advance_prepared_motion(*operation_id, engine);
    }
    return;
  }
  if (event.type != KernelEventType::kStateTransition &&
      event.type != KernelEventType::kExecuteStarted) {
    return;
  }
  auto state = event.type == KernelEventType::kExecuteStarted ? OperationStateKind::kExecuting
                                                              : operation_state(event.to_state);
  std::optional<OperationId> operation_id;
  std::optional<StopControlWorkToken> stop_wakeup;
  bool aggregate_stop_owns_terminal = false;
  std::optional<ErrorDTO> coordinated_pre_dispatch_failure;
  {
    std::lock_guard state_lock(state_mutex_);
    operation_id = find_event_operation_locked(engine, event.execution_id);
    auto* operation =
        operation_id.has_value() ? find_active_operation_locked(*operation_id) : nullptr;
    if (operation != nullptr && terminal(state) && state != OperationStateKind::kCompleted &&
        operation->intent.coordination_policy == OperationCoordinationPolicy::kCoordinatedAll &&
        !operation->state.active_stop_id.has_value() &&
        !operation->coordinated_dispatch_barrier_released &&
        std::none_of(
            operation->participants.begin(), operation->participants.end(),
            [](const auto& participant) { return participant.prepared_dispatch_released; })) {
      const auto* participant = operation->participant_for(engine);
      coordinated_pre_dispatch_failure =
          event.error.has_value() ? event.error : participant->last_subordinate_error;
      if (!coordinated_pre_dispatch_failure.has_value()) {
        coordinated_pre_dispatch_failure =
            error(std::string(to_string(ErrorCode::kExecutionRejected)),
                  "coordinated participant terminated before dispatch",
                  "coordinated_pre_dispatch_terminal_failure");
      }
    }
    if (operation != nullptr && operation->state.active_stop_id.has_value() &&
        operation->state.active_stop_generation.has_value() && operation->stop_effect_in_flight) {
      const auto transaction = std::find_if(
          stop_transactions_.begin(), stop_transactions_.end(), [&](const auto& candidate) {
            return candidate != nullptr && candidate->active && !candidate->terminal &&
                   candidate->stop_id == operation->state.active_stop_id &&
                   candidate->stop_generation == *operation->state.active_stop_generation;
          });
      if (transaction == stop_transactions_.end()) {
        motion_admission_closed_ = true;
      } else {
        stop_wakeup = (*transaction)->token;
        const auto* participant = operation->participant_for(engine);
        if (participant != nullptr) {
          const auto work = std::find_if(
              (*transaction)->participants.begin(), (*transaction)->participants.end(),
              [&](const auto& candidate) {
                return candidate.operation_id == operation->state.operation_id &&
                       candidate.plan.participant_id == participant->slot_owner.participant_id;
              });
          aggregate_stop_owns_terminal = work != (*transaction)->participants.end();
          if (work != (*transaction)->participants.end()) {
            observe_prepared_cancellation_terminal(*work, event.execution_id, state, event.error);
            if (work->terminal_error.has_value()) motion_admission_closed_ = true;
          }
        }
        stop_control_executor_->ArmDeadline(*stop_wakeup, clock_.now());
      }
    }
  }
  if (stop_wakeup.has_value()) {
    stop_control_executor_->Publish(*stop_wakeup);
  }
  if (aggregate_stop_owns_terminal) {
    // The aggregate Stop reducer is the only public terminal owner while its
    // exact participant settlements are in flight. Subordinate feedback is a
    // wake-up/evidence fact, not a competing operation transition.
    return;
  }
  if (coordinated_pre_dispatch_failure.has_value()) {
    // The execution owner's terminal event is decisive even if its private
    // prepared-session cleanup follows that event. An earlier error alone
    // was not decisive. Reuse the abort claim, including recursive callbacks.
    (void)fail_pre_dispatch(*operation_id, engine,
                                        std::move(*coordinated_pre_dispatch_failure));
    return;
  }
  if (!operation_id.has_value()) {
    return;
  }
  (void)apply_subordinate_state(*operation_id, engine, event.execution_id, state, 0.0, event.error,
                                true);
}

Result<void> ResourceAddressedExecutionKernel::reconcile_subordinate_state() {
  struct ReconcileTarget final {
    OperationId operation_id;
    IResolvedParticipantExecutionEngine* engine{};
    IResolvedPreparedParticipantExecutionEngine* prepared_motion_engine{};
    OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
    std::uint64_t subordinate_id{};
    std::uint64_t subordinate_generation{};
    std::string command_target;
    OperationStateKind public_state{OperationStateKind::kPrepared};
    bool prepared_dispatch_released{};
    std::optional<ControlBindingId> runtime_scale_control_binding_id;
  };
  std::vector<ReconcileTarget> targets;
  {
    std::lock_guard state_lock(state_mutex_);
    targets.reserve(active_operations_.size() * 2U);
    for (const auto& [operation_id, operation] : active_operations_) {
      // Operation controls own their subordinate transition until their
      // accepted effect returns. A scheduler pass skips, rather than waits on,
      // that exact operation; disjoint operations still reconcile. The claim
      // also coalesces concurrent scheduler calls without holding a lock over
      // subordinate I/O.
      if (operation->pending_control.has_value() || operation->runtime_scale_effect_in_flight ||
          operation->stop_effect_in_flight || operation->recovery_effect_in_flight ||
          operation->coordinated_cancellation_in_flight) {
        continue;
      }
      for (auto& participant : operation->participants) {
        if (participant.reconciliation_in_flight || participant.prepare_in_flight ||
            participant.dispatch_effect_in_flight || participant.subordinate_id == 0U) {
          continue;
        }
        const auto public_participant =
            std::find_if(operation->state.participants.begin(), operation->state.participants.end(),
                         [&](const auto& candidate) {
                           return candidate.participant_id == participant.slot_owner.participant_id;
                         });
        if (public_participant == operation->state.participants.end()) {
          continue;
        }
        participant.reconciliation_in_flight = true;
        std::optional<ControlBindingId> runtime_scale_control_binding_id;
        if (operation->runtime_scale_status.has_value()) {
          runtime_scale_control_binding_id =
              participant.binding.value().control_path->control_binding_id;
        }
        targets.push_back(ReconcileTarget{
            operation_id, participant.engine, participant.prepared_motion_engine,
            participant.payload_kind, participant.subordinate_id,
            participant.subordinate_generation,
            participant.binding.value().command_target_id.value(), public_participant->state,
            participant.prepared_dispatch_released, std::move(runtime_scale_control_binding_id)});
      }
    }
  }

  std::optional<ErrorDTO> first_error;
  for (const auto& target : targets) {
    Result<void> applied = Result<void>::Ok();
    if (target.public_state == OperationStateKind::kStopping || terminal(target.public_state)) {
      // StopControl and terminal settlement own these states. Nominal
      // subordinate polling must not reinterpret the backend's deliberate
      // cancellation/idle transition before the Stop certificate commits.
    } else if (target.payload_kind == OperationParticipantPayloadKind::kMotion) {
      bool dispatch_released = target.prepared_dispatch_released;
      if (target.prepared_motion_engine != nullptr && !dispatch_released) {
        applied = advance_prepared_motion(target.operation_id, target.engine);
        std::lock_guard state_lock(state_mutex_);
        const auto* operation = find_active_operation_locked(target.operation_id);
        const auto* participant =
            operation == nullptr ? nullptr : operation->participant_for(target.engine);
        dispatch_released = participant != nullptr && participant->prepared_dispatch_released;
      }
      if (!applied.ok() || !dispatch_released) {
        // Preparation is effect-free. Do not reinterpret the subordinate's
        // private Planning/Validating state as public execution progress while
        // the owner still holds the coordinated dispatch barrier.
      } else {
        applied = apply_subordinate_state(target.operation_id, target.engine, target.subordinate_id,
                                          target.public_state, 0.0, std::nullopt, true);
        if (applied.ok() && target.runtime_scale_control_binding_id.has_value()) {
          const auto scale_status = target.engine->runtime_scale_status(
              target.operation_id, *target.runtime_scale_control_binding_id);
          if (scale_status.ok()) {
            applied = apply_runtime_scale_status(target.engine, scale_status.value());
          } else {
            OperationCommitGuard commit(*this, target.operation_id);
            std::unique_lock state_lock(state_mutex_);
            auto* active_operation = find_active_operation_locked(target.operation_id);
            if (active_operation != nullptr && active_operation->runtime_scale_status.has_value()) {
              auto public_status = *active_operation->runtime_scale_status;
              public_status.state = RuntimeScaleApplicationState::kFailed;
              public_status.updated_at_ns = clock_.now();
              const auto* participant = active_operation->participant_for(target.engine);
              const auto row =
                  std::find_if(public_status.participants.begin(), public_status.participants.end(),
                               [&](const auto& item) {
                                 return participant && item.participant_id ==
                                                           participant->slot_owner.participant_id;
                               });
              if (row != public_status.participants.end()) {
                row->state = RuntimeScaleApplicationState::kFailed;
                row->error = scale_status.error();
              }
              applied = retain_runtime_scale_observation_locked(
                  state_lock, *active_operation, std::move(public_status),
                  "runtime_scale_reconcile_unavailable");
            }
          }
        }
      }
    } else if (!target.prepared_dispatch_released) {
      applied = advance_prepared_capability_command(target.operation_id, target.engine);
    } else {
      const auto status = target.engine->get_gripper_status(target.command_target);
      switch (status.state) {
        case GripperCommandState::kIdle:
          if (target.public_state == OperationStateKind::kStopped &&
              status.command_id == target.subordinate_id &&
              status.generation == target.subordinate_generation) {
            applied = Result<void>::Ok();
          } else {
            applied = apply_subordinate_state(
                target.operation_id, target.engine, target.subordinate_id,
                OperationStateKind::kInterventionRequired, status.progress,
                error("invariant_violation",
                      "accepted gripper command became idle without terminal evidence",
                      "gripper_command_lost"));
          }
          break;
        case GripperCommandState::kExecuting:
          applied = apply_subordinate_state(target.operation_id, target.engine,
                                            target.subordinate_id, OperationStateKind::kExecuting,
                                            status.progress, std::nullopt);
          break;
        case GripperCommandState::kCompleted:
          applied = apply_subordinate_state(target.operation_id, target.engine,
                                            target.subordinate_id, OperationStateKind::kCompleted,
                                            status.progress, std::nullopt);
          break;
        case GripperCommandState::kFailed:
          applied = apply_subordinate_state(
              target.operation_id, target.engine, target.subordinate_id,
              OperationStateKind::kFailed, status.progress,
              status.error.value_or(error("execution_aborted", "gripper command failed",
                                          "gripper_command_failed_without_error")));
          break;
      }
    }
    if (!applied.ok() && !first_error.has_value()) {
      first_error = applied.error();
    }
    {
      std::lock_guard state_lock(state_mutex_);
      auto* active_operation = find_active_operation_locked(target.operation_id);
      if (active_operation != nullptr) {
        auto* participant = active_operation->participant_for(target.engine);
        if (participant != nullptr) {
          participant->reconciliation_in_flight = false;
          settle_completed_control_locked(target.operation_id);
        }
      }
    }
  }
  return first_error.has_value() ? Result<void>::Err(*first_error) : Result<void>::Ok();
}

std::vector<ResourceSlotSnapshotDTO> ResourceAddressedExecutionKernel::resource_snapshot() const {
  return resource_slots_->snapshot();
}

ResourceSlotTableSnapshotDTO ResourceAddressedExecutionKernel::resource_table_snapshot() const {
  return resource_slots_->coherent_snapshot();
}

Result<OrphanResourceReconciliationResultDTO>
ResourceAddressedExecutionKernel::reconcile_orphaned_resources(
    const OrphanResourceReconciliationFactsDTO& facts) {
  if (orphan_resource_reconciliation_facts_field_error(facts).has_value()) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation facts are invalid",
        "orphan_resource_reconciliation_facts_invalid", ErrorCode::kInvalidRequest);
  }

  std::lock_guard submit_lock(submission_mutex_);
  std::lock_guard state_lock(state_mutex_);
  if (facts.request_owner_epoch != config_.request_owner_epoch ||
      facts.cell_id != config_.cell_id || !stop_retained_bundle_id_.has_value() ||
      !stop_graph_revision_.has_value() || facts.retained_bundle_id != *stop_retained_bundle_id_ ||
      facts.graph_revision != *stop_graph_revision_) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation crosses current Kernel identity",
        "orphan_resource_reconciliation_identity_mismatch", ErrorCode::kInvalidStateTransition);
  }

  const auto snapshot = resource_slots_->coherent_snapshot();
  if (snapshot.operation_resource_generation != facts.expected_operation_resource_generation) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation names a stale slot generation",
        "orphan_resource_reconciliation_generation_stale", ErrorCode::kInvalidStateTransition);
  }
  const auto configurations = resource_slots_->admission_snapshot({}).active_control_configurations;
  const auto now_ns = clock_.now();
  if (now_ns < facts.assessed_at_ns) {
    return reject<OrphanResourceReconciliationResultDTO>(
        "orphan-resource reconciliation was assessed in the future",
        "orphan_resource_reconciliation_time_invalid", ErrorCode::kInvalidRequest);
  }
  const auto elapsed_ns = now_ns - facts.assessed_at_ns;
  std::vector<ResourceId> resources;
  resources.reserve(facts.resources.size());
  for (const auto& fact : facts.resources) {
    const auto slot = std::lower_bound(
        snapshot.slots.begin(), snapshot.slots.end(), fact.resource_id,
        [](const auto& row, const ResourceId& resource) { return row.resource_id < resource; });
    if (slot == snapshot.slots.end() || slot->resource_id != fact.resource_id ||
        slot->state != ResourceSlotState::kUnreconciledExternalOrOrphanGoal ||
        slot->owner.has_value() || !fact.settlement.source_age_ns.has_value() ||
        !fact.settlement.freshness_budget_ns.has_value() ||
        elapsed_ns > *fact.settlement.freshness_budget_ns - *fact.settlement.source_age_ns) {
      return reject<OrphanResourceReconciliationResultDTO>(
          "orphan-resource reconciliation is stale or does not name a current orphan row",
          "orphan_resource_reconciliation_scope_or_freshness_changed",
          ErrorCode::kInvalidStateTransition);
    }

    const ActiveControlConfigurationDTO* active_configuration = nullptr;
    const auto readback = runtime_route_configuration_readbacks_.find(fact.control_domain_id);
    const bool has_domain_readback = readback != runtime_route_configuration_readbacks_.end();
    if (has_domain_readback &&
        readback->second.active_configuration.active_control_configuration_id ==
            fact.active_control_configuration_id &&
        readback->second.active_configuration.active_control_configuration_generation ==
            fact.active_control_configuration_generation &&
        readback->second.active_configuration.state ==
            ActiveControlConfigurationStateDTO::kReconciled) {
      active_configuration = &readback->second.active_configuration;
    }
    const std::vector<DesiredControlBindingRouteDTO>* retained_binding_routes = nullptr;
    if (!has_domain_readback) {
      for (const auto& registration : registrations_) {
        if (!registration.stop_readiness.has_value()) {
          continue;
        }
        const auto& readiness = registration.stop_readiness->value();
        const auto& expected = readiness.expected_controller_domain;
        const auto& plan = readiness.resolved_stop_base_plan;
        if (expected.control_domain_id == fact.control_domain_id &&
            plan.active_control_configuration_id == fact.active_control_configuration_id &&
            plan.active_control_configuration_generation ==
                fact.active_control_configuration_generation) {
          retained_binding_routes = &expected.desired_configuration.binding_routes;
          break;
        }
      }
    }

    bool matched_source = false;
    bool found_containing_registration = false;
    for (const auto& registration : registrations_) {
      const auto& binding = registration.binding.value();
      const bool contains = std::any_of(
          binding.resources.begin(), binding.resources.end(),
          [&](const auto& resource) { return resource.resource_id == fact.resource_id; });
      if (!contains) {
        continue;
      }
      found_containing_registration = true;
      if (!binding.control_path.has_value()) {
        return reject<OrphanResourceReconciliationResultDTO>(
            "orphan resource has no exact current control path",
            "orphan_resource_reconciliation_binding_missing", ErrorCode::kInvariantViolation);
      }
      const auto& path = *binding.control_path;
      const bool same_physical_owner =
          binding.controllable_component_id == fact.controllable_component_id &&
          binding.command_target_id == fact.command_target_id &&
          path.control_domain_id == fact.control_domain_id &&
          path.active_control_configuration_id == fact.active_control_configuration_id &&
          path.active_control_configuration_generation ==
              fact.active_control_configuration_generation &&
          path.provider_runtime_unit_id == fact.provider_runtime_unit_id &&
          path.provider_runtime_epoch == fact.provider_runtime_epoch &&
          fact.settlement.source_epoch == std::optional<RuntimeEpoch>(path.provider_runtime_epoch);
      if (!same_physical_owner) {
        return reject<OrphanResourceReconciliationResultDTO>(
            "orphan-resource reconciliation differs from a current binding",
            "orphan_resource_reconciliation_binding_mismatch", ErrorCode::kInvalidStateTransition);
      }

      const auto route_matches = [&](const auto& routes) {
        const auto route =
            std::lower_bound(routes.begin(), routes.end(), path.control_binding_id,
                             [](const auto& row, const ControlBindingId& binding_id) {
                               return row.control_binding_id < binding_id;
                             });
        return route != routes.end() && route->control_binding_id == path.control_binding_id &&
               route->control_binding_revision == path.control_binding_revision &&
               route->control_route_id == path.control_route_id;
      };
      const bool current_route =
          active_configuration != nullptr
              ? route_matches(active_configuration->binding_routes)
              : retained_binding_routes != nullptr && route_matches(*retained_binding_routes);
      if (!current_route) {
        return reject<OrphanResourceReconciliationResultDTO>(
            "orphan-resource reconciliation differs from current binding-route truth",
            "orphan_resource_reconciliation_configuration_mismatch",
            ErrorCode::kInvalidStateTransition);
      }
      matched_source =
          matched_source || (path.control_binding_id == fact.control_binding_id &&
                             path.control_binding_revision == fact.control_binding_revision);
    }
    const auto configuration =
        std::lower_bound(configurations.begin(), configurations.end(), fact.control_domain_id,
                         [](const auto& row, const ControlDomainId& domain) {
                           return row.control_domain_id < domain;
                         });
    if (!found_containing_registration || !matched_source ||
        configuration == configurations.end() ||
        configuration->control_domain_id != fact.control_domain_id ||
        configuration->active_control_configuration_id != fact.active_control_configuration_id ||
        configuration->active_control_configuration_generation !=
            fact.active_control_configuration_generation) {
      return reject<OrphanResourceReconciliationResultDTO>(
          "orphan-resource reconciliation differs from current route truth",
          "orphan_resource_reconciliation_configuration_mismatch",
          ErrorCode::kInvalidStateTransition);
    }
    resources.push_back(fact.resource_id);
  }

  auto reconciled = resource_slots_->reconcile_unreconciled_external_or_orphan_goal(
      facts.expected_operation_resource_generation, resources);
  if (!reconciled.ok()) {
    return Result<OrphanResourceReconciliationResultDTO>::Err(reconciled.error());
  }
  std::vector<ResourceId> remaining;
  for (const auto& slot : resource_slots_->snapshot()) {
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
      remaining.push_back(slot.resource_id);
    }
  }
  motion_admission_unreconciled_ = !remaining.empty();
  OrphanResourceReconciliationResultDTO result{std::move(resources), std::move(remaining),
                                               reconciled.value()};
  if (orphan_resource_reconciliation_result_field_error(result).has_value()) {
    std::terminate();
  }
  return Result<OrphanResourceReconciliationResultDTO>::Ok(std::move(result));
}

}  // namespace manyforge::core
