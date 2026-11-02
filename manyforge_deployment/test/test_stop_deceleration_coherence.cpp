#include <gtest/gtest.h>

#include "stop_deceleration_coherence_internal.hpp"

namespace {
namespace deployment = manyforge::deployment;
using deployment::detail::planning_joint_acceleration_limit_rad_s2;
using deployment::detail::stop_deceleration_coherence_error;

deployment::UrdfJointIr joint(std::string name,
                              std::optional<double> acceleration,
                              deployment::UrdfJointType type =
                                  deployment::UrdfJointType::kRevolute) {
  deployment::UrdfJointIr result{};
  result.name = std::move(name);
  result.type = type;
  result.coordinate_kind = deployment::UrdfCoordinateKind::kAngular;
  result.parent_link_name = "parent";
  result.child_link_name = "child";
  deployment::UrdfLimitIr limit{};
  limit.velocity = 2.0;
  limit.acceleration = acceleration;
  result.limit = limit;
  return result;
}

TEST(StopDecelerationCoherence, PlanningLimitIsTheModelJointAcceleration) {
  deployment::UrdfDocumentIr urdf{};
  urdf.joints.push_back(joint("joint1", 10.0));
  urdf.joints.push_back(joint("joint2", std::nullopt));
  auto mimic = joint("joint3", 7.0);
  mimic.mimic = deployment::UrdfMimicIr{"joint1", {}, {}, 1.0, 0.0};
  urdf.joints.push_back(mimic);
  urdf.joints.push_back(joint("joint4", 9.0, deployment::UrdfJointType::kFixed));

  EXPECT_EQ(planning_joint_acceleration_limit_rad_s2(urdf, "joint1"), 10.0);
  // No declared acceleration: the planner has no finite limit to compare.
  EXPECT_EQ(planning_joint_acceleration_limit_rad_s2(urdf, "joint2"),
            std::nullopt);
  EXPECT_EQ(planning_joint_acceleration_limit_rad_s2(urdf, "joint3"),
            std::nullopt);
  EXPECT_EQ(planning_joint_acceleration_limit_rad_s2(urdf, "joint4"),
            std::nullopt);
  EXPECT_EQ(planning_joint_acceleration_limit_rad_s2(urdf, "missing"),
            std::nullopt);
}

TEST(StopDecelerationCoherence, AcceptsEqualAndHigherDeceleration) {
  EXPECT_EQ(stop_deceleration_coherence_error("robot.arm.controlled_stop",
                                              "joint1", 10.0, 10.0),
            std::nullopt);
  EXPECT_EQ(stop_deceleration_coherence_error("robot.arm.controlled_stop",
                                              "joint1", 12.5, 10.0),
            std::nullopt);
  EXPECT_EQ(stop_deceleration_coherence_error("robot.arm.controlled_stop",
                                              "joint1", 0.5, std::nullopt),
            std::nullopt);
}

TEST(StopDecelerationCoherence, RefusesLowerDecelerationNamingEverything) {
  const auto error = stop_deceleration_coherence_error(
      "lite6_robot.arm.controlled_stop", "joint2", 4.0, 10.0);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(*error,
            "Stop profile lite6_robot.arm.controlled_stop, joint joint2: Stop "
            "deceleration limit 4 rad/s^2 (max_deceleration_rad_s2) is lower "
            "than the normal-motion planning acceleration limit 10 rad/s^2 "
            "(robot model joint acceleration limit). A Stop must be able to "
            "decelerate at least as hard as normal motion accelerates; set the "
            "Stop deceleration for joint2 to at least 10 rad/s^2");

  const auto fractional = stop_deceleration_coherence_error(
      "robot.arm.controlled_stop", "joint1", 9.99, 10.0);
  ASSERT_TRUE(fractional.has_value());
  EXPECT_NE(fractional->find("Stop deceleration limit 9.99 rad/s^2"),
            std::string::npos);
}

} // namespace
