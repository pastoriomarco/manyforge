#include <gtest/gtest.h>

#include <manyforge_core/addressed_observed_state_publication.hpp>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

[[nodiscard]] ObservedStateQueryContextDTO context(
    std::vector<CoordinateKind> kinds,
    StateAuthorityClass authority = StateAuthorityClass::kPredicted,
    std::int64_t freshness_budget_ns = 0) {
  std::vector<ResourceId> resources;
  std::vector<std::string> names;
  for (std::size_t index = 0U; index < kinds.size(); ++index) {
    resources.emplace_back("resource-" + std::to_string(index));
    names.push_back("joint-" + std::to_string(index));
  }
  return ObservedStateQueryContextDTO{
      ControllableComponentId("component-a"),
      CommandTargetId("target-a"),
      CapabilityId("state.observed.current"),
      ControlDomainId("domain-a"),
      ControlDomainRevision("domain-revision-a"),
      ControlBindingId("binding-a"),
      ControlBindingRevision("binding-revision-a"),
      ControllerTopologyId("topology-a"),
      TopologyRevision("topology-revision-a"),
      ControlRouteId("route-a"),
      std::move(resources),
      std::move(names),
      std::move(kinds),
      ControlInterfaceContractId("interface-a"),
      ControlInterfaceRevision("interface-revision-a"),
      ActiveControlConfigurationId("active-a"),
      ActiveControlConfigurationGeneration(4U),
      ProviderInstanceId("provider-a"),
      ProviderManifestId("manifest-a"),
      ProviderManifestRevision("manifest-revision-a"),
      RuntimeEpoch("runtime-epoch-a"),
      StateAuthorityId("authority-a"),
      authority,
      freshness_budget_ns};
}

[[nodiscard]] AddressedObservedStatePublicationConfig config(
    ObservedStateQueryContextDTO query) {
  const auto authority = query.state_authority_class;
  ObservedRobotStateSourceConfig source;
  source.source_id = "native-state";
  source.freshness_budget_ns = query.freshness_budget_ns;
  source.authority_profile_id = query.state_authority_id.value();
  if (authority == StateAuthorityClass::kMeasured) {
    source.kind = ObservedRobotStateSourceKind::kJointState;
    source.authority_profile = ObservedRobotStateAuthorityProfile::kMeasured;
  } else if (authority == StateAuthorityClass::kCommanded) {
    source.kind = ObservedRobotStateSourceKind::kCommandedState;
    source.authority_profile =
        ObservedRobotStateAuthorityProfile::kCommandedSimulation;
  } else {
    source.kind = ObservedRobotStateSourceKind::kPredictedState;
    source.authority_profile =
        ObservedRobotStateAuthorityProfile::kPredictedNative;
  }
  return {std::move(query), {std::move(source)}, 7U, 16U};
}

TEST(AddressedObservedStatePublicationTest,
     PreservesMixedAngularAndLinearUnitsAndExactBinding) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular, CoordinateKind::kLinear})));
  const auto names = publication.query_context().joint_names;
  const std::array positions{0.25, 0.0081};
  const std::array velocities{0.5, 0.002};

  const auto published = publication.publish(
      ObservedStateSourceKind::kPredictedState, names, positions, velocities,
      40, 50);
  ASSERT_TRUE(published.published);
  ASSERT_TRUE(published.complete);

  const auto state = publication.current_state(
      publication.query_context(), 1'000'000);
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().positions.size(), 2U);
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(state.value().positions[0]).value_rad,
      0.25);
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(state.value().positions[1]).value_m,
      0.0081);
  ASSERT_TRUE(state.value().velocities.has_value());
  EXPECT_DOUBLE_EQ(
      std::get<LinearVelocityDTO>((*state.value().velocities)[1]).value_m_s,
      0.002);
  EXPECT_EQ(state.value().control_binding_revision,
            publication.query_context().control_binding_revision);
  EXPECT_EQ(state.value().authority_generation, 7U);

  const auto latest = publication.publication(
      publication.query_context().control_binding_id,
      publication.query_context().control_binding_revision);
  ASSERT_TRUE(latest.ok());
  EXPECT_EQ(latest.value().publication_sequence, published.sequence);
  EXPECT_EQ(latest.value().snapshot, state.value());
}

