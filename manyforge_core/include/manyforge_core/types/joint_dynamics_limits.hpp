#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Per-joint absolute dynamics cap for one joint (spec/210 §3.3, Phase 4-P5.1K).
///
/// Each field is an ABSOLUTE cap in SI units, or std::nullopt meaning "no cap
/// from this source for this dimension". The kernel resolves the effective cap
/// as the conservative minimum across model/backend/controller/deployment
/// sources. Velocity and acceleration are enforced in TOPP-RA
/// time-parameterization; jerk is carried but only enforceable where the active
/// predictor constrains jerk (the discrete TOPP-RA path rejects a jerk cap as
/// unsupported rather than claiming enforcement). All present values must be
/// finite and strictly positive.
struct JointDynamicsLimitDTO {
  std::optional<double> max_velocity_rad_s;       // > 0, optional
  std::optional<double> max_acceleration_rad_s2;  // > 0, optional
  std::optional<double> max_jerk_rad_s3;          // > 0, optional
  friend bool operator==(const JointDynamicsLimitDTO&,
                         const JointDynamicsLimitDTO&) = default;
};

/// Per-joint dynamics limits keyed by canonical drivable joint name. An empty
/// map preserves prior behavior (no per-joint caps from this request).
using PerJointDynamicsLimitsDTO = std::map<std::string, JointDynamicsLimitDTO>;

/// Kernel-produced ordered cap binding for the exact all-angular planning
/// resource projection. Generated names are paired with their compiler-owned
/// ResourceId and cannot be supplied independently by public intent.
struct ResolvedResourceDynamicsLimitDTO final {
  ResourceId resource_id;
  std::string compiled_joint_name;
  JointDynamicsLimitDTO limit;

  friend bool operator==(const ResolvedResourceDynamicsLimitDTO&,
                         const ResolvedResourceDynamicsLimitDTO&) = default;
};

using PerResourceDynamicsLimitsDTO =
    std::vector<ResolvedResourceDynamicsLimitDTO>;

}  // namespace manyforge::core
