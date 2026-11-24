#include <gtest/gtest.h>

#include <manyforge_core/collision/ordered_contact_validation.hpp>

#include "collision_rule_test_helpers.hpp"
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

CollisionPairId test_pair() {
  return {"component/arm/model/arm_model/body/gripper/collision/0",
          "scene/part/shape/0"};
}

CollisionPolicySnapshot release_policy() {
  CollisionPolicySnapshot policy;
  policy.policy_id = "ordered_contact_test";
  policy.revision = {17, "policy-hash"};
  policy.require_full_diagnostics = true;
  auto entry = testing::release_rule(test_pair());
  testing::release_of(entry).max_start_penetration_m = 0.1;
  testing::release_of(entry).release_tolerance_m = 1e-5;
  testing::release_of(entry).min_release_progress_m = 0.001;
  testing::add_contact_rule(policy, entry);
  return policy;
}

CollisionPolicySnapshot binary_release_policy() {
  auto policy = release_policy();
  auto& entry = policy.contact_rules.front();
  testing::release_of(entry).release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  testing::release_of(entry).enforce_max_start_penetration = false;
  testing::release_of(entry).max_start_penetration_m = 0.0;
  testing::release_of(entry).release_tolerance_m = 0.0;
  testing::release_of(entry).min_release_progress_m = 0.0;
  testing::release_of(entry).require_strictly_decreasing_penetration = false;
  return policy;
}

CollisionPolicySnapshot bounded_penetration_policy(
    double max_penetration_m = 0.005) {
  auto policy = release_policy();
  policy.contact_rules.front().value =
      CollisionContactRule{max_penetration_m, 0.0, std::nullopt};
  return policy;
}

CollisionValidationWaypointRecord record_at(std::int64_t local_index,
                                            std::optional<double> clearance) {
  CollisionValidationWaypointRecord record;
  record.waypoint_index = local_index;
  record.time_from_start_ns = local_index * 10'000'000;
  record.result.engine_status = CollisionEngineStatus::kOk;
  record.result.decision = CollisionDecisionResult::kPass;
  if (clearance) {
    record.result.pair_clearances.push_back({test_pair(), *clearance});
    record.result.min_clearance_pair = test_pair();
    record.result.min_clearance_m = *clearance;
    if (*clearance < 0.0) {
      record.result.in_collision = true;
      record.result.normalized_pairs.push_back(test_pair());
      record.result.decision = CollisionDecisionResult::kFail;
    }
  }
  return record;
}

CollisionValidationWaypointRecord contact_record_at(
    std::int64_t local_index, bool in_contact,
    std::optional<double> clearance = std::nullopt,
    CollisionPairId pair = test_pair()) {
  CollisionValidationWaypointRecord record;
  record.waypoint_index = local_index;
  record.time_from_start_ns = local_index * 10'000'000;
  record.result.engine_status = CollisionEngineStatus::kOk;
  record.result.decision = CollisionDecisionResult::kPass;
  if (in_contact) {
    record.result.in_collision = true;
    record.result.normalized_pairs.push_back(pair);
  }
  if (clearance) {
    record.result.pair_clearances.push_back({pair, *clearance});
  }
  return record;
}

std::vector<CollisionValidationWaypointRecord> binary_records_for(
    const std::vector<bool>& contacts, std::size_t begin, std::size_t end) {
  std::vector<CollisionValidationWaypointRecord> records;
  for (std::size_t i = begin; i < end; ++i) {
    records.push_back(contact_record_at(
        static_cast<std::int64_t>(i - begin), contacts[i]));
  }
  return records;
}

std::vector<CollisionValidationWaypointRecord> records_for(const std::vector<double>& clearances,
                                                           std::size_t begin, std::size_t end) {
  std::vector<CollisionValidationWaypointRecord> records;
  for (std::size_t i = begin; i < end; ++i) {
    records.push_back(record_at(static_cast<std::int64_t>(i - begin), clearances[i]));
  }
  return records;
}

