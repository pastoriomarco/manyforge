// Unit tests for the CollisionPolicyEvaluator: free-space default,
// static-static filtering, the contact state machine of tracked contact-rule
// pairs (start / continued / separated / recontact violation), touch-link
// allowances, contact-rule kinds, and the diagnostic-incomplete fail-closed
// rule.

#include <manyforge_core/collision/collision_policy_evaluator.hpp>

#include "collision_rule_test_helpers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

TEST(CollisionPolicyEvaluator, TwoStaticBodiesAreNeverChecked) {
  auto snapshot = make_free_space_policy();
  const std::string base =
      "component/ur10e/model/ur10e_model/body/base/collision/0";
  const std::string fixture_a = "scene/fixture_a/shape/0";
  const std::string fixture_b = "scene/fixture_b/shape/0";
  snapshot.static_body_ids = {base, fixture_a, fixture_b};
  CollisionPolicyEvaluator ev(std::move(snapshot));

  // Fixed x fixed and static component body x fixed: one rule by class.
  auto out = ev.classify({pair(fixture_a, fixture_b), pair(base, fixture_b)},
                         rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 2u);
  for (const auto& entry : out.pairs) {
    EXPECT_EQ(entry.classification,
              CollisionPairClassification::kAllowedStaticStatic);
  }
  EXPECT_TRUE(out.decision.accepted);
}

