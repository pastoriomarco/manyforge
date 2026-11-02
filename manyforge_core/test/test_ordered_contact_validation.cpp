#include <gtest/gtest.h>

#include <manyforge_core/collision/ordered_contact_validation.hpp>
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
  SupportContactPolicyEntry entry;
  entry.pair = test_pair();
  entry.max_start_penetration_m = 0.1;
  entry.release_tolerance_m = 1e-5;
  entry.min_release_progress_m = 0.001;
  policy.support_contacts.push_back(entry);
  return policy;
}

CollisionPolicySnapshot binary_release_policy() {
  auto policy = release_policy();
  auto& entry = policy.support_contacts.front();
  entry.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  entry.enforce_max_start_penetration = false;
  entry.max_start_penetration_m = 0.0;
  entry.release_tolerance_m = 0.0;
  entry.min_release_progress_m = 0.0;
  entry.require_strictly_decreasing_penetration = false;
  return policy;
}

CollisionPolicySnapshot bounded_penetration_policy(
    double max_contact_penetration_m = 0.005) {
  auto policy = release_policy();
  auto& entry = policy.support_contacts.front();
  entry.mode = CollisionContactPolicyMode::kBoundedPenetration;
  entry.max_contact_penetration_m = max_contact_penetration_m;
  entry.enforce_max_start_penetration = false;
  entry.max_start_penetration_m = 0.0;
  entry.release_tolerance_m = 0.0;
  entry.min_release_progress_m = 0.0;
  entry.require_separation_by_end = false;
  entry.require_strictly_decreasing_penetration = false;
  entry.allow_recontact = false;
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
  duplicate_policy.support_contacts.push_back(duplicate_policy.support_contacts.front());
  auto summary = summarize_ordered_contact_chunk(duplicate_policy, {record_at(0, -0.01)}, 0, 1);
  EXPECT_FALSE(summary.valid);
  EXPECT_EQ(summary.invalid_reason, "duplicate_support_contact_identity");

  auto over_bound_policy = release_policy();
  over_bound_policy.support_contacts.clear();
  for (std::size_t i = 0; i <= kMaxOrderedSupportContactIdentities; ++i) {
    SupportContactPolicyEntry entry;
    entry.pair = {"robot_" + std::to_string(i), "object_" + std::to_string(i)};
    over_bound_policy.support_contacts.push_back(std::move(entry));
  }
  summary = summarize_ordered_contact_chunk(over_bound_policy, {record_at(0, -0.01)}, 0, 1);
  EXPECT_FALSE(summary.valid);
  EXPECT_EQ(summary.invalid_reason, "support_contact_identity_bound_exceeded");
}

TEST(OrderedContactValidationTest, SummaryStateRejectsMissingOrUnexpectedContactIdentity) {
  const auto policy = release_policy();
  const auto summary = summarize_ordered_contact_chunk(policy, {record_at(0, -0.01)}, 0, 1);
  SupportContactEvaluationStateMap state;
  std::string error;
  EXPECT_FALSE(advance_support_contact_state(summary, state, &error));
  EXPECT_EQ(error, "support_contact_summary_identity_mismatch");
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
  auto second_entry = policy.support_contacts.front();
  second_entry.pair = second_pair;
  policy.support_contacts.push_back(second_entry);

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
  EXPECT_EQ(violations.front().policy_entry_index, 1u);
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
            "contact_penetration_exceeds_policy");
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

}  // namespace
}  // namespace manyforge::core
