#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <manyforge_core/addressed_observed_state_publication.hpp>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
struct AddressedObservedStatePublicationTestAccess {
  /// Holds the context the way a route rebind does.
  static std::unique_lock<std::shared_mutex> LockContext(
      AddressedObservedStatePublication& publication) {
    return std::unique_lock(publication.route_context_mutex_);
  }
  /// Holds the context the way an ordinary reader does.
  static std::shared_lock<std::shared_mutex> ShareContext(
      AddressedObservedStatePublication& publication) {
    return std::shared_lock(publication.route_context_mutex_);
  }
};
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
  return ObservedStateQueryContextDTO{ControllableComponentId("component-a"),
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

[[nodiscard]] AddressedObservedStatePublicationConfig config(ObservedStateQueryContextDTO query) {
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
    source.authority_profile = ObservedRobotStateAuthorityProfile::kCommandedSimulation;
  } else {
    source.kind = ObservedRobotStateSourceKind::kPredictedState;
    source.authority_profile = ObservedRobotStateAuthorityProfile::kPredictedNative;
  }
  return {std::move(query), {std::move(source)}, 7U, 16U};
}

TEST(AddressedObservedStatePublicationTest, PreservesMixedAngularAndLinearUnitsAndExactBinding) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular, CoordinateKind::kLinear})));
  const auto names = publication.query_context().joint_names;
  const std::array positions{0.25, 0.0081};
  const std::array velocities{0.5, 0.002};

  const auto published = publication.publish(ObservedStateSourceKind::kPredictedState, names,
                                             positions, velocities, 40, 50);
  ASSERT_TRUE(published.published);
  ASSERT_TRUE(published.complete);

  const auto state = publication.current_state(publication.query_context(), 1'000'000);
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().positions.size(), 2U);
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(state.value().positions[0]).value_rad, 0.25);
  EXPECT_DOUBLE_EQ(std::get<LinearPositionDTO>(state.value().positions[1]).value_m, 0.0081);
  ASSERT_TRUE(state.value().velocities.has_value());
  EXPECT_DOUBLE_EQ(std::get<LinearVelocityDTO>((*state.value().velocities)[1]).value_m_s, 0.002);
  EXPECT_EQ(state.value().control_binding_revision,
            publication.query_context().control_binding_revision);
  EXPECT_EQ(state.value().authority_generation, 7U);

  const auto latest = publication.publication(publication.query_context().control_binding_id,
                                              publication.query_context().control_binding_revision);
  ASSERT_TRUE(latest.ok());
  EXPECT_EQ(latest.value().publication_sequence, published.sequence);
  EXPECT_EQ(latest.value().snapshot, state.value());
}

TEST(AddressedObservedStatePublicationTest,
     AsOfPublicationRetainsPriorSampleAndValidatesItsEnvelopeAtRequestedTime) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto names = publication.query_context().joint_names;
  const std::array first_position{0.1};
  const std::array second_position{0.2};
  const auto first = publication.publish(ObservedStateSourceKind::kPredictedState, names,
                                         first_position, {}, 100, 100);
  ASSERT_TRUE(first.published);
  const auto second = publication.publish(ObservedStateSourceKind::kPredictedState, names,
                                          second_position, {}, 101, 101);
  ASSERT_TRUE(second.published);

  const auto at_100 =
      publication.publication_at(publication.query_context().control_binding_id,
                                 publication.query_context().control_binding_revision, 100);
  ASSERT_TRUE(at_100.ok()) << at_100.error().message;
  EXPECT_EQ(at_100.value().publication_sequence, first.sequence);
  EXPECT_EQ(at_100.value().snapshot.received_timestamp_ns, 100);
  EXPECT_TRUE(validate_observed_state_publication(at_100.value(), 100).ok());

  const auto before_first =
      publication.publication_at(publication.query_context().control_binding_id,
                                 publication.query_context().control_binding_revision, 99);
  ASSERT_FALSE(before_first.ok());
  EXPECT_EQ(before_first.error().details.at("reason"),
            "observed_state_unavailable_at_evaluation_time");

  const auto at_101 =
      publication.publication_at(publication.query_context().control_binding_id,
                                 publication.query_context().control_binding_revision, 101);
  ASSERT_TRUE(at_101.ok());
  EXPECT_EQ(at_101.value().publication_sequence, second.sequence);

  const auto latest = publication.publication(publication.query_context().control_binding_id,
                                              publication.query_context().control_binding_revision);
  ASSERT_TRUE(latest.ok());
  EXPECT_EQ(latest.value().publication_sequence, second.sequence);
}

