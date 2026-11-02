#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

#include "manyforge_headless/operator_alarm_catalog.hpp"
#include "manyforge_headless/operator_alarm_owner.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::string_view kCellId = "cell.alpha";
constexpr std::string_view kSceneDefinition =
    "manyforge.scene.pose_apply_fault";
constexpr std::string_view kSceneSource = "scene.source";
constexpr std::string_view kSceneAdapter =
    "manyforge.alarm_adapter.runtime_scene_condition";
constexpr std::string_view kSceneEpoch = "scene.epoch.1";

CanonicalValue EmptyObject() {
  return CanonicalValue(CanonicalValue::Object{});
}

CanonicalValue Object(std::string key, std::string value) {
  return CanonicalValue(CanonicalValue::Object{
      {std::move(key), CanonicalValue(std::move(value))},
  });
}

/// Condition ids of the Kernel-bound projection (condition interlocks only).
std::vector<std::string>
GateMembers(const AlarmMotionAttentionProjection &projection) {
  std::vector<std::string> result;
  for (const auto &member : projection.members) {
    result.push_back(member.acknowledgement.condition_id);
  }
  return result;
}

class AlarmOwnerTest : public ::testing::Test {
protected:
  std::unique_ptr<AlarmLifecycleOwner>
  MakeOwner(std::set<std::string> required_domains = {"scene"},
            const std::size_t terminal_retention = 4096U,
            const std::size_t condition_capacity = 8192U,
            std::string owner_epoch = "alarm.owner.epoch.1",
            const std::size_t live_alarm_capacity = kDefaultLiveAlarmCapacity) {
    auto owner = std::make_unique<AlarmLifecycleOwner>(
        OperatorAlarmCatalog::Load(MANYFORGE_HEADLESS_TEST_ALARM_CATALOG),
        std::string(kCellId), "alarm.owner", std::move(owner_epoch),
        manyforge::core::HostBootStamp{"boot.test", 7U},
        [this]() { return ++monotonic_ns_; },
        [this]() {
          return "2026-08-05T00:00:" + std::to_string(monotonic_ns_ % 60) + "Z";
        },
        std::move(required_domains), terminal_retention, condition_capacity,
        live_alarm_capacity);
    owner->ReplaceResourceRegistry(
        Registry("bundle.a", {"resource.arm.a", "resource.arm.b"}));
    return owner;
  }

  static AlarmResourceRegistry Registry(std::string bundle,
                                        std::set<std::string> resources) {
    auto graph = "graph." + bundle;
    return {std::move(bundle), std::move(graph), std::move(resources)};
  }

  static void RegisterScene(AlarmLifecycleOwner &owner,
                            std::string epoch = std::string(kSceneEpoch),
                            const std::uint64_t epoch_order = 1U) {
    owner.RegisterSource("scene", std::string(kSceneSource),
                         std::string(kSceneAdapter), std::move(epoch),
                         epoch_order);
  }

  static void RegisterAll(AlarmLifecycleOwner &owner) {
    owner.RegisterSource("motion", "motion.source",
                         "manyforge.alarm_adapter.kernel_condition",
                         "motion.epoch.1", 1U);
    RegisterScene(owner);
    owner.RegisterSource("runtime_health", "runtime.source",
                         "manyforge.alarm_adapter.runtime_health_condition",
                         "runtime.epoch.1", 1U);
  }

  static void CompleteAll(AlarmLifecycleOwner &owner) {
    owner.CompleteSourceSnapshot("motion.source", "motion.epoch.1",
                                 AlarmConditionQuality::kGood);
    owner.CompleteSourceSnapshot(kSceneSource, kSceneEpoch,
                                 AlarmConditionQuality::kGood);
    owner.CompleteSourceSnapshot("runtime.source", "runtime.epoch.1",
                                 AlarmConditionQuality::kGood);
  }

  static AlarmConditionUpdate SceneUpdate(
      const std::uint64_t sequence, const bool active,
      std::string subject_id = "box",
      std::string epoch = std::string(kSceneEpoch),
      const AlarmConditionQuality quality = AlarmConditionQuality::kGood,
      std::vector<std::string> evidence_refs = {}) {
    return AlarmConditionUpdate{
        std::string(kSceneDefinition),
        AlarmSubject{"object", std::move(subject_id), {"resource.arm.a"}},
        active,
        std::string(kSceneSource),
        std::move(epoch),
        sequence,
        static_cast<std::int64_t>(100U + sequence),
        "2026-08-05T00:00:00Z",
        quality,
        EmptyObject(),
        Object("diagnostic", "pose-apply"),
        std::move(evidence_refs),
        Object("run_id", "run.i5"),
    };
  }

  static AlarmConditionUpdate MotionUpdate(const std::uint64_t sequence,
                                           const bool active,
                                           std::string subject_id,
                                           std::vector<std::string> resources) {
    return AlarmConditionUpdate{
        "manyforge.motion.runtime_collision",
        AlarmSubject{"operation", std::move(subject_id), std::move(resources)},
        active,
        "motion.source",
        "motion.epoch.1",
        sequence,
        static_cast<std::int64_t>(200U + sequence),
        "2026-08-05T00:00:01Z",
        AlarmConditionQuality::kGood,
        EmptyObject(),
        EmptyObject(),
        {},
        EmptyObject(),
    };
  }

  static AlarmConditionUpdate CellGlobalUpdate(const std::uint64_t sequence,
                                               const bool active) {
    return AlarmConditionUpdate{
        "manyforge.runtime.provider_unavailable",
        AlarmSubject{"cell", std::string(kCellId), {}},
        active,
        "runtime.source",
        "runtime.epoch.1",
        sequence,
        static_cast<std::int64_t>(300U + sequence),
        "2026-08-05T00:00:02Z",
        AlarmConditionQuality::kGood,
        EmptyObject(),
        EmptyObject(),
        {},
        EmptyObject(),
    };
  }

