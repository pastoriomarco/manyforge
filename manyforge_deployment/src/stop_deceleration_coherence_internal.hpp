#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <manyforge_deployment/model_source.hpp>

namespace manyforge::deployment::detail {

/// The per-joint acceleration ceiling the planner enforces for ordinary
/// motion: the robot model's URDF `<limit acceleration>` for that joint.
///
/// This is the exact value the native planner Scene imports for the joint
/// (RoboPlanJointLimitsFromRetainedModel). Per-motion dynamics caps and the
/// Program `acceleration_scale` (<= 1) can only lower it, so it is the largest
/// acceleration ordinary planning may command. Returns nullopt when the model
/// declares no acceleration for the joint, or the joint is absent, mimic, or
/// not a single-coordinate movable joint; the planner then has no finite
/// acceleration limit and there is nothing to compare against.
[[nodiscard]] std::optional<double> planning_joint_acceleration_limit_rad_s2(
    const UrdfDocumentIr &urdf, std::string_view joint_name);

/// Operator-readable refusal when a Stop profile's per-joint deceleration
/// limit is lower than the joint's normal-motion planning acceleration limit.
///
/// Equal is coherent. Returns nullopt when coherent or when no planning
/// acceleration limit is known. Never adjusts either value.
[[nodiscard]] std::optional<std::string> stop_deceleration_coherence_error(
    std::string_view stop_profile_label, std::string_view joint_name,
    double stop_max_deceleration_rad_s2,
    std::optional<double> planning_acceleration_limit_rad_s2);

} // namespace manyforge::deployment::detail