TEST(AddressedObservedStatePublicationTest,
     CurrentStateDoesNotCombineAnEarlierReadBoundaryWithANewerSample) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto names = publication.query_context().joint_names;
  const std::array first_position{0.1};
  const std::array second_position{0.2};
  ASSERT_TRUE(
      publication
          .publish(ObservedStateSourceKind::kPredictedState, names, first_position, {}, 100, 100)
          .published);
  ASSERT_TRUE(
      publication
          .publish(ObservedStateSourceKind::kPredictedState, names, second_position, {}, 101, 101)
          .published);

  const auto state = publication.current_state(publication.query_context(), 100);

  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().received_timestamp_ns, 100);
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(state.value().positions[0]).value_rad, 0.1);
}

TEST(AddressedObservedStatePublicationTest, RejectsForeignContextAndStaleMeasuredState) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular}, StateAuthorityClass::kMeasured, 100)));
  const auto names = publication.query_context().joint_names;
  const std::array positions{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kJointState, names, positions,
                           std::span<const double>{}, 10, 20)
                  .published);

  auto foreign = publication.query_context();
  foreign.control_route_id = ControlRouteId("other-route");
  const auto mismatched = publication.current_state(foreign, 30);
  ASSERT_FALSE(mismatched.ok());
  EXPECT_EQ(mismatched.error().details.at("reason"), "observed_state_query_context_mismatch");

  const auto stale = publication.current_state(publication.query_context(), 121);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "observation_stale");
}

TEST(AddressedObservedStatePublicationTest,
     MeasuredStateStaysCurrentWhenEveryRetainedSampleFollowsTheBoundary) {
  // Live Real flicker: the readiness boundary is taken first and the
  // broadcaster then fills every retained slot. The newest sample is current.
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular}, StateAuthorityClass::kMeasured, 10)));
  const auto names = publication.query_context().joint_names;
  const std::array positions{0.0};
  for (const std::int64_t received : {101, 102, 103, 104, 105}) {
    ASSERT_TRUE(publication
                    .publish(ObservedStateSourceKind::kJointState, names, positions,
                             std::span<const double>{}, received, received)
                    .published);
  }
  const auto state = publication.current_state(publication.query_context(), 100);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().received_timestamp_ns, 105);
}

TEST(AddressedObservedStatePublicationTest, AsOfCannotResurrectSupersededIncompleteState) {
  AddressedObservedStatePublication publication(config(context(
      {CoordinateKind::kAngular, CoordinateKind::kAngular}, StateAuthorityClass::kMeasured, 10)));
  const auto names = publication.query_context().joint_names;
  const std::array complete{0.1, 0.2};
  ASSERT_TRUE(
      publication.publish(ObservedStateSourceKind::kJointState, names, complete, {}, 100, 100)
          .complete);
  const std::array partial{0.3};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kJointState,
                           std::span<const std::string>(names).first(1), partial, {}, 101, 101)
                  .published);
  const auto before = publication.current_state(publication.query_context(), 100);
  ASSERT_TRUE(before.ok());
  EXPECT_EQ(before.value().received_timestamp_ns, 100);
  const auto after = publication.current_state(publication.query_context(), 101);
  ASSERT_FALSE(after.ok());
  // Missing coordinates are represented as invalid numeric slots, so the
  // coordinate validator rejects before the completeness check.
  EXPECT_EQ(after.error().details.at("reason"), "coordinate_position_nonfinite");
  EXPECT_FALSE(publication
                   .publication_at(publication.query_context().control_binding_id,
                                   publication.query_context().control_binding_revision, 101)
                   .ok());
  ASSERT_TRUE(
      publication.publish(ObservedStateSourceKind::kJointState, names, complete, {}, 102, 102)
          .complete);
  EXPECT_TRUE(publication.current_state(publication.query_context(), 112).ok());
  EXPECT_FALSE(publication.current_state(publication.query_context(), 113).ok());
}