TEST(AddressedObservedStatePublicationTest,
     AsOfPublicationRetainsPriorSampleAndValidatesItsEnvelopeAtRequestedTime) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular})));
  const auto names = publication.query_context().joint_names;
  const std::array first_position{0.1};
  const std::array second_position{0.2};
  const auto first = publication.publish(
      ObservedStateSourceKind::kPredictedState, names, first_position, {},
      100, 100);
  ASSERT_TRUE(first.published);
  const auto second = publication.publish(
      ObservedStateSourceKind::kPredictedState, names, second_position, {},
      101, 101);
  ASSERT_TRUE(second.published);

  const auto at_100 = publication.publication_at(
      publication.query_context().control_binding_id,
      publication.query_context().control_binding_revision, 100);
  ASSERT_TRUE(at_100.ok()) << at_100.error().message;
  EXPECT_EQ(at_100.value().publication_sequence, first.sequence);
  EXPECT_EQ(at_100.value().snapshot.received_timestamp_ns, 100);
  EXPECT_TRUE(validate_observed_state_publication(at_100.value(), 100).ok());

  const auto before_first = publication.publication_at(
      publication.query_context().control_binding_id,
      publication.query_context().control_binding_revision, 99);
  ASSERT_FALSE(before_first.ok());
  EXPECT_EQ(before_first.error().details.at("reason"),
            "observed_state_unavailable_at_evaluation_time");

  const auto at_101 = publication.publication_at(
      publication.query_context().control_binding_id,
      publication.query_context().control_binding_revision, 101);
  ASSERT_TRUE(at_101.ok());
  EXPECT_EQ(at_101.value().publication_sequence, second.sequence);

  const auto latest = publication.publication(
      publication.query_context().control_binding_id,
      publication.query_context().control_binding_revision);
  ASSERT_TRUE(latest.ok());
  EXPECT_EQ(latest.value().publication_sequence, second.sequence);
}

TEST(AddressedObservedStatePublicationTest,
     CurrentStateDoesNotCombineAnEarlierReadBoundaryWithANewerSample) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular})));
  const auto names = publication.query_context().joint_names;
  const std::array first_position{0.1};
  const std::array second_position{0.2};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, names,
                           first_position, {}, 100, 100)
                  .published);
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, names,
                           second_position, {}, 101, 101)
                  .published);

  const auto state = publication.current_state(publication.query_context(), 100);

  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().received_timestamp_ns, 100);
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(state.value().positions[0]).value_rad, 0.1);
}

TEST(AddressedObservedStatePublicationTest,
     RejectsForeignContextAndStaleMeasuredState) {
  AddressedObservedStatePublication publication(config(context(
      {CoordinateKind::kAngular}, StateAuthorityClass::kMeasured, 100)));
  const auto names = publication.query_context().joint_names;
  const std::array positions{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kJointState, names,
                           positions, std::span<const double>{}, 10, 20)
                  .published);

  auto foreign = publication.query_context();
  foreign.control_route_id = ControlRouteId("other-route");
  const auto mismatched = publication.current_state(foreign, 30);
  ASSERT_FALSE(mismatched.ok());
  EXPECT_EQ(mismatched.error().details.at("reason"),
            "observed_state_query_context_mismatch");

  const auto stale = publication.current_state(
      publication.query_context(), 121);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "observation_stale");
}

