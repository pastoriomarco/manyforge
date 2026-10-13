#include "manyforge_deployment/apply_affected_scope.hpp"

#include "apply_affected_scope_internal.hpp"
#include "native_execution_runtime_unit_internal.hpp"
#include <manyforge_deployment/apply_component_delta.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;

[[noreturn]] void Fail(const std::string &detail) {
  throw ApplyAffectedScopeError(detail);
}

template <typename Record, typename Id, typename Key>
[[nodiscard]] std::map<Id, const Record *>
IndexBy(const std::vector<Record> &records, Key key) {
  std::map<Id, const Record *> result;
  for (const auto &record : records) {
    const auto [_, inserted] = result.emplace(key(record), &record);
    if (!inserted) {
      Fail("verified retained bundle contains a duplicate indexed record");
    }
  }
  return result;
}

template <typename Record, typename Id, typename Key>
[[nodiscard]] std::set<Id> ChangedIds(const std::vector<Record> &predecessor,
                                      const std::vector<Record> &candidate,
                                      Key key) {
  const auto old_records = IndexBy<Record, Id>(predecessor, key);
  const auto new_records = IndexBy<Record, Id>(candidate, key);
  std::set<Id> changed;
  for (const auto &[id, record] : old_records) {
    const auto found = new_records.find(id);
    if (found == new_records.end() || *record != *found->second) {
      changed.insert(id);
    }
  }
  for (const auto &[id, record] : new_records) {
    const auto found = old_records.find(id);
    if (found == old_records.end() || *record != *found->second) {
      changed.insert(id);
    }
  }
  return changed;
}

/// Keys whose group of records differs, for tables that may hold several
/// records per key.
template <typename Record, typename Id, typename Key>
[[nodiscard]] std::set<Id> ChangedGroupedIds(const std::vector<Record> &predecessor,
                                             const std::vector<Record> &candidate,
                                             Key key) {
  std::map<Id, std::vector<const Record *>> old_groups;
  std::map<Id, std::vector<const Record *>> new_groups;
  for (const auto &record : predecessor) old_groups[key(record)].push_back(&record);
  for (const auto &record : candidate) new_groups[key(record)].push_back(&record);
  const auto same = [](const std::vector<const Record *> &lhs,
                       const std::vector<const Record *> &rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [](const Record *a, const Record *b) { return *a == *b; });
  };
  std::set<Id> changed;
  for (const auto &[id, records] : old_groups) {
    const auto found = new_groups.find(id);
    if (found == new_groups.end() || !same(records, found->second)) changed.insert(id);
  }
  for (const auto &[id, records] : new_groups) {
    if (!old_groups.contains(id)) changed.insert(id);
  }
  return changed;
}

[[nodiscard]] bool ClaimsAny(const core::ControlBindingDTO &binding,
                             const std::set<core::ResourceId> &resources) {
  return std::any_of(binding.resource_bindings.begin(),
                     binding.resource_bindings.end(),
                     [&resources](const core::ResourceBindingDTO &resource) {
                       return resources.contains(resource.resource_id);
                     });
}

void MarkBinding(const core::ControlBindingDTO &binding,
                 std::set<core::ResourceId> &resources) {
  for (const auto &resource : binding.resource_bindings) {
    resources.insert(resource.resource_id);
  }
}

void MarkAllBindings(const core::CellGraphDTO &graph,
                     std::set<core::ResourceId> &resources) {
  for (const auto &binding : graph.control_bindings) {
    MarkBinding(binding, resources);
  }
}

void MarkBindingId(const core::CellGraphDTO &graph,
                   const core::ControlBindingId &binding_id,
                   std::set<core::ResourceId> &resources) {
  for (const auto &binding : graph.control_bindings) {
    if (binding.control_binding_id == binding_id) {
      MarkBinding(binding, resources);
    }
  }
}