  static AlarmCallerAttribution Caller() {
    return {"request.1",
            "api.epoch.1",
            "operator_ui",
            "correlation.1",
            "test",
            "manyforge.test.alarm",
            "authenticated:test",
            "principal.operator"};
  }

  template <typename Callback>
  static void ExpectAlarmError(Callback &&callback,
                               const std::string_view expected_code) {
    try {
      callback();
      FAIL() << "expected OperatorAlarmError " << expected_code;
    } catch (const OperatorAlarmError &error) {
      EXPECT_EQ(error.code(), expected_code);
    }
  }

  std::int64_t monotonic_ns_{10000};
};

TEST_F(AlarmOwnerTest,
       ExactConditionLookupIsReadonlyAndReturnsAnIndependentCopy) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  const auto update = SceneUpdate(1U, true);
  const auto generation = owner->Snapshot().generation;
  EXPECT_FALSE(owner->ConditionPoint(kSceneDefinition, update.subject));
  EXPECT_EQ(owner->Snapshot().generation, generation);
  static_cast<void>(owner->Submit(update));
  auto point = owner->ConditionPoint(kSceneDefinition, update.subject);
  ASSERT_TRUE(point);
  ASSERT_TRUE(point->record);
  EXPECT_EQ(point->record->details, update.details);
  point->record->details = EmptyObject();
  const auto actual = owner->ConditionPoint(kSceneDefinition, update.subject);
  ASSERT_TRUE(actual);
  EXPECT_EQ(actual->record->details, update.details);
  EXPECT_EQ(actual->cursor->source_sequence, 1U);
}
TEST_F(AlarmOwnerTest,
       ActivationRepeatAcknowledgementReturnAndRecurrencePreserveSemantics) {
  auto owner = MakeOwner();
  RegisterScene(*owner);

  const auto before_normal = owner->Snapshot();
  const auto normal = owner->Submit(SceneUpdate(0U, false));
  EXPECT_EQ(normal.status, "normal_observed");
  const auto after_normal = owner->Snapshot();
  EXPECT_EQ(after_normal.live_count, 0U);
  EXPECT_EQ(after_normal.generation, before_normal.generation);
  EXPECT_GT(after_normal.state_revision, before_normal.state_revision);

  const auto activated =
      owner->Submit(SceneUpdate(1U, true, "box", std::string(kSceneEpoch),
                                AlarmConditionQuality::kGood, {"evidence.1"}));
  ASSERT_EQ(activated.records.size(), 1U);
  const auto first = activated.records.front();
  EXPECT_EQ(activated.status, "activated");
  EXPECT_EQ(first.lifecycle_state, AlarmLifecycleState::kActiveUnacknowledged);
  EXPECT_EQ(first.activation_count, 1U);
  EXPECT_EQ(first.occurrence_count, 1U);
  ASSERT_EQ(activated.lifecycle_facts.size(), 1U);
  EXPECT_EQ(activated.lifecycle_facts.front().event_type,
            "operator_alarm_activated");

  const auto repeated =
      owner->Submit(SceneUpdate(2U, true, "box", std::string(kSceneEpoch),
                                AlarmConditionQuality::kGood, {"evidence.2"}));
  ASSERT_EQ(repeated.records.size(), 1U);
  EXPECT_EQ(repeated.status, "active_repeat");
  EXPECT_EQ(repeated.records.front().activation_id, first.activation_id);
  EXPECT_EQ(repeated.records.front().occurrence_count, 2U);
  EXPECT_EQ(repeated.records.front().evidence_refs,
            (std::vector<std::string>{"evidence.1", "evidence.2"}));
  EXPECT_TRUE(repeated.lifecycle_facts.empty());

  const auto acknowledged = owner->Acknowledge(
      first.condition_id, first.activation_id,
      repeated.records.front().record_revision, Caller(), std::nullopt, true);
  ASSERT_EQ(acknowledged.records.size(), 1U);
  EXPECT_EQ(acknowledged.records.front().lifecycle_state,
            AlarmLifecycleState::kActiveAcknowledged);
  EXPECT_EQ(owner->Snapshot().unacknowledged_count, 0U);

  const auto standing_repeat = owner->Submit(SceneUpdate(3U, true));
  EXPECT_EQ(standing_repeat.records.front().lifecycle_state,
            AlarmLifecycleState::kActiveAcknowledged);
  EXPECT_EQ(standing_repeat.records.front().occurrence_count, 3U);

  const auto returned = owner->Submit(SceneUpdate(4U, false));
  EXPECT_EQ(returned.records.front().lifecycle_state,
            AlarmLifecycleState::kReturnedAcknowledged);
  EXPECT_EQ(owner->Snapshot().live_count, 0U);
  ASSERT_EQ(owner->TerminalRecords().size(), 1U);

  const auto recurrence = owner->Submit(SceneUpdate(5U, true));
  ASSERT_EQ(recurrence.records.size(), 1U);
  EXPECT_EQ(recurrence.status, "recurred");
  EXPECT_NE(recurrence.records.front().activation_id, first.activation_id);
  EXPECT_EQ(recurrence.records.front().activation_count, 2U);
  EXPECT_EQ(recurrence.records.front().occurrence_count, 1U);
  EXPECT_EQ(recurrence.records.front().lifecycle_state,
            AlarmLifecycleState::kActiveUnacknowledged);
}

