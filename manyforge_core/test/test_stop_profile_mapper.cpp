#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>

namespace manyforge::core {
namespace {

RuntimeResourceBindingDTO resource(std::string source_joint, std::string resource_id,
                                   CoordinateKind kind) {
  const std::string compiled = "compiled." + resource_id;
  return RuntimeResourceBindingDTO{ResourceRoleId("role." + source_joint),
                                   ResourceId(std::move(resource_id)), std::move(source_joint),
                                   compiled, kind};
}

MotionInterruptStopProfileDTO soft_profile() {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "soft.profile.v1";
  profile.jerk_limits_enabled = false;
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

TEST(StopProfileMapper, ExplicitJerkActivationResolvesOnceWithMatchingUnits) {
  auto profile = soft_profile();
  profile.jerk_limits_enabled = true;
  profile.per_joint_max_jerk = {{"joint_1", AngularJerkDTO{12.0}}};
  auto frozen = resolve_motion_interrupt_stop_jerk_limits(profile);
  ASSERT_TRUE(frozen.ok()) << frozen.error().message;
  EXPECT_DOUBLE_EQ(
      std::get<AngularJerkDTO>(frozen.value().per_joint_max_jerk.at("joint_1")).value_rad_s3, 12.0);
  EXPECT_DOUBLE_EQ(
      std::get<LinearJerkDTO>(frozen.value().per_joint_max_jerk.at("axis_z")).value_m_s3,
      motion_jerk_preset_limit(0.5, 0.25));
  EXPECT_EQ(profile.per_joint_max_jerk.size(), 1U);  // Authored provenance remains unchanged.
  EXPECT_EQ(resolve_motion_interrupt_stop_jerk_limits(frozen.value()).value(), frozen.value());
  auto changed_ramp = frozen.value();
  changed_ramp.deceleration_time_s = 0.10;
  EXPECT_EQ(resolve_motion_interrupt_stop_jerk_limits(changed_ramp).value().per_joint_max_jerk,
            frozen.value().per_joint_max_jerk);
  const auto mapped = map_motion_interrupt_stop_profile_to_contract_parameters(
      frozen.value(), MotionInterruptNodeCategory::kGeneric, resources(), defaults());
  ASSERT_TRUE(mapped.ok());
  EXPECT_TRUE(std::get<SoftOnPathStopParametersDTO>(mapped.value()).jerk_limits_enabled);
  EXPECT_DOUBLE_EQ(std::get<AngularJerkDTO>(*std::get<SoftOnPathStopParametersDTO>(mapped.value())
                                                 .per_resource_limits.front()
                                                 .max_jerk)
                       .value_rad_s3,
                   12.0);
  const auto projected =
      project_motion_interrupt_stop_profile_to_compiled_resources(frozen.value(), resources());
  ASSERT_TRUE(projected.ok());
  EXPECT_TRUE(projected.value().per_joint_max_jerk.contains("compiled.resource.a"));
}

TEST(StopProfileMapper, OffDoesNotInventLimitsAndUnreviewedHistoryCannotBePrepared) {
  EXPECT_FALSE(MotionInterruptStopProfileDTO{}.jerk_limits_enabled.has_value());
  auto profile = soft_profile();
  ASSERT_EQ(profile.jerk_limits_enabled, false);
  const auto off = resolve_motion_interrupt_stop_jerk_limits(profile);
  ASSERT_TRUE(off.ok());
  EXPECT_TRUE(off.value().per_joint_max_jerk.empty());
  EXPECT_TRUE(off.value().diagnostics.empty());
  profile.jerk_limits_enabled.reset();
  const auto old = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resources(), defaults());
  ASSERT_FALSE(old.ok());
  EXPECT_EQ(reason(old.error()), "motion_controlled_stop_jerk_activation_required");
}

TEST(StopProfileMapper, JerkRejectsUnknownWrongUnitNonFiniteAndHardProfiles) {
  for (const auto& value :
       {CoordinateJerkDTO{LinearJerkDTO{10.0}}, CoordinateJerkDTO{AngularJerkDTO{-1.0}},
        CoordinateJerkDTO{AngularJerkDTO{std::numeric_limits<double>::infinity()}}}) {
    auto profile = soft_profile();
    profile.jerk_limits_enabled = true;
    profile.per_joint_max_jerk["joint_1"] = value;
    EXPECT_FALSE(resolve_motion_interrupt_stop_jerk_limits(profile).ok());
  }
  auto profile = soft_profile();
  profile.per_joint_max_jerk["other"] = AngularJerkDTO{1.0};
  EXPECT_FALSE(resolve_motion_interrupt_stop_jerk_limits(profile).ok());
  profile = {};
  profile.profile_id = "hard";
  profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  profile.jerk_limits_enabled = true;
  EXPECT_FALSE(resolve_motion_interrupt_stop_jerk_limits(profile).ok());
}

TEST(StopProfileMapper, SpringPresetUsesOneFrozenHalfDurationCeiling) {
  auto profile = soft_profile();
  profile.jerk_limits_enabled = true;
  const auto soft = resolve_motion_interrupt_stop_jerk_limits(profile);
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  const auto spring = resolve_motion_interrupt_stop_jerk_limits(profile);
  ASSERT_TRUE(soft.ok());
  ASSERT_TRUE(spring.ok());
  EXPECT_DOUBLE_EQ(
      std::get<AngularJerkDTO>(spring.value().per_joint_max_jerk.at("joint_1")).value_rad_s3,
      motion_jerk_preset_limit(2.0, 0.125));
  EXPECT_NE(soft.value().per_joint_max_jerk, spring.value().per_joint_max_jerk);
}

TEST(StopProfileMapper, MapsTypedMixedCoordinatesToCanonicalResources) {
  auto profile = soft_profile();
  profile.per_joint_settle_velocity_threshold = {
      {"joint_1", AngularVelocityDTO{0.01}},
  };
  profile.recoverable_substitution_mode = MotionInterruptStopMode::kSpringBackHold;
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<SoftOnPathStopParametersDTO>(result.value()));
  const auto& mapped = std::get<SoftOnPathStopParametersDTO>(result.value());
  EXPECT_DOUBLE_EQ(mapped.deceleration_time_s, 0.25);
  EXPECT_TRUE(mapped.allow_natural_completion_on_short_path);
  EXPECT_EQ(mapped.recoverable_substitution_mode, MotionControlledStopMode::kSpringBackHold);
  ASSERT_EQ(mapped.per_resource_limits.size(), 2U);
  EXPECT_EQ(mapped.per_resource_limits[0].resource_id.value(), "resource.a");
  EXPECT_EQ(coordinate_kind(mapped.per_resource_limits[0].max_deceleration),
            CoordinateKind::kAngular);
  EXPECT_DOUBLE_EQ(
      std::get<AngularVelocityDTO>(mapped.per_resource_limits[0].settle_velocity_threshold)
          .value_rad_s,
      0.01);
  EXPECT_EQ(mapped.per_resource_limits[1].resource_id.value(), "resource.z");
  EXPECT_EQ(coordinate_kind(mapped.per_resource_limits[1].max_deceleration),
            CoordinateKind::kLinear);
  EXPECT_DOUBLE_EQ(
      std::get<LinearVelocityDTO>(mapped.per_resource_limits[1].settle_velocity_threshold)
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
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<SpringBackHoldStopParametersDTO>(result.value()));
  const auto& mapped = std::get<SpringBackHoldStopParametersDTO>(result.value());
  EXPECT_DOUBLE_EQ(mapped.deceleration_time_s, kDefaultDecelerationTimeS);
  EXPECT_EQ(mapped.per_resource_limits.size(), 2U);
  EXPECT_FALSE(mapped.allow_natural_completion_on_short_path);
  profile.allow_natural_completion_on_short_path = true;
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(std::get<SpringBackHoldStopParametersDTO>(result.value())
                  .allow_natural_completion_on_short_path);
}

TEST(StopProfileMapper, HardDecelerationUsesOnlyRetainedSettleTable) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "hard.profile.v1";
  profile.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);

  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(std::holds_alternative<HardDecelerationStopParametersDTO>(result.value()));
  EXPECT_EQ(std::get<HardDecelerationStopParametersDTO>(result.value()).per_resource_limits.size(),
            2U);
}

