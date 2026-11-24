#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

namespace manyforge::core {

/// Result bundle returned by CollisionPolicyEvaluator::classify().
///
/// `pairs` holds classified observed body-pairs plus any emitted policy-state
/// transition entries, deterministically sorted by (body_a, body_b)
/// lexicographic order; `decision` is the kernel-facing summary derived from
/// those pair classifications.
struct EvaluatedPairs {
  std::vector<CollisionPairResult> pairs;
  CollisionDecision decision;
};

/// How the evaluation surfaces treat one pair of
/// `CollisionPolicySnapshot::contact_rules`.
enum class ContactRuleKind {
  /// `release` rule: the ordered release state machine.
  kRelease,
  /// `contact` for the whole move (`end_from_fraction` 0) with a depth limit:
  /// the pair may touch at any sample; every colliding sample stays within
  /// the limit.
  kBoundedContact,
  /// `contact` for the whole move without a limit: allowed, depth not checked.
  kFreeContact,
  /// `contact` with an end window above 0: allowed only from that
  /// fraction on, never at the first sample; depth checked when a limit
  /// exists.
  kWindowedContact,
};

[[nodiscard]] inline ContactRuleKind contact_rule_kind(
    const CollisionPairContactRule& rule) noexcept {
  const auto* contact = rule.contact();
  if (contact == nullptr) {
    return ContactRuleKind::kRelease;
  }
  if (!contact->whole_move()) {
    return ContactRuleKind::kWindowedContact;
  }
  return contact->max_penetration_m.has_value() ? ContactRuleKind::kBoundedContact
                                                : ContactRuleKind::kFreeContact;
}

/// True for the pairs the ordered contact state machine tracks: release
/// rules and whole-move contact rules with a depth limit.
[[nodiscard]] inline bool contact_rule_is_tracked(
    const CollisionPairContactRule& rule) noexcept {
  const auto kind = contact_rule_kind(rule);
  return kind == ContactRuleKind::kRelease ||
         kind == ContactRuleKind::kBoundedContact;
}

/// True when a colliding sample of the pair needs a signed distance: a depth
/// limit exists, or a release rule uses signed-penetration evidence.
[[nodiscard]] inline bool contact_rule_needs_signed_distance(
    const CollisionPairContactRule& rule) noexcept {
  if (const auto* contact = rule.contact()) {
    return contact->max_penetration_m.has_value();
  }
  return rule.release()->release_evidence_mode ==
         ContactReleaseEvidenceMode::kSignedPenetration;
}

/// Number of pairs of `policy.contact_rules` the state machine tracks.
[[nodiscard]] std::size_t tracked_contact_rule_count(
    const CollisionPolicySnapshot& policy) noexcept;

/// Immutable lookup indexes derived from one CollisionPolicySnapshot. Safe to
/// share across per-pass evaluators; evaluators still own their mutable
/// contact state machines.
struct CollisionPolicyIndex {
  /// Revision of the immutable policy used to build these indexes. Runtime
  /// consumers compare this identity before trusting any allowance lookup.
  CollisionPolicyRevision source_policy_revision;
  /// Every pair under a contact or release rule, by canonical pair.
  std::map<CollisionPairId, CollisionPairContactRule, std::less<>>
      contact_rule_index;
  std::map<CollisionPairId, bool, std::less<>> touch_link_index;
};

/// Trajectory-scoped state for one tracked contact-rule pair. This state is
/// public so the ordered parallel validator can transfer a bounded chunk
/// prefix between independent worker-local evaluators. It is not deployment or
/// scene state and must never survive across validation attempts.
enum class SupportContactEvaluationState {
  kNotInContact,
  kInitialContact,
  kContactActive,
  kSeparated,
  kRecontactViolation,
};

using SupportContactEvaluationStateMap =
    std::map<CollisionPairId, SupportContactEvaluationState, std::less<>>;

/// Exact prefix state required to continue contact-release validation without
/// re-baselining at a runtime-lookahead boundary.
struct ContactReleaseEvaluationState {
  bool initialized = false;
  bool started_in_contact = false;
  /// Contact truth at the most recently accepted sample. This is independent
  /// of signed-distance evidence and is required by both evidence modes.
  bool in_contact = false;
  bool separated = false;
  std::optional<double> initial_depth_m;
  /// Release: depth at the last accepted colliding sample. Whole-move
  /// contact: depth at the last sample, 0 when it was not in contact.
  double previous_depth_m = 0.0;
  double best_depth_m = 0.0;

