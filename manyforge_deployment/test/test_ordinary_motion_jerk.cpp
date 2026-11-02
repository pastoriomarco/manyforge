#include <gtest/gtest.h>
#include <limits>
#include <manyforge_deployment/execution_runtime_contract.hpp>

namespace {
namespace core = manyforge::core;
using namespace manyforge::deployment::execution_runtime_contract;

const std::vector<core::RuntimeResourceBindingDTO> resources{
    {core::ResourceRoleId("arm"), core::ResourceId("robot.joint1"), "joint1",
     "compiled_joint1", core::CoordinateKind::kAngular}};
const core::JointLimitsDTO model{
    {"compiled_joint1"}, {-1.0}, {1.0}, {2.0}, {4.0}};

double maximum(const core::OrdinaryMotionJerkPolicy &policy) {
  return std::get<core::AngularJerkDTO>(policy.limits.at("joint1"))
      .value_rad_s3;
}

TEST(OrdinaryMotionJerk, OffDoesNotInventACapAndEnabledUsesStableModelPreset) {
  OrdinaryMotionJerkSettings settings;
  auto off = resolve_ordinary_motion_jerk_policy(settings, resources, {});
  ASSERT_TRUE(off.ok());
  EXPECT_FALSE(off.value().enabled);
  EXPECT_TRUE(off.value().limits.empty());
  settings.enabled = true;
  auto on = resolve_ordinary_motion_jerk_policy(settings, resources, model);
  ASSERT_TRUE(on.ok());
  EXPECT_DOUBLE_EQ(maximum(on.value()),
                   core::motion_jerk_preset_limit(4.0, 1.0));
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, {}).ok());
}

TEST(OrdinaryMotionJerk, TypedValueThenPresetAndOffAppliesNone) {
  OrdinaryMotionJerkSettings settings{true, 1.0, {{"joint1", 10.0}}};
  auto selected =
      resolve_ordinary_motion_jerk_policy(settings, resources, model);
  ASSERT_TRUE(selected.ok());
  EXPECT_DOUBLE_EQ(maximum(selected.value()), 10.0);
  settings.joint_limits.front().override_rad_s3.reset();
  selected = resolve_ordinary_motion_jerk_policy(settings, resources, model);
  ASSERT_TRUE(selected.ok());
  EXPECT_DOUBLE_EQ(maximum(selected.value()),
                   core::motion_jerk_preset_limit(4.0, 1.0));
  settings.enabled = false;
  selected = resolve_ordinary_motion_jerk_policy(settings, resources, {});
  ASSERT_TRUE(selected.ok());
  EXPECT_FALSE(selected.value().enabled);
  EXPECT_TRUE(selected.value().limits.empty());
}

TEST(OrdinaryMotionJerk, RejectsCrossedNamesUnitsAndMalformedInputs) {
  OrdinaryMotionJerkSettings settings{true, 1.0, {{"foreign", 10.0}}};
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, model).ok());
  settings.joint_limits.front().source_joint_name = "joint1";
  settings.joint_limits.front().override_rad_s3 =
      std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, model).ok());
  settings.joint_limits.clear();
  auto wrong = resources;
  wrong.front().coordinate_kind = core::CoordinateKind::kLinear;
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, wrong, model).ok());
  settings.preset_reference_time_s = 0.0;
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, model).ok());
}
} // namespace