TEST(AddressedObservedStatePublicationTest,
     AsOfRebindNeverRestampsPredecessorOrForeignOwnerEvidence) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto original = publication.query_context();
  const std::array position{0.1};
  const std::array velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, original.joint_names, position,
                           velocity, 100, 100)
                  .published);
  auto rebound = original;
  rebound.control_route_id = ControlRouteId("route-b");
  rebound.active_control_configuration_generation = ActiveControlConfigurationGeneration(5U);
  ASSERT_TRUE(publication.rebind_route_context(rebound, 110).ok());
  EXPECT_FALSE(publication.current_state(original, 110).ok());
  EXPECT_FALSE(publication.current_state(rebound, 109).ok());
  EXPECT_FALSE(
      publication.publication_at(rebound.control_binding_id, rebound.control_binding_revision, 109)
          .ok());
  const auto current = publication.current_state(rebound, 110);
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().control_route_id, rebound.control_route_id);
  EXPECT_EQ(current.value().received_timestamp_ns, 110);
  auto foreign = rebound;
  foreign.runtime_epoch = RuntimeEpoch("foreign.owner");
  EXPECT_FALSE(publication.rebind_route_context(foreign, 111).ok());
  AddressedObservedStatePublication replacement(config(foreign));
  EXPECT_FALSE(replacement.current_state(foreign, 112).ok());
}

TEST(AddressedObservedStatePublicationTest, RebindCannotRefreshAnExpiredSourceSample) {
  // A source sample received after the transition time is current
  // (received_sample_age.hpp); only an expired one refuses.
  for (const auto transition : {99, 110, 111}) {
    SCOPED_TRACE(transition);
    AddressedObservedStatePublication publication(
        config(context({CoordinateKind::kAngular}, StateAuthorityClass::kPredicted, 10)));
    const auto original = publication.query_context();
    const std::array position{0.1}, velocity{0.0};
    ASSERT_TRUE(publication
                    .publish(ObservedStateSourceKind::kPredictedState, original.joint_names,
                             position, velocity, 100, 100)
                    .published);
    auto next = original;
    next.control_route_id = ControlRouteId("successor");
    const auto rebound = publication.rebind_route_context(next, transition);
    EXPECT_EQ(rebound.ok(), transition <= 110);
    if (rebound.ok()) {
      // The seed is never stamped before the source sample it copies.
      EXPECT_EQ(publication.numeric_publication().acquire_latest().received_timestamp_ns(),
                std::max<std::int64_t>(transition, 100));
    } else {
      EXPECT_EQ(rebound.error().details.at("reason"),
                "observed_state_route_rebind_source_not_current");
      EXPECT_EQ(publication.query_context(), original);
      EXPECT_EQ(publication.numeric_publication().acquire_latest().received_timestamp_ns(), 100);
    }
  }
}

TEST(AddressedObservedStatePublicationTest,
     BusyContextRejectsWithoutBlockingNumericReadersOrWriters) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto query = publication.query_context();
  auto held = AddressedObservedStatePublicationTestAccess::LockContext(publication);
  const std::array positions{0.1};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, query.joint_names, positions,
                           {}, 100, 100)
                  .published);
  EXPECT_TRUE(publication.numeric_publication().acquire_as_of(100));
  EXPECT_EQ(publication.current_state(query, 100).error().details.at("reason"),
            "observed_state_route_context_busy");
  EXPECT_FALSE(
      publication.publication(query.control_binding_id, query.control_binding_revision).ok());
  EXPECT_FALSE(
      publication.publication_at(query.control_binding_id, query.control_binding_revision, 100)
          .ok());
  held.unlock();
  EXPECT_TRUE(publication.current_state(query, 100).ok());
}

TEST(AddressedObservedStatePublicationTest, OrdinaryReadersDoNotRefuseEachOther) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto query = publication.query_context();
  const std::array positions{0.1};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, query.joint_names, positions,
                           {}, 100, 100)
                  .published);
  // Another reader is inside its read. Every read kind still succeeds.
  auto reader = AddressedObservedStatePublicationTestAccess::ShareContext(publication);
  EXPECT_TRUE(publication.current_state(query, 100).ok());
  EXPECT_TRUE(
      publication.publication(query.control_binding_id, query.control_binding_revision).ok());
  EXPECT_TRUE(
      publication.publication_at(query.control_binding_id, query.control_binding_revision, 100)
          .ok());
  EXPECT_EQ(publication.query_context(), query);
}

