// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "feedback_callback.hpp"

namespace
{
namespace transport = manyforge_isaac_hardware::detail;
using transport::FeedbackValidity;

class FeedbackTest : public ::testing::Test
{
protected:
  std::array<std::string, 6> names{"a", "b", "c", "d", "e", "f"};
  transport::FeedbackLimits limits{100, 50};
  transport::FeedbackHandoff handoff{names, true, limits};
  sensor_msgs::msg::JointState message;

  void SetUp() override
  {
    message.name.assign(names.begin(), names.end());
    message.position = {1, 2, 3, 4, 5, 6};
    message.velocity = {0, 0, 0, 0, 0, 0};
    message.header.stamp.nanosec = 100;
  }

  void receive(std::int64_t source_now = 110, std::int64_t received = 1000)
  {
    transport::receive_joint_state(handoff, message, source_now, received);
  }
};

TEST_F(FeedbackTest, StartupHasNoObservation)
{
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kUnavailable);
}

TEST_F(FeedbackTest, MapsACompleteReorderedJointArrayWithoutChangingSourceTime)
{
  std::swap(message.name[0], message.name[5]);
  std::swap(message.position[0], message.position[5]);
  receive();
  const auto sample = handoff.read(1000);
  EXPECT_EQ(sample.validity, FeedbackValidity::kValid);
  EXPECT_EQ(sample.source_ns, 100);
  EXPECT_EQ(sample.received_ns, 1000);
  EXPECT_EQ(sample.source_age_ns, 10);
  EXPECT_EQ(sample.position, (transport::Positions{1, 2, 3, 4, 5, 6}));
}

TEST_F(FeedbackTest, IncompletePositionInvalidatesTheWholeSample)
{
  receive();
  ASSERT_EQ(handoff.read(1000).validity, FeedbackValidity::kValid);
  message.position.pop_back();
  message.header.stamp.nanosec = 101;
  receive(111, 1001);
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kIncomplete);
}

TEST_F(FeedbackTest, MissingVelocityIsNeverRetainedFromAnOlderSample)
{
  receive();
  message.velocity.clear();
  message.header.stamp.nanosec = 101;
  receive(111, 1001);
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kIncomplete);
}

TEST_F(FeedbackTest, ShortNamesAndShortVelocityAreRejected)
{
  message.name.pop_back();
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kIncomplete);
  message.name.push_back("f");
  message.velocity.pop_back();
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kIncomplete);
}

TEST_F(FeedbackTest, UnknownAndDuplicateJointsAreRejected)
{
  message.name[5] = "unknown";
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kUnknownJoint);
  message.name[5] = "a";
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kDuplicateJoint);
}

TEST_F(FeedbackTest, NonFinitePositionsAndVelocitiesAreRejected)
{
  message.position[2] = std::numeric_limits<double>::quiet_NaN();
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kNonFinite);
  message.position[2] = 3;
  message.velocity[0] = std::numeric_limits<double>::infinity();
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kNonFinite);
}

TEST_F(FeedbackTest, PositionOnlyScopeStillChecksAnyProvidedVelocity)
{
  transport::FeedbackHandoff position_only{names, false, limits};
  message.velocity.clear();
  transport::receive_joint_state(position_only, message, 110, 1000);
  EXPECT_EQ(position_only.read(1000).validity, FeedbackValidity::kValid);
  message.velocity = {0};
  transport::receive_joint_state(position_only, message, 110, 1000);
  EXPECT_EQ(position_only.read(1000).validity, FeedbackValidity::kIncomplete);
}

TEST_F(FeedbackTest, MissingOrMalformedSourceStampCannotBecomeFresh)
{
  message.header.stamp.nanosec = 0;
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kMissingSourceTime);
  message.header.stamp.nanosec = 1000000000U;
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kMissingSourceTime);
  message.header.stamp.nanosec = 100;
  message.header.stamp.sec = -1;
  receive();
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kMissingSourceTime);
}

TEST_F(FeedbackTest, RepeatedOrOlderSourceStampCannotRenewValidity)
{
  receive();
  ASSERT_EQ(handoff.read(1000).validity, FeedbackValidity::kValid);
  receive(120, 1001);
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kSourceTimeReordered);
  message.header.stamp.nanosec = 99;
  receive(121, 1002);
  EXPECT_EQ(handoff.read(1002).validity, FeedbackValidity::kSourceTimeReordered);
}

