#include "manyforge_deployment/apply_affected_scope.hpp"

#include "apply_affected_scope_internal.hpp"

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

void CloseResourceOverlap(const core::CellGraphDTO &predecessor,
                          const core::CellGraphDTO &candidate,
                          std::set<core::ResourceId> &resources) {
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
    changed = resources.size() != previous_size;
  }
}

} // namespace

namespace detail {

core::ConfigurationFenceScopeDTO
derive_apply_configuration_fence_scope_verified_values(
    const core::RetainedBundleDTO &old_bundle,
    const core::RetainedBundleDTO &new_bundle) {
  if (old_bundle.cell_graph.cell_id != new_bundle.cell_graph.cell_id) {
    Fail("Apply affected-scope bundles belong to different cells");
  }

  const auto &old_graph = old_bundle.cell_graph;
  const auto &new_graph = new_bundle.cell_graph;
  std::set<core::ResourceId> affected_resources;

  const bool cell_wide_product_changed =
      old_bundle.compiled_products.planning_collision_model !=
          new_bundle.compiled_products.planning_collision_model ||
      old_bundle.compiled_products.symbol_role_maps !=
          new_bundle.compiled_products.symbol_role_maps ||
      old_graph.model_instances != new_graph.model_instances ||
      old_graph.component_attachments != new_graph.component_attachments;
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
  const auto changed_runtime_products =
      ChangedIds<core::RetainedRuntimeUnitProductsDTO, core::RuntimeUnitId>(
          old_bundle.compiled_products.runtime_unit_products,
          new_bundle.compiled_products.runtime_unit_products,
          [](const core::RetainedRuntimeUnitProductsDTO &products) {
            return products.runtime_unit_id;
          });
  affected_runtime_units.insert(changed_runtime_products.begin(),
                                changed_runtime_products.end());
  ExpandRuntimeDependents(old_graph, affected_runtime_units);
  ExpandRuntimeDependents(new_graph, affected_runtime_units);
  MarkRuntimeUnitDependents(old_bundle, affected_runtime_units,
                            affected_resources);
  MarkRuntimeUnitDependents(new_bundle, affected_runtime_units,
                            affected_resources);

  CloseResourceOverlap(old_graph, new_graph, affected_resources);
  if (affected_resources.empty()) {
    return {};
  }

  const auto old_bindings =
      IndexBy<core::ControlBindingDTO, core::ControlBindingId>(
          old_graph.control_bindings,
          [](const core::ControlBindingDTO &binding) {
            return binding.control_binding_id;
          });
  const auto new_bindings =
      IndexBy<core::ControlBindingDTO, core::ControlBindingId>(
          new_graph.control_bindings,
          [](const core::ControlBindingDTO &binding) {
            return binding.control_binding_id;
          });
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

  const auto old_domains =
      IndexBy<core::ControlDomainDTO, core::ControlDomainId>(
          old_graph.control_domains, [](const core::ControlDomainDTO &domain) {
            return domain.control_domain_id;
          });
  const auto new_domains =
      IndexBy<core::ControlDomainDTO, core::ControlDomainId>(
          new_graph.control_domains, [](const core::ControlDomainDTO &domain) {
            return domain.control_domain_id;
          });
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
  if (!verify_retained_bundle_identity(predecessor) ||
      !verify_retained_bundle_identity(candidate)) {
    Fail("Apply affected-scope input failed retained-bundle identity proof");
  }
  return detail::derive_apply_configuration_fence_scope_verified_values(
      predecessor.value(), candidate.value());
}

} // namespace manyforge::deployment
