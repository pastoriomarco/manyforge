#pragma once

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
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
inline constexpr std::string_view kStatusEndpointRoleId =
    "ros2_control_execution_status";
inline constexpr std::string_view kProjectionEndpointRoleId =
    "planner_scene_projection";
inline constexpr std::string_view kLogicalSceneEndpointRoleId =
    "logical_scene_services";
inline constexpr std::string_view kImplementationId =
    "manyforge.ros2_control_execution_host";
inline constexpr std::string_view kProcessAdapterContractId =
    "manyforge.installed_native_target";
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
  std::string isaac_profile;
  std::uint32_t planner_rng_seed{};
  std::vector<double> initial_joint_positions_rad;
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

[[nodiscard]] Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id);

[[nodiscard]] std::string implementation_revision();

} // namespace manyforge::deployment::ros2_control_execution_runtime_contract
