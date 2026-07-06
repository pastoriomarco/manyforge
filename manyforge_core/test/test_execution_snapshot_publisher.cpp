#include <manyforge_core/execution_snapshot_publisher.hpp>

#include <gtest/gtest.h>

#include <utility>

namespace manyforge::core {
namespace {

TEST(ExecutionSnapshotPublisherTest, PublishAssignsMonotonicSequenceAndTimestamp) {
  ExecutionSnapshotPublisher publisher;

  ExecutionSnapshot first;
  first.execution_id = 10;
  publisher.publish(std::move(first), 1234);

  auto latest = publisher.latest();
  EXPECT_EQ(latest.execution_id, 10u);
  EXPECT_EQ(latest.timestamp_ns, 1234);
  EXPECT_EQ(latest.sequence_number, 1u);

  ExecutionSnapshot second;
  second.execution_id = 11;
  second.timestamp_ns = 5678;
  publisher.publish(std::move(second), 9999);

  latest = publisher.latest();
  EXPECT_EQ(latest.execution_id, 11u);
  EXPECT_EQ(latest.timestamp_ns, 5678);
  EXPECT_EQ(latest.sequence_number, 2u);
}

TEST(ExecutionSnapshotPublisherTest, LatestReturnsCopy) {
  ExecutionSnapshotPublisher publisher;
  ExecutionSnapshot snapshot;
  snapshot.joint_positions_rad = {1.0, 2.0};
  publisher.publish(std::move(snapshot), 1);

  auto copy = publisher.latest();
  copy.joint_positions_rad[0] = 42.0;

  auto latest = publisher.latest();
  ASSERT_EQ(latest.joint_positions_rad.size(), 2u);
  EXPECT_DOUBLE_EQ(latest.joint_positions_rad[0], 1.0);
  EXPECT_DOUBLE_EQ(latest.joint_positions_rad[1], 2.0);
}

}  // namespace
}  // namespace manyforge::core