TEST_F(AlarmOwnerTest, ClearBeforeAcknowledgementRetainsAttentionUntilAck) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  const auto activated = owner->Submit(SceneUpdate(1U, true)).records.front();
  const auto returned = owner->Submit(SceneUpdate(2U, false)).records.front();
  EXPECT_EQ(returned.lifecycle_state,
            AlarmLifecycleState::kReturnedUnacknowledged);
  auto snapshot = owner->Snapshot();
  EXPECT_EQ(snapshot.live_count, 1U);
  EXPECT_EQ(snapshot.active_count, 0U);
  EXPECT_EQ(snapshot.unacknowledged_count, 1U);

  const auto acknowledged =
      owner->Acknowledge(returned.condition_id, activated.activation_id,
                         returned.record_revision, Caller(), "inspected", true);
  EXPECT_EQ(acknowledged.records.front().lifecycle_state,
            AlarmLifecycleState::kReturnedAcknowledged);
  EXPECT_EQ(owner->Snapshot().live_count, 0U);
  EXPECT_EQ(owner->TerminalRecords().size(), 1U);
}

TEST_F(AlarmOwnerTest,
       DuplicateOrderingEpochAndUnknownQualityRulesAreFailClosed) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  const auto update = SceneUpdate(10U, true);
  const auto original = owner->Submit(update);
  const auto duplicate = owner->Submit(update);
  EXPECT_FALSE(duplicate.changed);
  EXPECT_EQ(duplicate.status, "duplicate");
  EXPECT_EQ(duplicate.records.front().record_revision,
            original.records.front().record_revision);

  ExpectAlarmError(
      [&]() { static_cast<void>(owner->Submit(SceneUpdate(10U, false))); },
      "operator_alarm_source_sequence_conflict");
  ExpectAlarmError(
      [&]() { static_cast<void>(owner->Submit(SceneUpdate(9U, true))); },
      "operator_alarm_source_sequence_stale");
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Submit(
            SceneUpdate(11U, false, "box", std::string(kSceneEpoch),
                        AlarmConditionQuality::kUnknown)));
      },
      "operator_alarm_clear_quality_unknown");

  owner->RegisterSource("scene", std::string(kSceneSource),
                        std::string(kSceneAdapter), "scene.epoch.2", 2U);
  ExpectAlarmError(
      [&]() { static_cast<void>(owner->Submit(SceneUpdate(11U, true))); },
      "operator_alarm_source_epoch_stale");
  const auto cleared =
      owner->Submit(SceneUpdate(0U, false, "box", "scene.epoch.2"));
  EXPECT_EQ(cleared.records.front().lifecycle_state,
            AlarmLifecycleState::kReturnedUnacknowledged);
}

TEST_F(AlarmOwnerTest, AcknowledgementsAreExactlyRevisionAndMembershipFenced) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  const auto first =
      owner->Submit(SceneUpdate(1U, true, "box.a")).records.front();
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.b")));
  const auto original = owner->Snapshot();

  static_cast<void>(owner->Submit(SceneUpdate(2U, true, "box.a")));
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Acknowledge(
            first.condition_id, first.activation_id, first.record_revision,
            Caller(), std::nullopt, true));
      },
      "operator_alarm_record_revision_stale");
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->AcknowledgeAll(
            original.unacknowledged_membership, original.owner_epoch,
            original.generation, Caller(), std::nullopt, true));
      },
      "operator_alarm_owner_generation_stale");
  EXPECT_EQ(owner->Snapshot().unacknowledged_count, 2U);

  const auto current = owner->Snapshot();
  const auto acknowledged = owner->AcknowledgeAll(
      current.unacknowledged_membership, current.owner_epoch,
      current.generation, Caller(), "reviewed", true);
  EXPECT_EQ(acknowledged.status, "acknowledged_all");
  EXPECT_EQ(acknowledged.records.size(), 2U);
  ASSERT_FALSE(acknowledged.lifecycle_facts.empty());
  EXPECT_EQ(acknowledged.lifecycle_facts.back().event_type,
            "operator_alarms_acknowledged");
  EXPECT_EQ(owner->Snapshot().unacknowledged_count, 0U);

  const auto replay =
      owner->Acknowledge(acknowledged.records.front().condition_id,
                         acknowledged.records.front().activation_id,
                         acknowledged.records.front().record_revision - 1U,
                         Caller(), std::nullopt, true);
  EXPECT_FALSE(replay.changed);
  EXPECT_EQ(replay.status, "already_acknowledged");
}

TEST_F(AlarmOwnerTest,
       AttentionNewestOrderingDoesNotChangeTheNormativeDefaultOrder) {
  auto owner = MakeOwner({"motion", "scene"});
  RegisterAll(*owner);
  static_cast<void>(owner->Submit(
      MotionUpdate(1U, true, "older.collision", {"resource.arm.a"})));
  static_cast<void>(owner->Submit(SceneUpdate(200U, true, "newer.scene")));

  const auto default_page = owner->Snapshot(1U);
  ASSERT_EQ(default_page.alarms.size(), 1U);
  EXPECT_EQ(default_page.alarms.front().definition_id,
            "manyforge.motion.runtime_collision");

  const auto attention_page =
      owner->Snapshot(1U, {}, AlarmSnapshotOrdering::kAttentionNewest);
  ASSERT_EQ(attention_page.alarms.size(), 1U);
  EXPECT_EQ(attention_page.alarms.front().definition_id,
            "manyforge.scene.pose_apply_fault");
  EXPECT_EQ(attention_page.live_count, default_page.live_count);
  EXPECT_EQ(attention_page.unacknowledged_membership,
            default_page.unacknowledged_membership);
}