TEST(StopProfileMapper, RejectsMissingExtraAndWrongUnitLimits) {
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto missing = soft_profile();
  missing.per_joint_max_deceleration.erase("axis_z");
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      missing, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_deceleration_coverage_mismatch");

  auto extra = soft_profile();
  extra.per_joint_max_deceleration.emplace("joint_other", AngularAccelerationDTO{1.0});
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      extra, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_deceleration_coverage_mismatch");

  auto wrong_unit = soft_profile();
  wrong_unit.per_joint_max_deceleration["axis_z"] = AngularAccelerationDTO{0.5};
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      wrong_unit, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_deceleration_limit_invalid");
}

TEST(StopProfileMapper, RejectsAliasedResourcesAndForbiddenSourceSymbols) {
  const auto default_rows = defaults();
  auto aliased_resources = resources();
  aliased_resources[1].resource_id = aliased_resources[0].resource_id;
  auto profile = soft_profile();
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, aliased_resources, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_resource_binding_invalid");

  auto format_resource_rows = resources();
  format_resource_rows[0].source_joint_name = std::string("axis", 4) +
                                              std::string("\xE2\x80\x8B", 3) +
                                              "z";  // U+200B ZERO WIDTH SPACE (Cf)
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, format_resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_resource_binding_invalid");

  auto control_resource_rows = resources();
  control_resource_rows[0].source_joint_name =
      std::string("axis", 4) + std::string("\xC2\x85", 2) + "z";  // U+0085 NEXT LINE (Cc)
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, control_resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_resource_binding_invalid");
}

