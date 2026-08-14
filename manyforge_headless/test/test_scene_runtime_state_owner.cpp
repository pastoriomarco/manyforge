#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <manyforge_content_identity/content_identity.hpp>

#include "manyforge_headless/scene_runtime_state_owner.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

SceneRuntimeIdentity RuntimeIdentity(const std::uint64_t generation) {
  return {
      "bundle-" + std::to_string(generation),
      "graph-" + std::to_string(generation),
      generation,
      generation,
      generation,
  };
}

PoseSourceObservation Observation(const std::uint64_t sequence,
                                  const std::int64_t observed_at_ns,
                                  const std::int64_t expires_at_ns,
                                  std::string value = "pose",
                                  std::string source_epoch = "source-epoch-1",
                                  const std::uint64_t source_epoch_order = 1U,
                                  std::string object_id = "object-1") {
  return {
      PoseObservationKey{"source-1", std::move(object_id), "continuous"},
      std::move(source_epoch),
      source_epoch_order,
      sequence,
      observed_at_ns,
      expires_at_ns,
      PoseSourceQuality::kGood,
      true,
      CanonicalValue(CanonicalValue::Object{
          {"value", CanonicalValue(std::move(value))},
      }),
  };
}

TEST(SceneRuntimeStateOwner,
     FencesResetSeedAndLatestReplayBeforeOpeningTheCandidate) {
  std::int64_t now = 10;
  std::uint64_t token_serial = 0U;
  SceneRuntimeStateOwner owner(
      [&] { return now; },
      [&] { return "scene-init-" + std::to_string(++token_serial); });
  owner.EstablishOpenRuntime(RuntimeIdentity(4U), 4U, "durable_bootstrap");

  const auto token =
      owner.BeginInitialization(RuntimeIdentity(5U), true, "program_load");
  EXPECT_EQ(token, "scene-init-1");
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kClosed);
  EXPECT_TRUE(owner.Snapshot().deferred);
  EXPECT_THROW(owner.MarkSeeded(token), SceneRuntimeStateConflictError);

  now = 20;
  EXPECT_EQ(owner.ObservePose(Observation(1U, 20, 100)).disposition,
            PoseObservationDisposition::kBuffered);
  EXPECT_EQ(owner.ObservePose(Observation(2U, 21, 101, "newest")).disposition,
            PoseObservationDisposition::kReplacedBuffered);

  owner.PrepareReset(token, "deferred_reset");
  owner.MarkSeeded(token);
  const auto first_batch = owner.BeginReplay(token);
  ASSERT_EQ(first_batch.observations.size(), 1U);
  EXPECT_EQ(first_batch.observations.front().source_sequence, 2U);

  now = 30;
  EXPECT_EQ(
      owner.ObservePose(Observation(3U, 30, 110, "during-replay")).disposition,
      PoseObservationDisposition::kBuffered);
  owner.CompleteReplay(first_batch, 1U);
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kReplaying);

  const auto second_batch = owner.BeginReplay(token);
  ASSERT_EQ(second_batch.observations.size(), 1U);
  EXPECT_EQ(second_batch.observations.front().source_sequence, 3U);
  owner.CompleteReplay(second_batch, 1U);
  const auto opened = owner.Snapshot();
  EXPECT_EQ(opened.state, SceneInitializationState::kOpen);
  EXPECT_FALSE(opened.deferred);
  EXPECT_EQ(opened.initialization_generation, 5U);
  EXPECT_EQ(opened.counters.closed, 1U);
  EXPECT_EQ(opened.counters.buffered, 3U);
  EXPECT_EQ(opened.counters.buffer_replaced, 1U);
  EXPECT_EQ(opened.counters.replayed, 2U);
  EXPECT_EQ(opened.counters.opened, 1U);

  now = 40;
  EXPECT_EQ(owner.ObservePose(Observation(4U, 40, 120)).disposition,
            PoseObservationDisposition::kApplyNow);
  EXPECT_FALSE(owner.Snapshot().pose_sources.front().buffered);
}

