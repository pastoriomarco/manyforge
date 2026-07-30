#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/value_schema_ref.hpp>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kRuntimeProfileApiVersion =
    "manyforge.runtime_profile.v1";
inline constexpr std::size_t kMaxRuntimeProfileRoleIds = 256U;

/// Ownership of one software runtime unit's process lifecycle.
///
/// This is deliberately unrelated to command authority, robot lifecycle, or
/// safety-stop ownership.
enum class RuntimeLifecycleControl : std::uint8_t {
  kManagerManaged = 0,
  kExternallyManaged,
};

[[nodiscard]] constexpr std::string_view to_string(
    RuntimeLifecycleControl value) noexcept {
  switch (value) {
    case RuntimeLifecycleControl::kManagerManaged:
      return "manager_managed";
    case RuntimeLifecycleControl::kExternallyManaged:
      return "externally_managed";
  }
  return {};
}

struct RuntimeProcessContractDTO {
  RuntimeLifecycleControl lifecycle_control;

  friend bool operator==(const RuntimeProcessContractDTO&,
                         const RuntimeProcessContractDTO&) = default;
};

struct RuntimeSupervisionContractDTO {
  EndpointRoleId status_endpoint_role_id;

  friend bool operator==(const RuntimeSupervisionContractDTO&,
                         const RuntimeSupervisionContractDTO&) = default;
};

/// Exact normalized payload for `manyforge.runtime_profile.v1`.
///
/// The API version is fixed by this type and `kRuntimeProfileApiVersion`;
/// it is not a caller-selectable in-memory member.
///
/// Endpoint-role IDs are profile-scoped logical source roles, never concrete
/// topic/service/action names. This declaration does not select executables,
/// images, adapters, commands, paths, topology, resources, providers,
/// dependencies, restart/freshness policy, or live process state. Those are
/// retained compiler products or Component Manager observations.
struct RuntimeProfileDTO {
  RuntimeProfileId runtime_profile_id;
  RuntimeProfileRevision runtime_profile_revision;
  ValueSchemaRefDTO configuration_schema_ref;
  RuntimeProcessContractDTO process_contract;
  RuntimeSupervisionContractDTO supervision_contract;
  std::vector<RuntimeRoleId> offered_runtime_role_ids;
  std::vector<EndpointRoleId> source_endpoint_role_ids;

  friend bool operator==(const RuntimeProfileDTO&,
                         const RuntimeProfileDTO&) = default;
};

}  // namespace manyforge::core
