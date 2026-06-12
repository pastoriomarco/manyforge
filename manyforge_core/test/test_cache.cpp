#include <gtest/gtest.h>
#include <manyforge_core/cache/trajectory_cache.hpp>

namespace manyforge::core {
namespace {

CollisionWorldRevision make_revision(uint64_t id = 1) {
  CollisionWorldRevision revision;
  revision.revision_id = id;
  revision.content_hash = "test_hash_" + std::to_string(id);
  return revision;
}

TrajectoryDTO make_trajectory(const std::string& label) {
  TrajectoryDTO t;
  t.joint_names = {label};
  TrajectoryWaypointDTO wp;
  wp.time_from_start_s = 0.0;
  wp.positions_rad = {1.0, 2.0};
  t.waypoints.push_back(wp);
  return t;
}

class TrajectoryCacheTest : public ::testing::Test {
 protected:
  TrajectoryCache cache{CacheConfig{3}};  // max 3 entries
};

// --- Store and lookup ---
TEST_F(TrajectoryCacheTest, StoreAndLookup) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  auto result = cache.peek_unvalidated("move_1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "t1");
  EXPECT_EQ(result->waypoints.size(), 1u);
}

TEST_F(TrajectoryCacheTest, LookupMiss_ReturnsNullopt) {
  auto result = cache.peek_unvalidated("nonexistent");
  EXPECT_FALSE(result.has_value());
}

// --- Overwrite ---
TEST_F(TrajectoryCacheTest, StoreOverwritesExisting) {
  cache.store("move_1", make_trajectory("v1"), make_revision());
  cache.store("move_1", make_trajectory("v2"), make_revision());
  auto result = cache.peek_unvalidated("move_1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "v2");
  EXPECT_EQ(cache.size(), 1u);
}

// --- Invalidate ---
TEST_F(TrajectoryCacheTest, InvalidateSpecificMoveId) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  cache.store("move_2", make_trajectory("t2"), make_revision());
  cache.invalidate("move_1");
  EXPECT_FALSE(cache.peek_unvalidated("move_1").has_value());
  EXPECT_TRUE(cache.peek_unvalidated("move_2").has_value());
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, InvalidateNonExistent_NoOp) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  cache.invalidate("nonexistent");
  EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TrajectoryCacheTest, InvalidateAll) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  cache.store("move_2", make_trajectory("t2"), make_revision());
  cache.invalidate_all();
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_FALSE(cache.peek_unvalidated("move_1").has_value());
  EXPECT_FALSE(cache.peek_unvalidated("move_2").has_value());
}

// --- Multiple independent entries ---
TEST_F(TrajectoryCacheTest, MultipleMovesIndependent) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  cache.store("move_2", make_trajectory("t2"), make_revision());
  cache.store("move_3", make_trajectory("t3"), make_revision());

  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.peek_unvalidated("move_1")->joint_names[0], "t1");
  EXPECT_EQ(cache.peek_unvalidated("move_2")->joint_names[0], "t2");
  EXPECT_EQ(cache.peek_unvalidated("move_3")->joint_names[0], "t3");
}

// --- Size and capacity ---
TEST_F(TrajectoryCacheTest, SizeCorrect) {
  EXPECT_EQ(cache.size(), 0u);
  cache.store("a", make_trajectory("a"), make_revision());
  EXPECT_EQ(cache.size(), 1u);
  cache.store("b", make_trajectory("b"), make_revision());
  EXPECT_EQ(cache.size(), 2u);
}

TEST_F(TrajectoryCacheTest, CapacityFromConfig) {
  EXPECT_EQ(cache.capacity(), 3u);
  TrajectoryCache big_cache{CacheConfig{500}};
  EXPECT_EQ(big_cache.capacity(), 500u);
}

TEST_F(TrajectoryCacheTest, DefaultCapacity) {
  TrajectoryCache default_cache;
  EXPECT_EQ(default_cache.capacity(), 1000u);
}

// --- Safety bound: FIFO eviction ---
TEST_F(TrajectoryCacheTest, EvictsOldestWhenFull) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  cache.store("move_2", make_trajectory("t2"), make_revision());
  cache.store("move_3", make_trajectory("t3"), make_revision());
  EXPECT_EQ(cache.size(), 3u);

  // 4th entry evicts oldest (move_1).
  cache.store("move_4", make_trajectory("t4"), make_revision());
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_FALSE(cache.peek_unvalidated("move_1").has_value());
  EXPECT_TRUE(cache.peek_unvalidated("move_2").has_value());
  EXPECT_TRUE(cache.peek_unvalidated("move_3").has_value());
  EXPECT_TRUE(cache.peek_unvalidated("move_4").has_value());
}

