#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/types/coordinate_state.hpp>

namespace manyforge::core {
namespace {

ControlledStateDTO angular_state() {
  return ControlledStateDTO{
      ControllableComponentId("robot_1"),
      CommandTargetId("arm"),
      ControlDomainId("motion"),
      ControlDomainRevision("sha256:domain"),
      ControlBindingId("arm_trajectory"),
      ControlBindingRevision("sha256:binding"),
      ControllerTopologyId("native"),
      TopologyRevision("sha256:topology"),
      ControlRouteId("native_route"),
      ActiveControlConfigurationId("native_active"),
      ActiveControlConfigurationGeneration(7U),
      {ResourceId("joint_1"), ResourceId("joint_2")},
      {"mf2_joint_1", "mf2_joint_2"},
      {AngularPositionDTO{0.25}, AngularPositionDTO{-0.5}},
      std::vector<CoordinateVelocityDTO>{AngularVelocityDTO{0.1},
                                         AngularVelocityDTO{0.0}},
  };
}

ObservedStateSnapshotDTO observed_from(const ControlledStateDTO& state) {
  return ObservedStateSnapshotDTO{
      state.controllable_component_id,
      state.command_target_id,
      state.control_domain_id,
      state.control_domain_revision,
      state.control_binding_id,
      state.control_binding_revision,
      ProviderInstanceId("native_provider"),
      ProviderManifestId("native_manifest"),
      ProviderManifestRevision("sha256:provider"),
      state.controller_topology_id,
      state.topology_revision,
      state.control_route_id,
      state.active_control_configuration_id,
      state.active_control_configuration_generation,
      state.resources,
      11U,
      state.joint_names,
      state.positions,
      state.velocities,
      "native_state",
      RuntimeEpoch("native_epoch_1"),
      ObservedStateSourceKind::kPredictedState,
      90,
      100,
      StateAuthorityId("native_predicted"),
      StateAuthorityClass::kPredicted,
      4U,
      0,
      true,
  };
}

TEST(CoordinateState, PreservesAngularAndLinearUnitsInOneAggregate) {
  auto state = angular_state();
  state.command_target_id = CommandTargetId("aggregate_arm_gripper");
  state.resources.push_back(ResourceId("gripper_slide"));
  state.joint_names.push_back("mf2_gripper_slide");
  state.positions.push_back(LinearPositionDTO{0.032});
  state.velocities->push_back(LinearVelocityDTO{0.001});

  EXPECT_TRUE(validate_controlled_state(state).ok());
  ASSERT_EQ(state.positions.size(), 3U);
  EXPECT_EQ(coordinate_kind(state.positions.front()), CoordinateKind::kAngular);
  EXPECT_EQ(coordinate_kind(state.positions.back()), CoordinateKind::kLinear);

  const auto legacy = angular_position_projection(state);
  ASSERT_FALSE(legacy.ok());
  EXPECT_EQ(legacy.error().details.at("reason"),
            "angular_projection_requires_all_angular_resources");
}

TEST(CoordinateState, AngularCompatibilityProjectionIsExactAndOrdered) {
  const auto projected = angular_position_projection(angular_state());
  ASSERT_TRUE(projected.ok());
  EXPECT_EQ(projected.value(), (std::vector<double>{0.25, -0.5}));
}

TEST(CoordinateState, RejectsDimensionDuplicateUnitAndFiniteFailures) {
  auto dimension = angular_state();
  dimension.joint_names.pop_back();
  EXPECT_EQ(validate_controlled_state(dimension).error().details.at("reason"),
            "coordinate_dimension_mismatch");

  auto duplicate = angular_state();
  duplicate.resources[1] = duplicate.resources[0];
  EXPECT_EQ(validate_controlled_state(duplicate).error().details.at("reason"),
            "coordinate_identity_missing_or_duplicate");

  auto unit = angular_state();
  (*unit.velocities)[1] = LinearVelocityDTO{0.0};
  EXPECT_EQ(validate_controlled_state(unit).error().details.at("reason"),
            "coordinate_unit_mismatch");

  auto nonfinite = angular_state();
  nonfinite.positions[0] =
      AngularPositionDTO{std::numeric_limits<double>::infinity()};
  EXPECT_EQ(validate_controlled_state(nonfinite).error().details.at("reason"),
            "coordinate_position_nonfinite");
}

TEST(CoordinateState, ObservedStateEnforcesAuthorityAndFreshness) {
  auto predicted = observed_from(angular_state());
  EXPECT_TRUE(validate_observed_state_snapshot(predicted, 500).ok());

  auto measured = predicted;
  measured.source_kind = ObservedStateSourceKind::kJointState;
  measured.state_authority_class = StateAuthorityClass::kMeasured;
  EXPECT_EQ(validate_observed_state_snapshot(measured, 500)
                .error()
                .details.at("reason"),
            "measured_freshness_budget_required");

  measured.freshness_budget_ns = 100;
  measured.received_timestamp_ns = 450;
  EXPECT_TRUE(validate_observed_state_snapshot(measured, 500).ok());
  EXPECT_EQ(validate_observed_state_snapshot(measured, 551)
                .error()
                .details.at("reason"),
            "observation_stale");

  auto mismatched = predicted;
  mismatched.source_kind = ObservedStateSourceKind::kJointState;
  EXPECT_EQ(validate_observed_state_snapshot(mismatched, 500)
                .error()
                .details.at("reason"),
            "source_authority_class_mismatch");
}

}  // namespace
}  // namespace manyforge::core