TEST_F(AlarmOwnerTest,
       RequiredDomainsRestartAndResourceAttentionRemainHeadlessTruth) {
  auto owner = MakeOwner({"motion", "runtime_health", "scene"});
  EXPECT_FALSE(owner->Snapshot().reconciled);
  RegisterAll(*owner);
  EXPECT_FALSE(owner->Snapshot().reconciled);
  CompleteAll(*owner);
  EXPECT_TRUE(owner->Snapshot().reconciled);

  const auto scoped =
      owner->Submit(MotionUpdate(1U, true, "operation.a", {"resource.arm.a"}))
          .records.front();
  const auto global = owner->Submit(CellGlobalUpdate(1U, true)).records.front();
  // The Kernel evaluates this exact projection; Headless only publishes it.
  auto projection = *owner->MotionAttentionProjection();
  EXPECT_EQ(projection.cell_id, kCellId);
  EXPECT_EQ(projection.owner_epoch, "alarm.owner.epoch.1");
  EXPECT_EQ(projection.owner_incarnation.boot_id, "boot.test");
  EXPECT_EQ(projection.owner_incarnation.boottime_us, 7U);
  EXPECT_EQ(projection.generation, owner->Snapshot().generation);
  EXPECT_TRUE(projection.reconciled);
  const auto wire = content_identity::canonical_json(
      OperatorAlarmMotionAttentionValue(projection));
  EXPECT_NE(wire.find("\"owner_host_boot_id\":\"boot.test\""), std::string::npos);
  EXPECT_NE(wire.find("\"owner_incarnation_us\":7"), std::string::npos);
  EXPECT_NE(wire.find("manyforge.operator_alarm_motion_attention.v3"),
            std::string::npos);
  // V2-5: acknowledgement never gates work, so neither unacknowledged
  // alarm (scoped or cell-global) is a motion-attention member.
  EXPECT_TRUE(projection.members.empty());
  EXPECT_EQ(scoped.lifecycle_state, AlarmLifecycleState::kActiveUnacknowledged);
  EXPECT_EQ(global.lifecycle_state, AlarmLifecycleState::kActiveUnacknowledged);

  static_cast<void>(
      owner->Acknowledge(global.condition_id, global.activation_id,
                         global.record_revision, Caller(), std::nullopt, true));
  projection = *owner->MotionAttentionProjection();
  EXPECT_TRUE(projection.members.empty());

  owner->MarkSourceQualityUnknown("motion.source", "motion.epoch.1");
  EXPECT_FALSE(owner->MotionAttentionProjection()->reconciled);

  auto replacement = MakeOwner({"motion", "runtime_health", "scene"}, 4096U,
                               8192U, "alarm.owner.epoch.2");
  RegisterAll(*replacement);
  const auto rediscovered =
      replacement
          ->Submit(MotionUpdate(1U, true, "operation.a", {"resource.arm.a"}))
          .records.front();
  EXPECT_NE(rediscovered.activation_id, scoped.activation_id);
  EXPECT_EQ(rediscovered.lifecycle_state,
            AlarmLifecycleState::kActiveUnacknowledged);
  EXPECT_FALSE(replacement->Snapshot().reconciled);
  CompleteAll(*replacement);
  EXPECT_TRUE(replacement->Snapshot().reconciled);
}

TEST_F(AlarmOwnerTest,
       PublishedMotionAttentionNamesEveryGenerationAndStaysImmutable) {
  auto owner = MakeOwner({"motion", "runtime_health", "scene"});
  RegisterAll(*owner);
  CompleteAll(*owner);
  const auto before = owner->MotionAttentionProjection();
  EXPECT_EQ(before->generation, owner->Snapshot().generation);
  EXPECT_TRUE(before->reconciled);
  EXPECT_TRUE(before->members.empty());

  // The condition interlock (no acknowledgement) is what gates motion.
  auto interlock = CellGlobalUpdate(1U, true);
  interlock.definition_id = "manyforge.runtime.alarm_scope_unavailable";
  static_cast<void>(owner->Submit(interlock));
  const auto gating = owner->MotionAttentionProjection();
  EXPECT_EQ(gating->generation, owner->Snapshot().generation);
  ASSERT_EQ(gating->members.size(), 1U);
  // A reader keeps the exact publication it obtained.
  EXPECT_TRUE(before->members.empty());

  // An unknown-quality observation of a condition at normal changes only
  // reconciliation; it is still published under its own generation.
  auto unknown = MotionUpdate(1U, false, "operation.b", {"resource.arm.b"});
  unknown.quality = AlarmConditionQuality::kUnknown;
  static_cast<void>(owner->Submit(unknown));
  const auto degraded = owner->MotionAttentionProjection();
  EXPECT_FALSE(degraded->reconciled);
  EXPECT_FALSE(owner->Snapshot().reconciled);
  EXPECT_GT(degraded->generation, gating->generation);
  EXPECT_EQ(degraded->generation, owner->Snapshot().generation);

  owner->MarkHealthUnavailable("test.health", "unavailable");
  EXPECT_EQ(owner->MotionAttentionProjection()->generation,
            owner->Snapshot().generation);
}

TEST_F(AlarmOwnerTest,
       RestatedHealthFactKeepsTheAcknowledgementGenerationFence) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.a")));
  owner->MarkHealthUnavailable("alarm_source_refresh_overdue", "overdue");
  const auto marked = owner->Snapshot();

  // A periodic reader restating the same fact (e.g. a 4 Hz publisher while
  // sources stay overdue) must not advance the generation.
  for (int read = 0; read < 8; ++read) {
    owner->MarkHealthUnavailable("alarm_source_refresh_overdue", "overdue");
    owner->MarkHealthUnavailableIf("alarm_source_refresh_overdue", "overdue",
                                   []() { return true; });
  }
  EXPECT_EQ(owner->Snapshot().generation, marked.generation);
  EXPECT_EQ(owner->MotionAttentionProjection()->generation, marked.generation);
  const auto acknowledged = owner->AcknowledgeAll(
      marked.unacknowledged_membership, marked.owner_epoch, marked.generation,
      Caller(), std::nullopt, true);
  EXPECT_EQ(acknowledged.status, "acknowledged_all");

  // A changed message, or a false predicate, is decided exactly.
  const auto before_change = owner->Snapshot().generation;
  owner->MarkHealthUnavailableIf("alarm_source_refresh_overdue", "changed",
                                 []() { return false; });
  EXPECT_EQ(owner->Snapshot().generation, before_change);
  owner->MarkHealthUnavailable("alarm_source_refresh_overdue", "changed");
  EXPECT_GT(owner->Snapshot().generation, before_change);
}

