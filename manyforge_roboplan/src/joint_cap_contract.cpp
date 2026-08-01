#include <manyforge_roboplan/joint_cap_contract.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace manyforge::roboplan_backend {

JointCapMeasurement measure_joint_cap_peaks(
    const roboplan::JointTrajectory& traj,
    const std::map<std::string, double>& vel_cap_by_name,
    const std::map<std::string, double>& acc_cap_by_name) {
  JointCapMeasurement m;
  const std::size_t n = traj.times.size();
  // Every capped joint MUST be present in the trajectory's joint set, otherwise
  // its cap is never measured and would falsely report as satisfied. A capped
  // joint absent from the trajectory cannot be verified -> fail closed. (The
  // planner also rejects non-active-group cap names up front; this is the
  // measurement-layer backstop against any trajectory/group-name divergence.)
  auto trajectory_has_joint = [&](const std::string& jn) {
    return std::find(traj.joint_names.begin(), traj.joint_names.end(), jn) !=
           traj.joint_names.end();
  };
  for (const auto& [jn, cap] : vel_cap_by_name) {
    (void)cap;
    if (!trajectory_has_joint(jn)) {
      m.samples_complete = false;
      m.incomplete_dimension = "velocity";
      m.incomplete_joint = jn;
      return m;
    }
  }
  for (const auto& [jn, cap] : acc_cap_by_name) {
    (void)cap;
    if (!trajectory_has_joint(jn)) {
      m.samples_complete = false;
      m.incomplete_dimension = "acceleration";
      m.incomplete_joint = jn;
      return m;
    }
  }
  for (std::size_t j = 0; j < traj.joint_names.size(); ++j) {
    const std::string& jn = traj.joint_names[j];
    const auto v_it = vel_cap_by_name.find(jn);
    const auto a_it = acc_cap_by_name.find(jn);
    const bool want_v = v_it != vel_cap_by_name.end();
    const bool want_a = a_it != acc_cap_by_name.end();
    if (!want_v && !want_a) {
      continue;  // joint not capped by this deployment (e.g. other group)
    }
    // A capped joint with no waypoints cannot be verified -> fail closed.
    if (n == 0) {
      m.samples_complete = false;
      m.incomplete_dimension = want_v ? "velocity" : "acceleration";
      m.incomplete_joint = jn;
      return m;
    }
    double peak_v = 0.0;
    double peak_a = 0.0;
    for (std::size_t w = 0; w < n; ++w) {
      if (want_v) {
        if (w >= traj.velocities.size() ||
            j >= static_cast<std::size_t>(traj.velocities[w].size())) {
          m.samples_complete = false;
          m.incomplete_dimension = "velocity";
          m.incomplete_joint = jn;
          return m;
        }
        peak_v = std::max(peak_v, std::abs(traj.velocities[w][j]));
      }
      if (want_a) {
        if (w >= traj.accelerations.size() ||
            j >= static_cast<std::size_t>(traj.accelerations[w].size())) {
          m.samples_complete = false;
          m.incomplete_dimension = "acceleration";
          m.incomplete_joint = jn;
          return m;
        }
        peak_a = std::max(peak_a, std::abs(traj.accelerations[w][j]));
      }
    }
    if (want_v && peak_v > v_it->second) {
      const double r = peak_v / v_it->second;
      if (r > m.worst_vel_ratio) {
        m.worst_vel_ratio = r;
        m.worst_vel_joint = jn;
        m.worst_vel_peak = peak_v;
      }
    }
    if (want_a && peak_a > a_it->second) {
      const double r = peak_a / a_it->second;
      if (r > m.worst_acc_ratio) {
        m.worst_acc_ratio = r;
        m.worst_acc_joint = jn;
        m.worst_acc_peak = peak_a;
      }
    }
  }
  return m;
}


}  // namespace manyforge::roboplan_backend
