#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {
namespace detail {

[[nodiscard]] execution_runtime_contract::OrdinaryMotionJerkSettings
decode_ordinary_motion_jerk_settings(const core::JsonObjectDTO& source);

[[nodiscard]] std::vector<
    execution_runtime_contract::Configuration::MotionTargetLimit>
decode_execution_motion_target_limits(
    const core::JsonValueDTO::Array &motion_target_limits);

/// DX777: pneumatic dwell may be zero; position-commanded duration stays
/// positive. Shared by authored ROS validation and retained row decoding.
[[nodiscard]] bool execution_gripper_command_duration_is_valid(
    std::string_view actuation_kind, double duration_s) noexcept;

/// Authored gripper target configurations (offline mock ROS runtime
/// units), sorted by target; `label` names the configuration in
/// diagnostics.
[[nodiscard]] std::vector<
    execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &gripper_target_configurations,
    std::string_view label);

/// Shared controller selection for runtime construction, retention and fence
/// derivation after runtime configuration decoding and graph verification.
[[nodiscard]] execution_runtime_contract::Configuration
select_native_component_configuration(
    execution_runtime_contract::Configuration configuration,
    const core::CellGraphDTO &graph, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

} // namespace detail
} // namespace manyforge::deployment