TEST(StopProfileMapper, RejectsUnknownLooseOrWrongUnitSettleOverride) {
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto unknown = soft_profile();
  unknown.per_joint_settle_velocity_threshold = {{"other", AngularVelocityDTO{0.01}}};
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      unknown, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_settle_override_unknown_joint");

  auto loose = soft_profile();
  loose.per_joint_settle_velocity_threshold = {{"joint_1", AngularVelocityDTO{0.03}}};
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      loose, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_settle_override_invalid");

  auto wrong_unit = soft_profile();
  wrong_unit.per_joint_settle_velocity_threshold = {{"axis_z", AngularVelocityDTO{0.001}}};
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      wrong_unit, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_settle_override_invalid");

  auto explicitly_empty = soft_profile();
  explicitly_empty.per_joint_settle_velocity_threshold.emplace();
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      explicitly_empty, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_profile_invalid");
}

TEST(StopProfileMapper, RejectsUnqualifiedOrPrecisionSubstitution) {
  const auto resource_rows = resources();
  const auto default_rows = defaults();

  auto wrong_pair = soft_profile();
  wrong_pair.recoverable_substitution_mode = MotionInterruptStopMode::kHardDeceleration;
  auto result = map_motion_interrupt_stop_profile_to_contract_parameters(
      wrong_pair, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_stop_profile_invalid");

  auto precision = soft_profile();
  precision.allow_natural_completion_on_short_path = false;
  precision.recoverable_substitution_mode = MotionInterruptStopMode::kSpringBackHold;
  result = map_motion_interrupt_stop_profile_to_contract_parameters(
      precision, MotionInterruptNodeCategory::kPrecisionContact, resource_rows, default_rows);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(reason(result.error()), "motion_interrupt_substitution_precision_contact_forbidden");
  precision.recoverable_substitution_mode.reset();
  precision.allow_natural_completion_on_short_path = true;
  for (const auto mode :
       {MotionInterruptStopMode::kSoftOnPath, MotionInterruptStopMode::kSpringBackHold}) {
    precision.stop_mode = mode;
    result = map_motion_interrupt_stop_profile_to_contract_parameters(
        precision, MotionInterruptNodeCategory::kPrecisionContact, resource_rows, default_rows);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(reason(result.error()),
              "motion_interrupt_natural_completion_precision_contact_forbidden");
  }
}

TEST(StopProfileMapper, ReconstructsExactPreparedBackendProfile) {
  auto profile = soft_profile();
  profile.per_joint_settle_velocity_threshold = {
      {"joint_1", AngularVelocityDTO{0.01}},
      {"axis_z", LinearVelocityDTO{0.002}},
  };
  const auto resource_rows = resources();
  const auto default_rows = defaults();
  auto mapped = map_motion_interrupt_stop_profile_to_contract_parameters(
      profile, MotionInterruptNodeCategory::kGeneric, resource_rows, default_rows);
  ASSERT_TRUE(mapped.ok()) << mapped.error().message;

  const auto& selected = std::get<SoftOnPathStopParametersDTO>(mapped.value());
  JsonValueDTO::Array limits;
  for (const auto& limit : selected.per_resource_limits) {
    JsonObjectDTO acceleration;
    if (const auto* angular = std::get_if<AngularAccelerationDTO>(&limit.max_deceleration)) {
      acceleration = {{"coordinate_kind", JsonValueDTO{"angular_rad_s2"}},
                      {"value_rad_s2", JsonValueDTO{angular->value_rad_s2}}};
    } else {
      const auto& linear = std::get<LinearAccelerationDTO>(limit.max_deceleration);
      acceleration = {{"coordinate_kind", JsonValueDTO{"linear_m_s2"}},
                      {"value_m_s2", JsonValueDTO{linear.value_m_s2}}};
    }
    JsonObjectDTO settle;
    if (const auto* angular = std::get_if<AngularVelocityDTO>(&limit.settle_velocity_threshold)) {
      settle = {{"coordinate_kind", JsonValueDTO{"angular_rad_s"}},
                {"value_rad_s", JsonValueDTO{angular->value_rad_s}}};
    } else {
      const auto& linear = std::get<LinearVelocityDTO>(limit.settle_velocity_threshold);
      settle = {{"coordinate_kind", JsonValueDTO{"linear_m_s"}},
                {"value_m_s", JsonValueDTO{linear.value_m_s}}};
    }
    limits.emplace_back(JsonObjectDTO{
        {"resource_id", JsonValueDTO{limit.resource_id.value()}},
        {"max_deceleration", JsonValueDTO{std::move(acceleration)}},
        {"settle_velocity_threshold", JsonValueDTO{std::move(settle)}},
    });
  }
  const JsonObjectDTO prepared{
      {"stop_mode", JsonValueDTO{"soft_on_path"}},
      {"jerk_limits_enabled", JsonValueDTO{false}},
      {"deceleration_time_s", JsonValueDTO{0.25}},
      {"per_resource_limits", JsonValueDTO{std::move(limits)}},
      {"allow_natural_completion_on_short_path", JsonValueDTO{true}},
  };

  auto reconstructed = map_motion_controlled_stop_contract_parameters_to_profile(
      prepared, resource_rows, "operator_soft_on_path");
  ASSERT_TRUE(reconstructed.ok()) << reconstructed.error().message;
  EXPECT_EQ(reconstructed.value().profile_id, "operator_soft_on_path");
  EXPECT_EQ(reconstructed.value().stop_mode, MotionInterruptStopMode::kSoftOnPath);
  EXPECT_EQ(reconstructed.value().deceleration_time_s, 0.25);
  EXPECT_TRUE(reconstructed.value().allow_natural_completion_on_short_path);
  EXPECT_EQ(reconstructed.value().per_joint_max_deceleration, profile.per_joint_max_deceleration);
  EXPECT_EQ(reconstructed.value().per_joint_settle_velocity_threshold,
            profile.per_joint_settle_velocity_threshold);
}

TEST(StopProfileMapper, ProjectsPortableProfileToExactCompiledJointNames) {
  auto profile = soft_profile();
  profile.per_joint_settle_velocity_threshold = {
      {"joint_1", AngularVelocityDTO{0.01}},
      {"axis_z", LinearVelocityDTO{0.002}},
  };
  const auto resource_rows = resources();

  auto projected =
      project_motion_interrupt_stop_profile_to_compiled_resources(profile, resource_rows);

  ASSERT_TRUE(projected.ok()) << projected.error().message;
  EXPECT_EQ(projected.value().profile_id, profile.profile_id);
  EXPECT_EQ(projected.value().stop_mode, profile.stop_mode);
  EXPECT_EQ(projected.value().per_joint_max_deceleration.size(), 2U);
  EXPECT_TRUE(projected.value().per_joint_max_deceleration.contains("compiled.resource.a"));
  EXPECT_TRUE(projected.value().per_joint_max_deceleration.contains("compiled.resource.z"));
  ASSERT_TRUE(projected.value().per_joint_settle_velocity_threshold.has_value());
  EXPECT_TRUE(
      projected.value().per_joint_settle_velocity_threshold->contains("compiled.resource.a"));
  EXPECT_TRUE(
      projected.value().per_joint_settle_velocity_threshold->contains("compiled.resource.z"));
  EXPECT_TRUE(profile.per_joint_max_deceleration.contains("joint_1"));
  EXPECT_FALSE(profile.per_joint_max_deceleration.contains("compiled.resource.a"));
}

TEST(StopProfileMapper, RejectsIncompleteCompiledJointProjection) {
  auto profile = soft_profile();
  auto resource_rows = resources();
  resource_rows.pop_back();

  auto projected =
      project_motion_interrupt_stop_profile_to_compiled_resources(profile, resource_rows);

  ASSERT_FALSE(projected.ok());
  EXPECT_EQ(reason(projected.error()), "motion_controlled_stop_joint_projection_mismatch");
}

TEST(StopProfileMapper, ProjectsOneCompleteLaneNeutralEvidenceObject) {
  const auto profile = soft_profile();
  ControlledStopResultDTO result;
  result.accepted = true;
  result.executed_stop_mode = MotionInterruptStopMode::kSoftOnPath;
  result.stop_command_source = MotionInterruptStopCommandSource::kNativeSimulatedPathRamp;
  result.backend_stop_confirmed = true;
  result.confirmation_latency_ns = 1'000'000'000;
  result.stop_distance_rad = 0.12;
  result.path_deviation_max_rad = 0.001;
  result.initial_path_deviation_rad = 0.0005;
  result.diagnostics.emplace("retimer", interrupt_structured_string("quintic"));

  const auto encoded =
      motion_controlled_stop_evidence(profile, result, "sha256:policy", "deployment_default");
  const auto* object = std::get_if<JsonObjectDTO>(&encoded.value);
  ASSERT_NE(object, nullptr);
  EXPECT_EQ(std::get<std::string>(object->at("profile_id").value), profile.profile_id);
  EXPECT_EQ(std::get<std::string>(object->at("stop_evidence_policy_revision").value),
            "sha256:policy");
  EXPECT_EQ(std::get<std::string>(object->at("actual_stop_mode").value), "soft_on_path");
  EXPECT_EQ(std::get<std::string>(object->at("stop_command_source").value),
            "native_simulated_path_ramp");
  const auto* diagnostics = std::get_if<JsonObjectDTO>(&object->at("provider_diagnostics").value);
  ASSERT_NE(diagnostics, nullptr);
  EXPECT_EQ(std::get<std::string>(diagnostics->at("retimer").value), "quintic");
}

}  // namespace
}  // namespace manyforge::core
