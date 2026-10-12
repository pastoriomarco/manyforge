#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/observed_robot_state_provider.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

ObservedRobotStateSourceConfig source(ObservedRobotStateSourceKind kind, std::string id,
                                      std::int64_t freshness_budget_ns = 100) {
  return ObservedRobotStateSourceConfig{
      kind, std::move(id), ObservedRobotStateAuthorityProfile::kMeasured, freshness_budget_ns};
}

TEST(ObservedRobotStatePublicationTest, SelectsFreshSourceByPrecedenceWithoutMixingJointOrder) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kActionFeedback, "action"),
                     source(ObservedRobotStateSourceKind::kJointState, "joint")});
  EXPECT_EQ(publication.metrics().reader_retry_exhausted_total, 0u);

  const std::vector<std::string> reversed{"j2", "j1"};
  const std::vector<double> action_positions{2.0, 1.0};
  const std::vector<double> action_velocities{0.2, 0.1};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kActionFeedback, reversed,
                           action_positions, action_velocities, 10, 10)
                  .published);
  const std::vector<std::string> canonical{"j1", "j2"};
  const std::vector<double> joint_positions{3.0, 4.0};
  const std::vector<double> joint_velocities{0.3, 0.4};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, canonical, joint_positions,
                           joint_velocities, 150, 150)
                  .published);

  auto action = publication.acquire(50);
  ASSERT_TRUE(action);
  EXPECT_EQ(action.source_id(), "action");
  EXPECT_EQ(action.positions_rad()[0], 1.0);
  EXPECT_EQ(action.positions_rad()[1], 2.0);
  EXPECT_EQ(action.source_timestamp_ns(), 10);

  auto joint = publication.acquire(150);
  ASSERT_TRUE(joint);
  EXPECT_EQ(joint.source_id(), "joint");
  EXPECT_EQ(joint.positions_rad()[0], 3.0);
  EXPECT_EQ(joint.positions_rad()[1], 4.0);
  EXPECT_EQ(joint.source_timestamp_ns(), 150);
  auto dto = publication.snapshot(150);
  ASSERT_TRUE(dto.has_value());
  EXPECT_EQ(dto->joint_names, canonical);
  EXPECT_EQ(dto->source_kind, ObservedRobotStateSourceKind::kJointState);
  EXPECT_EQ(dto->authority_profile, "measured");
  ASSERT_TRUE(dto->velocities_rad_s.has_value());
  EXPECT_EQ(*dto->velocities_rad_s, joint_velocities);
}

TEST(ObservedRobotStatePublicationTest, NegativeZeroObservationsArePublishedAsPositiveZero) {
  // Stage F: a resumed motion's plan start state is the observed state, and
  // canonical prepared data refuses -0.0 (content identity). A sample may
  // carry -0.0 (e.g. a Stop ramp's rest velocity, negative path slope x zero
  // path rate); the observed state stores the equal +0.0.
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kJointState, "joint")});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> positions{-0.0, -0.25};
  const std::vector<double> velocities{-0.0, -0.5 * 0.0};
  ASSERT_TRUE(std::signbit(velocities[1]));
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints, positions,
                           velocities, 10, 10)
                  .published);
  {
    auto view = publication.acquire(10);
    ASSERT_TRUE(view);
    ASSERT_TRUE(view.velocities_complete());
    EXPECT_EQ(view.positions_rad()[0], 0.0);
    EXPECT_FALSE(std::signbit(view.positions_rad()[0]));
    EXPECT_EQ(view.positions_rad()[1], -0.25);
    for (const double velocity : view.velocities_rad_s()) {
      EXPECT_EQ(velocity, 0.0);
      EXPECT_FALSE(std::signbit(velocity));
    }
  }

  PublishedObservedRobotStateProvider provider(publication);
  const auto state = provider.current_state(10);
  ASSERT_TRUE(state.ok()) << state.error().message;
  for (const auto& values : {state.value().positions_rad, state.value().velocities_rad_s}) {
    for (const double value : values) {
      EXPECT_FALSE(value == 0.0 && std::signbit(value));
      EXPECT_NO_THROW(static_cast<void>(
          content_identity::canonical_json(content_identity::CanonicalValue(value))));
    }
  }
}

