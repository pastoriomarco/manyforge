// Unit tests for the Chunk 3 CollisionPolicyEvaluator: free-space default,
// static-static filtering, support-contact state machine (start / continued
// / separated / recontact violation), touch-link allowances, and the
// diagnostic-incomplete fail-closed rule.

#include <manyforge_core/collision/collision_policy_evaluator.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace manyforge::core {
namespace {

CollisionPairId pair(const std::string& a, const std::string& b) {
  CollisionPairId p{a, b};
  if (p.body_b < p.body_a) std::swap(p.body_a, p.body_b);
  return p;
}

CollisionWorldRevision rev(std::uint64_t id, const std::string& hash) {
  CollisionWorldRevision r;
  r.revision_id = id;
  r.content_hash = hash;
  return r;
}

CollisionPolicySnapshot make_free_space_policy() {
  CollisionPolicySnapshot s;
  s.policy_id = "free_space";
  s.revision.revision_id = 1;
  s.revision.content_hash = "policy-free-space-v1";
  return s;
}

const CollisionPairResult* find_pair(const std::vector<CollisionPairResult>& v,
                                     const CollisionPairId& target) {
  for (const auto& entry : v) {
    if (entry.pair == target) return &entry;
  }
  return nullptr;
}

// --- Free-space default ----------------------------------------------------

TEST(CollisionPolicyEvaluator, FreeSpacePairBlocks) {
  CollisionPolicyEvaluator ev(make_free_space_policy());
  const auto p = pair(
      "component/ur10e/model/ur10e_model/body/wrist/collision/0",
      "scene/box_a/shape/0");
  auto out = ev.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 1u);
  EXPECT_EQ(out.pairs[0].classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_TRUE(out.pairs[0].is_first_failure);
  EXPECT_FALSE(out.decision.accepted);
  EXPECT_EQ(out.decision.action, CollisionDecisionAction::kReject);
  EXPECT_EQ(out.decision.severity, CollisionDecisionSeverity::kBlock);
}

TEST(CollisionPolicyEvaluator, NoObservedPairsAccepts) {
  CollisionPolicyEvaluator ev(make_free_space_policy());
  auto out = ev.classify({}, rev(2, "world-r2"));
  EXPECT_TRUE(out.pairs.empty());
  EXPECT_TRUE(out.decision.accepted);
  EXPECT_EQ(out.decision.action, CollisionDecisionAction::kDispatch);
}

// --- Static-static filtering -----------------------------------------------

TEST(CollisionPolicyEvaluator, StaticStaticAllowedWhenListed) {
  auto snapshot = make_free_space_policy();
  const auto p = pair("scene/fixture_a/shape/0",
                      "scene/fixture_b/shape/0");
  snapshot.allowed_static_static_pairs.push_back(p);
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 1u);
  EXPECT_EQ(out.pairs[0].classification,
            CollisionPairClassification::kAllowedStaticStatic);
  EXPECT_TRUE(out.decision.accepted);
}

TEST(CollisionPolicyEvaluator,
     StaticStaticAllowanceDoesNotMaskUnrelatedCollisions) {
  auto snapshot = make_free_space_policy();
  const auto allowed = pair("scene/fixture_a/shape/0",
                            "scene/fixture_b/shape/0");
  const auto unrelated = pair(
      "component/ur10e/model/ur10e_model/body/wrist/collision/0",
      "scene/cheese_box/shape/0");
  snapshot.allowed_static_static_pairs.push_back(allowed);
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({allowed, unrelated}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 2u);
  const auto* allowed_entry = find_pair(out.pairs, allowed);
  const auto* unrelated_entry = find_pair(out.pairs, unrelated);
  ASSERT_NE(allowed_entry, nullptr);
  ASSERT_NE(unrelated_entry, nullptr);
  EXPECT_EQ(allowed_entry->classification,
            CollisionPairClassification::kAllowedStaticStatic);
  EXPECT_EQ(unrelated_entry->classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_FALSE(out.decision.accepted);
}

// --- Touch-link allowances -------------------------------------------------

TEST(CollisionPolicyEvaluator,
     TouchLinkAllowedPairClassifiedAsAllowedTouchLink) {
  auto snapshot = make_free_space_policy();
  const auto p = pair(
      "component/ur10e/model/ur10e_model/body/wrist/collision/0",
      "scene/cheese_box/shape/0");
  snapshot.allowed_touch_link_pairs.push_back(p);
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 1u);
  EXPECT_EQ(out.pairs[0].classification,
            CollisionPairClassification::kAllowedTouchLink);
  EXPECT_TRUE(out.decision.accepted);
}

