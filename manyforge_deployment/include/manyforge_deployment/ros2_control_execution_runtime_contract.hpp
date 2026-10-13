#pragma once

#include <cstdint>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

class RetainedBundle;

} // namespace manyforge::deployment

namespace manyforge::deployment::ros2_control_execution_runtime_contract {

/// Build-owned identities for the retained ROS/JTC execution runtime. This is
/// a separate runtime product from native simulated execution; both reuse the
/// same Manager-owned installed-target process protocol.
inline constexpr std::string_view kProductContractId =
    "manyforge.ros2_control_execution_runtime_unit";
inline constexpr std::string_view kRuntimeProfileId =
    "manyforge.ros2_control_execution_runtime";
inline constexpr std::string_view kRuntimeRoleId = "ros2_control_execution";
/// CellId scopes this shared host role independently of robot membership.
inline constexpr std::string_view kCellHostRuntimeUnitId =
    "robot_cell.ros2_control_execution";
inline constexpr std::string_view kStatusEndpointRoleId =
    "ros2_control_execution_status";
inline constexpr std::string_view kProjectionEndpointRoleId =
    "planner_scene_projection";
inline constexpr std::string_view kLogicalSceneEndpointRoleId =
    "logical_scene_services";
inline constexpr std::string_view kImplementationId =
    "manyforge.ros2_control_execution_host";
inline constexpr std::string_view kProcessAdapterContractId =
    "manyforge.installed_execution_target";
inline constexpr std::string_view kRetrievalContractId =
    "manyforge.retained_raw_artifact";
inline constexpr std::string_view kStatusEndpointContractId =
    "manyforge.ros2_control_execution.status_direct";
inline constexpr std::string_view kProjectionEndpointContractId =
    "manyforge.planner_scene_projection.rpc";
inline constexpr std::string_view kLogicalSceneEndpointContractId =
    "manyforge.logical_scene.services";
// These are immutable public API identities, not endpoint-contract revisions.
// Their semantic owner is the ros2_control execution host.
inline constexpr std::string_view kRuntimeRoleApiContractId =
    "manyforge.api.ros2_control.execution";
inline constexpr std::string_view kStatusEndpointApiContractId =
    "manyforge.api.ros2_control.status";
inline constexpr std::string_view kProjectionEndpointApiContractId =
    "manyforge.api.ros2_control.projection";
inline constexpr std::string_view kLogicalSceneEndpointApiContractId =
    "manyforge.api.ros2_control.logical_scene";
inline constexpr std::string_view kOperatingSystem = "linux";
inline constexpr std::string_view kLaunchEnvironmentContract =
    "manyforge.ros2_control_execution_host.abi.v1";
inline constexpr std::string_view kInstalledTargetId =
    "manyforge_ros2_control_execution_host";
inline constexpr std::string_view kCandidateWorkerInstalledTargetId =
    "manyforge_roboplan_candidate_worker";

struct Configuration final {
  /// Deployment-authored fault Stop profile of one motion target, read
  /// directly as `main` did (design Q37): no derivation from the operator
  /// profile. `main`'s authored profile has no jerk field and `main` shaped
  /// every Stop ramp with its default jerk, so the decoded profile enables
  /// jerk limits with no per-joint override (default derived at Stop time).
  struct FaultStopProfile final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    core::MotionInterruptStopProfileDTO profile;

    friend bool operator==(const FaultStopProfile &,
                           const FaultStopProfile &) = default;
  };

  /// Q68: the controller profile each hosted controllable component selects
  /// inside the one ROS runtime unit (one controller manager per component,
  /// Q66). Exactly one of the lane selectors is present.
  struct ControllerProfile final {
    core::ControllableComponentId controllable_component_id;
    /// Isaac lane profile (`lite6`, `ur10e`).
    std::optional<std::string> isaac_profile;
    /// Offline mock lane profile (one per offline model family).
    std::optional<std::string> mock_profile;
    /// Q82 authored literal TF prefix; empty preserves unprefixed frames.
    std::string frame_prefix{};

    friend bool operator==(const ControllerProfile &,
                           const ControllerProfile &) = default;
  };

  /// One row per hosted controllable component, sorted by component.
  std::vector<ControllerProfile> controller_profiles;
  /// Q81: one deployment-authored logical world; machine facts stay at the
  /// site.
  std::optional<std::string> isaac_world;
  std::uint32_t planner_rng_seed{};
  std::vector<double> initial_joint_positions_rad;
  std::vector<
      native_execution_runtime_contract::Configuration::MotionTargetLimit>
      motion_target_limits;
  /// At most one per hosted motion target; absent targets have no fault
  /// Stop profile (as `main` without `default_fault_stop_profile`).
  std::vector<FaultStopProfile> default_fault_stop_profiles;
  /// Authored gripper target configurations (offline mock profiles carry the
  /// native profiles' values unchanged; Isaac profiles have none).
  std::vector<
      native_execution_runtime_contract::Configuration::GripperTargetConfiguration>
      gripper_target_configurations;
  std::uint32_t execution_tick_hz{};
  double collision_monitor_period_ms{};
  std::optional<double> runtime_collision_immediate_query_budget_ms;
  std::optional<double> runtime_collision_lookahead_query_budget_ms;
  std::optional<double> runtime_collision_lookahead_cycle_budget_ms;
  std::optional<double> runtime_collision_lookahead_s;
  std::optional<std::uint32_t> runtime_collision_lookahead_max_samples;
  std::optional<std::uint32_t> runtime_collision_lookahead_workers;
  std::optional<double> runtime_dynamic_body_max_age_cap_ms;
  std::optional<std::uint32_t> validation_collision_workers;

  friend bool operator==(const Configuration &,
                         const Configuration &) = default;
};

/// Q89: the same ROS cell host with no hosted components. Values retain the
/// established empty-host planner seed and cadence; no component is fabricated.
[[nodiscard]] CanonicalValue empty_host_configuration(
    const std::optional<std::string> &isaac_world = std::nullopt);

[[nodiscard]] CanonicalValue empty_host_deployment(
    const core::CellId &cell_id, const CanonicalValue &runtime_profile_reference,
    std::string_view deployment_id, std::string_view name,
    const std::optional<std::string> &isaac_world = std::nullopt);

[[nodiscard]] Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id);

[[nodiscard]] Configuration require_component_configuration(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

[[nodiscard]] CanonicalValue component_configuration_projection(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

/// The controller profile row of one hosted component, or none.
[[nodiscard]] const Configuration::ControllerProfile *
find_controller_profile(const Configuration &configuration,
                        const core::ControllableComponentId &component_id);

[[nodiscard]] std::string implementation_revision();

} // namespace manyforge::deployment::ros2_control_execution_runtime_contract