TEST(ObservedRobotStatePublicationTest, IncompleteHigherPrioritySourceFallsBackToCompleteSource) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kActionFeedback, "action"),
                     source(ObservedRobotStateSourceKind::kJointState, "joint")});
  const std::vector<std::string> one_joint{"j1"};
  const std::vector<double> one_position{9.0};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kActionFeedback, one_joint, one_position,
                           {}, 10, 10)
                  .published);
  const std::vector<std::string> both{"j1", "j2"};
  const std::vector<double> both_positions{1.0, 2.0};
  ASSERT_TRUE(
      publication
          .publish(ObservedRobotStateSourceKind::kJointState, both, both_positions, {}, 20, 20)
          .published);

  auto selected = publication.acquire(20);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_id(), "joint");
  EXPECT_TRUE(selected.complete());
}

TEST(ObservedRobotStatePublicationTest,
     AsOfSelectsNewestEligibleSampleWithoutReturningFuturePublication) {
  ObservedRobotStatePublication publication(
      {"j1"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 1000)});
  const std::vector<std::string> joints{"j1"};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints,
                           std::vector<double>{1.0}, {}, 100, 100)
                  .published);
  const auto second = publication.publish(ObservedRobotStateSourceKind::kJointState, joints,
                                          std::vector<double>{2.0}, {}, 101, 101);
  ASSERT_TRUE(second.published);

  const auto at_100 = publication.acquire_as_of(100);
  ASSERT_TRUE(at_100);
  EXPECT_EQ(at_100.received_timestamp_ns(), 100);
  EXPECT_DOUBLE_EQ(at_100.positions_rad().front(), 1.0);
  EXPECT_FALSE(publication.acquire_as_of(99));

  const auto at_101 = publication.acquire_as_of(101);
  ASSERT_TRUE(at_101);
  EXPECT_EQ(at_101.sequence(), second.sequence);
  EXPECT_DOUBLE_EQ(at_101.positions_rad().front(), 2.0);
  const auto latest = publication.acquire_latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest.sequence(), second.sequence);
}

TEST(ObservedRobotStatePublicationTest,
     AsOfCannotCrossAnEvictedInvalidationEvenWithAPinnedPredecessor) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 1000)});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> positions{1.0, 2.0};
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, positions, {}, 10, 10)
          .complete);
  const auto pinned = publication.acquire(10);
  ASSERT_TRUE(pinned);
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState,
                           std::span<const std::string>(joints).first(1),
                           std::span<const double>(positions).first(1), {}, 20, 20)
                  .published);
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, positions, {}, 30, 30)
          .complete);
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, positions, {}, 40, 40)
          .complete);
  EXPECT_FALSE(publication.acquire_as_of(20));
  EXPECT_EQ(pinned.received_timestamp_ns(), 10);
  // Current and retained, proven-newer history remain usable.
  const auto at_30 = publication.acquire_as_of(30);
  ASSERT_TRUE(at_30);
  EXPECT_EQ(at_30.received_timestamp_ns(), 30);
  EXPECT_TRUE(publication.acquire_as_of(40));
}

TEST(ObservedRobotStatePublicationTest,
     ProviderRejectsFallbackFromRequiredCommandAuthorityToMeasuredData) {
  auto command_source = ObservedRobotStateSourceConfig{
      ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
      ObservedRobotStateAuthorityProfile::kCommandedSimulation,
      /*budget_ns=*/0, "commanded_sim_v1"};
  ObservedRobotStatePublication publication(
      {"j1"}, {std::move(command_source),
               source(ObservedRobotStateSourceKind::kJointState, "joint", 1000)});
  const std::vector<std::string> joints{"j1"};
  const std::vector<double> measured{0.25};
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, measured, {}, 10, 10)
          .published);
  PublishedObservedRobotStateProvider provider(
      publication, ObservedRobotStateAuthorityProfile::kCommandedSimulation);

  const auto result = provider.current_state(10);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "observed_state_authority_mismatch");
  EXPECT_EQ(result.error().details.at("expected_authority_profile"), "commanded_simulation");
  EXPECT_EQ(result.error().details.at("actual_authority_profile"), "measured");
}

TEST(ObservedRobotStatePublicationTest, ProviderAcceptsTheDeploymentRequiredAuthorityProfile) {
  auto command_source = ObservedRobotStateSourceConfig{
      ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
      ObservedRobotStateAuthorityProfile::kCommandedSimulation,
      /*budget_ns=*/0, "commanded_sim_v1"};
  ObservedRobotStatePublication publication({"j1"}, {std::move(command_source)});
  const std::vector<std::string> joints{"j1"};
  const std::vector<double> commanded{0.75};
  const std::vector<double> velocities{0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kCommandedState, joints, commanded,
                           velocities, 10, 10)
                  .published);
  PublishedObservedRobotStateProvider provider(
      publication, ObservedRobotStateAuthorityProfile::kCommandedSimulation);

  const auto result = provider.current_state(10);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().positions_rad, commanded);
  EXPECT_EQ(result.value().velocities_rad_s, velocities);
}

