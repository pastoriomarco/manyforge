#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <roboplan/core/scene.hpp>
#include <roboplan/core/path_utils.hpp>
#include <roboplan/core/types.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_toppra/toppra.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

#include <manyforge_core/ports/iplanner_backend.hpp>

namespace manyforge::roboplan_backend {

/// Result of sampling per-joint peak velocity/acceleration against requested
/// absolute caps (Phase 4-P5.1K). Consumed by the conservative retiming loop in
/// RoboPlanPlannerBackend::plan and exercised directly by unit tests.
struct JointCapMeasurement {
  /// False when a capped dimension lacked a complete set of samples for a capped
  /// joint (the cap cannot be verified, so the caller MUST fail closed rather
  /// than treat the missing peak as zero). `incomplete_dimension` /
  /// `incomplete_joint` name the first offending case.
  bool samples_complete = true;
  std::string incomplete_dimension;  // "velocity" | "acceleration"
  std::string incomplete_joint;
  /// Worst measured/cap ratio per dimension across capped joints; > 1.0 means a
  /// cap is still violated on this pass. 1.0 when satisfied (or no cap).
  double worst_vel_ratio = 1.0;
  double worst_acc_ratio = 1.0;
  std::string worst_vel_joint;
  std::string worst_acc_joint;
  double worst_vel_peak = 0.0;  // rad/s
  double worst_acc_peak = 0.0;  // rad/s^2
};

/// Sample peak |velocity| / |acceleration| per capped joint from `traj` and
/// compare against the requested absolute caps. Caps are keyed by joint name;
/// only joints present in `traj.joint_names` are measured. If a capped
/// dimension is missing a sample for any waypoint of a capped joint, the
/// returned measurement has `samples_complete == false` and records the
/// offending dimension/joint (the trajectory cannot prove cap compliance).
JointCapMeasurement measure_joint_cap_peaks(
    const roboplan::JointTrajectory& traj,
    const std::map<std::string, double>& vel_cap_by_name,
    const std::map<std::string, double>& acc_cap_by_name);

/// Native C++ planner backend wrapping RoboPlan RRT + PathShortcutter + TOPPRA + IK.
/// No Python trampolines — all calls stay in C++.
class RoboPlanPlannerBackend final : public core::IPlannerBackend {
 public:
  RoboPlanPlannerBackend(std::shared_ptr<roboplan::Scene> scene,
                         const roboplan::RRTOptions& rrt_options,
                         std::shared_ptr<roboplan::PathShortcutter> shortcutter,
                         std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra,
                         std::shared_ptr<roboplan::SimpleIk> ik,
                         std::shared_ptr<std::mutex> scene_mutex = nullptr,
                         std::shared_ptr<roboplan::SimpleIk> single_shot_ik = nullptr,
                         std::optional<roboplan::SimpleIkOptions> ik_options = std::nullopt,
                         std::optional<roboplan::SimpleIkOptions> single_shot_ik_options =
                             std::nullopt);

  core::PlanResultDTO plan(const core::PlanRequestDTO& request) override;

  void request_cancel() override;

  core::IkResultDTO solve_ik(const core::IkRequestDTO& request) override;

 private:
  core::TrajectoryDTO convert_trajectory(const roboplan::JointTrajectory& traj);

  std::shared_ptr<roboplan::Scene> scene_;
  roboplan::RRTOptions rrt_options_;
  std::shared_ptr<roboplan::PathShortcutter> shortcutter_;
  std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra_;
  std::shared_ptr<roboplan::SimpleIk> ik_;
  std::shared_ptr<roboplan::SimpleIk> single_shot_ik_;
  std::shared_ptr<std::mutex> scene_mutex_;
  roboplan::SimpleIkOptions ik_options_;
  roboplan::SimpleIkOptions single_shot_ik_options_;
  std::atomic_bool cancel_requested_{false};
};

}  // namespace manyforge::roboplan_backend