TEST_F(FeedbackTest, SourceAgeAndLocalReceiptAgeExpireIndependently)
{
  receive(190, 1000);  // Source age is checked at receipt, in its own domain.
  EXPECT_EQ(handoff.read(1050).validity, FeedbackValidity::kValid);
  EXPECT_EQ(handoff.read(1051).validity, FeedbackValidity::kStale);
  message.header.stamp.nanosec = 200;
  receive(200, 1100);
  EXPECT_EQ(handoff.read(1150).validity, FeedbackValidity::kValid);
  EXPECT_EQ(handoff.read(1151).validity, FeedbackValidity::kStale);
}

TEST_F(FeedbackTest, StaleSourceIsRejectedAtReceipt)
{
  receive(201, 1000);
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kStale);
}

TEST_F(FeedbackTest, SourceClockReversalRequiresReconfiguration)
{
  receive();
  message.header.stamp.nanosec = 101;
  receive(109, 1001);
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kClockReversed);
  receive(111, 1002);
  EXPECT_EQ(handoff.read(1002).validity, FeedbackValidity::kClockReversed);
}

TEST_F(FeedbackTest, FutureSourceTimeIsInvalidAndNewerReceiptWaitsForTheNextRead)
{
  receive(99, 1000);
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kClockReversed);
  receive(110, 1001);
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kUnavailable);
}

TEST_F(FeedbackTest, FullHandoffInvalidatesUntilANewSampleAfterBoundedDrain)
{
  for (std::uint32_t i = 0; i < transport::kFeedbackCapacity; ++i) {
    message.header.stamp.nanosec = 100 + i;
    receive(110 + i, 1000 + i);
  }
  message.position.pop_back();  // An invalid dropped sample cannot leave valid old data.
  receive(120, 1010);
  EXPECT_EQ(handoff.read(1010).validity, FeedbackValidity::kOverflow);
  EXPECT_EQ(handoff.read(1011).validity, FeedbackValidity::kOverflow);
  message.position.push_back(6);
  message.header.stamp.nanosec = 121;
  receive(121, 1012);
  EXPECT_EQ(handoff.read(1012).validity, FeedbackValidity::kValid);
}

TEST(FeedbackFreshness, FullWidthClocksAreNeverAddedAcrossDomains)
{
  transport::FeedbackSnapshot sample;
  sample.validity = FeedbackValidity::kValid;
  sample.received_ns = 1;
  sample.source_age_ns = 100;
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(
    transport::freshness(sample, {maximum, maximum}, maximum), FeedbackValidity::kValid);
}

TEST_F(FeedbackTest, ConcurrentReceiptAfterReadClockCaptureDoesNotLatchAClockFault)
{
  // RT captured now=999, then NRT received=1000 before RT drains the queue.
  const std::int64_t captured_read_ns = 999;
  receive();
  EXPECT_EQ(handoff.read(captured_read_ns).validity, FeedbackValidity::kUnavailable);
  EXPECT_EQ(handoff.read(1000).validity, FeedbackValidity::kValid);
  message.header.stamp.nanosec = 101;
  receive(111, 1001);
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kValid);
}

TEST_F(FeedbackTest, ActualReadClockReversalBeforeReceiptCannotRestoreEligibility)
{
  receive();
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kValid);
  EXPECT_EQ(handoff.read(999).validity, FeedbackValidity::kClockReversed);
  EXPECT_EQ(handoff.read(1001).validity, FeedbackValidity::kClockReversed);
}

TEST_F(FeedbackTest, ReadClockReversalAfterReceiptCannotRestoreEligibility)
{
  receive();
  EXPECT_EQ(handoff.read(1040).validity, FeedbackValidity::kValid);
  EXPECT_EQ(handoff.read(1030).validity, FeedbackValidity::kClockReversed);
  EXPECT_EQ(handoff.read(1040).validity, FeedbackValidity::kClockReversed);
}

TEST_F(FeedbackTest, SimulationZeroRequiresAnObservedClockAndCannotRenew)
{
  transport::FeedbackHandoff simulation{names, true, limits,
    transport::FeedbackSourceClock::kRosSimulation};
  transport::SimulationClockInput clock;
  message.header.stamp.nanosec = 0;
  transport::receive_joint_state(simulation, message, clock.now_ns, 1000, clock.initialized);
  EXPECT_EQ(simulation.read(1000).validity, FeedbackValidity::kSourceClockUnavailable);
  rosgraph_msgs::msg::Clock clock_message;
  clock.receive(simulation, clock_message, 1001);
  transport::receive_joint_state(simulation, message, clock.now_ns, 1001, clock.initialized);
  EXPECT_EQ(simulation.read(1001).validity, FeedbackValidity::kValid);
  transport::receive_joint_state(simulation, message, clock.now_ns, 1002, clock.initialized);
  EXPECT_EQ(simulation.read(1002).validity, FeedbackValidity::kSourceTimeReordered);
}

