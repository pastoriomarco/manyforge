#include <gtest/gtest.h>
#include <limits>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>

namespace {
namespace core = manyforge::core;
using namespace manyforge::deployment::native_execution_runtime_contract;

const std::vector<core::RuntimeResourceBindingDTO> resources{
    {core::ResourceRoleId("arm"), core::ResourceId("robot.joint1"), "joint1",
     "compiled_joint1", core::CoordinateKind::kAngular}};
const core::JointLimitsDTO model{
    {"compiled_joint1"}, {-1.0}, {1.0}, {2.0}, {4.0}};

double maximum(const core::OrdinaryMotionJerkPolicy &policy) {
  return std::get<core::AngularJerkDTO>(policy.limits.at("joint1"))
      .value_rad_s3;
}

ImportedJointJerkLimit imported() {
  return {12.0, true, false, "robot_moveit_config/config/joint_limits.yaml",
          core::ContentDigest("sha256:" + std::string(64, 'a'))};
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

TEST(OrdinaryMotionJerk,
     OverrideThenActiveImportThenPresetWithNoCeilingBypass) {
  OrdinaryMotionJerkSettings settings{true, 1.0, {{"joint1", {}, imported()}}};
  auto selected =
      resolve_ordinary_motion_jerk_policy(settings, resources, model);
  ASSERT_TRUE(selected.ok());
  EXPECT_DOUBLE_EQ(maximum(selected.value()), 12.0);
  settings.joint_limits.front().override_rad_s3 = 10.0;
  selected = resolve_ordinary_motion_jerk_policy(settings, resources, model);
  ASSERT_TRUE(selected.ok());
  EXPECT_DOUBLE_EQ(maximum(selected.value()), 10.0);
  settings.joint_limits.front().override_rad_s3 = 13.0;
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, model).ok());
  settings.joint_limits.front().override_rad_s3.reset();
  settings.joint_limits.front().imported->enabled = false;
  selected = resolve_ordinary_motion_jerk_policy(settings, resources, model);
  ASSERT_TRUE(selected.ok());
  EXPECT_DOUBLE_EQ(maximum(selected.value()),
                   core::motion_jerk_preset_limit(4.0, 1.0));
  settings.enabled = false;
  EXPECT_TRUE(resolve_ordinary_motion_jerk_policy(settings, resources, {})
                  .value()
                  .limits.empty());
}

TEST(OrdinaryMotionJerk,
     RequiredImportedLimitsCannotBeDisabledOrLoseProvenance) {
  OrdinaryMotionJerkSettings settings{true, 1.0, {{"joint1", {}, imported()}}};
  settings.joint_limits.front().imported->machine_required = true;
  auto selected = resolve_ordinary_motion_jerk_policy(settings, resources, {});
  ASSERT_TRUE(selected.ok());
  EXPECT_TRUE(selected.value().machine_required);
  settings.enabled = false;
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, model).ok());
  settings.enabled = true;
  settings.joint_limits.front().imported->source_id.clear();
  EXPECT_FALSE(
      resolve_ordinary_motion_jerk_policy(settings, resources, model).ok());
}

TEST(OrdinaryMotionJerk, RejectsCrossedNamesUnitsAndMalformedInputs) {
  OrdinaryMotionJerkSettings settings{true, 1.0, {{"foreign", 10.0, {}}}};
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