TEST(ObservedRobotStatePublicationTest, ProviderRejectsAnInvalidatedNonExpiringAuthority) {
  auto command_source = ObservedRobotStateSourceConfig{
      ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
      ObservedRobotStateAuthorityProfile::kCommandedSimulation,
      /*budget_ns=*/0, "commanded_sim_v1"};
  ObservedRobotStatePublication publication({"j1"}, {std::move(command_source)});
  const std::vector<std::string> joints{"j1"};
  const std::vector<double> commanded{0.75};
  ASSERT_TRUE(
      publication
          .publish(ObservedRobotStateSourceKind::kCommandedState, joints, commanded, {}, 10, 10)
          .published);
  std::atomic<std::uint64_t> authority_epoch{2};
  PublishedObservedRobotStateProvider provider(
      publication, ObservedRobotStateAuthorityProfile::kCommandedSimulation, &authority_epoch);

  ASSERT_TRUE(provider.current_state(10).ok());
  authority_epoch.store(3, std::memory_order_release);
  const auto rejected = provider.current_state(10);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "observed_state_authority_invalidated");
  authority_epoch.store(4, std::memory_order_release);
  EXPECT_TRUE(provider.current_state(10).ok());
}

TEST(ObservedRobotStatePublicationTest, PinnedReadersCannotObserveReusedStorage) {
  ObservedRobotStatePublication publication(
      {"j1"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 1000)});
  const std::vector<std::string> joints{"j1"};
  const std::vector<double> one{1.0};
  const std::vector<double> two{2.0};
  const std::vector<double> three{3.0};
  const std::vector<double> four{4.0};
  ASSERT_TRUE(publication.publish(ObservedRobotStateSourceKind::kJointState, joints, one, {}, 1, 1)
                  .published);
  auto first = publication.acquire(1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(publication.publish(ObservedRobotStateSourceKind::kJointState, joints, two, {}, 2, 2)
                  .published);
  auto second = publication.acquire_as_of(2);
  ASSERT_TRUE(second);
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, three, {}, 3, 3)
          .published);
  auto third = publication.acquire(3);
  ASSERT_TRUE(third);

  const auto exhausted =
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, four, {}, 4, 4);
  EXPECT_FALSE(exhausted.published);
  EXPECT_STREQ(exhausted.reason, "publication_slots_exhausted");
  EXPECT_EQ(first.positions_rad()[0], 1.0);
  EXPECT_EQ(second.positions_rad()[0], 2.0);
  EXPECT_EQ(third.positions_rad()[0], 3.0);
  EXPECT_EQ(publication.metrics().slot_exhausted_total, 1u);
}

TEST(ObservedRobotStatePublicationTest, RejectsDuplicateCanonicalJointNames) {
  EXPECT_THROW(ObservedRobotStatePublication(
                   {"j1", "j1"}, {source(ObservedRobotStateSourceKind::kJointState, "joint")}),
               std::invalid_argument);
}

TEST(ObservedRobotStatePublicationTest, RejectsNonExpiringMeasuredSourceAtConstruction) {
  EXPECT_THROW(
      ObservedRobotStatePublication(
          {"j1"}, {{ObservedRobotStateSourceKind::kJointState, "joint",
                    ObservedRobotStateAuthorityProfile::kMeasured, 0, "ros2_control_measured_v1"}}),
      std::invalid_argument);
}

TEST(ObservedRobotStatePublicationTest, AllowsExplicitlyQualifiedNonExpiringPredictedSource) {
  EXPECT_NO_THROW(ObservedRobotStatePublication(
      {"j1"}, {{ObservedRobotStateSourceKind::kPredictedState, "predicted",
                ObservedRobotStateAuthorityProfile::kPredictedNative, 0, "native_predicted_v1"}}));
}