TEST(CollisionPolicyEvaluator,
     TouchLinkAllowanceDoesNotMaskOtherCollisions) {
  auto snapshot = make_free_space_policy();
  const auto allowed = pair(
      "component/ur10e/model/ur10e_model/body/wrist/collision/0",
      "scene/cheese_box/shape/0");
  const auto blocking = pair("scene/cheese_box/shape/0",
                             "scene/table_top/shape/0");
  snapshot.allowed_touch_link_pairs.push_back(allowed);
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({allowed, blocking}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 2u);
  const auto* a = find_pair(out.pairs, allowed);
  const auto* b = find_pair(out.pairs, blocking);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->classification,
            CollisionPairClassification::kAllowedTouchLink);
  EXPECT_EQ(b->classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_FALSE(out.decision.accepted);
}

TEST(ContactGraspPolicy,
     ExactOwnerResolvedBodyDoesNotMatchSameRoleOnAnotherComponent) {
  ContactGraspPolicyEntry policy;
  policy.policy_id = "contact_grasp.bound.test";
  policy.selected_object_id = "part";
  policy.allowed_robot_links = {
      "component/tool_a/model/gripper/body/finger_left/collision/0"};
  policy.max_penetration_m = 0.002;
  policy.terminal_window_start_fraction = 0.75;

  EXPECT_TRUE(contact_grasp_pair_matches(
      pair("component/tool_a/model/gripper/body/finger_left/collision/0",
           "scene/part/shape/0"),
      policy));
  EXPECT_FALSE(contact_grasp_pair_matches(
      pair("component/tool_b/model/gripper/body/finger_left/collision/0",
           "scene/part/shape/0"),
      policy));
}

// --- Support-contact state machine -----------------------------------------

CollisionPolicySnapshot make_support_contact_policy(
    const CollisionPairId& p, bool allow_recontact) {
  auto s = make_free_space_policy();
  s.policy_id = "pick_with_object";
  s.revision.revision_id = 5;
  SupportContactPolicyEntry entry;
  entry.pair = p;
  entry.max_start_penetration_m = 1e-3;
  entry.require_separation_by_end = true;
  entry.allow_recontact = allow_recontact;
  s.support_contacts.push_back(std::move(entry));
  return s;
}

TEST(CollisionPolicyEvaluator, SupportContactStartAllowedThenContinued) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  CollisionPolicyEvaluator ev(
      make_support_contact_policy(p, /*allow_recontact=*/false));

  auto first = ev.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(first.pairs.size(), 1u);
  EXPECT_EQ(first.pairs[0].classification,
            CollisionPairClassification::kSupportContactStart);
  EXPECT_TRUE(first.decision.accepted);

  auto second = ev.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(second.pairs.size(), 1u);
  EXPECT_EQ(second.pairs[0].classification,
            CollisionPairClassification::kSupportContactContinued);
  EXPECT_TRUE(second.decision.accepted);
}

TEST(CollisionPolicyEvaluator, SupportContactSeparationEmitsSeparatedEntry) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  CollisionPolicyEvaluator ev(
      make_support_contact_policy(p, /*allow_recontact=*/false));

  ev.classify({p}, rev(2, "world-r2"));         // start
  ev.classify({p}, rev(2, "world-r2"));         // continued
  auto separated = ev.classify({}, rev(2, "world-r2"));  // no contact

  ASSERT_EQ(separated.pairs.size(), 1u);
  EXPECT_EQ(separated.pairs[0].classification,
            CollisionPairClassification::kSupportContactSeparated);
  EXPECT_TRUE(separated.decision.accepted);
}

TEST(CollisionPolicyEvaluator,
     SupportContactRecontactBlocksWhenRecontactDisallowed) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  CollisionPolicyEvaluator ev(
      make_support_contact_policy(p, /*allow_recontact=*/false));

  ev.classify({p}, rev(2, "world-r2"));         // start
  ev.classify({}, rev(2, "world-r2"));          // separated
  auto recontact = ev.classify({p}, rev(2, "world-r2"));  // re-contact

  ASSERT_EQ(recontact.pairs.size(), 1u);
  EXPECT_EQ(recontact.pairs[0].classification,
            CollisionPairClassification::kSupportContactRecontactViolation);
  EXPECT_FALSE(recontact.decision.accepted);
  EXPECT_EQ(recontact.decision.action, CollisionDecisionAction::kReject);
}

