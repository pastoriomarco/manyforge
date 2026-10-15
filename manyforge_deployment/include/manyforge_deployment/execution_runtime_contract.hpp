#pragma once

#include <cstdint>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/validation.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment::execution_runtime_contract {

inline constexpr std::string_view kProjectionEndpointRoleId =
    "planner_scene_projection";
inline constexpr std::string_view kLogicalSceneEndpointRoleId =
    "logical_scene_services";
inline constexpr std::string_view kProcessAdapterContractId =
    "manyforge.installed_execution_target";
inline constexpr std::string_view kRetrievalContractId =
    "manyforge.retained_raw_artifact";
inline constexpr std::string_view kProjectionEndpointContractId =
    "manyforge.planner_scene_projection.rpc";
inline constexpr std::string_view kLogicalSceneEndpointContractId =
    "manyforge.logical_scene.services";
inline constexpr std::string_view kOperatingSystem = "linux";
inline constexpr std::string_view kCandidateWorkerInstalledTargetId =
    "manyforge_roboplan_candidate_worker";
inline constexpr std::string_view kManagerLivenessPolicyId =
    "manyforge.retained_native_manager_liveness";
inline constexpr std::string_view kManagerLivenessLocalSafeContractId =
    "manyforge.native_execution.manager_loss_safe";

struct ImportedJointJerkLimit final {
  double maximum_rad_s3{};
  bool enabled{};
  bool machine_required{};
  std::string source_id;
  core::ContentDigest source_digest;
  friend bool operator==(const ImportedJointJerkLimit &,
                         const ImportedJointJerkLimit &) = default;
};

struct JointJerkSetting final {
  std::string source_joint_name;
  std::optional<double> override_rad_s3;
  std::optional<ImportedJointJerkLimit> imported;
  friend bool operator==(const JointJerkSetting &,
                         const JointJerkSetting &) = default;
};

struct OrdinaryMotionJerkSettings final {
  bool enabled{};
  double preset_reference_time_s{1.0};
  std::vector<JointJerkSetting> joint_limits;
  friend bool operator==(const OrdinaryMotionJerkSettings &,
                         const OrdinaryMotionJerkSettings &) = default;
};

/// Cold resolution against the immutable model, never a trajectory's duration.
/// Imports/overrides retain their source identity in the saved settings; the
/// returned private policy contains only the limits needed for admission.
[[nodiscard]] core::Result<core::OrdinaryMotionJerkPolicy>
resolve_ordinary_motion_jerk_policy(
    const OrdinaryMotionJerkSettings &settings,
    std::span<const core::RuntimeResourceBindingDTO> resources,
    const core::JointLimitsDTO &model_limits);

struct Configuration final {
  struct MotionTargetLimit final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    double maximum_tcp_linear_speed_mps{};
    OrdinaryMotionJerkSettings ordinary_motion_jerk{};

    friend bool operator==(const MotionTargetLimit &,
                           const MotionTargetLimit &) = default;
  };

  struct DirectControlJointLimit final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    std::string source_joint_name;
    std::optional<double> max_velocity_rad_s;
    std::optional<double> max_acceleration_rad_s2;
    std::optional<double> max_jerk_rad_s3;

    friend bool operator==(const DirectControlJointLimit &,
                           const DirectControlJointLimit &) = default;
  };

  struct GripperTargetConfiguration {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    std::string description;
    std::string actuation_kind;
    std::string open_state_ref;
    std::string closed_state_ref;
    double command_duration_s{};
    std::vector<std::string> touch_links;
    std::string mount_frame;
    std::string attach_frame;
    std::string tcp_frame;
    std::vector<std::string> auxiliary_frames;

    friend bool operator==(const GripperTargetConfiguration &,
                           const GripperTargetConfiguration &) = default;
  };

  std::uint32_t planner_rng_seed{};
  std::vector<double> initial_joint_positions_rad;
  std::uint32_t execution_tick_hz{};
  double collision_monitor_period_ms{};
  std::vector<MotionTargetLimit> motion_target_limits;
  std::vector<DirectControlJointLimit> direct_control_joint_limits;
  std::vector<GripperTargetConfiguration> gripper_target_configurations;

  friend bool operator==(const Configuration &,
                         const Configuration &) = default;
};

[[nodiscard]] const core::ManagerLivenessPolicyDTO&
manager_liveness_policy();

} // namespace manyforge::deployment::execution_runtime_contract
