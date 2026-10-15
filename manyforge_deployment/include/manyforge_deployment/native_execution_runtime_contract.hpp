#pragma once

#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment {
class RetainedBundle;
}

namespace manyforge::deployment::native_execution_runtime_contract {
using execution_runtime_contract::Configuration;
// DX753: preserve the shared cold-law type used by the selected-ROS jerk test.
using execution_runtime_contract::OrdinaryMotionJerkSettings;
using execution_runtime_contract::ImportedJointJerkLimit;
// DX-753: retain this identity for the compiled, held Apply-scope branch.
inline constexpr std::string_view kRuntimeProfileId =
    "manyforge.native_execution_runtime";
// DX-753: preserve the compiled, held per-component identity projection.
[[nodiscard]] Configuration require_configuration(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id);
[[nodiscard]] Configuration require_component_configuration(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);
[[nodiscard]] CanonicalValue component_configuration_projection(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);
} // namespace manyforge::deployment::native_execution_runtime_contract
