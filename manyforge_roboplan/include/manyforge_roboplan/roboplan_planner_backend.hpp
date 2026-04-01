#pragma once

#include <memory>

#include <roboplan/core/scene.hpp>
#include <roboplan/core/path_utils.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_toppra/toppra.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

#include <manyforge_core/ports/iplanner_backend.hpp>

namespace manyforge::roboplan_backend {

/// Native C++ planner backend wrapping RoboPlan RRT + PathShortcutter + TOPPRA + IK.
/// No Python trampolines — all calls stay in C++.
class RoboPlanPlannerBackend final : public core::IPlannerBackend {
 public:
  RoboPlanPlannerBackend(std::shared_ptr<roboplan::Scene> scene,
                         std::shared_ptr<roboplan::RRT> rrt,
                         std::shared_ptr<roboplan::PathShortcutter> shortcutter,
                         std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra,
                         std::shared_ptr<roboplan::SimpleIk> ik);

  core::PlanResultDTO plan(const core::PlanRequestDTO& request) override;

 private:
  core::TrajectoryDTO convert_trajectory(const roboplan::JointTrajectory& traj);

  std::shared_ptr<roboplan::Scene> scene_;
  std::shared_ptr<roboplan::RRT> rrt_;
  std::shared_ptr<roboplan::PathShortcutter> shortcutter_;
  std::shared_ptr<roboplan::PathParameterizerTOPPRA> toppra_;
  std::shared_ptr<roboplan::SimpleIk> ik_;
};

}  // namespace manyforge::roboplan_backend
