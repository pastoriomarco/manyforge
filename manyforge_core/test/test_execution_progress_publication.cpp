#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <manyforge_core/execution_progress_publication.hpp>

namespace manyforge::core {
namespace {

TEST(ExecutionProgressPublicationTest,
     PublishesMonotonicGenerationBoundCursorAndInvalidatesExactly) {
  ExecutionProgressPublication publication(ExecutionProgressSourceKind::kBackendCommanded,
                                           ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                           "isaac_command_authority", 0);

  const auto first = publication.publish(7, 100, 10, 11);
  ASSERT_TRUE(first.published);
  auto snapshot = publication.acquire();
  EXPECT_TRUE(snapshot.valid);
  EXPECT_FALSE(snapshot.terminal);
  EXPECT_EQ(snapshot.execution_generation, 7u);
  EXPECT_EQ(snapshot.trajectory_time_ns, 100);
  EXPECT_EQ(snapshot.authority_profile, "isaac_command_authority");

  const auto regressed = publication.publish(7, 99, 12, 13);
  EXPECT_FALSE(regressed.published);
  EXPECT_STREQ(regressed.reason, "trajectory_time_regressed");
  EXPECT_FALSE(publication.invalidate_generation(6));
  EXPECT_TRUE(publication.acquire().valid);
  EXPECT_TRUE(publication.invalidate_generation(7));
  EXPECT_FALSE(publication.acquire().valid);

  const auto retired = publication.publish(7, 101, 14, 15);
  EXPECT_FALSE(retired.published);
  EXPECT_STREQ(retired.reason, "execution_generation_retired");
  const auto stale = publication.publish(6, 0, 16, 17);
  EXPECT_FALSE(stale.published);
  EXPECT_STREQ(stale.reason, "execution_generation_regressed");
  EXPECT_FALSE(publication.acquire().valid);

  const auto next = publication.publish(8, 0, 20, 21);
  ASSERT_TRUE(next.published);
  EXPECT_GT(next.sequence, first.sequence);
  snapshot = publication.acquire();
  EXPECT_EQ(snapshot.execution_generation, 8u);
  EXPECT_EQ(snapshot.trajectory_time_ns, 0);
}

TEST(ExecutionProgressPublicationTest,
     TerminalHandoffFencesLateCallbacksUntilExactRetirement) {
  ExecutionProgressPublication publication(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation,
      "isaac_command_authority", 0);

  ASSERT_TRUE(publication.publish(11, 400, 100, 101).published);
  const auto terminal = publication.publish_terminal(11, 500, 102, 103);
  ASSERT_TRUE(terminal.published);
  auto snapshot = publication.acquire();
  EXPECT_TRUE(snapshot.valid);
  EXPECT_TRUE(snapshot.terminal);
  EXPECT_EQ(snapshot.execution_generation, 11u);
  EXPECT_EQ(snapshot.trajectory_time_ns, 500);

  const auto duplicate = publication.publish_terminal(11, 500, 104, 105);
  EXPECT_TRUE(duplicate.published);
  EXPECT_STREQ(duplicate.reason, "already_terminal");
  EXPECT_EQ(duplicate.sequence, terminal.sequence);

  const auto late_callback = publication.publish(11, 501, 106, 107);
  EXPECT_FALSE(late_callback.published);
  EXPECT_STREQ(late_callback.reason, "execution_generation_terminal");
  EXPECT_FALSE(publication.invalidate_generation(10));
  EXPECT_TRUE(publication.acquire().terminal);
  EXPECT_TRUE(publication.invalidate_generation(11));
  snapshot = publication.acquire();
  EXPECT_FALSE(snapshot.valid);
  EXPECT_FALSE(snapshot.terminal);
  const auto late_terminal =
      publication.publish_terminal(11, 500, 110, 111);
  EXPECT_FALSE(late_terminal.published);
  EXPECT_STREQ(late_terminal.reason, "execution_generation_retired");

  const auto replacement = publication.publish(12, 0, 108, 109);
  ASSERT_TRUE(replacement.published);
  snapshot = publication.acquire();
  EXPECT_TRUE(snapshot.valid);
  EXPECT_FALSE(snapshot.terminal);
  EXPECT_EQ(snapshot.execution_generation, 12u);
}

TEST(ExecutionProgressPublicationTest,
     HazardAndTerminalAuthoritiesAreAtomicallyOrdered) {
  ExecutionProgressPublication terminal_first(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation,
      "terminal_first", 0);
  ASSERT_TRUE(terminal_first.publish(20, 100, 1, 1).published);
  ASSERT_TRUE(
      terminal_first.publish_terminal(20, 200, 2, 2).published);
  EXPECT_EQ(terminal_first.claim_hazard_authority(20),
            ExecutionProgressHazardClaimResult::kTerminal);

  ExecutionProgressPublication hazard_first(
      ExecutionProgressSourceKind::kBackendCommanded,
      ExecutionProgressAuthorityProfile::kCommandedSimulation,
      "hazard_first", 0);
  ASSERT_TRUE(hazard_first.publish(30, 100, 1, 1).published);
  EXPECT_EQ(hazard_first.claim_hazard_authority(30),
            ExecutionProgressHazardClaimResult::kClaimed);
  const auto losing_terminal =
      hazard_first.publish_terminal(30, 200, 2, 2);
  EXPECT_FALSE(losing_terminal.published);
  EXPECT_STREQ(losing_terminal.reason,
               "execution_generation_hazard_claimed");
  EXPECT_FALSE(hazard_first.publish(30, 150, 3, 3).published);
  EXPECT_EQ(hazard_first.claim_hazard_authority(31),
            ExecutionProgressHazardClaimResult::kGenerationMismatch);
}

TEST(ExecutionProgressPublicationTest, HazardObservationDoesNotClaimClearOrCrossGenerations) {
  ExecutionProgressPublication publication(ExecutionProgressSourceKind::kBackendCommanded,
                                           ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                           "handover", 0);
  EXPECT_FALSE(publication.hazard_authority_claimed(7).has_value());
  ASSERT_TRUE(publication.publish(7, 100, 1, 1).published);
  EXPECT_EQ(publication.hazard_authority_claimed(7), false);
  EXPECT_EQ(publication.claim_hazard_authority(7), ExecutionProgressHazardClaimResult::kClaimed);
  // acquire() remains valid; the separate existing authority must nevertheless
  // block a handover before any newer generation can clear it.
  EXPECT_TRUE(publication.acquire().valid);
  EXPECT_EQ(publication.hazard_authority_claimed(7), true);
  EXPECT_EQ(publication.hazard_authority_claimed(7), true);
  EXPECT_FALSE(publication.hazard_authority_claimed(8).has_value());
  EXPECT_FALSE(publication.publish_terminal(7, 200, 2, 2).published);
  ASSERT_TRUE(publication.publish(8, 0, 3, 3).published);
  EXPECT_FALSE(publication.hazard_authority_claimed(7).has_value());
  EXPECT_EQ(publication.hazard_authority_claimed(8), false);
}

TEST(ExecutionProgressPublicationTest,
     MeasuredAuthorityRequiresFreshnessAndRejectsSourceConfusion) {
  EXPECT_THROW(ExecutionProgressPublication(ExecutionProgressSourceKind::kMeasuredProjection,
                                            ExecutionProgressAuthorityProfile::kMeasuredPhysical,
                                            "physical", 0),
               std::invalid_argument);
  EXPECT_THROW(ExecutionProgressPublication(ExecutionProgressSourceKind::kBackendCommanded,
                                            ExecutionProgressAuthorityProfile::kMeasuredPhysical,
                                            "invalid", 100),
               std::invalid_argument);
  EXPECT_THROW(ExecutionProgressPublication(ExecutionProgressSourceKind::kMeasuredProjection,
                                            ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                            "invalid", 100),
               std::invalid_argument);
}

TEST(ExecutionProgressPublicationTest, ReaderOverlappingFeedbackWritesKeepsTheLatestCursor) {
  // Live failure: the lookahead read the cursor while a feedback write was in
  // flight and got valid=false, generation 0, time 0 for an active motion.
  ExecutionProgressPublication publication(ExecutionProgressSourceKind::kBackendCommanded,
                                           ExecutionProgressAuthorityProfile::kCommandedSimulation,
                                           "mock_command_authority", 0);
  constexpr std::uint64_t kGeneration = 8;
  constexpr std::int64_t kWrites = 200000;
  ASSERT_TRUE(publication.publish(kGeneration, 0, 0, 0).published);

  std::atomic<bool> done{false};
  std::thread writer([&]() {
    for (std::int64_t time_ns = 1; time_ns <= kWrites; ++time_ns) {
      (void)publication.publish(kGeneration, time_ns, time_ns, time_ns);
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(1);
      while (std::chrono::steady_clock::now() < until) {
      }
    }
    done.store(true, std::memory_order_release);
  });

  std::int64_t reads = 0;
  std::int64_t invalid = 0;
  std::int64_t previous_time_ns = 0;
  while (!done.load(std::memory_order_acquire)) {
    const auto cursor = publication.acquire();
    ++reads;
    if (!cursor.valid || cursor.execution_generation != kGeneration) {
      ++invalid;
      continue;
    }
    EXPECT_GE(cursor.trajectory_time_ns, previous_time_ns);
    EXPECT_EQ(cursor.source_timestamp_ns, cursor.trajectory_time_ns);
    EXPECT_EQ(cursor.received_timestamp_ns, cursor.trajectory_time_ns);
    previous_time_ns = cursor.trajectory_time_ns;
  }
  writer.join();

  EXPECT_GT(reads, kWrites);
  EXPECT_EQ(invalid, 0);
  const auto last = publication.acquire();
  EXPECT_TRUE(last.valid);
  EXPECT_EQ(last.trajectory_time_ns, kWrites);
}

}  // namespace
}  // namespace manyforge::core
