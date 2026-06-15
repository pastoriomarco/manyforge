#include <manyforge_roboplan/ros2_control_capability.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

bool soft_on_path_capability_config_valid(
    const Ros2ControlExecutionBackendConfig& config) {
  if (!std::isfinite(config.soft_on_path_stop_traj_delta_s) ||
      config.soft_on_path_stop_traj_delta_s < 0.0 ||
      !std::isfinite(config.soft_on_path_sample_period_s) ||
      config.soft_on_path_sample_period_s <= 0.0) {
    return false;
  }
  if (config.soft_on_path_max_initial_path_deviation_rad.has_value() &&
      (!std::isfinite(*config.soft_on_path_max_initial_path_deviation_rad) ||
       *config.soft_on_path_max_initial_path_deviation_rad < 0.0)) {
    return false;
  }
  if (config.soft_on_path_max_initial_velocity_deviation_rad_s.has_value() &&
      (!std::isfinite(
           *config.soft_on_path_max_initial_velocity_deviation_rad_s) ||
       *config.soft_on_path_max_initial_velocity_deviation_rad_s < 0.0)) {
    return false;
  }
  for (const auto& [joint, jerk] : config.soft_on_path_max_jerk_rad_s3) {
    if (joint.empty() || !std::isfinite(jerk) || jerk <= 0.0) {
      return false;
    }
  }
  return true;
}

}  // namespace

core::MotionInterruptStopCapabilityDTO
compute_ros2_control_motion_interrupt_stop_capability(
    const Ros2ControlExecutionBackendConfig& config) {
  core::MotionInterruptStopCapabilityDTO cap;
  cap.derivative_proof_level = "none";
  cap.scaling_source_visibility =
      config.enable_jtc_speed_scaling_input ? "jtc_speed_scaling_input"
                                            : "not_configured";
  cap.scaling_source_probe_status =
      config.enable_jtc_speed_scaling_input ? "configured_not_live_probed"
                                            : "not_applicable";

  // Empty `required_joints` is a runtime-rejected misconfiguration. The
  // audit-facing helper reports it separately so deployment tools can
  // distinguish "deployment cannot start" from "deployment starts without
  // declaring a specific stop mode."
  if (config.required_joints.empty()) {
    cap.capability_profile_id = "ros2_control_v1_invalid_config";
    cap.hard_stop_level = "unavailable";
    cap.unsupported_reasons["all"] = "required_joints_empty";
    return cap;
  }

  cap.supported_stop_modes = {core::MotionInterruptStopMode::kSpringBackHold};

  bool decelerate_evidence_complete = config.decelerate_on_cancel;
  if (decelerate_evidence_complete) {
    for (const auto& joint : config.required_joints) {
      if (!config.velocity_state_interface_joints.contains(joint)) {
        decelerate_evidence_complete = false;
        break;
      }
      const auto it =
          config.max_deceleration_on_cancel_rad_s2.find(joint);
      if (it == config.max_deceleration_on_cancel_rad_s2.end() ||
          !(it->second > 0.0) || !std::isfinite(it->second)) {
        decelerate_evidence_complete = false;
        break;
      }
    }
  }
  if (decelerate_evidence_complete) {
    cap.supported_stop_modes.insert(
        core::MotionInterruptStopMode::kHardDeceleration);
    cap.capability_profile_id = "ros2_control_v1_hard_decel";
    cap.hard_stop_level = "hard_deceleration";
  } else {
    cap.capability_profile_id = "ros2_control_v1";
    cap.hard_stop_level = "jtc_cancel_unprofiled_hold";
    cap.unsupported_reasons["hard_deceleration"] =
        "decelerate_on_cancel_evidence_incomplete";
  }

  const auto append_soft_profile = [&](const std::string& suffix) {
    cap.supported_stop_modes.insert(
        core::MotionInterruptStopMode::kSoftOnPath);
    if (cap.capability_profile_id.value_or("") ==
        "ros2_control_v1_hard_decel") {
      cap.capability_profile_id = "ros2_control_v1_hard_decel_" + suffix;
    } else {
      cap.capability_profile_id = "ros2_control_v1_" + suffix;
    }
    core::MotionInterruptSoftOnPathPlannerConfigDTO soft_config;
    cap.derivative_proof_level = "manyforge_core_sampled_path_ramp";
    soft_config.stop_traj_delta_s = config.soft_on_path_stop_traj_delta_s;
    soft_config.sample_period_s = config.soft_on_path_sample_period_s;
    soft_config.max_initial_path_deviation_rad =
        config.soft_on_path_max_initial_path_deviation_rad;
    soft_config.max_initial_velocity_deviation_rad_s =
        config.soft_on_path_max_initial_velocity_deviation_rad_s;
    soft_config.per_joint_max_jerk_rad_s3 =
        config.soft_on_path_max_jerk_rad_s3;
    soft_config.allow_model_velocity_fallback =
        config.allow_soft_on_path_model_velocity_fallback;
    cap.soft_on_path_planner_config = std::move(soft_config);
  };

  // soft_on_path is NOT gated on the JTC speed-scaling configuration: when
  // speed-scaling is active the
  // backend asserts unit execution scale atomically with the preempting stop
  // dispatch (see execute_controlled_stop), so the computed wall-time stop is
  // honored regardless. The path-ramp profile is therefore advertised whenever
  // it is enabled and validly configured.
  if (config.enable_soft_on_path_path_ramp &&
      soft_on_path_capability_config_valid(config)) {
    append_soft_profile("soft_on_path_path_ramp");
  } else if (config.enable_soft_on_path_experimental &&
             soft_on_path_capability_config_valid(config)) {
    append_soft_profile("soft_on_path_experimental");
  } else if (config.enable_soft_on_path_path_ramp ||
             config.enable_soft_on_path_experimental) {
    cap.unsupported_reasons["soft_on_path"] =
        "soft_on_path_capability_config_invalid";
  } else {
    cap.unsupported_reasons["soft_on_path"] = "soft_on_path_not_enabled";
  }
  return cap;
}

core::ErrorDTO make_ros2_control_fault_error(
    core::ErrorCode code,
    std::string message,
    std::string reason_code,
    std::map<std::string, std::string> details) {
  details["backend"] = "ros2_control";
  details["reason_code"] = std::move(reason_code);
  details["surface"] = "ros2_control_execution_backend";
  return core::ErrorDTO{
      std::string(core::to_string(code)), std::move(message), std::move(details)};
}

}  // namespace manyforge::roboplan_backend
