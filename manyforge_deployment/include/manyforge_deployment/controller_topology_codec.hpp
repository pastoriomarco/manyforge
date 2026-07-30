#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/controller_topology.hpp>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/control_interface_contract_codec.hpp"

namespace manyforge::deployment {

class ControllerTopologyCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A reusable controller-topology declaration whose exact build-qualified
/// schema, semantic identity, typed catalog referent, contract dependencies,
/// and declaration-local graph invariants have been verified.
///
/// Qualification does not choose concrete controller plugins, configuration,
/// generated names, runtime endpoints, active state, or a control-domain
/// instance. Those are retained-product and runtime responsibilities.
class QualifiedControllerTopology final {
 public:
  [[nodiscard]] const core::ControllerTopologyDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  QualifiedControllerTopology(core::ControllerTopologyDTO value,
                              CanonicalValue normalized_root)
      : value_(std::move(value)),
        normalized_root_(std::move(normalized_root)) {}

  core::ControllerTopologyDTO value_;
  CanonicalValue normalized_root_;

  friend QualifiedControllerTopology resolve_and_qualify_controller_topology(
      const core::ControllerTopologyCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view,
      std::span<const QualifiedControlInterfaceContract>);
};

/// Resolves and qualifies one exact user-authored controller-topology
/// declaration.
///
/// `qualified_contracts` is the caller-prefiltered, bounded typed dependency
/// closure for this topology (at most 256 qualified referents). Raw contract
/// DTOs and open maps are intentionally not accepted at this trust boundary.
/// Every contract selector must resolve in this closure, so the same bound
/// also limits the topology-wide union of distinct selected contracts.
[[nodiscard]] QualifiedControllerTopology
resolve_and_qualify_controller_topology(
    const core::ControllerTopologyCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes,
    std::span<const QualifiedControlInterfaceContract> qualified_contracts);

}  // namespace manyforge::deployment
