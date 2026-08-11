#include <gtest/gtest.h>

#include <manyforge_core/types/stop_control.hpp>
#include <string_view>

namespace manyforge::core {
namespace {

TEST(StopControlContract, RegisteredCommandTableIsExactCanonicalAndClosed) {
  const auto rows = registered_stop_command_definitions();
  ASSERT_EQ(rows.size(), 4U);
  EXPECT_EQ(rows[0].registered_command_kind_id.value(),
            "manyforge.stop.controlled.provider");
  EXPECT_EQ(rows[0].stop_action_class, StopActionClass::kControlled);
  EXPECT_FALSE(rows[0].safety_system_request_action_class.has_value());
  EXPECT_EQ(rows[1].registered_command_kind_id.value(),
            "manyforge.stop.safety.emergency_request");
  EXPECT_EQ(rows[1].safety_system_request_action_class,
            SafetySystemRequestActionClass::kEmergencyStopRequest);
  EXPECT_EQ(rows[2].registered_command_kind_id.value(),
            "manyforge.stop.safety.protective_request");
  EXPECT_EQ(rows[2].safety_system_request_action_class,
            SafetySystemRequestActionClass::kProtectiveStopRequest);
  EXPECT_EQ(rows[3].registered_command_kind_id.value(),
            "manyforge.stop.software_hard");
  EXPECT_EQ(rows[3].stop_action_class, StopActionClass::kSoftwareHard);
  EXPECT_FALSE(rows[3].safety_system_request_action_class.has_value());
}

TEST(StopControlContract, LookupNeverClassifiesBySpelling) {
  const auto* hard = find_registered_stop_command_definition(
      RegisteredStopCommandKindId("manyforge.stop.software_hard"));
  ASSERT_NE(hard, nullptr);
  EXPECT_EQ(hard->stop_action_class, StopActionClass::kSoftwareHard);
  EXPECT_EQ(find_registered_stop_command_definition(
                RegisteredStopCommandKindId("vendor.software_hard")),
            nullptr);
}

TEST(StopControlContract, MotionParametersRetainTypedCoordinateKinds) {
  const ControlledStopResourceLimitDTO angular{
      ResourceId("joint.a"), AngularAccelerationDTO{1.5},
      AngularVelocityDTO{0.01}};
  const ControlledStopResourceLimitDTO linear{
      ResourceId("axis.z"), LinearAccelerationDTO{0.5},
      LinearVelocityDTO{0.001}};
  EXPECT_EQ(coordinate_kind(angular.max_deceleration),
            CoordinateKind::kAngular);
  EXPECT_EQ(coordinate_kind(angular.settle_velocity_threshold),
            CoordinateKind::kAngular);
  EXPECT_EQ(coordinate_kind(linear.max_deceleration),
            CoordinateKind::kLinear);
  EXPECT_EQ(coordinate_kind(linear.settle_velocity_threshold),
            CoordinateKind::kLinear);
}

}  // namespace
}  // namespace manyforge::core