TEST(SceneRuntimeStateOwner,
     AdoptsOnlyMonotonicFinalizedOpenRuntimeWithoutInitializationEffects) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  owner.EstablishOpenRuntime(RuntimeIdentity(4U), 4U, "durable_bootstrap");
  static_cast<void>(owner.ObservePose(Observation(1U, 10, 100)));
  ASSERT_EQ(owner.Snapshot().pose_sources.size(), 1U);

  EXPECT_FALSE(owner.AdoptFinalizedOpenRuntime(RuntimeIdentity(4U), 4U,
                                               "durable_apply_finalized"));
  now = 20;
  EXPECT_TRUE(owner.AdoptFinalizedOpenRuntime(RuntimeIdentity(6U), 6U,
                                              "durable_apply_finalized"));
  const auto adopted = owner.Snapshot();
  ASSERT_TRUE(adopted.runtime_identity.has_value());
  EXPECT_EQ(*adopted.runtime_identity, RuntimeIdentity(6U));
  EXPECT_EQ(adopted.initialization_generation, 6U);
  EXPECT_EQ(adopted.state, SceneInitializationState::kOpen);
  EXPECT_FALSE(adopted.initialization_token.has_value());
  EXPECT_TRUE(adopted.pose_sources.empty());
  EXPECT_EQ(adopted.counters.closed, 0U);
  EXPECT_EQ(adopted.counters.opened, 0U);

  EXPECT_THROW(static_cast<void>(owner.AdoptFinalizedOpenRuntime(
                   RuntimeIdentity(5U), 5U, "stale_apply")),
               SceneRuntimeStateConflictError);
  EXPECT_THROW(static_cast<void>(owner.AdoptFinalizedOpenRuntime(
                   RuntimeIdentity(7U), 6U, "conflicting_apply")),
               SceneRuntimeStateConflictError);

  static_cast<void>(
      owner.BeginInitialization(RuntimeIdentity(7U), false, "program_load"));
  EXPECT_THROW(static_cast<void>(owner.AdoptFinalizedOpenRuntime(
                   RuntimeIdentity(8U), 8U, "concurrent_apply")),
               SceneRuntimeStateConflictError);
}

TEST(SceneRuntimeStateOwner,
     SupersessionAndFailedReplayCannotOpenWithStaleCompletion) {
  std::int64_t now = 10;
  std::uint64_t token_serial = 0U;
  SceneRuntimeStateOwner owner(
      [&] { return now; },
      [&] { return "scene-init-" + std::to_string(++token_serial); });
  owner.EstablishOpenRuntime(RuntimeIdentity(1U), 1U, "bootstrap");

  const auto first =
      owner.BeginInitialization(RuntimeIdentity(2U), false, "first");
  static_cast<void>(owner.ObservePose(Observation(1U, 10, 100)));
  owner.PrepareReset(first, "reset");
  owner.MarkSeeded(first);
  const auto stale_batch = owner.BeginReplay(first);

  const auto second =
      owner.BeginInitialization(RuntimeIdentity(3U), false, "superseding");
  EXPECT_THROW(owner.CompleteReplay(stale_batch, 1U),
               SceneRuntimeStateConflictError);
  auto snapshot = owner.Snapshot();
  EXPECT_EQ(snapshot.state, SceneInitializationState::kClosed);
  EXPECT_EQ(snapshot.initialization_generation, 3U);
  EXPECT_EQ(snapshot.counters.stale_completion_rejected, 1U);

  static_cast<void>(owner.ObservePose(Observation(2U, 10, 100, "second")));
  owner.PrepareReset(second, "reset");
  owner.MarkSeeded(second);
  const auto failed_batch = owner.BeginReplay(second);
  auto altered_batch = failed_batch;
  ASSERT_FALSE(altered_batch.observations.empty());
  altered_batch.observations.front().payload =
      CanonicalValue(CanonicalValue::Object{
          {"value", CanonicalValue("altered")},
      });
  EXPECT_THROW(
      owner.CompleteReplay(altered_batch, altered_batch.observations.size()),
      SceneRuntimeStateConflictError);
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kReplaying);
  owner.FailReplay(failed_batch, "backend apply failed");
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kFailed);
  EXPECT_EQ(owner.Snapshot().counters.failed, 1U);

  owner.PrepareReset(second, "retry");
  owner.MarkSeeded(second);
  const auto retry_batch = owner.BeginReplay(second);
  owner.CompleteReplay(retry_batch, retry_batch.observations.size());
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kOpen);

  const auto third =
      owner.BeginInitialization(RuntimeIdentity(4U), true, "retire-me");
  owner.Retire(third, "replacement_selected");
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kRetired);
  EXPECT_THROW(owner.PrepareReset(first, "stale"),
               SceneRuntimeStateConflictError);
}

