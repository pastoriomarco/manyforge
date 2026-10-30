// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_xarm_control/lifecycle_profile.hpp>
#include <manyforge_xarm_control/xarm_device_adapter.hpp>

namespace manyforge::xarm_control {
namespace {

using roboplan_backend::RetainedRos2ControlSwitchOutcome;
using roboplan_backend::VendorAdapterSelection;
using roboplan_backend::VendorJointSampleUse;
using namespace std::chrono_literals;

[[nodiscard]] VendorAdapterSelection Selection() {
  return {.vendor = std::string(kXArmVendorAdapterFamily),
          .driver_namespace = "/lite6_robot/xarm",
          .axis_count = 6,
          .joint_agreement_tolerance_rad = 0.01};
}

// V-A: the row RoboPlan's vendor registry lists; the persisted provider ids
// are unchanged.
TEST(XArmDeviceAdapter, FamilyRowCarriesTheAdapterAndTheLifecycleProviders) {
  const auto family = xarm_vendor_adapter_family();
  EXPECT_EQ(family.vendor, "xarm");
  EXPECT_EQ(family.make_adapter, &make_xarm_device_adapter);
  EXPECT_EQ(family.arm_provider_manifest_id, "manyforge.provider.ros2_control.xarm");
  EXPECT_EQ(family.make_arm_catalog, &make_xarm_ros2_control_lifecycle_catalog);
  EXPECT_EQ(family.gripper_provider_manifest_id,
            "manyforge.provider.ros2_control.xarm.gripper");
}

TEST(XArmDeviceAdapter, ValidatesItsDataBeforeAnyRosEffect) {
  EXPECT_NE(make_xarm_device_adapter(Selection()), nullptr);
  for (const std::string ns : {"", "lite6_robot/xarm", "/lite6_robot/xarm/"}) {
    auto selection = Selection();
    selection.driver_namespace = ns;
    EXPECT_THROW(static_cast<void>(make_xarm_device_adapter(selection)),
                 std::invalid_argument)
        << ns;
  }
  for (const int axes : {0, 8}) {
    auto selection = Selection();
    selection.axis_count = axes;
    EXPECT_THROW(static_cast<void>(make_xarm_device_adapter(selection)),
                 std::invalid_argument)
        << axes;
  }
  for (const double tolerance : {0.0, -0.01, std::numeric_limits<double>::quiet_NaN()}) {
    auto selection = Selection();
    selection.joint_agreement_tolerance_rad = tolerance;
    EXPECT_THROW(static_cast<void>(make_xarm_device_adapter(selection)),
                 std::invalid_argument)
        << tolerance;
  }
}

// Not started: the status is unavailable and every primitive, gate and tool
// context step refuses before effect (unchanged by the move).
TEST(XArmDeviceAdapter, NotStartedRefusesBeforeEffect) {
  const auto adapter = make_xarm_device_adapter(Selection());
  EXPECT_EQ(adapter->ReadHealth(0).cause,
            core::LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_EQ(adapter->Observe(0).cause,
            core::LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_EQ(adapter->VendorJointStateTopic(), "/lite6_robot/xarm/joint_states");
  const auto gate = adapter->VendorJointSampleGateFn();
  ASSERT_TRUE(gate);
  EXPECT_EQ(gate(0, std::vector<double>(6, 0.1)), VendorJointSampleUse::kDisplay);

  const auto clear = adapter->Dispatch(
      core::LifecyclePrimitiveId(std::string(core::kDeviceClearErrorPrimitiveId)),
      std::nullopt, std::stop_token{});
  EXPECT_EQ(clear.outcome, RetainedRos2ControlSwitchOutcome::kNotSent);
  ASSERT_TRUE(clear.error.has_value());
  EXPECT_EQ(clear.error->details.at("reason"), "xarm_adapter_not_started");

  const auto other = adapter->Dispatch(
      core::LifecyclePrimitiveId("ros2_control.controller.activate"), std::nullopt,
      std::stop_token{});
  EXPECT_EQ(other.outcome, RetainedRos2ControlSwitchOutcome::kNotSent);
  ASSERT_TRUE(other.error.has_value());
  EXPECT_EQ(other.error->details.at("reason"), "ros2_control_lifecycle_primitive_unknown");

  const auto gate_check = adapter->CheckActivation(0, std::vector<double>(6, 0.0));
  ASSERT_FALSE(gate_check.ok());
  EXPECT_EQ(gate_check.error().details.at("reason"), "xarm_status_unavailable");

  const roboplan_backend::VendorToolContextCommand context{};
  const auto applied =
      adapter->SendToolContext(context, true, std::nullopt, std::stop_token{});
  ASSERT_FALSE(applied.ok());
  EXPECT_EQ(applied.error->details.at("reason"), "xarm_adapter_not_started");

  const auto reengaged = adapter->ReengageAtRest(std::stop_token{});
  ASSERT_FALSE(reengaged.ok());
  EXPECT_EQ(reengaged.error->details.at("reason"), "xarm_adapter_not_started");
}

// V-A (A2-A5): the xArm adapter's own answers, before it is started. M6
// item 23: every bound joint exactly zero is the e-stop signature (hardware
// active), never shown; other samples are shown, and lead only by the
// port's rule (not started: never). Arming needs fresh status without an
// error (not started: refused). The plugin's 5 s transient. A call refused
// before it is sent: typed, with the re-engage's effect still possible (as
// before V-A).
TEST(XArmDeviceAdapter, XArmAdapterAnswersItsVendorFacts) {
  const auto adapter = make_xarm_device_adapter(
      {.vendor = "xarm",
       .driver_namespace = "/manyforge/real_lite6/xarm",
       .axis_count = 6,
       .joint_agreement_tolerance_rad = 0.01});
  const auto gate = adapter->VendorJointSampleGateFn();
  ASSERT_TRUE(gate);
  EXPECT_EQ(gate(1, {0.0, 0.0}), VendorJointSampleUse::kIgnore);
  EXPECT_EQ(gate(1, {0.0, 0.3}), VendorJointSampleUse::kDisplay);
  const auto armable = adapter->CheckArmable(1);
  ASSERT_FALSE(armable.ok());
  EXPECT_EQ(armable.error().details.at("reason"), "ros2_control_device_not_armable");
  EXPECT_EQ(adapter->ActivationTransientBound(), 5s);
  const auto applied = adapter->SendToolContext({}, false, std::nullopt, std::stop_token{});
  ASSERT_TRUE(applied.error.has_value());
  EXPECT_EQ(applied.error->details.at("reason"), "xarm_adapter_not_started");
  EXPECT_FALSE(applied.effect_possible);
  EXPECT_FALSE(applied.unresponsive);
  const auto reengaged = adapter->ReengageAtRest(std::stop_token{});
  ASSERT_TRUE(reengaged.error.has_value());
  EXPECT_EQ(reengaged.error->details.at("reason"), "xarm_adapter_not_started");
  EXPECT_TRUE(reengaged.effect_possible);
  EXPECT_FALSE(reengaged.unresponsive);
}

} // namespace
} // namespace manyforge::xarm_control