TEST(OrderedContactValidationTest, SupportTransitionCompositionMatchesOneWholeSummary) {
  const auto policy = release_policy();
  const std::vector<double> clearances{-0.04, -0.03, 0.002, -0.02, 0.003, 0.004};
  const auto whole = summarize_ordered_contact_chunk(policy, records_for(clearances, 0, 6), 0, 6);
  const auto first = summarize_ordered_contact_chunk(policy, records_for(clearances, 0, 2), 0, 2);
  const auto middle = summarize_ordered_contact_chunk(policy, records_for(clearances, 2, 4), 2, 4);
  const auto last = summarize_ordered_contact_chunk(policy, records_for(clearances, 4, 6), 4, 6);

  const std::vector<SupportContactEvaluationState> entry_states{
      SupportContactEvaluationState::kNotInContact,
      SupportContactEvaluationState::kInitialContact,
      SupportContactEvaluationState::kContactActive,
      SupportContactEvaluationState::kSeparated,
      SupportContactEvaluationState::kRecontactViolation,
  };
  for (const auto entry_state : entry_states) {
    SCOPED_TRACE(static_cast<int>(entry_state));
    auto whole_state = initial_support_contact_state(policy);
    ASSERT_EQ(whole_state.size(), 1u);
    whole_state.begin()->second = entry_state;
    auto partitioned_state = whole_state;
    std::string error;
    ASSERT_TRUE(advance_support_contact_state(whole, whole_state, &error)) << error;
    ASSERT_TRUE(advance_support_contact_state(first, partitioned_state, &error)) << error;
    ASSERT_TRUE(advance_support_contact_state(middle, partitioned_state, &error)) << error;
    ASSERT_TRUE(advance_support_contact_state(last, partitioned_state, &error)) << error;
    EXPECT_EQ(partitioned_state, whole_state);
  }
}

TEST(OrderedContactValidationTest, ReleaseMergeRejectsPolicyRevisionOrRangeSequenceMismatch) {
  const auto policy = release_policy();
  const std::vector<double> clearances{-0.04, -0.03, 0.002, 0.004};
  auto first = summarize_ordered_contact_chunk(policy, records_for(clearances, 0, 2), 0, 2);
  auto second = summarize_ordered_contact_chunk(policy, records_for(clearances, 2, 4), 2, 4);

  std::string error;
  second.policy_revision.revision_id += 1;
  EXPECT_TRUE(
      merge_contact_release_summaries(policy, {first, second}, clearances.size(), &error).empty());
  EXPECT_EQ(error, "ordered_contact_summary_sequence_mismatch");

  second = summarize_ordered_contact_chunk(policy, records_for(clearances, 2, 4), 2, 4);
  second.range_begin = 3;
  error.clear();
  EXPECT_TRUE(
      merge_contact_release_summaries(policy, {first, second}, clearances.size(), &error).empty());
  EXPECT_EQ(error, "ordered_contact_summary_sequence_mismatch");

  second.range_begin = 2;
  second.range_end = 3;
  error.clear();
  EXPECT_TRUE(
      merge_contact_release_summaries(policy, {first, second}, clearances.size(), &error).empty());
  EXPECT_EQ(error, "ordered_contact_summary_coverage_mismatch");
}

TEST(OrderedContactValidationTest,
     DuplicateAndOverBoundContactIdentitiesAreRejectedDeterministically) {
  auto duplicate_policy = release_policy();
  testing::add_contact_rule(duplicate_policy, duplicate_policy.contact_rules.front());
  auto summary = summarize_ordered_contact_chunk(duplicate_policy, {record_at(0, -0.01)}, 0, 1);
  EXPECT_FALSE(summary.valid);
  EXPECT_EQ(summary.invalid_reason, "duplicate_contact_rule_pair");

  auto over_bound_policy = release_policy();
  over_bound_policy.contact_rules.clear();
  for (std::size_t i = 0; i <= kMaxOrderedSupportContactIdentities; ++i) {
    auto entry = testing::release_rule({"robot_" + std::to_string(i), "object_" + std::to_string(i)});
    testing::add_contact_rule(over_bound_policy, std::move(entry));
  }
  summary = summarize_ordered_contact_chunk(over_bound_policy, {record_at(0, -0.01)}, 0, 1);
  EXPECT_FALSE(summary.valid);
  EXPECT_EQ(summary.invalid_reason, "contact_rule_pair_bound_exceeded");
}

TEST(OrderedContactValidationTest, SummaryStateRejectsMissingOrUnexpectedContactIdentity) {
  const auto policy = release_policy();
  const auto summary = summarize_ordered_contact_chunk(policy, {record_at(0, -0.01)}, 0, 1);
  SupportContactEvaluationStateMap state;
  std::string error;
  EXPECT_FALSE(advance_support_contact_state(summary, state, &error));
  EXPECT_EQ(error, "contact_rule_summary_identity_mismatch");
}