TEST_F(AlarmOwnerTest,
       ConditionWithoutAcknowledgementGatesMotionExactlyWhileActive) {
  auto owner = MakeOwner({"motion", "runtime_health", "scene"});
  RegisterAll(*owner);
  CompleteAll(*owner);
  const auto scope_update = [](const std::uint64_t sequence,
                               const bool active) {
    auto update = CellGlobalUpdate(sequence, active);
    update.definition_id = "manyforge.runtime.alarm_scope_unavailable";
    return update;
  };

  // Activation needs no acknowledgement: it enters the standing state with no
  // acknowledgement attribution, yet gates every reservation while active.
  const auto activated = owner->Submit(scope_update(1U, true));
  EXPECT_EQ(activated.status, "activated");
  const auto scope = activated.records.front();
  EXPECT_FALSE(scope.acknowledgement_required);
  EXPECT_EQ(scope.lifecycle_state, AlarmLifecycleState::kActiveAcknowledged);
  EXPECT_FALSE(scope.acknowledged_at_ns.has_value());
  EXPECT_FALSE(scope.acknowledgement_attribution.has_value());
  EXPECT_TRUE(GatesMotion(scope));
  auto snapshot = owner->Snapshot();
  EXPECT_EQ(snapshot.unacknowledged_count, 0U);
  EXPECT_TRUE(snapshot.unacknowledged_membership.empty());
  EXPECT_EQ(snapshot.motion_attention_count, 1U);
  ASSERT_EQ(snapshot.motion_attention_membership.size(), 1U);
  EXPECT_EQ(snapshot.motion_attention_membership.front()
                .acknowledgement.condition_id,
            scope.condition_id);
  EXPECT_EQ(owner->Metrics().motion_attention, 1U);
  auto projection = *owner->MotionAttentionProjection();
  EXPECT_EQ(GateMembers(projection),
            std::vector<std::string>{scope.condition_id});

  // An acknowledgement attempt changes nothing; only the cause clears it.
  const auto attempted =
      owner->Acknowledge(scope.condition_id, scope.activation_id,
                         scope.record_revision, Caller(), std::nullopt, true);
  EXPECT_EQ(attempted.status, "already_acknowledged");
  EXPECT_FALSE(attempted.changed);
  EXPECT_EQ(GateMembers(*owner->MotionAttentionProjection()),
            std::vector<std::string>{scope.condition_id});

  // V2-5: an acknowledgement-required alarm beside it gates nothing, before
  // or after its acknowledgement; only the condition still gates.
  const auto provider =
      owner->Submit(CellGlobalUpdate(1U, true)).records.front();
  EXPECT_TRUE(provider.acknowledgement_required);
  EXPECT_FALSE(GatesMotion(provider));
  EXPECT_EQ(owner->Snapshot().motion_attention_count, 1U);
  projection = *owner->MotionAttentionProjection();
  EXPECT_EQ(GateMembers(projection),
            std::vector<std::string>{scope.condition_id});
  static_cast<void>(owner->Acknowledge(provider.condition_id,
                                       provider.activation_id,
                                       provider.record_revision, Caller(),
                                       std::nullopt, true));
  projection = *owner->MotionAttentionProjection();
  EXPECT_EQ(GateMembers(projection),
            std::vector<std::string>{scope.condition_id});

  // Return to normal ends the gate at once and leaves the live list without
  // a returned-unacknowledged state.
  const auto returned = owner->Submit(scope_update(2U, false));
  EXPECT_EQ(returned.status, "returned");
  EXPECT_EQ(returned.records.front().lifecycle_state,
            AlarmLifecycleState::kReturnedAcknowledged);
  EXPECT_FALSE(GatesMotion(returned.records.front()));
  snapshot = owner->Snapshot();
  EXPECT_EQ(snapshot.motion_attention_count, 0U);
  EXPECT_EQ(snapshot.live_count, 1U);
  EXPECT_EQ(snapshot.alarms.front().condition_id, provider.condition_id);
  projection = *owner->MotionAttentionProjection();
  EXPECT_TRUE(projection.reconciled);
  EXPECT_TRUE(projection.members.empty());
  ASSERT_EQ(owner->TerminalRecords().size(), 1U);
  EXPECT_EQ(owner->TerminalRecords().front().condition_id, scope.condition_id);

  const auto recurred = owner->Submit(scope_update(3U, true));
  EXPECT_EQ(recurred.status, "recurred");
  EXPECT_EQ(recurred.records.front().lifecycle_state,
            AlarmLifecycleState::kActiveAcknowledged);
  EXPECT_EQ(GateMembers(*owner->MotionAttentionProjection()),
            std::vector<std::string>{scope.condition_id});
}

