#pragma once

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

class RetainedBundle;

}

namespace manyforge::deployment::native_execution_runtime_contract {

/// Build-owned identities for the one qualified native execution runtime.
///
/// These values let the retained-product verifier, Component Manager adapter,
/// and installed execution host select the same closed contract without
/// exposing compiler IR or accepting caller-selected launch data.
inline constexpr std::string_view kProductContractId =
    "manyforge.native_execution_runtime_unit";
inline constexpr std::string_view kRuntimeProfileId =
    "manyforge.native_execution_runtime";
inline constexpr std::string_view kRuntimeRoleId = "native_execution";
inline constexpr std::string_view kStatusEndpointRoleId =
    "native_execution_status";
inline constexpr std::string_view kProjectionEndpointRoleId =
    "planner_scene_projection";
inline constexpr std::string_view kLogicalSceneEndpointRoleId =
    "logical_scene_services";
inline constexpr std::string_view kImplementationId =
    "manyforge.native_execution_host";
inline constexpr std::string_view kProcessAdapterContractId =
    "manyforge.installed_native_target";
inline constexpr std::string_view kRetrievalContractId =
    "manyforge.retained_raw_artifact";
inline constexpr std::string_view kStatusEndpointContractId =
    "manyforge.native_execution.status_direct";
inline constexpr std::string_view kProjectionEndpointContractId =
    "manyforge.planner_scene_projection.rpc";
inline constexpr std::string_view kLogicalSceneEndpointContractId =
    "manyforge.logical_scene.services";
// These are immutable public API identities, not endpoint-contract revisions.
// Their semantic owner is the native execution host.
inline constexpr std::string_view kRuntimeRoleApiContractId =
    "manyforge.api.native.execution";
inline constexpr std::string_view kStatusEndpointApiContractId =
    "manyforge.api.native.status";
inline constexpr std::string_view kProjectionEndpointApiContractId =
    "manyforge.api.native.projection";
inline constexpr std::string_view kLogicalSceneEndpointApiContractId =
    "manyforge.api.native.logical_scene";
inline constexpr std::string_view kOperatingSystem = "linux";
inline constexpr std::string_view kLaunchEnvironmentContract =
    "manyforge.native_execution_host.abi.v1";
inline constexpr std::string_view kInstalledTargetId =
    "manyforge_native_execution_host";
inline constexpr std::string_view kCandidateWorkerInstalledTargetId =
    "manyforge_roboplan_candidate_worker";

/// Closed, typed projection of one verified retained native-runtime unit.
///
/// The retained bundle remains the only configuration authority. Consumers
/// use this projection instead of reparsing a manifest or accepting launch-
/// time replacements for product-owned runtime semantics.
struct Configuration final {
  std::uint32_t planner_rng_seed{};
  std::vector<double> initial_joint_positions_rad;
  std::uint32_t execution_tick_hz{};
  double collision_monitor_period_ms{};

  friend bool operator==(const Configuration &,
                         const Configuration &) = default;
};

/// Select and decode one exact native-runtime unit from an independently
/// verified retained bundle. Throws RetainedBundleError when the unit,
/// profile, or normalized configuration differs from the build-owned row.
[[nodiscard]] Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id);

/// Revision embedded independently in the retained descriptor and installed
/// host. A Manager child handshake must match this build-owned value.
[[nodiscard]] std::string implementation_revision();

} // namespace manyforge::deployment::native_execution_runtime_contract
