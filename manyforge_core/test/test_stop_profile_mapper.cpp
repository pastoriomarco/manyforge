#include <gtest/gtest.h>

#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>

namespace manyforge::core {
namespace {

RuntimeResourceBindingDTO resource(std::string source_joint,
                                   std::string resource_id,
                                   CoordinateKind kind) {
  const std::string compiled = "compiled." + resource_id;
  return RuntimeResourceBindingDTO{
      ResourceRoleId("role." + source_joint),
      ResourceId(std::move(resource_id)), std::move(source_joint),
      compiled, kind};
}

MotionInterruptStopProfileDTO soft_profile() {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "soft.profile.v1";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.25;
  profile.per_joint_max_deceleration = {
      {"axis_z", LinearAccelerationDTO{0.5}},
      {"joint_1", AngularAccelerationDTO{2.0}},
  };
  profile.allow_natural_completion_on_short_path = true;
  return profile;
}

std::vector<RuntimeResourceBindingDTO> resources() {
  return {
      resource("axis_z", "resource.z", CoordinateKind::kLinear),
      resource("joint_1", "resource.a", CoordinateKind::kAngular),
  };
}

std::vector<HardStopResourceLimitDTO> defaults() {
  return {
      {ResourceId("resource.a"), AngularVelocityDTO{0.02}},
      {ResourceId("resource.z"), LinearVelocityDTO{0.004}},
  };
}

std::string reason(const ErrorDTO& error) {
  return error.details.at("reason");
}

TEST(StopProfileMapper, MapsTypedMixedCoordinatesToCanonicalResources) {
  auto profile = soft_profile();
  profile.per_joint_settle_velocity_threshold = {
      {"joint_1", AngularVelocityDTO{0.01}},
  };
  profile.recoverable_substitution_mode =
      MotionInterruptStopMode::kSpringBackHold;
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<SoftOnPathStopParametersDTO>(
      result.value()));
  const auto& mapped =
      std::get<SoftOnPathStopParametersDTO>(result.value());
  EXPECT_DOUBLE_EQ(mapped.deceleration_time_s, 0.25);
  EXPECT_TRUE(mapped.allow_natural_completion_on_short_path);
  EXPECT_EQ(mapped.recoverable_substitution_mode,
            MotionControlledStopMode::kSpringBackHold);
  ASSERT_EQ(mapped.per_resource_limits.size(), 2U);
  EXPECT_EQ(mapped.per_resource_limits[0].resource_id.value(), "resource.a");
  EXPECT_EQ(coordinate_kind(mapped.per_resource_limits[0].max_deceleration),
            CoordinateKind::kAngular);
  EXPECT_DOUBLE_EQ(
      std::get<AngularVelocityDTO>(
          mapped.per_resource_limits[0].settle_velocity_threshold)
          .value_rad_s,
      0.01);
  EXPECT_EQ(mapped.per_resource_limits[1].resource_id.value(), "resource.z");
  EXPECT_EQ(coordinate_kind(mapped.per_resource_limits[1].max_deceleration),
            CoordinateKind::kLinear);
  EXPECT_DOUBLE_EQ(
      std::get<LinearVelocityDTO>(
          mapped.per_resource_limits[1].settle_velocity_threshold)
          .value_m_s,
      0.004);
}

TEST(StopProfileMapper, SpringBackUsesHistoricalDefaultAndDropsSoftFields) {
  auto profile = soft_profile();
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s.reset();
  profile.allow_natural_completion_on_short_path = false;
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<SpringBackHoldStopParametersDTO>(
      result.value()));
  const auto& mapped =
      std::get<SpringBackHoldStopParametersDTO>(result.value());
  EXPECT_DOUBLE_EQ(mapped.deceleration_time_s, kDefaultDecelerationTimeS);
  EXPECT_EQ(mapped.per_resource_limits.size(), 2U);
}

TEST(StopProfileMapper, HardDecelerationUsesOnlyRetainedSettleTable) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "hard.profile.v1";
  profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<HardDecelerationStopParametersDTO>(
      result.value()));
  EXPECT_EQ(std::get<HardDecelerationStopParametersDTO>(result.value())
                .per_resource_limits.size(),
            2U);
}

TEST(StopProfileMapper, RejectsMissingExtraAndWrongUnitLimits) {
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto missing = soft_profile();
  missing.per_joint_max_deceleration.erase("axis_z");
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      missing, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_deceleration_coverage_mismatch");

  auto extra = soft_profile();
  extra.per_joint_max_deceleration.emplace(
      "joint_other", AngularAccelerationDTO{1.0});
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      extra, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_deceleration_coverage_mismatch");

  auto wrong_unit = soft_profile();
  wrong_unit.per_joint_max_deceleration["axis_z"] =
      AngularAccelerationDTO{0.5};
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      wrong_unit, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_deceleration_limit_invalid");
}

TEST(StopProfileMapper, RejectsAliasedResourcesAndForbiddenSourceSymbols) {
  const auto default_rows = defaults();
  auto aliased_resources = resources();
  aliased_resources[1].resource_id = aliased_resources[0].resource_id;
  auto profile = soft_profile();
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, aliased_resources,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_resource_binding_invalid");

  auto format_resource_rows = resources();
  format_resource_rows[0].source_joint_name =
      std::string("axis", 4) + std::string("\xE2\x80\x8B", 3) +
      "z";  // U+200B ZERO WIDTH SPACE (Cf)
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, format_resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_resource_binding_invalid");

  auto control_resource_rows = resources();
  control_resource_rows[0].source_joint_name =
      std::string("axis", 4) + std::string("\xC2\x85", 2) +
      "z";  // U+0085 NEXT LINE (Cc)
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, control_resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_resource_binding_invalid");
}

TEST(StopProfileMapper, RejectsUnknownLooseOrWrongUnitSettleOverride) {
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto unknown = soft_profile();
  unknown.per_joint_settle_velocity_threshold = {
      {"other", AngularVelocityDTO{0.01}}};
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      unknown, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_settle_override_unknown_joint");

  auto loose = soft_profile();
  loose.per_joint_settle_velocity_threshold = {
      {"joint_1", AngularVelocityDTO{0.03}}};
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      loose, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_settle_override_invalid");

  auto wrong_unit = soft_profile();
  wrong_unit.per_joint_settle_velocity_threshold = {
      {"axis_z", AngularVelocityDTO{0.001}}};
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      wrong_unit, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_settle_override_invalid");

  auto explicitly_empty = soft_profile();
  explicitly_empty.per_joint_settle_velocity_threshold.emplace();
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      explicitly_empty, MotionInterruptNodeCategory::kGeneric,
      resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_stop_profile_invalid");
}

TEST(StopProfileMapper, RejectsUnqualifiedOrPrecisionSubstitution) {
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto wrong_pair = soft_profile();
  wrong_pair.recoverable_substitution_mode =
      MotionInterruptStopMode::kHardDeceleration;
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      wrong_pair, MotionInterruptNodeCategory::kGeneric, resource_rows,
      default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_profile_invalid");

  auto precision = soft_profile();
  precision.allow_natural_completion_on_short_path = false;
  precision.recoverable_substitution_mode =
      MotionInterruptStopMode::kSpringBackHold;
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      precision, MotionInterruptNodeCategory::kPrecisionContact,
      resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()),
            "motion_interrupt_substitution_precision_contact_forbidden");
}

}  // namespace
}  // namespace manyforge::core
