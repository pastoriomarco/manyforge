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
     DeploymentPolicyControlsAcceptanceWithoutChangingSourceKey) {
  SceneRuntimeStateOwner owner([] { return 10; },
                               [] { return "policy-token"; });
  owner.EstablishOpenRuntime(RuntimeIdentity(1U), 1U, "bootstrap");
  auto observed = Observation(1U, 10, 100);
  observed.key.policy_id = "before_planning";
  EXPECT_EQ(owner.ObservePose(observed, "continuous").disposition,
            PoseObservationDisposition::kApplyNow);
  ASSERT_EQ(owner.Snapshot().pose_sources.size(), 1U);
  EXPECT_EQ(owner.Snapshot().pose_sources.front().observation.key.policy_id,
            "before_planning");
  observed.source_sequence = 2U;
  EXPECT_EQ(owner.ObservePose(observed, "observe_only").disposition,
            PoseObservationDisposition::kObservedOnly);
  const auto token =
      owner.BeginInitialization(RuntimeIdentity(2U), false, "next");
  observed.source_sequence = 3U;
  EXPECT_EQ(owner.ObservePose(observed, "continuous").disposition,
            PoseObservationDisposition::kBuffered);
  owner.PrepareReset(token, "reset");
  owner.MarkSeeded(token);
  const auto batch = owner.BeginReplay(token);
  ASSERT_EQ(batch.observations.size(), 1U);
  EXPECT_EQ(batch.observations.front(), observed);
  owner.FailReplay(batch, "apply failed");
  owner.PrepareReset(token, "retry");
  owner.MarkSeeded(token);
  const auto retry = owner.BeginReplay(token);
  EXPECT_EQ(retry.observations, batch.observations);
  owner.CompleteReplay(retry, 1U);
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kOpen);
}

TEST(SceneRuntimeStateOwner, ExactRetirementErasesOnlyThatSource) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token"; });
  const auto identity = RuntimeIdentity(4U);
  owner.EstablishOpenRuntime(identity, 4U, "bootstrap");
  static_cast<void>(
      owner.ObservePose(Observation(1U, 10, 100, "old", "old-base.1")));
  auto other = Observation(1U, 10, 100, "unrelated");
  other.key.source_id = "other-source";
  static_cast<void>(owner.ObservePose(other));
  owner.RetirePoseSource(identity, "source-1");
  ASSERT_EQ(owner.Snapshot().pose_sources.size(), 1U);
  EXPECT_EQ(owner.Snapshot().pose_sources.front().observation, other);
  EXPECT_TRUE(
      owner.ObservePose(Observation(1U, 10, 100, "new", "new-base.1")).fresh);
}

TEST(SceneRuntimeStateOwner, CarriedObjectLossEventsArePublishedAndBounded) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token"; });
  owner.EstablishOpenRuntime(RuntimeIdentity(4U), 4U, "bootstrap");
  EXPECT_EQ(SceneRuntimeStateValue(owner.Snapshot())
                .find_object_member("carried_object_losses"),
            nullptr);
  owner.RecordCarriedObjectLoss(
      {"event-0", "box", "box moved", "attachment.1", "robot.1", "gripper",
       core::GraspLostResponseDTO{false, true, "part_lost", false}});
  EXPECT_NE(content_identity::canonical_json(
                SceneRuntimeStateValue(owner.Snapshot()))
                .find(R"("carried_object_losses":[{"carried_object_attachment_id":"attachment.1","code":"scene.carried_object_lost","command_target_id":"gripper","controllable_component_id":"robot.1","event_id":"event-0","full_text":"box moved","on_grasp_lost":{"detach":false,"raise_alarm":false,"set_key":"part_lost","stop_motion":true},"scene_resource_id":"box"}])"),
            std::string::npos);
  // An alarm without the Stop is refused.
  EXPECT_THROW(owner.RecordCarriedObjectLoss(
                   {"event-x", "box", "box moved", "attachment.1", "robot.1",
                    "gripper",
                    core::GraspLostResponseDTO{true, false, std::nullopt, true}}),
               SceneRuntimeStateError);
  for (std::size_t index = 1U; index <= kMaximumCarriedObjectLossEvents; ++index) {
    owner.RecordCarriedObjectLoss({"event-" + std::to_string(index), "box",
                                   "box moved", "attachment.1", "robot.1",
                                   "gripper", core::GraspLostResponseDTO{}});
  }
  const auto losses = owner.Snapshot().carried_object_losses;
  ASSERT_EQ(losses.size(), kMaximumCarriedObjectLossEvents);
  EXPECT_EQ(losses.front().event_id, "event-1");
  EXPECT_EQ(losses.back().event_id,
            "event-" + std::to_string(kMaximumCarriedObjectLossEvents));
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
     RetiresOnlyOldSourceBeforeSameSourceReopensWithANewEpoch) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  const auto identity = RuntimeIdentity(4U);
  owner.EstablishOpenRuntime(identity, 4U, "durable_bootstrap");
  static_cast<void>(owner.ObservePose(Observation(1U, 10, 100)));
  auto peer = Observation(1U, 10, 100, "peer");
  peer.key.source_id = "peer-source";
  peer.source_epoch = "peer-epoch";
  static_cast<void>(owner.ObservePose(peer));
  const auto before = owner.Snapshot();
  owner.RetirePoseSource(identity, "source-1");
  const auto cleared = owner.Snapshot();
  ASSERT_EQ(cleared.pose_sources.size(), 1U);
  EXPECT_EQ(cleared.pose_sources.front().observation, peer);
  EXPECT_EQ(cleared.runtime_identity, before.runtime_identity);
  EXPECT_EQ(cleared.initialization_generation,
            before.initialization_generation);
  EXPECT_EQ(cleared.state, before.state);
  EXPECT_EQ(cleared.updated_at_ns, before.updated_at_ns);
  EXPECT_EQ(content_identity::canonical_json(SceneRuntimeStateValue(cleared))
                .find("source-epoch-1"),
            std::string::npos);
  EXPECT_EQ(cleared.counters.retired, before.counters.retired);
  // Erasure removes the old source's sequence/epoch ordering as well. The
  // replacement starts from its own exact new epoch, with no old pose reused.
  auto replacement = Observation(1U, 10, 100, "new", "new-base.1");
  EXPECT_EQ(owner.ObservePose(replacement).disposition,
            PoseObservationDisposition::kApplyNow);
  const auto after = owner.Snapshot();
  ASSERT_EQ(after.pose_sources.size(), 2U);
  EXPECT_EQ(after.pose_sources.back().observation, replacement);
}