TEST(OrderedContactValidationTest,
     BinaryContactPlateauThenSeparationPassesWholeAndPartitionedMerge) {
  const auto policy = binary_release_policy();
  const std::vector<bool> contacts{true, true, true, false};
  const auto whole = summarize_ordered_contact_chunk(
      policy, binary_records_for(contacts, 0, contacts.size()), 0,
      contacts.size());
  const auto first = summarize_ordered_contact_chunk(
      policy, binary_records_for(contacts, 0, 2), 0, 2);
  const auto second = summarize_ordered_contact_chunk(
      policy, binary_records_for(contacts, 2, 4), 2, 4);

  std::string error;
  ContactReleaseEvaluationStateMap whole_exit;
  const auto whole_violations = merge_contact_release_summaries(
      policy, {whole}, contacts.size(), &error, nullptr, true, &whole_exit);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(whole_violations.empty());

  ContactReleaseEvaluationStateMap partitioned_exit;
  const auto partitioned_violations = merge_contact_release_summaries(
      policy, {first, second}, contacts.size(), &error, nullptr, true,
      &partitioned_exit);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(partitioned_violations.empty());
  EXPECT_EQ(partitioned_exit.size(), 1u);
  EXPECT_EQ(partitioned_exit, whole_exit);
  EXPECT_FALSE(partitioned_exit.begin()->second.in_contact);
  EXPECT_TRUE(partitioned_exit.begin()->second.separated);
  EXPECT_FALSE(
      partitioned_exit.begin()->second.initial_depth_m.has_value());
}

TEST(OrderedContactValidationTest,
     BinaryContactRejectsSameCountIdentityReplacement) {
  auto policy = binary_release_policy();
  const CollisionPairId second_pair{
      "component/arm/model/arm_model/body/gripper/collision/0",
      "scene/other/shape/0"};
  auto second_entry = policy.contact_rules.front();
  second_entry.pair = second_pair;
  testing::add_contact_rule(policy, second_entry);

  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, true));
  records.push_back(contact_record_at(1, true, std::nullopt, second_pair));
  const auto summary =
      summarize_ordered_contact_chunk(policy, records, 0, records.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, records.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  // contact_rules is sorted by pair: scene/other sorts before scene/part.
  EXPECT_EQ(violations.front().policy_entry_index, 0u);
  EXPECT_EQ(violations.front().waypoint_index, 1);
  EXPECT_EQ(violations.front().reason,
            "new_active_contact_after_clear_start");
}

TEST(OrderedContactValidationTest, BinaryContactRejectsRecontact) {
  const auto policy = binary_release_policy();
  const std::vector<bool> contacts{true, false, true, false};
  const auto summary = summarize_ordered_contact_chunk(
      policy, binary_records_for(contacts, 0, contacts.size()), 0,
      contacts.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, contacts.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().waypoint_index, 2);
  EXPECT_EQ(violations.front().reason, "recontact_after_release");
}

TEST(OrderedContactValidationTest,
     BinaryContactRequiresFinalPairAbsenceWithoutMetrics) {
  const auto policy = binary_release_policy();
  const std::vector<bool> contacts{true, true};
  const auto summary = summarize_ordered_contact_chunk(
      policy, binary_records_for(contacts, 0, contacts.size()), 0,
      contacts.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, contacts.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().waypoint_index, 1);
  EXPECT_EQ(violations.front().reason, "final_sample_not_separated");
  EXPECT_FALSE(violations.front().initial_depth_m.has_value());
  EXPECT_FALSE(violations.front().current_depth_m.has_value());
}

TEST(OrderedContactValidationTest,
     SignedPenetrationAllowsAbsentPairWithoutClearance) {
  const auto policy = release_policy();
  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, true, -0.04));
  records.push_back(contact_record_at(1, false));
  const auto summary =
      summarize_ordered_contact_chunk(policy, records, 0, records.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, records.size(), &error);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(violations.empty());
}