void MarkComponent(const core::CellGraphDTO &graph,
                   const core::ControllableComponentId &component_id,
                   std::set<core::ResourceId> &resources) {
  for (const auto &binding : graph.control_bindings) {
    if (binding.controllable_component_id == component_id) {
      MarkBinding(binding, resources);
    }
  }
}

using NativeComponentConfigurations =
    std::map<core::ControllableComponentId,
             native_execution_runtime_contract::Configuration>;

struct NativeHostConfigurations final {
  native_execution_runtime_contract::Configuration shared;
  NativeComponentConfigurations components;
};

[[nodiscard]] NativeHostConfigurations
NativeConfigurations(const core::CellGraphDTO &graph,
                     const core::RuntimeUnitDTO &unit) {
  // Sealed inputs already passed bundle/schema verification. Decode once per
  // host, then use the same ownership/coordinate selection as construction
  // and retained component identity.
  const auto configuration =
      detail::decode_native_execution_runtime_configuration(
          unit.configuration, graph, unit.runtime_unit_id);
  NativeHostConfigurations result{configuration, {}};
  result.shared.initial_joint_positions_rad.clear();
  result.shared.motion_target_limits.clear();
  result.shared.direct_control_joint_limits.clear();
  result.shared.gripper_target_configurations.clear();
  for (const auto &component : graph.controllable_components) {
    if (std::find(component.runtime_unit_ids.begin(),
                  component.runtime_unit_ids.end(), unit.runtime_unit_id) !=
        component.runtime_unit_ids.end()) {
      result.components.emplace(
          component.controllable_component_id,
          detail::select_native_component_configuration(
              configuration, graph, unit.runtime_unit_id,
              component.controllable_component_id));
    }
  }
  return result;
}

[[nodiscard]] bool MarkNativeConfigurationChanges(
    const core::CellGraphDTO &old_graph, const core::RuntimeUnitDTO &old_unit,
    const core::CellGraphDTO &new_graph, const core::RuntimeUnitDTO &new_unit,
    std::set<core::ResourceId> &resources) {
  const auto old_configurations = NativeConfigurations(old_graph, old_unit);
  const auto new_configurations = NativeConfigurations(new_graph, new_unit);
  for (const auto &[component, configuration] : old_configurations.components) {
    const auto found = new_configurations.components.find(component);
    if (found == new_configurations.components.end() ||
        found->second != configuration) {
      MarkComponent(old_graph, component, resources);
      MarkComponent(new_graph, component, resources);
    }
  }
  for (const auto &[component, configuration] : new_configurations.components) {
    static_cast<void>(configuration);
    if (!old_configurations.components.contains(component)) {
      MarkComponent(new_graph, component, resources);
    }
  }
  return old_configurations.shared != new_configurations.shared;
}

void MarkDomain(const core::CellGraphDTO &graph,
                const core::ControlDomainId &domain_id,
                std::set<core::ResourceId> &resources) {
  for (const auto &binding : graph.control_bindings) {
    if (binding.control_domain_id == domain_id) {
      MarkBinding(binding, resources);
    }
  }
}

void MarkProviderTarget(const core::CapabilityProviderBindingDTO &provider,
                        const core::CellGraphDTO &graph,
                        std::set<core::ResourceId> &resources) {
  std::visit(
      [&graph, &resources](const auto &target) {
        using Target = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<
                          Target,
                          core::CellGlobalCapabilityProviderTargetDTO>) {
          MarkAllBindings(graph, resources);
        } else if constexpr (
            std::is_same_v<Target,
                           core::TargetBindingCapabilityProviderTargetDTO>) {
          MarkBindingId(graph, target.control_binding_id, resources);
        } else {
          static_assert(
              std::is_same_v<Target,
                             core::ControlDomainCapabilityProviderTargetDTO>);
          MarkDomain(graph, target.control_domain_id, resources);
        }
      },
      provider.target);
}