TEST(AddressedObservedStatePublicationTest,
     RepublishesPredictedOwnerSampleAtSameProviderEpochRouteRebind) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular})));
  const auto names = publication.query_context().joint_names;
  const std::array old_position{0.1};
  const std::array<double, 1U> old_velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, names,
                           old_position, old_velocity, 100, 100)
                  .published);

  auto rebound = publication.query_context();
  rebound.control_route_id = ControlRouteId("route-b");
  rebound.active_control_configuration_id =
      ActiveControlConfigurationId("active-b");
  rebound.active_control_configuration_generation =
      ActiveControlConfigurationGeneration(5U);
  ASSERT_TRUE(publication.rebind_route_context(std::move(rebound), 110).ok());

  const auto rebound_state = publication.current_state(
      publication.query_context(), 110);
  ASSERT_TRUE(rebound_state.ok()) << rebound_state.error().message;
  EXPECT_EQ(rebound_state.value().control_route_id, ControlRouteId("route-b"));
  EXPECT_EQ(rebound_state.value().received_timestamp_ns, 110);
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(rebound_state.value().positions.front())
          .value_rad,
      0.1);

  const auto rebound_publication = publication.publication(
      publication.query_context().control_binding_id,
      publication.query_context().control_binding_revision);
  ASSERT_TRUE(rebound_publication.ok());
  EXPECT_EQ(rebound_publication.value().publication_sequence, 2U);
  EXPECT_EQ(rebound_publication.value().snapshot.control_route_id,
            ControlRouteId("route-b"));

  const std::array new_position{0.2};
  const std::array<double, 1U> new_velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, names,
                           new_position, new_velocity, 111, 111)
                  .published);
  const auto current =
      publication.current_state(publication.query_context(), 111);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_EQ(current.value().control_route_id, ControlRouteId("route-b"));
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(current.value().positions.front()).value_rad,
      0.2);
}

TEST(AddressedObservedStatePublicationTest,
     DoesNotRebindMeasuredAuthorityWithoutAFreshMeasurement) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular},
                     StateAuthorityClass::kMeasured, 100)));
  const auto names = publication.query_context().joint_names;
  const std::array position{0.1};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kJointState, names,
                           position, std::span<const double>{}, 100, 100)
                  .published);

  auto rebound = publication.query_context();
  rebound.control_route_id = ControlRouteId("route-b");
  const auto result =
      publication.rebind_route_context(std::move(rebound), 110);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "observed_state_route_rebind_not_permitted");
}

TEST(AddressedObservedStatePublicationTest,
     AllowsLegacyProjectionOnlyForProvenAllAngularTarget) {
  AddressedObservedStatePublication angular(
      config(context({CoordinateKind::kAngular, CoordinateKind::kAngular})));
  AddressedAngularObservedRobotStateAdapter adapter(angular);
  const auto names = angular.query_context().joint_names;
  const std::array positions{0.1, -0.2};
  ASSERT_TRUE(angular
                  .publish(ObservedStateSourceKind::kPredictedState, names,
                           positions, std::span<const double>{}, 1, 2)
                  .published);
  const auto legacy = adapter.current_state(3);
  ASSERT_TRUE(legacy.ok());
  EXPECT_EQ(legacy.value().joint_names, names);
  EXPECT_EQ(legacy.value().positions_rad,
            (std::vector<double>{0.1, -0.2}));
  EXPECT_EQ(adapter.publication(), &angular.numeric_publication());

  AddressedObservedStatePublication mixed(
      config(context({CoordinateKind::kAngular, CoordinateKind::kLinear})));
  EXPECT_THROW((void)AddressedAngularObservedRobotStateAdapter(mixed),
               std::invalid_argument);
}

TEST(AddressedObservedStatePublicationTest,
     RejectsAuthoritySourceAndResourceShapeMismatchAtConstruction) {
  auto wrong_source = config(context({CoordinateKind::kAngular}));
  wrong_source.source_precedence.front().kind =
      ObservedRobotStateSourceKind::kJointState;
  EXPECT_THROW((void)AddressedObservedStatePublication(
                   std::move(wrong_source)),
               std::invalid_argument);

  auto wrong_shape = config(context({CoordinateKind::kAngular}));
  wrong_shape.query_context.coordinate_kinds.push_back(
      CoordinateKind::kLinear);
  EXPECT_THROW((void)AddressedObservedStatePublication(
                   std::move(wrong_shape)),
               std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::core