TEST(CollisionPolicyEvaluator, StaticBodyAgainstAMovingBodyIsStillChecked) {
  auto snapshot = make_free_space_policy();
  const std::string fixture_a = "scene/fixture_a/shape/0";
  const std::string fixture_b = "scene/fixture_b/shape/0";
  snapshot.static_body_ids = {fixture_a, fixture_b};
  const auto allowed = pair(fixture_a, fixture_b);
  const auto moving = pair(
      "component/ur10e/model/ur10e_model/body/wrist/collision/0", fixture_a);
  // A carried object is never static, whatever it touches.
  const auto carried = pair("scene/cheese_box/shape/0", fixture_b);
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify({allowed, moving, carried}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 3u);
  const auto* allowed_entry = find_pair(out.pairs, allowed);
  const auto* moving_entry = find_pair(out.pairs, moving);
  const auto* carried_entry = find_pair(out.pairs, carried);
  ASSERT_NE(allowed_entry, nullptr);
  ASSERT_NE(moving_entry, nullptr);
  ASSERT_NE(carried_entry, nullptr);
  EXPECT_EQ(allowed_entry->classification,
            CollisionPairClassification::kAllowedStaticStatic);
  EXPECT_EQ(moving_entry->classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_EQ(carried_entry->classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_FALSE(out.decision.accepted);
}

TEST(CollisionPolicyEvaluator, ManyFixedObjectsNeedNoPairList) {
  // 200 cell-fixed objects: 200 static bodies, not 19900 pairs.
  auto snapshot = make_free_space_policy();
  for (int index = 0; index < 200; ++index) {
    snapshot.static_body_ids.push_back(
        "scene/fixed_" + std::to_string(1000 + index).substr(1U) + "/shape/0");
  }
  ASSERT_TRUE(std::is_sorted(snapshot.static_body_ids.begin(),
                             snapshot.static_body_ids.end()));
  const auto first = snapshot.static_body_ids.front();
  const auto last = snapshot.static_body_ids.back();
  CollisionPolicyEvaluator ev(std::move(snapshot));

  auto out = ev.classify(
      {pair(first, last),
       pair("component/ur10e/model/ur10e_model/body/wrist/collision/0", last)},
      rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 2u);
  EXPECT_EQ(find_pair(out.pairs, pair(first, last))->classification,
            CollisionPairClassification::kAllowedStaticStatic);
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

TEST(ContactRuleKind, FollowsTheRuleValue) {
  const auto p = pair("component/tool_a/model/gripper/body/finger_left/collision/0",
                      "scene/part/shape/0");
  const auto release = testing::release_rule(p);
  const auto bounded = testing::contact_rule(p, 0.002);
  const auto free_contact = testing::contact_rule(p);
  const auto windowed = testing::contact_rule(p, std::nullopt, 0.8);
  const auto windowed_limited = testing::contact_rule(p, 0.002, 0.8);

  EXPECT_EQ(contact_rule_kind(release), ContactRuleKind::kRelease);
  EXPECT_EQ(contact_rule_kind(bounded), ContactRuleKind::kBoundedContact);
  EXPECT_EQ(contact_rule_kind(free_contact), ContactRuleKind::kFreeContact);
  EXPECT_EQ(contact_rule_kind(windowed), ContactRuleKind::kWindowedContact);
  EXPECT_EQ(contact_rule_kind(windowed_limited), ContactRuleKind::kWindowedContact);

  EXPECT_TRUE(contact_rule_is_tracked(release));
  EXPECT_TRUE(contact_rule_is_tracked(bounded));
  EXPECT_FALSE(contact_rule_is_tracked(free_contact));
  EXPECT_FALSE(contact_rule_is_tracked(windowed));

  // A signed distance is needed only for a depth limit or signed release
  // evidence.
  EXPECT_TRUE(contact_rule_needs_signed_distance(release));
  EXPECT_TRUE(contact_rule_needs_signed_distance(bounded));
  EXPECT_FALSE(contact_rule_needs_signed_distance(free_contact));
  EXPECT_FALSE(contact_rule_needs_signed_distance(windowed));
  EXPECT_TRUE(contact_rule_needs_signed_distance(windowed_limited));
  auto binary = testing::release_rule(p);
  testing::release_of(binary).release_evidence_mode =
      ContactReleaseEvidenceMode::kBinaryContact;
  EXPECT_FALSE(contact_rule_needs_signed_distance(binary));
}

TEST(ContactRuleKind, SelectedDistancePairsAreThePairsThatNeedADepth) {
  const auto finger = pair("component/tool_a/model/gripper/body/finger_left/collision/0",
                           "scene/part/shape/0");
  const auto palm = pair("component/tool_a/model/gripper/body/palm/collision/0",
                         "scene/part/shape/0");
  const auto table = pair("scene/part/shape/0", "scene/table/shape/0");
  const auto wall = pair("scene/part/shape/0", "scene/wall/shape/0");
  auto policy = make_free_space_policy();
  testing::add_contact_rule(policy, testing::contact_rule(finger, 0.002, 0.8));
  testing::add_contact_rule(policy, testing::contact_rule(palm));
  testing::add_contact_rule(policy, testing::release_rule(table));
  auto binary = testing::release_rule(wall);
  testing::release_of(binary) = CollisionReleaseRule{
      ContactReleaseEvidenceMode::kBinaryContact, false, 0.0, 0.0, 0.0, true, false, false};
  testing::add_contact_rule(policy, std::move(binary));

  std::vector<CollisionPairId> expected{finger, table};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(build_selected_collision_distance_pairs(policy), expected);
  EXPECT_EQ(tracked_contact_rule_count(policy), 2u);
}

TEST(CollisionPolicyEvaluator, WholeMoveContactWithoutLimitIsAllowedAtEverySample) {
  const auto p = pair("component/tool_a/model/gripper/body/finger_left/collision/0",
                      "scene/part/shape/0");
  auto policy = make_free_space_policy();
  testing::add_contact_rule(policy, testing::contact_rule(p));
  CollisionPolicyEvaluator evaluator(std::move(policy));

  for (int sample = 0; sample < 3; ++sample) {
    const auto out = evaluator.classify({p}, rev(2, "world-r2"));
    ASSERT_EQ(out.pairs.size(), 1u);
    EXPECT_EQ(out.pairs.front().classification,
              CollisionPairClassification::kAllowedContactRule);
    EXPECT_TRUE(out.decision.accepted);
  }
  // No contact state is tracked for it.
  EXPECT_TRUE(evaluator.support_contact_state().empty());
}

TEST(CollisionPolicyEvaluator, WindowedContactStaysBlockingForTheSurfaceToDecide) {
  const auto p = pair("component/tool_a/model/gripper/body/finger_left/collision/0",
                      "scene/part/shape/0");
  auto policy = make_free_space_policy();
  testing::add_contact_rule(policy, testing::contact_rule(p, 0.002, 0.8));
  CollisionPolicyEvaluator evaluator(std::move(policy));

  const auto out = evaluator.classify({p}, rev(2, "world-r2"));
  ASSERT_EQ(out.pairs.size(), 1u);
  EXPECT_EQ(out.pairs.front().classification,
            CollisionPairClassification::kBlockingCollision);
  EXPECT_FALSE(out.decision.accepted);
}

// --- Contact state machine of tracked rules ---------------------------------

CollisionPolicySnapshot make_release_policy(
    const CollisionPairId& p, bool allow_recontact) {
  auto s = make_free_space_policy();
  s.policy_id = "pick_with_object";
  s.revision.revision_id = 5;
  auto entry = testing::release_rule(p);
  testing::release_of(entry).max_start_penetration_m = 1e-3;
  testing::release_of(entry).require_separation_by_end = true;
  testing::release_of(entry).allow_recontact = allow_recontact;
  testing::add_contact_rule(s, std::move(entry));
  return s;
}

TEST(CollisionPolicyEvaluator, SupportContactStartAllowedThenContinued) {
  const auto p = pair("scene/cheese_box/shape/0",
                      "scene/table/shape/0");
  CollisionPolicyEvaluator ev(
      make_release_policy(p, /*allow_recontact=*/false));

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
      make_release_policy(p, /*allow_recontact=*/false));

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
      make_release_policy(p, /*allow_recontact=*/false));

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
      make_release_policy(p, /*allow_recontact=*/true));

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
      make_release_policy(p, /*allow_recontact=*/false));

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
  auto policy = testing::release_rule(pair("scene/part/shape/0", "scene/table/shape/0"));
  testing::release_of(policy).release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  testing::release_of(policy).enforce_max_start_penetration = false;
  testing::release_of(policy).release_tolerance_m = 0.0;
  testing::release_of(policy).min_release_progress_m = 0.0;
  testing::release_of(policy).require_strictly_decreasing_penetration = false;

  ContactReleaseEvaluationState state;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.started_in_contact);
  EXPECT_TRUE(state.in_contact);
  EXPECT_FALSE(state.initial_depth_m.has_value());

  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.in_contact);
  EXPECT_FALSE(state.initial_depth_m.has_value());

  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.separated);
  EXPECT_FALSE(state.in_contact);
}

