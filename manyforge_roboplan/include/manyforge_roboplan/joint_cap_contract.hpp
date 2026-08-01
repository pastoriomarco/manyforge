#pragma once

#include <map>
#include <string>

#include <roboplan/core/types.hpp>

namespace manyforge::roboplan_backend {

// Owner-neutral cap-verification seam extracted from
// roboplan_planner_backend.hpp/.cpp at parent
// c4b075fa6dfb69e463c61f3efa8cfce8de681e8a (deleted by 0822218). The future
// I6 planning adapter must integrate this seam; the old planner owner is not
// restored here.
struct JointCapMeasurement {
  bool samples_complete = true;
  std::string incomplete_dimension;
  std::string incomplete_joint;
  double worst_vel_ratio = 1.0;
  double worst_acc_ratio = 1.0;
  std::string worst_vel_joint;
  std::string worst_acc_joint;
  double worst_vel_peak = 0.0;
  double worst_acc_peak = 0.0;
};

/// Measures requested absolute caps and fails closed when a capped joint or
/// capped dimension cannot be sampled completely.
JointCapMeasurement measure_joint_cap_peaks(
    const roboplan::JointTrajectory& trajectory,
    const std::map<std::string, double>& velocity_caps_by_name,
    const std::map<std::string, double>& acceleration_caps_by_name);

}  // namespace manyforge::roboplan_backend
