// Merge 6: Isaac robot rows are arm-only and tool rows own their gripper
// transport; the combined ids are no Isaac row (D2 keeps them as Mock test
// fixtures only).
// Uses slice C's `retained_isaac_gripper_bridge` (merge 6 build).
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace {

using namespace manyforge::roboplan_backend;

TEST(RetainedIsaacProfileRows, RobotRowsCarryTheArm) {
  EXPECT_EQ(retained_isaac_controller_lane("lite6", {}).hardware_component_name,
            "IsaacLite6TopicBasedSystem");
  EXPECT_EQ(retained_isaac_controller_lane("ur10e", {}).hardware_component_name,
            "IsaacUR10eTopicBasedSystem");
  // A tool row has no arm; its lane names only the shared joint states.
  for (const auto *profile : {"lite_gripper", "robotiq_2f_140"}) {
    SCOPED_TRACE(profile);
    const auto lane = retained_isaac_controller_lane(profile, {});
    EXPECT_TRUE(lane.hardware_component_name.empty());
    EXPECT_EQ(lane.joint_states_topic, "/isaac_joint_states");
  }
  for (const auto *profile : {"ur5e", "lite6_lite_gripper", "ur10e_robotiq_2f_140"}) {
    SCOPED_TRACE(profile);
    EXPECT_THROW(static_cast<void>(retained_isaac_controller_lane(profile, {})),
                 std::invalid_argument);
  }
}

TEST(RetainedIsaacProfileRows, ToolRowsOwnTheGripperTransport) {
  const auto bridge = retained_isaac_gripper_bridge("robotiq_2f_140");
  ASSERT_TRUE(bridge.has_value());
  EXPECT_EQ(bridge->action_name, "robotiq_gripper_controller/gripper_cmd");
  EXPECT_EQ(bridge->command_topic, "/isaac_gripper_state");
  EXPECT_EQ(bridge->state_topic, "/isaac_joint_states");
  EXPECT_EQ(bridge->parameters_file, "isaac_robotiq_2f_140_gripper_bridge.yaml");
  // The Lite gripper uses direct effort commands: no bridge.
  EXPECT_FALSE(retained_isaac_gripper_bridge("lite_gripper").has_value());
  // The arm-only robot rows carry no gripper facts, and a combined id is no row.
  for (const auto *profile : {"lite6", "ur10e", "lite6_lite_gripper", "ur10e_robotiq_2f_140"}) {
    SCOPED_TRACE(profile);
    EXPECT_THROW(static_cast<void>(retained_isaac_gripper_bridge(profile)),
                 std::invalid_argument);
  }
}

} // namespace
