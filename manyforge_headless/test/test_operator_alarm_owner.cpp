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

class AlarmOwnerTest : public ::testing::Test {
protected:
  std::unique_ptr<AlarmLifecycleOwner>
  MakeOwner(std::set<std::string> required_domains = {"scene"},
            const std::size_t terminal_retention = 4096U,
            const std::size_t condition_capacity = 8192U,
            std::string owner_epoch = "alarm.owner.epoch.1",
            const std::size_t live_alarm_capacity = kDefaultLiveAlarmCapacity) {
    return std::make_unique<AlarmLifecycleOwner>(
        OperatorAlarmCatalog::Load(MANYFORGE_HEADLESS_TEST_ALARM_CATALOG),
        std::string(kCellId),
        std::set<std::string>{"resource.arm.a", "resource.arm.b"},
        "alarm.owner", std::move(owner_epoch),
        [this]() { return ++monotonic_ns_; },
        [this]() {
          return "2026-08-05T00:00:" + std::to_string(monotonic_ns_ % 60) + "Z";
        },
        std::move(required_domains), terminal_retention, condition_capacity,
        live_alarm_capacity);
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
  auto unrelated = owner->EvaluateMotionAttention({"resource.arm.b"});
  EXPECT_TRUE(unrelated.available);
  EXPECT_TRUE(unrelated.blocked);
  ASSERT_EQ(unrelated.matching_alarms.size(), 1U);
  EXPECT_EQ(unrelated.matching_alarms.front().condition_id,
            global.condition_id);

  static_cast<void>(
      owner->Acknowledge(global.condition_id, global.activation_id,
                         global.record_revision, Caller(), std::nullopt, true));
  unrelated = owner->EvaluateMotionAttention({"resource.arm.b"});
  EXPECT_TRUE(unrelated.available);
  EXPECT_FALSE(unrelated.blocked);
  const auto intersecting = owner->EvaluateMotionAttention({"resource.arm.a"});
  EXPECT_TRUE(intersecting.blocked);
  ASSERT_EQ(intersecting.matching_alarms.size(), 1U);
  EXPECT_EQ(intersecting.matching_alarms.front().condition_id,
            scoped.condition_id);

  owner->MarkSourceQualityUnknown("motion.source", "motion.epoch.1");
  const auto unavailable = owner->EvaluateMotionAttention({"resource.arm.b"});
  EXPECT_FALSE(unavailable.available);
  EXPECT_TRUE(unavailable.blocked);

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
  EXPECT_TRUE(owner->EvaluateMotionAttention({"resource.arm.b"}).blocked);

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
  EXPECT_THROW(
      AlarmLifecycleOwner(
          OperatorAlarmCatalog::Load(MANYFORGE_HEADLESS_TEST_ALARM_CATALOG),
          std::string(kCellId), std::move(oversized_registry), "alarm.owner",
          "alarm.owner.epoch.1", [this]() { return ++monotonic_ns_; },
          [this]() { return "2026-08-05T00:00:00Z"; }),
      OperatorAlarmError);

  auto owner = MakeOwner({"motion"});
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