TEST(SceneRuntimeStateOwner,
     OrdersPoseLifetimesRejectsConflictsAndContainsCapacity) {
  std::int64_t now = 50;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; },
                               1U);
  owner.EstablishOpenRuntime(RuntimeIdentity(1U), 1U, "bootstrap");
  const auto token =
      owner.BeginInitialization(RuntimeIdentity(2U), false, "load");

  const auto expired = Observation(1U, 10, 20);
  EXPECT_EQ(owner.ObservePose(expired).disposition,
            PoseObservationDisposition::kObservedOnly);
  EXPECT_FALSE(owner.Snapshot().pose_sources.front().fresh);
  EXPECT_EQ(owner.ObservePose(expired).disposition,
            PoseObservationDisposition::kDuplicate);
  EXPECT_THROW(
      static_cast<void>(owner.ObservePose(Observation(1U, 10, 20, "conflict"))),
      SceneRuntimeStateConflictError);
  EXPECT_THROW(static_cast<void>(owner.ObservePose(Observation(0U, 9, 20))),
               SceneRuntimeStateConflictError);
  EXPECT_THROW(static_cast<void>(owner.ObservePose(Observation(
                   2U, 30, 100, "epoch-conflict", "other-epoch", 1U))),
               SceneRuntimeStateConflictError);

  EXPECT_EQ(owner
                .ObservePose(
                    Observation(0U, 50, 100, "new-epoch", "source-epoch-2", 2U))
                .disposition,
            PoseObservationDisposition::kBuffered);
  EXPECT_THROW(
      static_cast<void>(owner.ObservePose(Observation(
          1U, 50, 100, "second-key", "source-epoch-1", 1U, "object-2"))),
      SceneRuntimeStateCapacityError);

  owner.PrepareReset(token, "reset");
  owner.MarkSeeded(token);
  const auto batch = owner.BeginReplay(token);
  ASSERT_EQ(batch.observations.size(), 1U);
  EXPECT_EQ(batch.observations.front().source_epoch, "source-epoch-2");
}

TEST(SceneRuntimeStateOwner,
     RetainsBeforePlanningPoseWithoutApplyingOrBarrierReplay) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  owner.EstablishOpenRuntime(RuntimeIdentity(1U), 1U, "bootstrap");

  auto observation = Observation(1U, 10, 100);
  observation.key.policy_id = "before_planning";
  EXPECT_EQ(owner.ObservePose(observation).disposition,
            PoseObservationDisposition::kObservedOnly);
  ASSERT_EQ(owner.Snapshot().pose_sources.size(), 1U);
  EXPECT_FALSE(owner.Snapshot().pose_sources.front().buffered);

  const auto token =
      owner.BeginInitialization(RuntimeIdentity(2U), false, "load");
  observation.source_sequence = 2U;
  observation.observed_at_ns = 20;
  observation.expires_at_ns = 120;
  now = 20;
  EXPECT_EQ(owner.ObservePose(observation).disposition,
            PoseObservationDisposition::kObservedOnly);
  EXPECT_FALSE(owner.Snapshot().pose_sources.front().buffered);
  owner.PrepareReset(token, "reset");
  owner.MarkSeeded(token);
  const auto batch = owner.BeginReplay(token);
  EXPECT_TRUE(batch.observations.empty());
  owner.CompleteReplay(batch, 0U);
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kOpen);
}

TEST(SceneRuntimeStateOwner, EmitsOneClosedCanonicalProjection) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  owner.EstablishOpenRuntime(RuntimeIdentity(1U), 1U, "bootstrap");
  static_cast<void>(
      owner.BeginInitialization(RuntimeIdentity(2U), true, "program_load"));
  static_cast<void>(owner.ObservePose(Observation(1U, 10, 100)));

  const auto bytes = content_identity::canonical_json(
      SceneRuntimeStateValue(owner.Snapshot()));
  EXPECT_NE(bytes.find("\"api_version\":\"manyforge.scene_runtime_state.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"state\":\"closed\""), std::string::npos);
  EXPECT_NE(bytes.find("\"initialization_token\":\"token-1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"buffered\":true"), std::string::npos);
}

} // namespace
} // namespace manyforge::headless
