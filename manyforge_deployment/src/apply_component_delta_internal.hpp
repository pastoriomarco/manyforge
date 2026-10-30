#pragma once

#include <set>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>

namespace manyforge::deployment::detail {

/// The typed facts one component's runtime depends on in one retained bundle
/// (one Apply side). Uninstalled; exposed for the partial-Apply tests.
struct ApplyComponentRelations final {
  std::set<core::ModelInstanceId> model_instances;
  std::set<core::ControlBindingId> bindings;
  std::set<core::ControlDomainId> domains;
  std::set<core::RuntimeUnitId> runtime_units;
  std::set<core::CapabilityProviderBindingId> providers;

  friend bool operator==(const ApplyComponentRelations &,
                         const ApplyComponentRelations &) = default;
};

/// `index` is built from `bundle.cell_graph` (the same Apply side).
[[nodiscard]] ApplyComponentRelations
apply_component_relations(const core::RetainedBundleDTO &bundle,
                          const CellKinematicIndex &index,
                          const core::ControllableComponentDTO &component);

} // namespace manyforge::deployment::detail