TEST(ObservedRobotStatePublicationTest, RejectsReceiptRegressionWithoutReplacingPublishedSample) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 100)});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> first_positions{1.0, 2.0};
  const auto first = publication.publish(ObservedRobotStateSourceKind::kJointState, joints,
                                         first_positions, {}, 10, 100);
  ASSERT_TRUE(first.published);

  const std::vector<double> older_positions{3.0, 4.0};
  const auto older = publication.publish(ObservedRobotStateSourceKind::kJointState, joints,
                                         older_positions, {}, 11, 99);
  EXPECT_FALSE(older.published);
  EXPECT_STREQ(older.reason, "received_timestamp_regressed");

  auto selected = publication.acquire(100);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.sequence(), first.sequence);
  ASSERT_EQ(selected.positions_rad().size(), first_positions.size());
  EXPECT_EQ(selected.positions_rad()[0], first_positions[0]);
  EXPECT_EQ(selected.positions_rad()[1], first_positions[1]);
  EXPECT_EQ(publication.metrics().rejected_total, 1U);
}

TEST(ObservedRobotStatePublicationTest, ConcurrentReaderNeverObservesMixedPublication) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 1'000'000)});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> initial{0.0, 0.0};
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, initial, {}, 1, 1)
          .published);

  std::atomic<bool> writer_done{false};
  std::atomic<bool> mixed_sample{false};
  std::thread writer([&]() {
    for (int i = 1; i <= 20'000; ++i) {
      const std::vector<double> positions{static_cast<double>(i), -static_cast<double>(i)};
      while (!publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints, positions, {}, i + 1,
                           i + 1)
                  .published) {
        std::this_thread::yield();
      }
    }
    writer_done.store(true, std::memory_order_release);
  });
  std::thread reader([&]() {
    while (!writer_done.load(std::memory_order_acquire)) {
      auto view = publication.acquire(20'001);
      if (view && view.complete() && view.positions_rad().size() == 2u &&
          view.positions_rad()[0] != -view.positions_rad()[1]) {
        mixed_sample.store(true, std::memory_order_release);
        return;
      }
    }
  });
  writer.join();
  reader.join();
  EXPECT_FALSE(mixed_sample.load(std::memory_order_acquire));
}

TEST(ObservedRobotStatePublicationTest, ConcurrentInvalidSamplesCannotResurrectPinnedPredecessor) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 1000000)});
  const std::vector<std::string> names{"j1", "j2"};
  const std::vector<double> complete{0.0, 0.0}, partial{0.0};
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, names, complete, {}, 1, 1)
          .complete);
  auto retained = publication.acquire(1);
  ASSERT_TRUE(retained);
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState,
                           std::span<const std::string>(names).first(1), partial, {}, 2, 2)
                  .published);
  std::atomic<bool> start{}, resurrected{};
  std::thread writer([&] {
    while (!start.load()) std::this_thread::yield();
    for (int i = 3; i < 4003; ++i) {
      while (!publication
                  .publish(ObservedRobotStateSourceKind::kJointState,
                           std::span<const std::string>(names).first(1), partial, {}, i, i)
                  .published)
        std::this_thread::yield();
    }
  });
  start = true;
  for (int i = 0; i < 4000; ++i) {
    if (publication.acquire_as_of(10000)) resurrected = true;
  }
  writer.join();
  EXPECT_FALSE(resurrected.load());
  EXPECT_EQ(retained.sequence(), 1U);
}

TEST(ObservedRobotStatePublicationTest, BoundedConcurrentAsOfReaderNeverObservesMixedPublication) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"}, {source(ObservedRobotStateSourceKind::kJointState, "joint", 1'000'000)});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> initial{0.0, 0.0};
  ASSERT_TRUE(
      publication.publish(ObservedRobotStateSourceKind::kJointState, joints, initial, {}, 1, 1)
          .published);

  constexpr int kPublicationCount = 4'000;
  constexpr int kReaderAttempts = 4'000;
  std::atomic<bool> start{false};
  std::atomic<bool> mixed_sample{false};
  std::thread writer([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int i = 1; i <= kPublicationCount; ++i) {
      const std::vector<double> positions{static_cast<double>(i), -static_cast<double>(i)};
      while (!publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints, positions, {}, i + 1,
                           i + 1)
                  .published) {
        std::this_thread::yield();
      }
    }
  });
  std::thread reader([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int attempt = 0;
         attempt < kReaderAttempts && !mixed_sample.load(std::memory_order_acquire); ++attempt) {
      auto view = publication.acquire_as_of(kPublicationCount + 1);
      if (view && view.complete() && view.positions_rad().size() == 2u &&
          view.positions_rad()[0] != -view.positions_rad()[1]) {
        mixed_sample.store(true, std::memory_order_release);
        return;
      }
    }
  });
  start.store(true, std::memory_order_release);
  writer.join();
  reader.join();
  EXPECT_FALSE(mixed_sample.load(std::memory_order_acquire));
}

}  // namespace
}  // namespace manyforge::core