[[nodiscard]] bool
ReferencesRuntimeUnit(const core::CapabilityProviderBindingDTO &provider,
                      const std::set<core::RuntimeUnitId> &runtime_units) {
  return std::any_of(
      provider.endpoint_bindings.begin(), provider.endpoint_bindings.end(),
      [&runtime_units](
          const core::CapabilityProviderEndpointBindingDTO &endpoint) {
        if (endpoint.source_kind !=
                core::CapabilityEndpointSourceKind::kRuntimeUnit ||
            !std::holds_alternative<core::RuntimeUnitId>(endpoint.source_id)) {
          return false;
        }
        return runtime_units.contains(
            std::get<core::RuntimeUnitId>(endpoint.source_id));
      });
}

void ExpandRuntimeDependents(const core::CellGraphDTO &graph,
                             std::set<core::RuntimeUnitId> &affected) {
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &runtime_unit : graph.runtime_units) {
      if (affected.contains(runtime_unit.runtime_unit_id)) {
        continue;
      }
      if (std::any_of(runtime_unit.dependency_runtime_unit_ids.begin(),
                      runtime_unit.dependency_runtime_unit_ids.end(),
                      [&affected](const core::RuntimeUnitId &dependency) {
                        return affected.contains(dependency);
                      })) {
        affected.insert(runtime_unit.runtime_unit_id);
        changed = true;
      }
    }
  }
}

void MarkRuntimeUnitDependents(const core::RetainedBundleDTO &bundle,
                               const std::set<core::RuntimeUnitId> &affected,
                               std::set<core::ResourceId> &resources) {
  for (const auto &component : bundle.cell_graph.controllable_components) {
    if (std::any_of(component.runtime_unit_ids.begin(),
                    component.runtime_unit_ids.end(),
                    [&affected](const core::RuntimeUnitId &runtime_unit_id) {
                      return affected.contains(runtime_unit_id);
                    })) {
      MarkComponent(bundle.cell_graph, component.controllable_component_id,
                    resources);
    }
  }
  for (const auto &domain : bundle.cell_graph.control_domains) {
    if (std::any_of(domain.runtime_unit_ids.begin(),
                    domain.runtime_unit_ids.end(),
                    [&affected](const core::RuntimeUnitId &runtime_unit_id) {
                      return affected.contains(runtime_unit_id);
                    })) {
      MarkDomain(bundle.cell_graph, domain.control_domain_id, resources);
    }
  }
  for (const auto &provider : bundle.capability_provider_bindings) {
    if (ReferencesRuntimeUnit(provider, affected)) {
      MarkProviderTarget(provider, bundle.cell_graph, resources);
    }
  }
}

void CloseResourceOverlap(
    const core::CellGraphDTO &predecessor, const core::CellGraphDTO &candidate,
    std::set<core::ResourceId> &resources,
    const std::vector<const core::ControlBindingDTO *> &actual_bindings) {
  bool changed = true;
  while (changed) {
    const auto previous_size = resources.size();
    for (const auto &binding : predecessor.control_bindings) {
      if (ClaimsAny(binding, resources)) {
        MarkBinding(binding, resources);
      }
    }
    for (const auto &binding : candidate.control_bindings) {
      if (ClaimsAny(binding, resources)) {
        MarkBinding(binding, resources);
      }
    }
    for (const auto *binding : actual_bindings) {
      if (ClaimsAny(*binding, resources))
        MarkBinding(*binding, resources);
    }
    changed = resources.size() != previous_size;
  }
}

} // namespace