TEST(SceneRuntimeStateOwner,
     SourceRetirementRefusesInvalidStaleOrReplayingStateWithoutChanges) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  owner.EstablishOpenRuntime(RuntimeIdentity(4U), 4U, "durable_bootstrap");
  static_cast<void>(owner.ObservePose(Observation(1U, 10, 100)));
  const auto snapshot_bytes = [&] {
    return content_identity::canonical_json(
        SceneRuntimeStateValue(owner.Snapshot()));
  };
  const auto before = snapshot_bytes();
  EXPECT_THROW(owner.RetirePoseSource(RuntimeIdentity(5U), "source-1"),
               SceneRuntimeStateConflictError);
  EXPECT_THROW(owner.RetirePoseSource(RuntimeIdentity(4U), ""),
               SceneRuntimeStateError);
  EXPECT_THROW(
      owner.RetirePoseSource(RuntimeIdentity(4U), std::string(257U, 'x')),
      SceneRuntimeStateError);
  EXPECT_EQ(snapshot_bytes(), before);
  const auto token =
      owner.BeginInitialization(RuntimeIdentity(5U), false, "load");
  const auto closed = snapshot_bytes();
  EXPECT_THROW(owner.RetirePoseSource(RuntimeIdentity(5U), "source-1"),
               SceneRuntimeStateConflictError);
  EXPECT_EQ(snapshot_bytes(), closed);
  static_cast<void>(owner.ObservePose(Observation(2U, 10, 100)));
  owner.PrepareReset(token, "reset");
  owner.MarkSeeded(token);
  const auto batch = owner.BeginReplay(token);
  const auto replaying = snapshot_bytes();
  EXPECT_THROW(owner.RetirePoseSource(RuntimeIdentity(5U), "source-1"),
               SceneRuntimeStateConflictError);
  EXPECT_EQ(snapshot_bytes(), replaying);
  // The refused retirement did not change the existing exact replay protocol.
  owner.CompleteReplay(batch, batch.observations.size());
  EXPECT_EQ(owner.Snapshot().state, SceneInitializationState::kOpen);
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
  altered_batch = failed_batch;
  altered_batch.observations.front().payload_quality = PoseSourceQuality::kUnknown;
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
  EXPECT_NE(bytes.find("\"api_version\":\"manyforge.scene_runtime_state.v2\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"state\":\"closed\""), std::string::npos);
  EXPECT_NE(bytes.find("\"initialization_token\":\"token-1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"buffered\":true"), std::string::npos);
  EXPECT_NE(bytes.find("\"payload_quality\":\"good\""), std::string::npos);
  EXPECT_EQ(bytes.find("\"quality\""), std::string::npos);
}

} // namespace
} // namespace manyforge::headless
