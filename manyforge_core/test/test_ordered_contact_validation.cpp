#include <gtest/gtest.h>

#include <manyforge_core/collision/ordered_contact_validation.hpp>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

CollisionPairId test_pair() {
  return {"robot/gripper", "object/part"};
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

}  // namespace
}  // namespace manyforge::core
