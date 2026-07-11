#include <gtest/gtest.h>

#include <manyforge_core/observed_robot_state_publication.hpp>

#include <string>
#include <atomic>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

ObservedRobotStateSourceConfig source(
    ObservedRobotStateSourceKind kind,
    std::string id,
    std::int64_t freshness_budget_ns = 100) {
  return ObservedRobotStateSourceConfig{
      kind, std::move(id), ObservedRobotStateAuthorityProfile::kMeasured,
      freshness_budget_ns};
}

TEST(ObservedRobotStatePublicationTest,
     SelectsFreshSourceByPrecedenceWithoutMixingJointOrder) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"},
      {source(ObservedRobotStateSourceKind::kActionFeedback, "action"),
       source(ObservedRobotStateSourceKind::kJointState, "joint")});
  EXPECT_EQ(publication.metrics().reader_retry_exhausted_total, 0u);

  const std::vector<std::string> reversed{"j2", "j1"};
  const std::vector<double> action_positions{2.0, 1.0};
  const std::vector<double> action_velocities{0.2, 0.1};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kActionFeedback,
                           reversed, action_positions, action_velocities, 10, 10)
                  .published);
  const std::vector<std::string> canonical{"j1", "j2"};
  const std::vector<double> joint_positions{3.0, 4.0};
  const std::vector<double> joint_velocities{0.3, 0.4};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState,
                           canonical, joint_positions, joint_velocities, 150, 150)
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
  EXPECT_EQ(dto->source_kind,
            ObservedRobotStateSourceKind::kJointState);
  EXPECT_EQ(dto->authority_profile, "measured");
  ASSERT_TRUE(dto->velocities_rad_s.has_value());
  EXPECT_EQ(*dto->velocities_rad_s, joint_velocities);
}

TEST(ObservedRobotStatePublicationTest,
     IncompleteHigherPrioritySourceFallsBackToCompleteSource) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"},
      {source(ObservedRobotStateSourceKind::kActionFeedback, "action"),
       source(ObservedRobotStateSourceKind::kJointState, "joint")});
  const std::vector<std::string> one_joint{"j1"};
  const std::vector<double> one_position{9.0};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kActionFeedback,
                           one_joint, one_position, {}, 10, 10)
                  .published);
  const std::vector<std::string> both{"j1", "j2"};
  const std::vector<double> both_positions{1.0, 2.0};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, both,
                           both_positions, {}, 20, 20)
                  .published);

  auto selected = publication.acquire(20);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.source_id(), "joint");
  EXPECT_TRUE(selected.complete());
}

TEST(ObservedRobotStatePublicationTest,
     PinnedReadersCannotObserveReusedStorage) {
  ObservedRobotStatePublication publication(
      {"j1"},
      {source(ObservedRobotStateSourceKind::kJointState, "joint", 1000)});
  const std::vector<std::string> joints{"j1"};
  const std::vector<double> one{1.0};
  const std::vector<double> two{2.0};
  const std::vector<double> three{3.0};
  const std::vector<double> four{4.0};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints,
                           one, {}, 1, 1)
                  .published);
  auto first = publication.acquire(1);
  ASSERT_TRUE(first);
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints,
                           two, {}, 2, 2)
                  .published);
  auto second = publication.acquire(2);
  ASSERT_TRUE(second);
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints,
                           three, {}, 3, 3)
                  .published);
  auto third = publication.acquire(3);
  ASSERT_TRUE(third);

  const auto exhausted = publication.publish(
      ObservedRobotStateSourceKind::kJointState, joints, four, {}, 4, 4);
  EXPECT_FALSE(exhausted.published);
  EXPECT_STREQ(exhausted.reason, "publication_slots_exhausted");
  EXPECT_EQ(first.positions_rad()[0], 1.0);
  EXPECT_EQ(second.positions_rad()[0], 2.0);
  EXPECT_EQ(third.positions_rad()[0], 3.0);
  EXPECT_EQ(publication.metrics().slot_exhausted_total, 1u);
}

TEST(ObservedRobotStatePublicationTest, RejectsDuplicateCanonicalJointNames) {
  EXPECT_THROW(
      ObservedRobotStatePublication(
          {"j1", "j1"},
          {source(ObservedRobotStateSourceKind::kJointState, "joint")}),
      std::invalid_argument);
}

TEST(ObservedRobotStatePublicationTest,
     RejectsNonExpiringMeasuredSourceAtConstruction) {
  EXPECT_THROW(
      ObservedRobotStatePublication(
          {"j1"},
          {{ObservedRobotStateSourceKind::kJointState, "joint",
            ObservedRobotStateAuthorityProfile::kMeasured, 0,
            "ros2_control_measured_v1"}}),
      std::invalid_argument);
}

TEST(ObservedRobotStatePublicationTest,
     AllowsExplicitlyQualifiedNonExpiringPredictedSource) {
  EXPECT_NO_THROW(ObservedRobotStatePublication(
      {"j1"},
      {{ObservedRobotStateSourceKind::kPredictedState, "predicted",
        ObservedRobotStateAuthorityProfile::kPredictedNative, 0,
        "native_predicted_v1"}}));
}

TEST(ObservedRobotStatePublicationTest,
     RejectsReceiptRegressionWithoutReplacingPublishedSample) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"},
      {source(ObservedRobotStateSourceKind::kJointState, "joint", 100)});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> first_positions{1.0, 2.0};
  const auto first = publication.publish(
      ObservedRobotStateSourceKind::kJointState, joints, first_positions, {},
      10, 100);
  ASSERT_TRUE(first.published);

  const std::vector<double> older_positions{3.0, 4.0};
  const auto older = publication.publish(
      ObservedRobotStateSourceKind::kJointState, joints, older_positions, {},
      11, 99);
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

TEST(ObservedRobotStatePublicationTest,
     ConcurrentReaderNeverObservesMixedPublication) {
  ObservedRobotStatePublication publication(
      {"j1", "j2"},
      {source(ObservedRobotStateSourceKind::kJointState, "joint",
              1'000'000)});
  const std::vector<std::string> joints{"j1", "j2"};
  const std::vector<double> initial{0.0, 0.0};
  ASSERT_TRUE(publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints,
                           initial, {}, 1, 1)
                  .published);

  std::atomic<bool> writer_done{false};
  std::atomic<bool> mixed_sample{false};
  std::thread writer([&]() {
    for (int i = 1; i <= 20'000; ++i) {
      const std::vector<double> positions{static_cast<double>(i),
                                          -static_cast<double>(i)};
      while (!publication
                  .publish(ObservedRobotStateSourceKind::kJointState, joints,
                           positions, {}, i + 1, i + 1)
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

}  // namespace
}  // namespace manyforge::core
