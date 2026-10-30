#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {
namespace detail {

// DX-753: archival profile identity for held scope and shared cold-input decoding.
inline constexpr std::string_view kNativeExecutionRuntimeProfileId =
    native_execution_runtime_contract::kRuntimeProfileId;

// DX-753: archival Draft/import profile metadata; no live runtime admission.
inline constexpr std::string_view kNativeExecutionRuntimeConfigurationSchemaId =
    "manyforge.value_schema.runtime.native_execution.v1";
inline constexpr std::string_view kNativeExecutionRuntimeConfigurationSchemaRevision =
    "sha256:19db78f3505f27815b6a90556de58caa0541c91193ead9b6a643f475383d5190";
inline constexpr std::string_view kNativeExecutionRuntimeRoleId = "native_execution";
inline constexpr std::string_view kNativeExecutionRuntimeStatusEndpointRoleId =
    "native_execution_status";
inline constexpr std::string_view kNativeExecutionRuntimeProjectionEndpointRoleId =
    execution_runtime_contract::kProjectionEndpointRoleId;
inline constexpr std::string_view kNativeExecutionRuntimeLogicalSceneEndpointRoleId =
    execution_runtime_contract::kLogicalSceneEndpointRoleId;
[[nodiscard]] std::string native_execution_runtime_profile_document();
[[nodiscard]] std::string native_execution_runtime_profile_revision();

[[nodiscard]] execution_runtime_contract::OrdinaryMotionJerkSettings
decode_ordinary_motion_jerk_settings(const core::JsonObjectDTO& source);

[[nodiscard]] std::vector<
    execution_runtime_contract::Configuration::MotionTargetLimit>
decode_execution_motion_target_limits(
    const core::JsonValueDTO::Array &motion_target_limits,
    bool include_ordinary_jerk);

/// DX777: pneumatic dwell may be zero; position-commanded duration stays
/// positive. Shared by authored ROS validation and retained row decoding.
[[nodiscard]] bool execution_gripper_command_duration_is_valid(
    std::string_view actuation_kind, double duration_s) noexcept;

/// Authored gripper target configurations (native and offline mock ROS
/// runtime units), sorted by target; `label` names the configuration in
/// diagnostics.
[[nodiscard]] std::vector<
    execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &gripper_target_configurations,
    std::string_view label);

// DX-753: retained schema guard for held per-component identity projection.
[[nodiscard]] bool native_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept;

// DX-753: retained only for the compiled, held per-component Apply scope.
[[nodiscard]] execution_runtime_contract::Configuration
decode_native_execution_runtime_configuration(
    const core::JsonObjectDTO &configuration, const core::CellGraphDTO &graph,
    const core::RuntimeUnitId &runtime_unit_id);

/// Shared controller selection for runtime construction, retention and fence
/// derivation after runtime configuration decoding and graph verification.
[[nodiscard]] execution_runtime_contract::Configuration
select_native_component_configuration(
    execution_runtime_contract::Configuration configuration,
    const core::CellGraphDTO &graph, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

} // namespace detail
} // namespace manyforge::deployment
