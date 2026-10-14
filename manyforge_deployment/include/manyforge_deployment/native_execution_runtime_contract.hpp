#pragma once

#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <cstdint>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <optional>
#include <span>
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
/// The CellId scopes this infrastructure role; controller membership does not.
inline constexpr std::string_view kCellHostRuntimeUnitId =
    "robot_cell.native_execution";
/// Build-owned infrastructure configuration, with no controller-specific rows.
/// Shared by startup bootstrap and an authored empty Deployment.
[[nodiscard]] CanonicalValue empty_host_configuration();
[[nodiscard]] CanonicalValue empty_host_deployment(
    const core::CellId &cell_id, const CanonicalValue &runtime_profile_reference,
    std::string_view deployment_id, std::string_view name);
inline constexpr std::string_view kStatusEndpointRoleId =
    "native_execution_status";
using execution_runtime_contract::kProjectionEndpointRoleId;
using execution_runtime_contract::kLogicalSceneEndpointRoleId;
inline constexpr std::string_view kImplementationId =
    "manyforge.native_execution_host";
using execution_runtime_contract::kProcessAdapterContractId;
using execution_runtime_contract::kRetrievalContractId;
inline constexpr std::string_view kStatusEndpointContractId =
    "manyforge.native_execution.status_direct";
using execution_runtime_contract::kProjectionEndpointContractId;
using execution_runtime_contract::kLogicalSceneEndpointContractId;
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
using execution_runtime_contract::kOperatingSystem;
inline constexpr std::string_view kLaunchEnvironmentContract =
    "manyforge.native_execution_host.abi.v1";
inline constexpr std::string_view kInstalledTargetId =
    "manyforge_native_execution_host";
using execution_runtime_contract::kCandidateWorkerInstalledTargetId;
using execution_runtime_contract::kManagerLivenessPolicyId;
using execution_runtime_contract::kManagerLivenessLocalSafeContractId;

/// Closed, typed projection of one verified retained native-runtime unit.
///
/// The retained bundle remains the only configuration authority. Consumers
/// use this projection instead of reparsing a manifest or accepting launch-
/// time replacements for product-owned runtime semantics.
using execution_runtime_contract::ImportedJointJerkLimit;
using execution_runtime_contract::JointJerkSetting;
using execution_runtime_contract::OrdinaryMotionJerkSettings;
using execution_runtime_contract::Configuration;
using execution_runtime_contract::resolve_ordinary_motion_jerk_policy;

/// Select and decode one exact native-runtime unit from an independently
/// verified retained bundle. Throws RetainedBundleError when the unit,
/// profile, or normalized configuration differs from the build-owned row.
[[nodiscard]] Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id);

/// Select one component's controller settings using the exact motion binding
/// resource order. Rejects ambiguous ownership or incomplete aggregate state.
/// The native adapter currently supports at most one motion-execution target
/// per component. Multiple motion targets are refused, never partially selected.
[[nodiscard]] Configuration require_component_configuration(
    const RetainedBundle &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

/// The same selection in canonical form for retained controller identity.
/// Shared settings remain dependencies; unrelated target rows are excluded.
[[nodiscard]] CanonicalValue component_configuration_projection(
    const RetainedBundle &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

/// Build-owned closed policy embedded in every native runtime manifest.
///
/// The manifest artifact is part of the runtime product and therefore of the
/// retained-bundle identity. Runtime consumers receive this typed value only
/// after the exact manifest/product/bundle verification has succeeded.
using execution_runtime_contract::manager_liveness_policy;

/// Diagnostic implementation revision embedded in the retained descriptor.
/// Runtime compatibility uses the semantic contract offer, not this value.
[[nodiscard]] std::string implementation_revision();

} // namespace manyforge::deployment::native_execution_runtime_contract
