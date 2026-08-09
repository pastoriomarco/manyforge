#pragma once

#include <string>
#include <string_view>

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
inline constexpr std::string_view kOperatingSystem = "linux";
inline constexpr std::string_view kLaunchEnvironmentContract =
    "manyforge.native_execution_host.abi.v1";
inline constexpr std::string_view kInstalledTargetId =
    "manyforge_native_execution_host";

/// Revision embedded independently in the retained descriptor and installed
/// host. A Manager child handshake must match this build-owned value.
[[nodiscard]] std::string implementation_revision();

} // namespace manyforge::deployment::native_execution_runtime_contract