TEST(OrderedContactValidationTest,
     SignedPenetrationFailsClosedWhenCollidingPairHasNoMetric) {
  const auto policy = release_policy();
  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, true, -0.04));
  records.push_back(contact_record_at(1, true));
  const auto summary =
      summarize_ordered_contact_chunk(policy, records, 0, records.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, records.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().waypoint_index, 1);
  EXPECT_EQ(violations.front().decision,
            CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(violations.front().reason,
            "missing_contact_release_clearance");
}

TEST(OrderedContactValidationTest,
     BoundedPenetrationAllowsAppearanceRecontactAndFinalContactAcrossChunks) {
  const auto policy = bounded_penetration_policy();
  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, false));
  records.push_back(contact_record_at(1, true, -0.004));
  records.push_back(contact_record_at(2, false));
  records.push_back(contact_record_at(3, true, -0.003));
  records.push_back(contact_record_at(4, true, -0.005));

  const auto whole = summarize_ordered_contact_chunk(
      policy, records, 0, records.size());
  const auto first_records =
      std::vector<CollisionValidationWaypointRecord>(records.begin(),
                                                     records.begin() + 2);
  const auto first = summarize_ordered_contact_chunk(
      policy, first_records, 0, 2);
  auto second_records =
      std::vector<CollisionValidationWaypointRecord>(records.begin() + 2,
                                                     records.end());
  for (std::size_t i = 0; i < second_records.size(); ++i) {
    second_records[i].waypoint_index = static_cast<std::int64_t>(i);
  }
  const auto second = summarize_ordered_contact_chunk(
      policy, second_records, 2, records.size());

  std::string error;
  ContactReleaseEvaluationStateMap whole_exit;
  const auto whole_violations = merge_contact_release_summaries(
      policy, {whole}, records.size(), &error, nullptr, true, &whole_exit);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(whole_violations.empty());

  ContactReleaseEvaluationStateMap partitioned_exit;
  const auto partitioned_violations = merge_contact_release_summaries(
      policy, {first, second}, records.size(), &error, nullptr, true,
      &partitioned_exit);
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(partitioned_violations.empty());
  EXPECT_EQ(partitioned_exit, whole_exit);
  ASSERT_EQ(partitioned_exit.size(), 1u);
  EXPECT_TRUE(partitioned_exit.begin()->second.in_contact);
}

TEST(OrderedContactValidationTest, SpringStepIsRefusedInsideAChunkAndAcrossChunks) {
  // OD-356: stroke 10 mm, at most 2 mm deeper per 1 mm sample.
  auto policy = bounded_penetration_policy(0.010);
  testing::contact_of(policy.contact_rules.front()).max_compression_step_m = 0.002;
  const CollisionValidationPassConfig spacing;  // one step per sample
  const auto run = [&](const std::vector<double>& depths, std::size_t split) {
    std::vector<CollisionValidationWaypointRecord> first;
    std::vector<CollisionValidationWaypointRecord> second;
    for (std::size_t i = 0; i < depths.size(); ++i) {
      const bool touching = depths[i] > 0.0;
      auto& chunk = i < split ? first : second;
      chunk.push_back(contact_record_at(static_cast<std::int64_t>(i < split ? i : i - split),
                                        touching, touching ? -depths[i] : 0.01));
    }
    std::vector<OrderedContactChunkSummary> summaries{
        summarize_ordered_contact_chunk(policy, first, 0, split, &spacing)};
    if (!second.empty()) {
      summaries.push_back(
          summarize_ordered_contact_chunk(policy, second, split, depths.size(), &spacing));
    }
    std::string error;
    auto violations = merge_contact_release_summaries(policy, summaries, depths.size(), &error,
                                                      nullptr, true, nullptr);
    EXPECT_TRUE(error.empty()) << error;
    return violations;
  };

  const std::vector<double> smooth{0.0, 0.0015, 0.003, 0.0045, 0.006, 0.006, 0.0};
  EXPECT_TRUE(run(smooth, smooth.size()).empty());
  EXPECT_TRUE(run(smooth, 3).empty());

  const std::vector<double> jump{0.0, 0.001, 0.002, 0.0045, 0.005};
  for (const std::size_t split : {jump.size(), std::size_t{3}, std::size_t{2}}) {
    SCOPED_TRACE(split);
    const auto violations = run(jump, split);
    ASSERT_EQ(violations.size(), 1u);
    EXPECT_EQ(violations.front().reason, "contact_compression_step_exceeded");
    EXPECT_EQ(violations.front().waypoint_index, 3);
  }

  // Touching a step face: clear, then 3 mm deep.
  const auto face = run({0.0, 0.0, 0.003}, 3);
  ASSERT_EQ(face.size(), 1u);
  EXPECT_EQ(face.front().waypoint_index, 2);
}