TEST_F(AlarmOwnerTest,
       ResourceScopeFollowsTheActiveConfigurationAndRetainsReplacedAlarms) {
  auto owner = std::make_unique<AlarmLifecycleOwner>(
      OperatorAlarmCatalog::Load(MANYFORGE_HEADLESS_TEST_ALARM_CATALOG),
      std::string(kCellId), "alarm.owner", "alarm.owner.epoch.1",
      manyforge::core::HostBootStamp{"boot.test", 7U},
      [this]() { return ++monotonic_ns_; },
      [this]() { return "2026-08-05T00:00:00Z"; },
      std::set<std::string>{"motion"});
  owner->RegisterSource("motion", "motion.source",
                        "manyforge.alarm_adapter.kernel_condition",
                        "motion.epoch.1", 1U);
  owner->CompleteSourceSnapshot("motion.source", "motion.epoch.1",
                                AlarmConditionQuality::kGood);
  // The projection names the configuration its subjects were judged against;
  // none before one is observed.
  EXPECT_FALSE(
      owner->MotionAttentionProjection()->active_configuration.has_value());
  EXPECT_NE(content_identity::canonical_json(OperatorAlarmMotionAttentionValue(
                *owner->MotionAttentionProjection()))
                .find("\"active_configuration\":null"),
            std::string::npos);

  // Empty startup: no configuration is active, so no resource is either.
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Submit(
            MotionUpdate(1U, true, "operation.a", {"resource.arm.a"})));
      },
      "operator_alarm_scope_inactive");
  EXPECT_TRUE(owner->Snapshot().reconciled);
  EXPECT_EQ(owner->Snapshot().live_count, 0U);

  owner->ReplaceResourceRegistry(Registry("bundle.a", {"resource.arm.a"}));
  const auto retained =
      owner->Submit(MotionUpdate(2U, true, "operation.a", {"resource.arm.a"}))
          .records.front();

  // The replacement configuration names other resources. The admitted
  // alarm is kept; a new activation on a removed resource is refused and
  // does not disturb the current feed or its retained member.
  const auto before_replacement = owner->Snapshot().generation;
  owner->ReplaceResourceRegistry(Registry("bundle.b", {"resource.arm.b"}));
  EXPECT_EQ(owner->Snapshot().live_count, 1U);
  // A new configuration identity advances the generation and is published.
  const auto replaced = owner->MotionAttentionProjection();
  EXPECT_GT(replaced->generation, before_replacement);
  EXPECT_EQ(replaced->generation, owner->Snapshot().generation);
  ASSERT_TRUE(replaced->active_configuration.has_value());
  EXPECT_EQ(*replaced->active_configuration,
            (AlarmConfigurationIdentity{"bundle.b", "graph.bundle.b"}));
  EXPECT_NE(content_identity::canonical_json(
                OperatorAlarmMotionAttentionValue(*replaced))
                .find("\"active_configuration\":{\"graph_revision\":"
                      "\"graph.bundle.b\",\"retained_bundle_id\":\"bundle.b\"}"),
            std::string::npos);
  // Re-observing the same configuration is not a new identity.
  owner->ReplaceResourceRegistry(Registry("bundle.b", {"resource.arm.b"}));
  EXPECT_EQ(owner->Snapshot().generation, replaced->generation);
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Submit(
            MotionUpdate(3U, true, "operation.stale", {"resource.arm.a"})));
      },
      "operator_alarm_scope_inactive");
  const auto replacement =
      owner->Submit(MotionUpdate(4U, true, "operation.b", {"resource.arm.b"}))
          .records.front();
  auto snapshot = owner->Snapshot();
  EXPECT_TRUE(snapshot.reconciled);
  EXPECT_EQ(snapshot.live_count, 2U);
  // The retained alarm keeps its admitted resources after the replacement
  // configuration no longer names them (neither gates motion: V2-5).
  EXPECT_EQ(retained.subject.resource_ids, std::vector<std::string>{"resource.arm.a"});
  EXPECT_TRUE(owner->MotionAttentionProjection()->members.empty());

  // The retained alarm still follows its lifecycle: its own return to normal
  // is accepted, and it can be acknowledged.
  const auto returned = owner->Submit(
      MotionUpdate(5U, false, "operation.a", {"resource.arm.a"}));
  ASSERT_EQ(returned.records.size(), 1U);
  EXPECT_EQ(returned.records.front().condition_id, retained.condition_id);
  EXPECT_EQ(returned.records.front().lifecycle_state,
            AlarmLifecycleState::kReturnedUnacknowledged);
  static_cast<void>(owner->Acknowledge(
      retained.condition_id, retained.activation_id,
      returned.records.front().record_revision, Caller(), std::nullopt, true));
  snapshot = owner->Snapshot();
  ASSERT_EQ(snapshot.live_count, 1U);
  EXPECT_EQ(snapshot.alarms.front().condition_id, replacement.condition_id);

  const auto metrics = owner->Metrics();
  EXPECT_EQ(metrics.totals.at("scope_rejections"), 2U);
  EXPECT_EQ(metrics.totals.at("resource_registry_replacements"), 3U);
}

TEST_F(AlarmOwnerTest, CapacityAndTerminalRetentionNeverEvictLiveAttention) {
  auto owner = MakeOwner({"scene"}, 2U, 2U, "alarm.owner.epoch.1", 2U);
  RegisterScene(*owner);
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.a")));
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.b")));
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.c")));
      },
      "operator_alarm_condition_capacity_exhausted");
  auto snapshot = owner->Snapshot();
  EXPECT_EQ(snapshot.live_count, 2U);
  EXPECT_FALSE(snapshot.health_errors.empty());
  EXPECT_FALSE(owner->MotionAttentionProjection()->reconciled);

  owner->ReconcileHealth("operator_alarm_condition_capacity_exhausted");
  const auto terminalize = snapshot.alarms.front();
  const auto acknowledged = owner->Acknowledge(
      terminalize.condition_id, terminalize.activation_id,
      terminalize.record_revision, Caller(), std::nullopt, true);
  static_cast<void>(owner->Submit(
      SceneUpdate(2U, false, acknowledged.records.front().subject.subject_id)));
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.c")));
  EXPECT_EQ(owner->Snapshot().live_count, 2U);

  const auto next = owner->Snapshot().alarms.front();
  static_cast<void>(owner->Acknowledge(next.condition_id, next.activation_id,
                                       next.record_revision, Caller(),
                                       std::nullopt, true));
  static_cast<void>(
      owner->Submit(SceneUpdate(2U, false, next.subject.subject_id)));
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.d")));
  EXPECT_LE(owner->TerminalRecords().size(), 2U);
}