namespace detail {

core::ConfigurationFenceScopeDTO
derive_apply_configuration_fence_scope_verified_values(
    const core::RetainedBundleDTO &old_bundle,
    const core::RetainedBundleDTO &new_bundle,
    const std::span<const core::ControllableComponentId>
        actual_restart_components,
    const std::span<const core::RetainedBundleDTO *const>
        actual_terminal_products) {
  if (old_bundle.cell_graph.cell_id != new_bundle.cell_graph.cell_id) {
    Fail("Apply affected-scope bundles belong to different cells");
  }

  const auto &old_graph = old_bundle.cell_graph;
  const auto &new_graph = new_bundle.cell_graph;
  std::set<core::ResourceId> affected_resources;

  // Retained policy records are a runtime dependency of every component
  // (see apply_component_delta.hpp), so their change fences the whole cell.
  const bool cell_wide_product_changed =
      old_bundle.compiled_products.planning_collision_model !=
          new_bundle.compiled_products.planning_collision_model ||
      old_bundle.compiled_products.symbol_role_maps !=
          new_bundle.compiled_products.symbol_role_maps ||
      old_graph.model_instances != new_graph.model_instances ||
      old_graph.component_attachments != new_graph.component_attachments ||
      old_bundle.retained_policy_records != new_bundle.retained_policy_records;
  if (cell_wide_product_changed) {
    MarkAllBindings(old_graph, affected_resources);
    MarkAllBindings(new_graph, affected_resources);
  }

  const auto changed_components =
      ChangedIds<core::ControllableComponentDTO, core::ControllableComponentId>(
          old_graph.controllable_components, new_graph.controllable_components,
          [](const core::ControllableComponentDTO &component) {
            return component.controllable_component_id;
          });
  for (const auto &id : changed_components) {
    MarkComponent(old_graph, id, affected_resources);
    MarkComponent(new_graph, id, affected_resources);
  }

  const auto changed_domains =
      ChangedIds<core::ControlDomainDTO, core::ControlDomainId>(
          old_graph.control_domains, new_graph.control_domains,
          [](const core::ControlDomainDTO &domain) {
            return domain.control_domain_id;
          });
  for (const auto &id : changed_domains) {
    MarkDomain(old_graph, id, affected_resources);
    MarkDomain(new_graph, id, affected_resources);
  }

  const auto changed_bindings =
      ChangedIds<core::ControlBindingDTO, core::ControlBindingId>(
          old_graph.control_bindings, new_graph.control_bindings,
          [](const core::ControlBindingDTO &binding) {
            return binding.control_binding_id;
          });
  for (const auto &id : changed_bindings) {
    MarkBindingId(old_graph, id, affected_resources);
    MarkBindingId(new_graph, id, affected_resources);
  }

  const auto changed_desired =
      ChangedIds<core::DesiredControlConfigurationDTO, core::ControlDomainId>(
          old_bundle.desired_control_configurations,
          new_bundle.desired_control_configurations,
          [](const core::DesiredControlConfigurationDTO &desired) {
            return desired.control_domain_id;
          });
  const auto changed_domain_products =
      ChangedIds<core::RetainedControlDomainProductsDTO, core::ControlDomainId>(
          old_bundle.compiled_products.control_domain_products,
          new_bundle.compiled_products.control_domain_products,
          [](const core::RetainedControlDomainProductsDTO &products) {
            return products.control_domain_id;
          });
  for (const auto &id : changed_desired) {
    MarkDomain(old_graph, id, affected_resources);
    MarkDomain(new_graph, id, affected_resources);
  }
  for (const auto &id : changed_domain_products) {
    MarkDomain(old_graph, id, affected_resources);
    MarkDomain(new_graph, id, affected_resources);
  }

  const auto old_providers = IndexBy<core::CapabilityProviderBindingDTO,
                                     core::CapabilityProviderBindingId>(
      old_bundle.capability_provider_bindings,
      [](const core::CapabilityProviderBindingDTO &provider) {
        return provider.capability_provider_binding_id;
      });
  const auto new_providers = IndexBy<core::CapabilityProviderBindingDTO,
                                     core::CapabilityProviderBindingId>(
      new_bundle.capability_provider_bindings,
      [](const core::CapabilityProviderBindingDTO &provider) {
        return provider.capability_provider_binding_id;
      });
  const auto changed_providers = ChangedIds<core::CapabilityProviderBindingDTO,
                                            core::CapabilityProviderBindingId>(
      old_bundle.capability_provider_bindings,
      new_bundle.capability_provider_bindings,
      [](const core::CapabilityProviderBindingDTO &provider) {
        return provider.capability_provider_binding_id;
      });
  for (const auto &id : changed_providers) {
    if (const auto found = old_providers.find(id);
        found != old_providers.end()) {
      MarkProviderTarget(*found->second, old_graph, affected_resources);
    }
    if (const auto found = new_providers.find(id);
        found != new_providers.end()) {
      MarkProviderTarget(*found->second, new_graph, affected_resources);
    }
  }

  auto affected_runtime_units =
      ChangedIds<core::RuntimeUnitDTO, core::RuntimeUnitId>(
          old_graph.runtime_units, new_graph.runtime_units,
          [](const core::RuntimeUnitDTO &runtime_unit) {
            return runtime_unit.runtime_unit_id;
          });
  std::set<core::RuntimeUnitId> stable_native_hosts;
  const auto new_runtime_units = IndexBy<core::RuntimeUnitDTO, core::RuntimeUnitId>(
      new_graph.runtime_units,
      [](const auto &unit) { return unit.runtime_unit_id; });
  for (const auto &old_unit : old_graph.runtime_units) {
    const auto found = new_runtime_units.find(old_unit.runtime_unit_id);
    if (old_unit.runtime_profile_id.value() !=
            native_execution_runtime_contract::kRuntimeProfileId ||
        found == new_runtime_units.end()) {
      continue;
    }
    const auto &new_unit = *found->second;
    if (old_unit.runtime_profile_id != new_unit.runtime_profile_id ||
        old_unit.runtime_profile_revision != new_unit.runtime_profile_revision ||
        old_unit.dependency_runtime_unit_ids != new_unit.dependency_runtime_unit_ids) {
      continue;
    }
    // A stable native host carries the union of controller settings. Only
    // changed owners are affected by that union; shared settings remain in
    // every selected configuration. Infrastructure products are handled below.
    stable_native_hosts.insert(old_unit.runtime_unit_id);
    if (!MarkNativeConfigurationChanges(old_graph, old_unit, new_graph, new_unit,
                                        affected_resources)) {
      affected_runtime_units.erase(old_unit.runtime_unit_id);
    }
    // Shared settings also affect runtime/domain/provider consumers of the
    // host; retain ordinary dependency closure for those changes.
  }
  const auto changed_runtime_products =
      ChangedIds<core::RetainedRuntimeUnitProductsDTO, core::RuntimeUnitId>(
          old_bundle.compiled_products.runtime_unit_products,
          new_bundle.compiled_products.runtime_unit_products,
          [](const core::RetainedRuntimeUnitProductsDTO &products) {
            return products.runtime_unit_id;
          });
  affected_runtime_units.insert(changed_runtime_products.begin(),
                                changed_runtime_products.end());
  const auto changed_runtime_contracts =
      ChangedGroupedIds<core::RetainedRuntimeContractRequirementDTO, core::RuntimeUnitId>(
          old_bundle.runtime_contract_requirements,
          new_bundle.runtime_contract_requirements,
          [](const core::RetainedRuntimeContractRequirementDTO &requirement) {
            return requirement.runtime_unit_id;
          });
  for (const auto &unit : changed_runtime_contracts) {
    // Native requirements are the verified aggregate of the installed
    // bindings/providers, whose own changes are already included above.
    // Generic runtimes retain whole-unit dependency closure.
    if (!stable_native_hosts.contains(unit)) {
      affected_runtime_units.insert(unit);
    }
  }
  ExpandRuntimeDependents(old_graph, affected_runtime_units);
  ExpandRuntimeDependents(new_graph, affected_runtime_units);
  MarkRuntimeUnitDependents(old_bundle, affected_runtime_units,
                            affected_resources);
  MarkRuntimeUnitDependents(new_bundle, affected_runtime_units,
                            affected_resources);

  std::vector<const core::ControlBindingDTO *> actual_bindings;
  std::map<core::ControlDomainId, const core::ControlDomainDTO *>
      actual_domains;
  std::set<core::ControllableComponentId> actual_components;
  if (actual_terminal_products.size() > kMaximumApplyComponentDeltaRows ||
      actual_restart_components.size() > kMaximumApplyComponentDeltaRows) {
    Fail("actual_installed_state_unknown: terminal source inventory exceeds "
         "its component bound");
  }
  if (!std::is_sorted(actual_restart_components.begin(),
                      actual_restart_components.end()) ||
      std::adjacent_find(actual_restart_components.begin(),
                         actual_restart_components.end()) !=
          actual_restart_components.end()) {
    Fail("actual Apply restart components are not a bounded canonical "
         "inventory");
  }
  for (const auto *source : actual_terminal_products) {
    if (!source || source->cell_graph.cell_id != old_graph.cell_id)
      Fail("actual_installed_state_unknown: terminal product has a foreign "
           "cell");
    for (const auto &binding : source->cell_graph.control_bindings) {
      if (!std::binary_search(actual_restart_components.begin(),
                              actual_restart_components.end(),
                              binding.controllable_component_id))
        continue;
      actual_components.insert(binding.controllable_component_id);
      actual_bindings.push_back(&binding);
      MarkBinding(binding, affected_resources);
      const auto domain = std::find_if(
          source->cell_graph.control_domains.begin(),
          source->cell_graph.control_domains.end(), [&](const auto &row) {
            return row.control_domain_id == binding.control_domain_id;
          });
      if (domain == source->cell_graph.control_domains.end())
        Fail("actual_installed_state_unknown: terminal source binding has no "
             "actual domain");
      const auto [found, inserted] =
          actual_domains.emplace(domain->control_domain_id, &*domain);
      if (!inserted && *found->second != *domain)
        Fail("actual_installed_state_unknown: terminal products conflict on "
             "actual domain revision");
    }
  }
  for (const auto &component : actual_restart_components) {
    const auto present = [&](const auto &graph) {
      return std::any_of(graph.controllable_components.begin(),
                         graph.controllable_components.end(),
                         [&](const auto &row) {
                           return row.controllable_component_id == component;
                         });
    };
    if (!present(old_graph) && !present(new_graph) &&
        !actual_components.contains(component)) {
      Fail("actual Apply restart component is absent from both verified "
           "products");
    }
    MarkComponent(old_graph, component, affected_resources);
    MarkComponent(new_graph, component, affected_resources);
  }
  CloseResourceOverlap(old_graph, new_graph, affected_resources,
                       actual_bindings);
  if (affected_resources.empty()) {
    return {};
  }

  auto old_bindings = IndexBy<core::ControlBindingDTO, core::ControlBindingId>(
      old_graph.control_bindings, [](const core::ControlBindingDTO &binding) {
        return binding.control_binding_id;
      });
  const auto new_bindings =
      IndexBy<core::ControlBindingDTO, core::ControlBindingId>(
          new_graph.control_bindings,
          [](const core::ControlBindingDTO &binding) {
            return binding.control_binding_id;
          });
  for (const auto *binding : actual_bindings) {
    const auto [found, inserted] =
        old_bindings.emplace(binding->control_binding_id, binding);
    if (!inserted && *found->second != *binding)
      Fail("actual_installed_state_unknown: terminal products conflict on "
           "actual binding revision");
  }
  std::set<core::ControlBindingId> selected_binding_ids;
  for (const auto &[id, binding] : old_bindings) {
    if (ClaimsAny(*binding, affected_resources)) {
      selected_binding_ids.insert(id);
    }
  }
  for (const auto &[id, binding] : new_bindings) {
    if (ClaimsAny(*binding, affected_resources)) {
      selected_binding_ids.insert(id);
    }
  }

  std::set<core::ControllableComponentId> component_ids;
  std::set<core::ControlDomainId> domain_ids;
  std::vector<core::ConfigurationFenceBindingScopeDTO> bindings;
  bindings.reserve(selected_binding_ids.size());
  for (const auto &id : selected_binding_ids) {
    const auto old_found = old_bindings.find(id);
    const auto new_found = new_bindings.find(id);
    const auto *old_binding =
        old_found == old_bindings.end() ? nullptr : old_found->second;
    const auto *new_binding =
        new_found == new_bindings.end() ? nullptr : new_found->second;
    if (old_binding != nullptr) {
      component_ids.insert(old_binding->controllable_component_id);
      domain_ids.insert(old_binding->control_domain_id);
    }
    if (new_binding != nullptr) {
      component_ids.insert(new_binding->controllable_component_id);
      domain_ids.insert(new_binding->control_domain_id);
    }
    bindings.push_back(core::ConfigurationFenceBindingScopeDTO{
        id,
        old_binding == nullptr
            ? std::nullopt
            : std::optional(old_binding->control_binding_revision),
        new_binding == nullptr
            ? std::nullopt
            : std::optional(new_binding->control_binding_revision),
    });
  }

  auto old_domains = IndexBy<core::ControlDomainDTO, core::ControlDomainId>(
      old_graph.control_domains, [](const core::ControlDomainDTO &domain) {
        return domain.control_domain_id;
      });
  const auto new_domains =
      IndexBy<core::ControlDomainDTO, core::ControlDomainId>(
          new_graph.control_domains, [](const core::ControlDomainDTO &domain) {
            return domain.control_domain_id;
          });
  for (const auto &[id, domain] : actual_domains) {
    const auto [found, inserted] = old_domains.emplace(id, domain);
    if (!inserted && *found->second != *domain)
      Fail("actual_installed_state_unknown: terminal source conflicts with the "
           "actual predecessor domain");
  }
  std::vector<core::ConfigurationFenceDomainScopeDTO> domains;
  domains.reserve(domain_ids.size());
  for (const auto &id : domain_ids) {
    const auto old_found = old_domains.find(id);
    const auto new_found = new_domains.find(id);
    domains.push_back(core::ConfigurationFenceDomainScopeDTO{
        id,
        old_found == old_domains.end()
            ? std::nullopt
            : std::optional(old_found->second->control_domain_revision),
        new_found == new_domains.end()
            ? std::nullopt
            : std::optional(new_found->second->control_domain_revision),
    });
  }

  core::ConfigurationFenceScopeDTO result{
      {component_ids.begin(), component_ids.end()},
      std::move(domains),
      std::move(bindings),
      {affected_resources.begin(), affected_resources.end()},
  };
  if (!result.valid()) {
    Fail("derived Apply affected scope is not canonical");
  }
  return result;
}

} // namespace detail