TEST_F(TrajectoryCacheTest, OverwriteDoesNotEvict) {
  cache.store("move_1", make_trajectory("t1"), make_revision());
  cache.store("move_2", make_trajectory("t2"), make_revision());
  cache.store("move_3", make_trajectory("t3"), make_revision());

  // Overwrite move_1 — should not evict anything.
  cache.store("move_1", make_trajectory("t1_updated"), make_revision());
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.peek_unvalidated("move_1")->joint_names[0], "t1_updated");
  EXPECT_TRUE(cache.peek_unvalidated("move_2").has_value());
  EXPECT_TRUE(cache.peek_unvalidated("move_3").has_value());
}

// --- Empty trajectory ---
// --- Chunk 2: revision-aware cache ---
TEST_F(TrajectoryCacheTest, RevisionAware_StoreAndLookupSucceedsOnExactMatch) {
  CollisionWorldRevision rev{7, "abc"};
  cache.store("move_1", make_trajectory("t1"), rev);
  auto result = cache.lookup_with_revision("move_1", rev);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->joint_names[0], "t1");
}

TEST_F(TrajectoryCacheTest, RevisionAware_MismatchRevisionMissesAndEvicts) {
  CollisionWorldRevision rev_old{7, "abc"};
  CollisionWorldRevision rev_new{8, "def"};
  cache.store("move_1", make_trajectory("t1"), rev_old);

  auto miss = cache.lookup_with_revision("move_1", rev_new);
  EXPECT_FALSE(miss.has_value());

  // Subsequent lookup-by-key sees the entry was evicted.
  EXPECT_FALSE(cache.peek_unvalidated("move_1").has_value());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, RevisionAware_DifferentHashSameIdIsMiss) {
  CollisionWorldRevision rev_a{7, "abc"};
  CollisionWorldRevision rev_b{7, "xyz"};
  cache.store("move_1", make_trajectory("t1"), rev_a);

  EXPECT_FALSE(cache.lookup_with_revision("move_1", rev_b).has_value());
  // Stale entry was evicted.
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, RevisionAware_ExpectedUnsetMissesEvenWhenStoredUnset) {
  // Revision-aware lookup fails closed when the caller cannot identify the
  // authoritative collision-world revision.
  cache.store("move_1", make_trajectory("t1"), CollisionWorldRevision{});
  auto miss =
      cache.lookup_with_revision("move_1", CollisionWorldRevision{});
  EXPECT_FALSE(miss.has_value());
  EXPECT_TRUE(cache.peek_unvalidated("move_1").has_value());
}

TEST_F(TrajectoryCacheTest, RevisionAware_ExpectedUnsetButStoredTaggedMisses) {
  CollisionWorldRevision rev{7, "abc"};
  cache.store("move_1", make_trajectory("t1"), rev);

  // Caller without an authoritative revision MUST NOT see the tagged hit.
  EXPECT_FALSE(
      cache.lookup_with_revision("move_1", CollisionWorldRevision{})
          .has_value());

  // Entry NOT evicted: it remains valid for a future caller that does
  // supply the correct revision.
  EXPECT_EQ(cache.size(), 1u);
  auto subsequent = cache.lookup_with_revision("move_1", rev);
  ASSERT_TRUE(subsequent.has_value());
}

TEST_F(TrajectoryCacheTest, RevisionAware_ExpectedTaggedButStoredUnsetEvicts) {
  // Adapter promoted to publishing revisions after this entry was stored
  // with an unset tag. The pre-promotion entry can no longer be trusted.
  cache.store("move_1", make_trajectory("t1"), CollisionWorldRevision{});
  CollisionWorldRevision rev{7, "abc"};
  EXPECT_FALSE(cache.lookup_with_revision("move_1", rev).has_value());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, RevisionAware_RevisionOfReturnsTaggedValue) {
  CollisionWorldRevision rev{7, "abc"};
  cache.store("move_1", make_trajectory("t1"), rev);
  auto stored = cache.revision_of("move_1");
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->revision_id, 7u);
  EXPECT_EQ(stored->content_hash, "abc");
}

TEST_F(TrajectoryCacheTest, RevisionAware_RevisionOfReturnsNulloptForUntagged) {
  cache.store("move_1", make_trajectory("t1"), CollisionWorldRevision{});
  EXPECT_FALSE(cache.revision_of("move_1").has_value());
}

TEST_F(TrajectoryCacheTest, RevisionAware_OverwriteUpdatesRevision) {
  CollisionWorldRevision rev_a{7, "abc"};
  CollisionWorldRevision rev_b{8, "def"};
  cache.store("move_1", make_trajectory("t1"), rev_a);
  cache.store("move_1", make_trajectory("t1_updated"), rev_b);

  // Old revision no longer matches.
  EXPECT_FALSE(cache.lookup_with_revision("move_1", rev_a).has_value());
  // After the failed lookup the entry was evicted.
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(TrajectoryCacheTest, StoreEmptyTrajectory) {
  TrajectoryDTO empty;
  EXPECT_TRUE(empty.empty());
  cache.store("move_empty", empty, make_revision());
  auto result = cache.peek_unvalidated("move_empty");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

}  // namespace
}  // namespace manyforge::core