TEST_F(AlarmOwnerTest, LiveCapacityIsIndependentFromTerminalCursorCapacity) {
  auto owner = MakeOwner({"scene"}, 4096U, 8192U, "alarm.owner.epoch.1", 2U);
  RegisterScene(*owner);
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.a")));
  static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.b")));
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Submit(SceneUpdate(1U, true, "box.c")));
      },
      "operator_alarm_condition_capacity_exhausted");
  EXPECT_EQ(owner->Snapshot().live_count, 2U);
  EXPECT_EQ(owner->Metrics().live_condition_points, 2U);
}

TEST_F(AlarmOwnerTest, BulkAcknowledgementCardinalityIsBounded) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  std::vector<AlarmAcknowledgementMember> members(
      kMaximumAlarmBulkMembers + 1U,
      AlarmAcknowledgementMember{"condition-000000000000000000000000",
                                 "activation.1", 1U});
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->AcknowledgeAll(std::move(members),
                                                "alarm.owner.epoch.1", 1U,
                                                Caller(), std::nullopt, true));
      },
      "operator_alarm_acknowledge_all_members_invalid");
}

TEST_F(AlarmOwnerTest, SubjectAndRegistryCardinalityAreBounded) {
  std::set<std::string> oversized_registry;
  for (std::size_t index = 0U; index <= kMaximumAlarmResourceRegistry;
       ++index) {
    oversized_registry.insert("resource." + std::to_string(index));
  }
  auto owner = MakeOwner({"motion"});
  ExpectAlarmError(
      [&]() {
        owner->ReplaceResourceRegistry(
            Registry("bundle.oversized", std::move(oversized_registry)));
      },
      "operator_alarm_configuration_invalid");
  EXPECT_EQ(owner->Metrics().totals.at("resource_registry_replacements"), 1U);
  owner->RegisterSource("motion", "motion.source",
                        "manyforge.alarm_adapter.kernel_condition",
                        "motion.epoch.1", 1U);
  std::vector<std::string> resources;
  for (std::size_t index = 0U; index <= kMaximumAlarmSubjectResources;
       ++index) {
    resources.push_back("resource." + std::to_string(index));
  }
  auto update = MotionUpdate(1U, true, "operation.a", std::move(resources));
  ExpectAlarmError(
      [&]() { static_cast<void>(owner->Submit(std::move(update))); },
      "operator_alarm_scope_invalid");
}

TEST_F(AlarmOwnerTest, RejectedNewConditionCannotEvictATerminalSourceCursor) {
  auto owner = MakeOwner({"scene"}, 2U, 1U, "alarm.owner.epoch.1", 1U);
  RegisterScene(*owner);
  const auto active =
      owner->Submit(SceneUpdate(1U, true, "box.a")).records.front();
  const auto acknowledged =
      owner->Acknowledge(active.condition_id, active.activation_id,
                         active.record_revision, Caller(), std::nullopt, true);
  static_cast<void>(owner->Submit(
      SceneUpdate(2U, false, acknowledged.records.front().subject.subject_id)));
  EXPECT_EQ(owner->Snapshot().live_count, 0U);

  auto invalid = SceneUpdate(3U, true, "box.b");
  invalid.details = Object("blob", std::string(33U * 1024U, 'x'));
  ExpectAlarmError([&]() { static_cast<void>(owner->Submit(invalid)); },
                   "operator_alarm_condition_update_invalid");

  auto conflicting = SceneUpdate(2U, true, "box.a");
  ExpectAlarmError([&]() { static_cast<void>(owner->Submit(conflicting)); },
                   "operator_alarm_source_sequence_conflict");
  EXPECT_EQ(owner->Snapshot().live_count, 0U);
}

TEST_F(AlarmOwnerTest, ConcurrentDuplicateBurstHasOneActivation) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  const auto update = SceneUpdate(1U, true);
  constexpr std::size_t kThreadCount = 32U;
  std::barrier start(static_cast<std::ptrdiff_t>(kThreadCount));
  std::atomic<std::size_t> activated{0U};
  std::atomic<std::size_t> duplicated{0U};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    threads.emplace_back([&]() {
      start.arrive_and_wait();
      const auto result = owner->Submit(update);
      activated.fetch_add(result.status == "activated" ? 1U : 0U);
      duplicated.fetch_add(result.status == "duplicate" ? 1U : 0U);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(activated.load(), 1U);
  EXPECT_EQ(duplicated.load(), kThreadCount - 1U);
  const auto snapshot = owner->Snapshot();
  ASSERT_EQ(snapshot.alarms.size(), 1U);
  EXPECT_EQ(snapshot.live_count, 1U);
  EXPECT_EQ(snapshot.alarms.front().occurrence_count, 1U);
}

TEST_F(AlarmOwnerTest, ConcurrentClearAndAckConvergeWithoutLostAttention) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  const auto original = owner->Submit(SceneUpdate(1U, true)).records.front();
  std::barrier start(2);
  std::string acknowledgement_result;
  std::string clear_result;
  std::thread acknowledge([&]() {
    start.arrive_and_wait();
    try {
      acknowledgement_result =
          owner
              ->Acknowledge(original.condition_id, original.activation_id,
                            original.record_revision, Caller(), std::nullopt,
                            true)
              .status;
    } catch (const OperatorAlarmError &error) {
      acknowledgement_result = error.code();
    }
  });
  std::thread clear([&]() {
    start.arrive_and_wait();
    clear_result = owner->Submit(SceneUpdate(2U, false)).status;
  });
  acknowledge.join();
  clear.join();
  EXPECT_EQ(clear_result, "returned");
  if (owner->Snapshot().unacknowledged_count != 0U) {
    const auto returned = owner->Snapshot().alarms.front();
    static_cast<void>(owner->Acknowledge(
        returned.condition_id, returned.activation_id, returned.record_revision,
        Caller(), std::nullopt, true));
  }
  EXPECT_EQ(owner->Snapshot().live_count, 0U);
  ASSERT_FALSE(owner->TerminalRecords().empty());
  EXPECT_EQ(owner->TerminalRecords(1U).front().lifecycle_state,
            AlarmLifecycleState::kReturnedAcknowledged);
  EXPECT_TRUE(acknowledgement_result == "acknowledged" ||
              acknowledgement_result == "operator_alarm_record_revision_stale");
}