TEST(CollisionPolicyEvaluator, SupportContactRecontactAllowedWhenPolicySays) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  CollisionPolicyEvaluator ev(
      make_support_contact_policy(p, /*allow_recontact=*/true));

  ev.classify({p}, rev(2, "world-r2"));         // start
  ev.classify({}, rev(2, "world-r2"));          // separated
  auto recontact = ev.classify({p}, rev(2, "world-r2"));  // re-contact OK

  ASSERT_EQ(recontact.pairs.size(), 1u);
  EXPECT_EQ(recontact.pairs[0].classification,
            CollisionPairClassification::kSupportContactContinued);
  EXPECT_TRUE(recontact.decision.accepted);
}

TEST(CollisionPolicyEvaluator, ResetClearsSupportContactStateMachine) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  CollisionPolicyEvaluator ev(
      make_support_contact_policy(p, /*allow_recontact=*/false));

  ev.classify({p}, rev(2, "world-r2"));         // start
  ev.classify({}, rev(2, "world-r2"));          // separated

  ev.reset();
  // After reset, contact appearing again is a fresh start (not recontact).
  auto after_reset = ev.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(after_reset.pairs.size(), 1u);
  EXPECT_EQ(after_reset.pairs[0].classification,
            CollisionPairClassification::kSupportContactStart);
  EXPECT_TRUE(after_reset.decision.accepted);
}

TEST(ContactReleaseTransition,
     BinaryContactUsesPresenceOnlyAndAllowsContactPlateaus) {
  SupportContactPolicyEntry policy;
  policy.pair = pair("scene/part/shape/0", "scene/table/shape/0");
  policy.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  policy.enforce_max_start_penetration = false;
  policy.release_tolerance_m = 0.0;
  policy.min_release_progress_m = 0.0;
  policy.require_strictly_decreasing_penetration = false;

  ContactReleaseEvaluationState state;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.started_in_contact);
  EXPECT_TRUE(state.in_contact);
  EXPECT_FALSE(state.initial_depth_m.has_value());

  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.in_contact);
  EXPECT_FALSE(state.initial_depth_m.has_value());

  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.separated);
  EXPECT_FALSE(state.in_contact);
}

TEST(ContactReleaseTransition, BinaryContactRejectsNewPairAndRecontact) {
  SupportContactPolicyEntry policy;
  policy.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  policy.enforce_max_start_penetration = false;
  policy.release_tolerance_m = 0.0;
  policy.min_release_progress_m = 0.0;
  policy.require_strictly_decreasing_penetration = false;

  ContactReleaseEvaluationState initially_clear;
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt},
                  initially_clear)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt},
                initially_clear)
                .failure,
            ContactReleaseTransitionFailure::kNewContactAfterClearStart);

  ContactReleaseEvaluationState released;
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = std::nullopt},
                  released)
                  .accepted());
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt},
                  released)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt},
                released)
                .failure,
            ContactReleaseTransitionFailure::kRecontactAfterRelease);
}

TEST(ContactReleaseTransition,
     SignedPenetrationNeedsMetricOnlyWhilePairIsColliding) {
  SupportContactPolicyEntry policy;
  policy.max_start_penetration_m = 0.1;

  ContactReleaseEvaluationState state;
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = -0.04},
                  state)
                  .accepted());
  EXPECT_DOUBLE_EQ(*state.initial_depth_m, 0.04);

  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.separated);
  EXPECT_DOUBLE_EQ(state.best_depth_m, 0.0);

  ContactReleaseEvaluationState missing;
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt},
                missing)
                .failure,
            ContactReleaseTransitionFailure::
                kMissingOrInvalidSignedPenetration);
}

TEST(ContactReleaseTransition,
     SignedPenetrationUsesCollisionPresenceAndRejectsContradictoryMetric) {
  SupportContactPolicyEntry policy;
  policy.max_start_penetration_m = 0.1;

  ContactReleaseEvaluationState touching;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = 0.0},
                  touching)
                  .accepted());
  EXPECT_TRUE(touching.started_in_contact);
  EXPECT_TRUE(touching.in_contact);

  ContactReleaseEvaluationState contradictory;
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = 0.001},
                contradictory)
                .failure,
            ContactReleaseTransitionFailure::
                kMissingOrInvalidSignedPenetration);
}

TEST(ContactReleaseTransition,
     BoundedPenetrationAllowsAppearanceRecontactAndFinalContact) {
  SupportContactPolicyEntry policy;
  policy.mode = CollisionContactPolicyMode::kBoundedPenetration;
  policy.max_contact_penetration_m = 0.005;

  ContactReleaseEvaluationState state;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = -0.004},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = -0.003},
                  state)
                  .accepted());
  EXPECT_TRUE(state.in_contact);
  EXPECT_FALSE(state.initial_depth_m.has_value());
}