  friend bool operator==(const ContactReleaseEvaluationState&,
                         const ContactReleaseEvaluationState&) = default;
};

using ContactReleaseEvaluationStateMap =
    std::map<CollisionPairId, ContactReleaseEvaluationState, std::less<>>;

/// Build the canonical signed-distance pair selection of one policy: every
/// contact-rule pair whose rule needs a signed distance (a depth limit, or a
/// release with signed-penetration evidence). Sorted, unique.
[[nodiscard]] std::vector<CollisionPairId>
build_selected_collision_distance_pairs(const CollisionPolicySnapshot& policy);

/// One logical pair observation for the shared contact-release state
/// machine. `signed_clearance_m` is required only when `in_contact` is true and
/// the rule needs a signed distance. Binary release rules ignore it, and
/// absent pairs must not fabricate a zero clearance.
struct ContactReleaseSampleEvidence {
  bool in_contact = false;
  std::optional<double> signed_clearance_m;
  /// Path steps of at most kSpringSampleTravelM tool travel since the
  /// previous sample; a spring's step limit scales with it. Absent (or 0):
  /// the spacing is unknown and the step is not checked.
  std::optional<double> path_steps;
};

/// The step limit of a spring rule (OD-356) between two samples `path_steps`
/// apart; absent when the rule has none or the spacing is unknown.
[[nodiscard]] inline std::optional<double> compression_step_limit_m(
    const CollisionContactRule& contact, const std::optional<double>& path_steps) noexcept {
  if (!contact.max_compression_step_m || !path_steps || !(*path_steps > 0.0)) {
    return std::nullopt;
  }
  return *contact.max_compression_step_m * std::max(1.0, *path_steps);
}

/// The step limit one pair checks on its own depth: a spring rule without a
/// spring group. A grouped rule's step limit applies to the rule's depth over
/// all its pairs (OD-371, `spring_group_step_violations`).
[[nodiscard]] inline std::optional<double> pair_compression_step_limit_m(
    const CollisionContactRule& contact, const std::optional<double>& path_steps) noexcept {
  return contact.spring_group ? std::nullopt : compression_step_limit_m(contact, path_steps);
}

/// Typed failure emitted by the allocation-free per-sample transition helper.
/// Callers translate this stable category into surface-specific decisions and
/// diagnostics without reimplementing the state transition.
enum class ContactReleaseTransitionFailure {
  kNone,
  kInvalidPolicy,
  kMissingOrInvalidSignedPenetration,
  kNewContactAfterClearStart,
  kInitialPenetrationExceedsPolicy,
  kRecontactAfterRelease,
  kRecontactPenetrationExceedsPolicy,
  kPenetrationIncreased,
  kContactPenetrationExceeded,
  /// A spring's penetration grew by more than its step limit (OD-356).
  kContactCompressionStepExceeded,
};

struct ContactReleaseTransitionResult {
  ContactReleaseTransitionFailure failure =
      ContactReleaseTransitionFailure::kNone;
  std::optional<double> penetration_depth_m;

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return failure == ContactReleaseTransitionFailure::kNone;
  }
};

/// Validate one contact-rule pair as consumed by every decision surface: a
/// contact value needs a finite limit >= 0 and a finite fraction in 0..1 where
/// present; a release value needs finite numbers >= 0, and binary-contact
/// evidence admits only the default or the neutral signed-only fields.
[[nodiscard]] bool contact_rule_valid(
    const CollisionPairContactRule& rule) noexcept;

/// Advance one tracked contact-rule pair (`contact_rule_is_tracked`) by one
/// trajectory-ordered sample.
///
/// The first call establishes C_0. Later contact for an initially-clear pair is
/// rejected; absence marks separation; disallowed recontact is rejected. The
/// signed release mode additionally validates and updates penetration evidence
/// while a pair is colliding. A whole-move contact rule with a depth limit
/// permits arbitrary contact appearance/disappearance but validates every
/// colliding depth against its limit. The helper performs no allocation and
/// never infers contact from a distance tolerance.
[[nodiscard]] ContactReleaseTransitionResult advance_contact_release_sample(
    const CollisionPairContactRule& rule,
    const ContactReleaseSampleEvidence& sample,
    ContactReleaseEvaluationState& state) noexcept;