TEST_F(FeedbackTest, SimulationPauseExpiresByLocalTimeRegardlessOfSourceRate)
{
  transport::FeedbackHandoff simulation{names, true, limits,
    transport::FeedbackSourceClock::kRosSimulation};
  transport::receive_joint_state(simulation, message, 190, 1000);
  EXPECT_EQ(simulation.read(1050).validity, FeedbackValidity::kValid);
  EXPECT_EQ(simulation.read(1051).validity, FeedbackValidity::kStale);
}

TEST_F(FeedbackTest, SimulationResetWithoutAnotherJointSampleInvalidatesAndLatches)
{
  transport::FeedbackHandoff simulation{names, true, limits,
    transport::FeedbackSourceClock::kRosSimulation};
  transport::SimulationClockInput clock;
  rosgraph_msgs::msg::Clock clock_message;
  clock_message.clock.nanosec = 110;
  clock.receive(simulation, clock_message, 1000);
  transport::receive_joint_state(simulation, message, clock.now_ns, 1000, clock.initialized);
  EXPECT_EQ(simulation.read(1000).validity, FeedbackValidity::kValid);
  clock_message.clock.nanosec = 0;
  clock.receive(simulation, clock_message, 1001);
  EXPECT_EQ(simulation.read(1001).validity, FeedbackValidity::kClockReversed);
  clock_message.clock.nanosec = 111;
  clock.receive(simulation, clock_message, 1002);
  message.header.stamp.nanosec = 101;
  transport::receive_joint_state(simulation, message, clock.now_ns, 1002, clock.initialized);
  EXPECT_EQ(simulation.read(1002).validity, FeedbackValidity::kClockReversed);
}

TEST_F(FeedbackTest, SimulationSampleAheadOfObservedClockIsRefusedWithoutConversion)
{
  transport::FeedbackHandoff simulation{names, true, limits,
    transport::FeedbackSourceClock::kRosSimulation};
  transport::receive_joint_state(simulation, message, 99, 1000);
  EXPECT_EQ(simulation.read(1000).validity, FeedbackValidity::kClockReversed);
  transport::receive_joint_state(simulation, message, 110, 1001);
  EXPECT_EQ(simulation.read(1001).validity, FeedbackValidity::kValid);
}

TEST(SpscQueue, FullDropsNewWithoutOverwritingAndConsumerCoalescesLatest)
{
  transport::SpscQueue<int, 2> queue;
  EXPECT_TRUE(queue.push(1));
  EXPECT_TRUE(queue.push(2));
  EXPECT_FALSE(queue.push(3));
  int value = 0;
  EXPECT_EQ(queue.pop_latest(value), 2U);
  EXPECT_EQ(value, 2);
  EXPECT_EQ(queue.pop_latest(value), 0U);
  EXPECT_TRUE(queue.push(4));
  EXPECT_TRUE(queue.pop(value));
  EXPECT_EQ(value, 4);
}

TEST(SpscQueue, ConcurrentOwnershipPreservesOrderAcrossWraparound)
{
  transport::SpscQueue<std::uint64_t, 8> queue;
  constexpr std::uint64_t count = 50000;
  std::thread producer([&]() {
      for (std::uint64_t i = 1; i <= count; ++i) {
        while (!queue.push(i)) {}
      }
    });
  bool ordered = true;
  for (std::uint64_t expected = 1; expected <= count; ++expected) {
    std::uint64_t actual = 0;
    while (!queue.pop(actual)) {}
    ordered = ordered && actual == expected;
  }
  producer.join();
  EXPECT_TRUE(ordered);
}

TEST(TransportFacts, SuppressionAndLocalFenceNeverProveDownstreamDeliveryOrDrain)
{
  const transport::TransportFacts facts;
  EXPECT_EQ(facts.disposition, transport::WriteDisposition::kSuppressedInactive);
  EXPECT_TRUE(facts.future_publication_fenced);
  EXPECT_FALSE(facts.command_submitted);
  EXPECT_FALSE(facts.command_realized);
  EXPECT_FALSE(facts.downstream_delivery_known);
  EXPECT_FALSE(facts.downstream_drained);
}

}  // namespace