TEST(ContactReleaseTransition,
     BoundedPenetrationRejectsInvalidTypedThreshold) {
  SupportContactPolicyEntry policy;
  policy.mode = CollisionContactPolicyMode::kBoundedPenetration;
  policy.max_contact_penetration_m = -0.001;
  ContactReleaseEvaluationState state;

  const auto result = advance_contact_release_sample(
      policy, {.in_contact = false, .signed_clearance_m = std::nullopt}, state);

  EXPECT_EQ(result.failure,
            ContactReleaseTransitionFailure::kInvalidPolicy);
  EXPECT_FALSE(state.initialized);
}

TEST(ContactReleaseTransition,
     SharedHelperRejectsModeScopedFieldMixturesBeforeStateMutation) {
  SupportContactPolicyEntry bounded;
  bounded.mode = CollisionContactPolicyMode::kBoundedPenetration;
  bounded.max_contact_penetration_m = 0.005;
  bounded.release_tolerance_m = 0.002;
  ContactReleaseEvaluationState bounded_state;
  EXPECT_FALSE(support_contact_policy_entry_valid(bounded));
  EXPECT_EQ(advance_contact_release_sample(
                bounded,
                {.in_contact = true, .signed_clearance_m = -0.001},
                bounded_state)
                .failure,
            ContactReleaseTransitionFailure::kInvalidPolicy);
  EXPECT_FALSE(bounded_state.initialized);

  SupportContactPolicyEntry signed_release;
  signed_release.max_contact_penetration_m = 0.001;
  ContactReleaseEvaluationState release_state;
  EXPECT_FALSE(support_contact_policy_entry_valid(signed_release));
  EXPECT_EQ(advance_contact_release_sample(
                signed_release,
                {.in_contact = false, .signed_clearance_m = std::nullopt},
                release_state)
                .failure,
            ContactReleaseTransitionFailure::kInvalidPolicy);
  EXPECT_FALSE(release_state.initialized);

  SupportContactPolicyEntry binary;
  binary.release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  binary.enforce_max_start_penetration = false;
  binary.release_tolerance_m = 0.0;
  binary.require_strictly_decreasing_penetration = false;
  binary.min_release_progress_m = 0.001;
  ContactReleaseEvaluationState binary_state;
  EXPECT_FALSE(support_contact_policy_entry_valid(binary));
  EXPECT_EQ(advance_contact_release_sample(
                binary,
                {.in_contact = false, .signed_clearance_m = std::nullopt},
                binary_state)
                .failure,
            ContactReleaseTransitionFailure::kInvalidPolicy);
  EXPECT_FALSE(binary_state.initialized);
}

TEST(ContactReleaseTransition,
     BoundedPenetrationRequiresMetricAndEnforcesEveryContactSample) {
  SupportContactPolicyEntry policy;
  policy.mode = CollisionContactPolicyMode::kBoundedPenetration;
  policy.max_contact_penetration_m = 0.005;

  ContactReleaseEvaluationState missing;
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt},
                missing)
                .failure,
            ContactReleaseTransitionFailure::
                kMissingOrInvalidSignedPenetration);

  ContactReleaseEvaluationState over_bound;
  const auto result = advance_contact_release_sample(
      policy, {.in_contact = true, .signed_clearance_m = -0.006},
      over_bound);
  EXPECT_EQ(result.failure,
            ContactReleaseTransitionFailure::
                kContactPenetrationExceedsPolicy);
  ASSERT_TRUE(result.penetration_depth_m);
  EXPECT_DOUBLE_EQ(*result.penetration_depth_m, 0.006);
}

TEST(CollisionPolicyEvaluator,
     BoundedPenetrationRecontactIsAllowedIndependentOfReleaseFlag) {
  const auto p = pair(
      "component/arm/model/arm_model/body/tool/collision/0",
      "scene/part/shape/0");
  auto snapshot = make_support_contact_policy(p, /*allow_recontact=*/false);
  snapshot.support_contacts.front().mode =
      CollisionContactPolicyMode::kBoundedPenetration;
  snapshot.support_contacts.front().max_contact_penetration_m = 0.005;
  CollisionPolicyEvaluator evaluator(std::move(snapshot));

  EXPECT_TRUE(evaluator.classify({p}, rev(2, "world-r2")).decision.accepted);
  EXPECT_TRUE(evaluator.classify({}, rev(2, "world-r2")).decision.accepted);
  const auto recontact = evaluator.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(recontact.pairs.size(), 1u);
  EXPECT_EQ(recontact.pairs.front().classification,
            CollisionPairClassification::kSupportContactContinued);
  EXPECT_TRUE(recontact.decision.accepted);
}