TEST(OrderedContactValidationTest,
     BoundedPenetrationRejectsFirstOverBoundSampleAtExactWaypoint) {
  const auto policy = bounded_penetration_policy();
  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, true, -0.004));
  records.push_back(contact_record_at(1, false));
  records.push_back(contact_record_at(2, true, -0.006));
  const auto summary = summarize_ordered_contact_chunk(
      policy, records, 0, records.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, records.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().waypoint_index, 2);
  EXPECT_EQ(violations.front().decision, CollisionDecisionResult::kFail);
  EXPECT_EQ(violations.front().reason,
            "contact_penetration_exceeded");
  ASSERT_TRUE(violations.front().current_depth_m);
  EXPECT_DOUBLE_EQ(*violations.front().current_depth_m, 0.006);
}

TEST(OrderedContactValidationTest,
     BoundedPenetrationFailsClosedOnlyWhenCollidingMetricIsMissing) {
  const auto policy = bounded_penetration_policy();
  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, false));
  records.push_back(contact_record_at(1, true));
  const auto summary = summarize_ordered_contact_chunk(
      policy, records, 0, records.size());

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, records.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().waypoint_index, 1);
  EXPECT_EQ(violations.front().decision,
            CollisionDecisionResult::kInconclusiveFail);
  EXPECT_EQ(violations.front().reason,
            "missing_contact_release_clearance");
}

TEST(OrderedContactValidationTest,
     UntrackedContactRulesAreSkippedAndKeepTheirPolicyIndex) {
  // contact_rules: [0] whole-move contact without limit, [1] windowed
  // contact, [2] the tracked release pair. Only [2] gets a summary and a
  // violation, reported with its index in contact_rules.
  auto policy = release_policy();
  const auto release = policy.contact_rules.front();
  policy.contact_rules.clear();
  testing::add_contact_rule(policy, testing::contact_rule({"a_robot", "object"}));
  testing::add_contact_rule(policy, testing::contact_rule({"b_robot", "object"}, 0.002, 0.8));
  testing::add_contact_rule(policy, release);

  EXPECT_EQ(initial_support_contact_state(policy).size(), 1u);

  std::vector<CollisionValidationWaypointRecord> records;
  records.push_back(contact_record_at(0, true, -0.04));
  records.push_back(contact_record_at(1, false));
  records.push_back(contact_record_at(2, true, -0.01));
  const auto summary =
      summarize_ordered_contact_chunk(policy, records, 0, records.size());
  ASSERT_TRUE(summary.valid);
  EXPECT_EQ(summary.support_contact_transitions.size(), 1u);
  EXPECT_EQ(summary.contact_release_summaries.size(), 1u);

  std::string error;
  const auto violations = merge_contact_release_summaries(
      policy, {summary}, records.size(), &error);
  ASSERT_TRUE(error.empty());
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().policy_entry_index, 2u);
  EXPECT_EQ(violations.front().reason, "recontact_after_release");
}


// OD-371: a spring slider and the tube it carries, both against an endplate,
// under one grouped spring rule (stroke 10 mm, 2 mm per sample).
CollisionPairId slider_pair() {
  return {"component/holder/model/holder_model/body/holder_rod/collision/0",
          "scene/endplate/shape/0"};
}
CollisionPairId tube_pair() { return {"scene/endplate/shape/0", "scene/tube/shape/0"}; }

CollisionPolicySnapshot spring_slider_policy(bool grouped) {
  CollisionPolicySnapshot policy;
  policy.policy_id = "spring_slider_test";
  policy.revision = {19, "spring-slider-hash"};
  policy.require_full_diagnostics = true;
  for (const auto& pair : {slider_pair(), tube_pair()}) {
    auto entry = testing::contact_rule(pair, 0.010, std::nullopt, "tube_into_endplate");
    testing::contact_of(entry).max_compression_step_m = 0.002;
    if (grouped) {
      testing::contact_of(entry).spring_group =
          CollisionSpringGroup{ControllableComponentId("holder"), "holder_rod", {0.0, 0.0, 1.0}};
    }
    testing::add_contact_rule(policy, entry);
  }
  return policy;
}