TEST(ContactReleaseTransition, BinaryContactRejectsNewPairAndRecontact) {
  auto policy = testing::release_rule();
  testing::release_of(policy).release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  testing::release_of(policy).enforce_max_start_penetration = false;
  testing::release_of(policy).release_tolerance_m = 0.0;
  testing::release_of(policy).min_release_progress_m = 0.0;
  testing::release_of(policy).require_strictly_decreasing_penetration = false;

  ContactReleaseEvaluationState initially_clear;
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  initially_clear)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                initially_clear)
                .failure,
            ContactReleaseTransitionFailure::kNewContactAfterClearStart);

  ContactReleaseEvaluationState released;
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  released)
                  .accepted());
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  released)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                released)
                .failure,
            ContactReleaseTransitionFailure::kRecontactAfterRelease);
}

TEST(ContactReleaseTransition,
     SignedPenetrationNeedsMetricOnlyWhilePairIsColliding) {
  auto policy = testing::release_rule();
  testing::release_of(policy).max_start_penetration_m = 0.1;

  ContactReleaseEvaluationState state;
  ASSERT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = -0.04, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_DOUBLE_EQ(*state.initial_depth_m, 0.04);

  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.separated);
  EXPECT_DOUBLE_EQ(state.best_depth_m, 0.0);

  ContactReleaseEvaluationState missing;
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                missing)
                .failure,
            ContactReleaseTransitionFailure::
                kMissingOrInvalidSignedPenetration);
}

TEST(ContactReleaseTransition,
     SignedPenetrationUsesCollisionPresenceAndRejectsContradictoryMetric) {
  auto policy = testing::release_rule();
  testing::release_of(policy).max_start_penetration_m = 0.1;

  ContactReleaseEvaluationState touching;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = 0.0, .path_steps = std::nullopt},
                  touching)
                  .accepted());
  EXPECT_TRUE(touching.started_in_contact);
  EXPECT_TRUE(touching.in_contact);

  ContactReleaseEvaluationState contradictory;
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = 0.001, .path_steps = std::nullopt},
                contradictory)
                .failure,
            ContactReleaseTransitionFailure::
                kMissingOrInvalidSignedPenetration);
}

