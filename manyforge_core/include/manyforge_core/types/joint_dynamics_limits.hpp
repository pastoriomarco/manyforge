#pragma once

#include <map>
#include <optional>
#include <string>

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
};

/// Per-joint dynamics limits keyed by canonical drivable joint name. An empty
/// map preserves prior behavior (no per-joint caps from this request).
using PerJointDynamicsLimitsDTO = std::map<std::string, JointDynamicsLimitDTO>;

}  // namespace manyforge::core