TEST(CollisionPolicyEvaluator,
     SharedPolicyIndexDoesNotShareSupportContactStateMachine) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  auto snapshot = std::make_shared<CollisionPolicySnapshot>(
      make_support_contact_policy(p, /*allow_recontact=*/false));
  auto index = build_collision_policy_index(*snapshot);
  CollisionPolicyEvaluator first(snapshot, index);
  CollisionPolicyEvaluator second(snapshot, index);

  first.classify({p}, rev(2, "world-r2"));
  first.classify({}, rev(2, "world-r2"));
  auto first_recontact = first.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(first_recontact.pairs.size(), 1u);
  EXPECT_EQ(first_recontact.pairs[0].classification,
            CollisionPairClassification::kSupportContactRecontactViolation);
  EXPECT_FALSE(first_recontact.decision.accepted);

  auto second_first_contact = second.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(second_first_contact.pairs.size(), 1u);
  EXPECT_EQ(second_first_contact.pairs[0].classification,
            CollisionPairClassification::kSupportContactStart);
  EXPECT_TRUE(second_first_contact.decision.accepted);
}

// --- Diagnostic-incomplete fail-closed -------------------------------------

TEST(CollisionPolicyEvaluator,
     DiagnosticIncompleteFailsClosedWhenPolicyRequiresFullDiagnostics) {
  auto snapshot = make_free_space_policy();
  snapshot.require_full_diagnostics = true;
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({pair("a", "b")}, rev(2, "world-r2"),
                         /*diagnostics_complete=*/false);
  ASSERT_EQ(out.pairs.size(), 1u);
  EXPECT_EQ(out.pairs[0].classification,
            CollisionPairClassification::kDiagnosticIncomplete);
  EXPECT_FALSE(out.decision.accepted);
  EXPECT_EQ(out.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(out.decision.severity, CollisionDecisionSeverity::kBlock);
}

TEST(CollisionPolicyEvaluator,
     DiagnosticIncompleteWithNoPairsStillFailsClosed) {
  auto snapshot = make_free_space_policy();
  snapshot.require_full_diagnostics = true;
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({}, rev(2, "world-r2"),
                         /*diagnostics_complete=*/false);
  EXPECT_TRUE(out.pairs.empty());
  EXPECT_FALSE(out.decision.accepted);
  EXPECT_EQ(out.decision.action, CollisionDecisionAction::kInconclusive);
  EXPECT_EQ(out.decision.severity, CollisionDecisionSeverity::kBlock);
}

TEST(CollisionPolicyEvaluator,
     DiagnosticIncompleteDoesNotForceFailWhenPolicyAcceptsDegradedMode) {
  auto snapshot = make_free_space_policy();
  snapshot.require_full_diagnostics = false;
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({pair("a", "b")}, rev(2, "world-r2"),
                         /*diagnostics_complete=*/false);
  ASSERT_EQ(out.pairs.size(), 1u);
  EXPECT_EQ(out.pairs[0].classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_FALSE(out.decision.accepted);
}

// --- Determinism -----------------------------------------------------------

TEST(CollisionPolicyEvaluator, ResultPairsAreSortedDeterministically) {
  CollisionPolicyEvaluator ev(make_free_space_policy());
  auto p1 = pair("z/shape", "a/shape");
  auto p2 = pair("a/shape", "m/shape");
  auto p3 = pair("b/shape", "c/shape");
  auto out = ev.classify({p1, p2, p3}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 3u);
  EXPECT_TRUE(out.pairs[0].pair < out.pairs[1].pair);
  EXPECT_TRUE(out.pairs[1].pair < out.pairs[2].pair);
}

TEST(CollisionPolicyEvaluator,
     FirstFailureMarkerLandsOnLexicographicallyEarliestBlocker) {
  CollisionPolicyEvaluator ev(make_free_space_policy());
  auto early = pair("a/shape", "b/shape");
  auto later = pair("c/shape", "d/shape");
  auto out = ev.classify({later, early}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 2u);
  // The first entry in sorted order should be early; it gets the first-failure marker.
  EXPECT_EQ(out.pairs[0].pair, early);
  EXPECT_TRUE(out.pairs[0].is_first_failure);
  EXPECT_FALSE(out.pairs[1].is_first_failure);
}

}  // namespace
}  // namespace manyforge::core