core::ConfigurationFenceScopeDTO
derive_apply_configuration_fence_scope(const RetainedBundle &predecessor,
                                       const RetainedBundle &candidate) {
  return derive_apply_configuration_fence_scope(predecessor, candidate, {});
}

core::ConfigurationFenceScopeDTO derive_apply_configuration_fence_scope(
    const RetainedBundle &predecessor, const RetainedBundle &candidate,
    const std::span<const core::ControllableComponentId>
        actual_restart_components,
    const std::span<const RetainedBundle *const> actual_terminal_products) {
  if (!verify_retained_bundle_identity(predecessor) ||
      !verify_retained_bundle_identity(candidate)) {
    Fail("Apply affected-scope input failed retained-bundle identity proof");
  }
  std::vector<const core::RetainedBundleDTO *> sources;
  sources.reserve(actual_terminal_products.size());
  for (const auto *product : actual_terminal_products) {
    if (!product || !verify_retained_bundle_identity(*product))
      Fail("actual_installed_state_unknown: terminal source product failed "
           "byte identity proof");
    sources.push_back(&product->value());
  }
  return detail::derive_apply_configuration_fence_scope_verified_values(
      predecessor.value(), candidate.value(), actual_restart_components,
      sources);
}

} // namespace manyforge::deployment
