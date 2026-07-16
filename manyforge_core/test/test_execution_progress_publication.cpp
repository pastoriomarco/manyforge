#include <gtest/gtest.h>

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

}  // namespace
}  // namespace manyforge::core