TEST(ContactReleaseTransition,
     BoundedPenetrationAllowsAppearanceRecontactAndFinalContact) {
  auto policy = testing::contact_rule({}, 0.005);

  ContactReleaseEvaluationState state;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = -0.004, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy,
                  {.in_contact = true, .signed_clearance_m = -0.003, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(state.in_contact);
  EXPECT_FALSE(state.initial_depth_m.has_value());
}

TEST(ContactReleaseTransition,
     BoundedPenetrationRejectsInvalidTypedThreshold) {
  auto policy = testing::contact_rule({}, -0.001);
  ContactReleaseEvaluationState state;

  const auto result = advance_contact_release_sample(
      policy, {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt}, state);

  EXPECT_EQ(result.failure,
            ContactReleaseTransitionFailure::kInvalidPolicy);
  EXPECT_FALSE(state.initialized);
}

TEST(ContactReleaseTransition, InvalidRuleValuesAreRefusedBeforeStateMutation) {
  // A release with binary evidence admits only the default or the neutral
  // signed-only fields.
  auto binary = testing::release_rule();
  testing::release_of(binary).release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  testing::release_of(binary).enforce_max_start_penetration = false;
  testing::release_of(binary).release_tolerance_m = 0.0;
  testing::release_of(binary).require_strictly_decreasing_penetration = false;
  testing::release_of(binary).min_release_progress_m = 0.001;
  ContactReleaseEvaluationState binary_state;
  EXPECT_FALSE(contact_rule_valid(binary));
  EXPECT_EQ(advance_contact_release_sample(
                binary,
                {.in_contact = false, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                binary_state)
                .failure,
            ContactReleaseTransitionFailure::kInvalidPolicy);
  EXPECT_FALSE(binary_state.initialized);

  auto negative_release = testing::release_rule();
  testing::release_of(negative_release).release_tolerance_m = -0.001;
  EXPECT_FALSE(contact_rule_valid(negative_release));

  EXPECT_FALSE(contact_rule_valid(testing::contact_rule({}, -0.001)));
  EXPECT_FALSE(contact_rule_valid(testing::contact_rule({}, 0.001, 1.5)));
  EXPECT_FALSE(contact_rule_valid(testing::contact_rule({}, std::nullopt, -0.1)));
  EXPECT_TRUE(contact_rule_valid(testing::contact_rule()));
  EXPECT_TRUE(contact_rule_valid(testing::contact_rule({}, 0.0, 0.0)));
  EXPECT_TRUE(contact_rule_valid(testing::release_rule()));
}

TEST(ContactReleaseTransition, UntrackedContactRulesAreNotStateMachineInput) {
  // Whole-move contact without a limit and windowed contact have no tracked
  // state; the state machine refuses them without changing state.
  for (const auto& rule : {testing::contact_rule(), testing::contact_rule({}, 0.002, 0.8)}) {
    ContactReleaseEvaluationState state;
    EXPECT_EQ(advance_contact_release_sample(
                  rule, {.in_contact = true, .signed_clearance_m = -0.001, .path_steps = std::nullopt}, state)
                  .failure,
              ContactReleaseTransitionFailure::kInvalidPolicy);
    EXPECT_FALSE(state.initialized);
  }
}

TEST(ContactReleaseTransition,
     BoundedPenetrationRequiresMetricAndEnforcesEveryContactSample) {
  auto policy = testing::contact_rule({}, 0.005);

  ContactReleaseEvaluationState missing;
  EXPECT_EQ(advance_contact_release_sample(
                policy,
                {.in_contact = true, .signed_clearance_m = std::nullopt, .path_steps = std::nullopt},
                missing)
                .failure,
            ContactReleaseTransitionFailure::
                kMissingOrInvalidSignedPenetration);

  ContactReleaseEvaluationState over_bound;
  const auto result = advance_contact_release_sample(
      policy, {.in_contact = true, .signed_clearance_m = -0.006, .path_steps = std::nullopt},
      over_bound);
  EXPECT_EQ(result.failure,
            ContactReleaseTransitionFailure::
                kContactPenetrationExceeded);
  ASSERT_TRUE(result.penetration_depth_m);
  EXPECT_DOUBLE_EQ(*result.penetration_depth_m, 0.006);
}

TEST(ContactReleaseTransition, GroupedSpringPairKeepsItsDepthLimitButNotItsOwnStep) {
  // OD-371: a pair of a grouped spring is not a spring on its own; the rule's
  // step is checked over all its pairs (spring_group_step_violations).
  auto policy = testing::contact_rule({}, 0.010);
  testing::contact_of(policy).max_compression_step_m = 0.002;
  testing::contact_of(policy).spring_group =
      CollisionSpringGroup{ControllableComponentId("holder"), "holder_rod", {0.0, 0.0, 1.0}};
  ContactReleaseEvaluationState state;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = false, .signed_clearance_m = 0.002, .path_steps = std::nullopt},
                  state)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = true, .signed_clearance_m = -0.003, .path_steps = 1.0},
                  state)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy, {.in_contact = true, .signed_clearance_m = -0.011, .path_steps = 1.0},
                state)
                .failure,
            ContactReleaseTransitionFailure::kContactPenetrationExceeded);
}

