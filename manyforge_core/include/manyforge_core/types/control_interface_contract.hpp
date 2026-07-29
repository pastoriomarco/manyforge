#pragma once

#include <cstddef>
#include <manyforge_core/types/strong_id.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kControlInterfaceContractApiVersion =
    "manyforge.control_interface_contract.v1";
inline constexpr std::size_t kMaxControlInterfaceContractItems = 4096U;

/// One authored command- or state-interface claim for an abstract resource
/// role. Concrete deployment ResourceId values are bound later by the
/// deployment compiler.
struct ResourceInterfaceClaimDTO {
  ResourceRoleId resource_role_id;
  std::string interface_name;

  friend bool operator==(const ResourceInterfaceClaimDTO&,
                         const ResourceInterfaceClaimDTO&) = default;
};

/// Exact normalized payload for
/// `manyforge.control_interface_contract.v1`.
///
/// The API version is fixed by the C++ type and
/// `kControlInterfaceContractApiVersion`. All three vectors preserve authored
/// semantic order.
struct ControlInterfaceContractDTO {
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  std::vector<ResourceInterfaceClaimDTO> command_interfaces;
  std::vector<ResourceInterfaceClaimDTO> state_interfaces;
  std::vector<ResourceRoleId> ordering;

  friend bool operator==(const ControlInterfaceContractDTO&,
                         const ControlInterfaceContractDTO&) = default;
};

}  // namespace manyforge::core