TEST(AddressedObservedStatePublicationTest,
     RebindWaitsForAReaderAndNeverExposesAPartialRoute) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto original = publication.query_context();
  const std::array position{0.1}, velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, original.joint_names, position,
                           velocity, 100, 100)
                  .published);
  auto next = original;
  next.control_route_id = ControlRouteId("successor");

  auto reader = AddressedObservedStatePublicationTestAccess::ShareContext(publication);
  auto rebind = std::async(std::launch::async, [&publication, next] {
    return publication.rebind_route_context(next, 110);
  });
  // The rebind is not refused and cannot complete while the reader holds the
  // context; the reader keeps seeing the predecessor route.
  EXPECT_EQ(rebind.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  const auto during = publication.current_state(original, 100);
  ASSERT_TRUE(during.ok());
  EXPECT_EQ(during.value().control_route_id, original.control_route_id);

  reader.unlock();
  const auto rebound = rebind.get();
  ASSERT_TRUE(rebound.ok());
  EXPECT_EQ(publication.query_context(), next);
  EXPECT_FALSE(publication.current_state(original, 110).ok());
  const auto after = publication.current_state(next, 110);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value().control_route_id, next.control_route_id);
}

TEST(AddressedObservedStatePublicationTest,
     RepeatedConcurrentRouteChangesHaveNoLifetimeAllowanceOrMixedIdentity) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto original = publication.query_context();
  const std::array position{0.1}, velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, original.joint_names, position,
                           velocity, 100, 100)
                  .published);
  std::barrier start(2);
  std::atomic<bool> done{}, failed{};
  std::thread writer([&] {
    start.arrive_and_wait();
    for (std::uint64_t i = 1; i <= 5000; ++i) {
      auto next = original;
      next.active_control_configuration_generation = ActiveControlConfigurationGeneration(4 + i);
      if (!publication.rebind_route_context(std::move(next), static_cast<std::int64_t>(100 + i))
               .ok())
        failed = true;
    }
    done = true;
  });
  start.arrive_and_wait();
  do {
    const auto query = publication.query_context();
    const auto state = publication.current_state(query, 100000);
    if (state.ok()) {
      EXPECT_EQ(state.value().active_control_configuration_generation,
                query.active_control_configuration_generation);
      EXPECT_EQ(
          state.value().received_timestamp_ns,
          static_cast<std::int64_t>(96 + query.active_control_configuration_generation.value()));
    }
  } while (!done.load());
  writer.join();
  EXPECT_FALSE(failed.load());
  EXPECT_EQ(publication.query_context().active_control_configuration_generation.value(), 5004U);
  EXPECT_EQ(original.active_control_configuration_generation.value(), 4U);
}

TEST(AddressedObservedStatePublicationTest,
     RepublishesPredictedOwnerSampleAtSameProviderEpochRouteRebind) {
  AddressedObservedStatePublication publication(config(context({CoordinateKind::kAngular})));
  const auto names = publication.query_context().joint_names;
  const std::array old_position{0.1};
  const std::array<double, 1U> old_velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, names, old_position,
                           old_velocity, 100, 100)
                  .published);

  auto rebound = publication.query_context();
  rebound.control_route_id = ControlRouteId("route-b");
  rebound.active_control_configuration_id = ActiveControlConfigurationId("active-b");
  rebound.active_control_configuration_generation = ActiveControlConfigurationGeneration(5U);
  ASSERT_TRUE(publication.rebind_route_context(std::move(rebound), 110).ok());

  const auto rebound_state = publication.current_state(publication.query_context(), 110);
  ASSERT_TRUE(rebound_state.ok()) << rebound_state.error().message;
  EXPECT_EQ(rebound_state.value().control_route_id, ControlRouteId("route-b"));
  EXPECT_EQ(rebound_state.value().received_timestamp_ns, 110);
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(rebound_state.value().positions.front()).value_rad,
                   0.1);

  const auto rebound_publication =
      publication.publication(publication.query_context().control_binding_id,
                              publication.query_context().control_binding_revision);
  ASSERT_TRUE(rebound_publication.ok());
  EXPECT_EQ(rebound_publication.value().publication_sequence, 2U);
  EXPECT_EQ(rebound_publication.value().snapshot.control_route_id, ControlRouteId("route-b"));

  const std::array new_position{0.2};
  const std::array<double, 1U> new_velocity{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kPredictedState, names, new_position,
                           new_velocity, 111, 111)
                  .published);
  const auto current = publication.current_state(publication.query_context(), 111);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_EQ(current.value().control_route_id, ControlRouteId("route-b"));
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(current.value().positions.front()).value_rad, 0.2);
}