TEST(ContactReleaseTransition, SpringRefusesACompressionJumpButAllowsSmoothPressing) {
  // OD-356: a spring of 10 mm stroke whose depth may grow 2 mm per step.
  auto policy = testing::contact_rule({}, 0.010);
  testing::contact_of(policy).max_compression_step_m = 0.002;

  ContactReleaseEvaluationState smooth;
  for (const double depth : {0.0, 0.0015, 0.003, 0.0045, 0.006, 0.006, 0.004}) {
    EXPECT_TRUE(advance_contact_release_sample(
                    policy,
                    {.in_contact = depth > 0.0,
                     .signed_clearance_m = depth > 0.0 ? std::optional<double>(-depth)
                                                       : std::optional<double>(0.01),
                     .path_steps = 1.0},
                    smooth)
                    .accepted());
  }

  ContactReleaseEvaluationState step;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = true, .signed_clearance_m = -0.001, .path_steps = 1.0},
                  step)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy, {.in_contact = true, .signed_clearance_m = -0.0035, .path_steps = 1.0},
                step)
                .failure,
            ContactReleaseTransitionFailure::kContactCompressionStepExceeded);

  // First touch of a step face: from clear to 3 mm deep in one sample.
  ContactReleaseEvaluationState face;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = false, .signed_clearance_m = 0.002, .path_steps = std::nullopt}, face)
                  .accepted());
  EXPECT_EQ(advance_contact_release_sample(
                policy, {.in_contact = true, .signed_clearance_m = -0.003, .path_steps = 1.0},
                face)
                .failure,
            ContactReleaseTransitionFailure::kContactCompressionStepExceeded);

  // Two steps apart the limit doubles; an unknown spacing is not checked.
  ContactReleaseEvaluationState spaced;
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = true, .signed_clearance_m = -0.001, .path_steps = std::nullopt}, spaced)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = true, .signed_clearance_m = -0.0045, .path_steps = 2.0},
                  spaced)
                  .accepted());
  EXPECT_TRUE(advance_contact_release_sample(
                  policy, {.in_contact = true, .signed_clearance_m = -0.0095, .path_steps = std::nullopt}, spaced)
                  .accepted());
}

TEST(ContactReleaseTransition, SpringStepLimitNeedsTheStroke) {
  auto policy = testing::contact_rule({}, std::nullopt);
  testing::contact_of(policy).max_compression_step_m = 0.002;
  EXPECT_FALSE(contact_rule_valid(policy));
  testing::contact_of(policy).max_penetration_m = 0.01;
  EXPECT_TRUE(contact_rule_valid(policy));
  testing::contact_of(policy).max_compression_step_m = 0.0;
  EXPECT_FALSE(contact_rule_valid(policy));
}

TEST(CollisionPolicyEvaluator,
     BoundedPenetrationRecontactIsAllowedIndependentOfReleaseFlag) {
  const auto p = pair(
      "component/arm/model/arm_model/body/tool/collision/0",
      "scene/part/shape/0");
  auto snapshot = make_release_policy(p, /*allow_recontact=*/false);
  snapshot.contact_rules.front().value = CollisionContactRule{0.005, 0.0, std::nullopt};
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
      make_release_policy(p, /*allow_recontact=*/false));
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

// --- Collision clearance: the margin list names its rules -----------------

TEST(CollisionClearanceMargins, PairLookupGivesMarginAndRuleId) {
  const auto wrist_wall = pair("component/arm/model/arm_model/body/wrist/collision/0",
                               "scene/wall/shape/0");
  const auto wrist_tray = pair("component/arm/model/arm_model/body/wrist/collision/0",
                               "scene/tray/shape/0");
  const auto unlisted = pair("component/arm/model/arm_model/body/base/collision/0",
                             "scene/wall/shape/0");
  CollisionClearanceMargins margins;
  margins.rule_ids = {"everything", "wrist_near_tray"};
  margins.pairs = {CollisionPairMargin{wrist_tray, 0.005, 1U},
                   CollisionPairMargin{wrist_wall, 0.001, 0U}};
  ASSERT_TRUE(std::is_sorted(
      margins.pairs.begin(), margins.pairs.end(),
      [](const CollisionPairMargin& a, const CollisionPairMargin& b) { return a.pair < b.pair; }));

  EXPECT_DOUBLE_EQ(margins.margin_for(wrist_tray), 0.005);
  EXPECT_EQ(margins.rule_id_for(wrist_tray), "wrist_near_tray");
  EXPECT_DOUBLE_EQ(margins.margin_for(wrist_wall), 0.001);
  EXPECT_EQ(margins.rule_id_for(wrist_wall), "everything");
  // A pair that is not listed keeps margin 0 and names no rule.
  EXPECT_DOUBLE_EQ(margins.margin_for(unlisted), 0.0);
  EXPECT_TRUE(margins.rule_id_for(unlisted).empty());
  EXPECT_EQ(margins.find(unlisted), nullptr);
}

}  // namespace
}  // namespace manyforge::core