/// One sample: the slider's and the tube's depth (0: not touching).
CollisionValidationWaypointRecord slider_record_at(std::int64_t index, double slider_depth,
                                                   double tube_depth) {
  CollisionValidationWaypointRecord record;
  record.waypoint_index = index;
  record.time_from_start_ns = index * 10'000'000;
  record.result.engine_status = CollisionEngineStatus::kOk;
  record.result.decision = CollisionDecisionResult::kPass;
  for (const auto& [pair, depth] : {std::pair{slider_pair(), slider_depth},
                                    std::pair{tube_pair(), tube_depth}}) {
    if (depth > 0.0) {
      record.result.in_collision = true;
      record.result.decision = CollisionDecisionResult::kFail;
      record.result.normalized_pairs.push_back(pair);
      record.result.pair_clearances.push_back({pair, -depth});
    }
  }
  return record;
}

std::vector<ContactReleaseViolation> spring_slider_violations(
    const CollisionPolicySnapshot& policy,
    const std::vector<std::pair<double, double>>& depths) {
  std::vector<CollisionValidationWaypointRecord> records;
  for (std::size_t i = 0; i < depths.size(); ++i) {
    records.push_back(slider_record_at(static_cast<std::int64_t>(i), depths[i].first,
                                       depths[i].second));
  }
  const CollisionValidationPassConfig spacing;  // one step per sample
  std::string error;
  auto violations = merge_contact_release_summaries(
      policy, {summarize_ordered_contact_chunk(policy, records, 0, records.size(), &spacing)},
      records.size(), &error, nullptr, true, nullptr);
  EXPECT_TRUE(error.empty()) << error;
  for (auto& violation : spring_group_release_violations(policy, records, spacing)) {
    violations.push_back(std::move(violation));
  }
  return violations;
}

TEST(OrderedContactValidationTest, GroupedSpringContactMovesBetweenBodiesWithoutAStep) {
  // The tube touches first; the slider takes over at 2.6 mm, 1.6 mm deeper
  // than the rule was: one spring, no jump.
  const std::vector<std::pair<double, double>> handover{
      {0.0, 0.0}, {0.0, 0.001}, {0.0026, 0.0025}, {0.004, 0.0}};
  EXPECT_TRUE(spring_slider_violations(spring_slider_policy(true), handover).empty());

  // Without a group each pair is its own spring: the slider jumps to 2.6 mm.
  const auto per_pair = spring_slider_violations(spring_slider_policy(false), handover);
  ASSERT_EQ(per_pair.size(), 1u);
  EXPECT_EQ(per_pair.front().reason, "contact_compression_step_exceeded");
  EXPECT_EQ(per_pair.front().waypoint_index, 2);
}

TEST(OrderedContactValidationTest, GroupedSpringStepLimitAppliesToTheGroupDepth) {
  const auto policy = spring_slider_policy(true);
  // The rule's depth goes 1 mm -> 2.6 mm -> 5.5 mm: the last step is 2.9 mm.
  const auto violations = spring_slider_violations(
      policy, {{0.0, 0.0}, {0.0, 0.001}, {0.0026, 0.0025}, {0.0055, 0.003}});
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations.front().reason, "contact_compression_step_exceeded");
  EXPECT_EQ(violations.front().waypoint_index, 3);
  ASSERT_TRUE(violations.front().current_depth_m);
  EXPECT_DOUBLE_EQ(*violations.front().current_depth_m, 0.0055);
  // Reported on the rule's deepest pair, the slider's.
  EXPECT_EQ(policy.contact_rules[violations.front().policy_entry_index].pair, slider_pair());

  // The depth limit stays per pair (the deepest pair is the rule's depth).
  const auto deep = spring_slider_violations(
      policy, {{0.0, 0.0}, {0.0, 0.0015}, {0.0, 0.003}, {0.0, 0.0045}, {0.0, 0.006},
               {0.0, 0.0075}, {0.0, 0.009}, {0.0105, 0.0}});
  ASSERT_EQ(deep.size(), 1u);
  EXPECT_EQ(deep.front().reason, "contact_penetration_exceeded");
}

}  // namespace
}  // namespace manyforge::core