TEST(AddressedObservedStatePublicationTest,
     DoesNotRebindMeasuredAuthorityWithoutAFreshMeasurement) {
  AddressedObservedStatePublication publication(
      config(context({CoordinateKind::kAngular}, StateAuthorityClass::kMeasured, 100)));
  const auto names = publication.query_context().joint_names;
  const std::array position{0.1};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kJointState, names, position,
                           std::span<const double>{}, 100, 100)
                  .published);

  auto rebound = publication.query_context();
  rebound.control_route_id = ControlRouteId("route-b");
  // The route identity may change, but the measurement is never
  // re-stamped; the new route has no state until a fresh measurement.
  ASSERT_TRUE(publication.rebind_route_context(rebound, 110).ok());
  EXPECT_EQ(publication.query_context().control_route_id, ControlRouteId("route-b"));
  const auto stranded = publication.current_state(publication.query_context(), 110);
  ASSERT_FALSE(stranded.ok());
  EXPECT_EQ(stranded.error().details.at("reason"),
            "observed_state_context_not_current_at_evaluation_time");
  EXPECT_FALSE(publication
                   .publication(publication.query_context().control_binding_id,
                                publication.query_context().control_binding_revision)
                   .ok());
  // The same identity is a no-op; a negative transition time still refuses.
  EXPECT_TRUE(publication.rebind_route_context(rebound, 111).ok());
  auto other = rebound;
  other.control_route_id = ControlRouteId("route-c");
  const auto refused = publication.rebind_route_context(other, -1);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "observed_state_route_rebind_not_permitted");

  const std::array fresh{0.2};
  ASSERT_TRUE(publication
                  .publish(ObservedStateSourceKind::kJointState, names, fresh,
                           std::span<const double>{}, 112, 112)
                  .published);
  const auto current = publication.current_state(publication.query_context(), 112);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_EQ(current.value().control_route_id, ControlRouteId("route-b"));
  EXPECT_EQ(current.value().received_timestamp_ns, 112);
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(current.value().positions.front()).value_rad, 0.2);
}

TEST(AddressedObservedStatePublicationTest, AllowsLegacyProjectionOnlyForProvenAllAngularTarget) {
  AddressedObservedStatePublication angular(
      config(context({CoordinateKind::kAngular, CoordinateKind::kAngular})));
  AddressedAngularObservedRobotStateAdapter adapter(angular);
  const auto names = angular.query_context().joint_names;
  const std::array positions{0.1, -0.2};
  ASSERT_TRUE(angular
                  .publish(ObservedStateSourceKind::kPredictedState, names, positions,
                           std::span<const double>{}, 1, 2)
                  .published);
  const auto legacy = adapter.current_state(3);
  ASSERT_TRUE(legacy.ok());
  EXPECT_EQ(legacy.value().joint_names, names);
  EXPECT_EQ(legacy.value().positions_rad, (std::vector<double>{0.1, -0.2}));
  EXPECT_EQ(adapter.publication(), &angular.numeric_publication());

  AddressedObservedStatePublication mixed(
      config(context({CoordinateKind::kAngular, CoordinateKind::kLinear})));
  EXPECT_THROW((void)AddressedAngularObservedRobotStateAdapter(mixed), std::invalid_argument);
}

TEST(AddressedObservedStatePublicationTest,
     RejectsAuthoritySourceAndResourceShapeMismatchAtConstruction) {
  auto wrong_source = config(context({CoordinateKind::kAngular}));
  wrong_source.source_precedence.front().kind = ObservedRobotStateSourceKind::kJointState;
  EXPECT_THROW((void)AddressedObservedStatePublication(std::move(wrong_source)),
               std::invalid_argument);

  auto wrong_shape = config(context({CoordinateKind::kAngular}));
  wrong_shape.query_context.coordinate_kinds.push_back(CoordinateKind::kLinear);
  EXPECT_THROW((void)AddressedObservedStatePublication(std::move(wrong_shape)),
               std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::core