[[nodiscard]] constexpr const char* contact_release_transition_failure_name(
    ContactReleaseTransitionFailure failure) noexcept {
  switch (failure) {
    case ContactReleaseTransitionFailure::kNone:
      return "none";
    case ContactReleaseTransitionFailure::kInvalidPolicy:
      return "invalid_collision_rule";
    case ContactReleaseTransitionFailure::kMissingOrInvalidSignedPenetration:
      return "missing_contact_release_clearance";
    case ContactReleaseTransitionFailure::kNewContactAfterClearStart:
      return "new_active_contact_after_clear_start";
    case ContactReleaseTransitionFailure::kInitialPenetrationExceedsPolicy:
      return "initial_penetration_exceeds_policy";
    case ContactReleaseTransitionFailure::kRecontactAfterRelease:
      return "recontact_after_release";
    case ContactReleaseTransitionFailure::kRecontactPenetrationExceedsPolicy:
      return "recontact_penetration_exceeds_policy";
    case ContactReleaseTransitionFailure::kPenetrationIncreased:
      return "penetration_increased";
    case ContactReleaseTransitionFailure::kContactPenetrationExceeded:
      return "contact_penetration_exceeded";
    case ContactReleaseTransitionFailure::kContactCompressionStepExceeded:
      return "contact_compression_step_exceeded";
  }
  return "unknown_contact_release_failure";
}

std::shared_ptr<const CollisionPolicyIndex> build_collision_policy_index(
    const CollisionPolicySnapshot& snapshot);

/// CollisionPolicyEvaluator.
///
/// Takes the immutable CollisionPolicySnapshot and a list of normalized,
/// observed body pairs, and produces:
///
///   - per-pair CollisionPairClassification values, deterministically
///     ordered by (body_a, body_b) lexicographic order;
///   - the overall CollisionDecision the kernel uses to gate dispatch.
///
/// The evaluator maintains the contact state machine of the tracked
/// contact-rule pairs across repeated classify() calls within one validation
/// pass. Callers MUST invoke reset() between independent passes (e.g. between
/// moves) so stale contact state cannot leak across trajectories.
///
/// A pair under a whole-move contact rule without a depth limit classifies as
/// kAllowedContactRule. A pair under a contact rule with an end window
/// classifies as kBlockingCollision here; the validation coordinator and the
/// runtime surfaces apply its window and depth and relabel an allowed sample
/// kAllowedContactRule.
///
/// Determinism: classify() is deterministic for fixed inputs; observation
/// order across pairs does not affect the result. Two-input scenarios
/// that exercise the contact state machine (e.g. "first call:
/// contact, second call: separated, third call: contact again") rely on
/// the call ordering itself, which is the trajectory-order semantics
/// the plan defines.
class CollisionPolicyEvaluator {
 public:
  explicit CollisionPolicyEvaluator(CollisionPolicySnapshot snapshot);
  explicit CollisionPolicyEvaluator(
      std::shared_ptr<const CollisionPolicySnapshot> snapshot,
      std::shared_ptr<const CollisionPolicyIndex> index = nullptr);

  /// Clear contact state machine entries. Call between independent
  /// validation passes.
  void reset();

  /// Replace the complete trajectory-scoped contact state. The input must
  /// contain exactly the canonical tracked contact-rule pairs of the
  /// immutable policy index. Returns false without changing state when it does
  /// not match; callers must fail closed rather than guessing a prefix.
  bool restore_support_contact_state(const SupportContactEvaluationStateMap& state);

  const SupportContactEvaluationStateMap& support_contact_state() const {
    return support_contact_state_;
  }

  /// Classify the observed pairs.
  ///
  /// @param observed_pairs Pairs reported in collision by the engine.
  /// @param world_revision Active CollisionWorldRevision under which
  ///   `observed_pairs` were produced. A pair of two static bodies of the
  ///   snapshot (`static_body_ids`) classifies kAllowedStaticStatic.
  /// @param diagnostics_complete Whether the engine supplied every
  ///   diagnostic the policy requires (pair identity, depth, etc.).
  ///   When false and `require_full_diagnostics` is true, every pair
  ///   classifies as kDiagnosticIncomplete.
  /// @return Per-pair classifications + overall CollisionDecision.
  EvaluatedPairs classify(
      const std::vector<CollisionPairId>& observed_pairs,
      const CollisionWorldRevision& world_revision,
      bool diagnostics_complete = true);

  /// Read-only access to the snapshot. Useful for callers that want to
  /// echo the policy revision into a CollisionResult.
  const CollisionPolicySnapshot& snapshot() const { return *snapshot_; }

 private:
  std::shared_ptr<const CollisionPolicySnapshot> snapshot_;
  std::shared_ptr<const CollisionPolicyIndex> index_;

  /// Pair lookups built from `snapshot_` at construction so classify()
  /// runs in O(N log N) where N is the number of observed pairs.
  SupportContactEvaluationStateMap support_contact_state_;
};

}  // namespace manyforge::core