TEST_F(AlarmOwnerTest,
       MetricsPreserveHistoricalRecurrenceFloodChatterAndConflictEvidence) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  for (std::uint64_t activation = 0U; activation < 11U; ++activation) {
    const auto sequence = activation * 2U + 1U;
    static_cast<void>(owner->Submit(SceneUpdate(sequence, true)));
    if (activation < 10U) {
      static_cast<void>(owner->Submit(SceneUpdate(sequence + 1U, false)));
    }
  }
  ExpectAlarmError(
      [&]() { static_cast<void>(owner->Submit(SceneUpdate(20U, true))); },
      "operator_alarm_source_sequence_stale");
  const auto active = owner->Snapshot().alarms.front();
  static_cast<void>(
      owner->Acknowledge(active.condition_id, active.activation_id,
                         active.record_revision, Caller(), std::nullopt, true));
  owner->MarkHealthUnavailable("historian_delivery_unavailable",
                               "injected persistence failure");
  owner->ReconcileHealth("historian_delivery_unavailable");

  const auto metrics = owner->Metrics();
  EXPECT_EQ(metrics.totals.at("activations"), 11U);
  EXPECT_EQ(metrics.totals.at("recurrences"), 10U);
  EXPECT_EQ(metrics.totals.at("returns"), 10U);
  EXPECT_EQ(metrics.totals.at("stale_updates"), 1U);
  EXPECT_EQ(metrics.totals.at("out_of_order_sequence_updates"), 1U);
  EXPECT_EQ(metrics.totals.at("acknowledgements"), 1U);
  EXPECT_EQ(metrics.totals.at("historian_delivery_failures"), 1U);
  EXPECT_EQ(metrics.totals.at("historian_delivery_recoveries"), 1U);
  EXPECT_TRUE(metrics.alarm_flood_candidate);
  ASSERT_EQ(metrics.chattering_candidates.size(), 1U);
  EXPECT_EQ(metrics.chattering_candidates.front().condition_id,
            active.condition_id);
  EXPECT_EQ(metrics.ingest_latency_ns.count, 22U);
  EXPECT_EQ(metrics.acknowledgement_latency_ns.count, 1U);
  const auto encoded =
      content_identity::canonical_json(OperatorAlarmMetricsValue(metrics));
  EXPECT_NE(encoded.find("manyforge.operator_alarm_metrics.v1"),
            std::string::npos);
  EXPECT_NE(encoded.find("engineering_assessment_not_safety_limit"),
            std::string::npos);
}

TEST_F(AlarmOwnerTest, LongStandingAlarmMetricsRemainExactBeyondThirtyYears) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  static_cast<void>(owner->Submit(SceneUpdate(1U, true)));
  const auto active = owner->Snapshot().alarms.front();
  constexpr std::int64_t years31 = 31LL * 366 * 24 * 60 * 60 * 1000000000;
  monotonic_ns_ = years31;
  static_cast<void>(owner->Acknowledge(active.condition_id, active.activation_id,
                                      active.record_revision, Caller(), std::nullopt, true));
  monotonic_ns_ = 2 * years31;
  const auto metrics = owner->Metrics();
  ASSERT_TRUE(metrics.acknowledgement_latency_ns.maximum_ns.has_value());
  EXPECT_GT(*metrics.acknowledgement_latency_ns.maximum_ns, 9007199254740991ULL);
  EXPECT_GT(*metrics.current_standing_duration_ns.maximum_ns, 9007199254740991ULL);
  EXPECT_FALSE(metrics.acknowledgement_latency_ns.sum_saturated);
  const auto encoded = content_identity::canonical_json(OperatorAlarmMetricsValue(metrics));
  EXPECT_NE(encoded.find("\"max_ns\":\""), std::string::npos);
  EXPECT_NE(encoded.find("\"sum_saturated\":false"), std::string::npos);
}

TEST_F(AlarmOwnerTest, BoundedCanonicalInputsAndSnapshotShapeAreEnforced) {
  auto owner = MakeOwner();
  RegisterScene(*owner);
  auto unsorted = SceneUpdate(1U, true);
  unsorted.subject.resource_ids = {"resource.arm.b", "resource.arm.a"};
  ExpectAlarmError([&]() { static_cast<void>(owner->Submit(unsorted)); },
                   "operator_alarm_scope_invalid");

  auto oversized = SceneUpdate(1U, true);
  oversized.details = Object("blob", std::string(33U * 1024U, 'x'));
  ExpectAlarmError([&]() { static_cast<void>(owner->Submit(oversized)); },
                   "operator_alarm_condition_update_invalid");
  EXPECT_EQ(owner->Snapshot().live_count, 0U);

  const auto record = owner->Submit(SceneUpdate(1U, true)).records.front();
  const auto encoded = content_identity::canonical_json(
      OperatorAlarmSnapshotValue(owner->Snapshot()));
  EXPECT_NE(encoded.find(
                "\"schema_version\":\"manyforge.operator_alarm_snapshot.v2\""),
            std::string::npos);
  EXPECT_NE(encoded.find(record.condition_id), std::string::npos);
  EXPECT_NE(encoded.find("\"motion_attention_membership\""), std::string::npos);
  ExpectAlarmError(
      [&]() {
        static_cast<void>(owner->Snapshot(kMaximumAlarmSnapshotPage + 1U));
      },
      "operator_alarm_limit_invalid");
}

} // namespace
} // namespace manyforge::headless
