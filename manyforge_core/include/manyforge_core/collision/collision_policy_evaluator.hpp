#pragma once

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

/// Immutable lookup indexes derived from one CollisionPolicySnapshot. Safe to
/// share across per-pass evaluators; evaluators still own their mutable
/// support-contact state machines.
struct CollisionPolicyIndex {
  /// Revision of the immutable policy used to build these indexes. Runtime
  /// consumers compare this identity before trusting any allowance lookup.
  CollisionPolicyRevision source_policy_revision;
  std::map<CollisionPairId, SupportContactPolicyEntry, std::less<>>
      support_contact_index;
  std::map<CollisionPairId, bool, std::less<>> static_static_index;
  std::map<CollisionPairId, bool, std::less<>> touch_link_index;
  std::map<std::string, ContactGraspPolicyEntry, std::less<>>
      contact_grasp_index;
};

/// Trajectory-scoped state for one support-contact identity. This state is
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
  double previous_depth_m = 0.0;
  double best_depth_m = 0.0;

  friend bool operator==(const ContactReleaseEvaluationState&,
                         const ContactReleaseEvaluationState&) = default;
};

using ContactReleaseEvaluationStateMap =
    std::map<CollisionPairId, ContactReleaseEvaluationState, std::less<>>;

/// Match one canonical logical collision pair against an authored contact-
/// grasp binding. This is shared by validation, runtime selection, and policy
/// classification so pattern interpretation cannot drift between surfaces.
[[nodiscard]] bool contact_grasp_pair_matches(
    const CollisionPairId& pair,
    const ContactGraspPolicyEntry& policy) noexcept;

/// Build the complete canonical metric-pair selection for one immutable world.
/// Signed-release and bounded-penetration pairs are always included; binary
/// release pairs are excluded. When `active_contact_policy_id` names a contact-
/// grasp entry, its object/link pattern is resolved to concrete world body
/// pairs before the query hot path.
[[nodiscard]] std::vector<CollisionPairId>
build_selected_collision_distance_pairs(
    const CollisionPolicySnapshot& policy,
    const CollisionWorldSnapshot& world,
    std::string_view active_contact_policy_id = {});

/// Clearance margins for one move (collision clearance), from the
/// existing contact policies. `margin_m` goes on every pair of a robot body
/// (a component-model body or a carried scene body) and an environment body
/// (a detached scene resource or a runtime object). A pair the policy allows
/// to touch keeps margin 0: static, touch-link and support-contact pairs, and
/// the active contact-grasp entry's pairs. Their penetration tolerances stay
/// with the policy evaluator, which needs the real contact evidence. Self
/// pairs (robot-robot) and environment-only pairs keep 0. Null when margin_m
/// is not positive or no pair qualifies.
[[nodiscard]] std::shared_ptr<const CollisionClearanceMargins>
build_collision_clearance_margins(
    const CollisionWorldSnapshot& world,
    const CollisionPolicySnapshot& policy,
    std::string_view active_contact_policy_id, double margin_m);

/// One logical support-pair observation for the shared contact-release state
/// machine. `signed_clearance_m` is required only when `in_contact` is true and
/// the policy selects signed-penetration release evidence or bounded-
/// penetration policy mode. Binary release policies ignore it, and absent pairs
/// must not fabricate a zero clearance.
struct ContactReleaseSampleEvidence {
  bool in_contact = false;
  std::optional<double> signed_clearance_m;
};

/// Typed failure emitted by the allocation-free per-sample transition helper.
/// Callers translate this stable category into surface-specific decisions and
/// diagnostics without reimplementing the state transition.
enum class ContactReleaseTransitionFailure {
  kNone,
  kInvalidPolicy,
  kContactProcessNotImplemented,
  kMissingOrInvalidSignedPenetration,
  kNewContactAfterClearStart,
  kInitialPenetrationExceedsPolicy,
  kRecontactAfterRelease,
  kRecontactPenetrationExceedsPolicy,
  kPenetrationIncreased,
  kContactPenetrationExceedsPolicy,
};

struct ContactReleaseTransitionResult {
  ContactReleaseTransitionFailure failure =
      ContactReleaseTransitionFailure::kNone;
  std::optional<double> penetration_depth_m;

  [[nodiscard]] constexpr bool accepted() const noexcept {
    return failure == ContactReleaseTransitionFailure::kNone;
  }
};

/// Validate the mode-scoped internal policy representation consumed by every
/// ordered decision surface. Typed DTO defaults are accepted as the
/// presence-less representation of omitted fields; arbitrary mixtures of
/// release-only and bounded-only values fail closed.
[[nodiscard]] bool support_contact_policy_entry_valid(
    const SupportContactPolicyEntry& policy) noexcept;

/// Advance one contact-release identity by one trajectory-ordered sample.
///
/// The first call establishes C_0. Later contact for an initially-clear pair is
/// rejected; absence marks separation; disallowed recontact is rejected. The
/// signed release mode additionally validates and updates penetration evidence
/// while a pair is colliding. Bounded-penetration mode permits arbitrary
/// contact appearance/disappearance but validates every colliding depth against
/// its bound. The helper performs no allocation and never infers contact from a
/// distance tolerance.
[[nodiscard]] ContactReleaseTransitionResult advance_contact_release_sample(
    const SupportContactPolicyEntry& policy,
    const ContactReleaseSampleEvidence& sample,
    ContactReleaseEvaluationState& state) noexcept;

[[nodiscard]] constexpr const char* contact_release_transition_failure_name(
    ContactReleaseTransitionFailure failure) noexcept {
  switch (failure) {
    case ContactReleaseTransitionFailure::kNone:
      return "none";
    case ContactReleaseTransitionFailure::kInvalidPolicy:
      return "invalid_contact_policy";
    case ContactReleaseTransitionFailure::kContactProcessNotImplemented:
      return "contact_process_policy_not_implemented";
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
    case ContactReleaseTransitionFailure::kContactPenetrationExceedsPolicy:
      return "contact_penetration_exceeds_policy";
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
/// The evaluator maintains the support-contact state machine across
/// repeated classify() calls within one validation pass. Callers MUST
/// invoke reset() between independent passes (e.g. between moves) so
/// stale support-contact state cannot leak across trajectories.
///
/// Determinism: classify() is deterministic for fixed inputs; observation
/// order across pairs does not affect the result. Two-input scenarios
/// that exercise the support-contact state machine (e.g. "first call:
/// contact, second call: separated, third call: contact again") rely on
/// the call ordering itself, which is the trajectory-order semantics
/// the plan defines.
class CollisionPolicyEvaluator {
 public:
  explicit CollisionPolicyEvaluator(CollisionPolicySnapshot snapshot);
  explicit CollisionPolicyEvaluator(
      std::shared_ptr<const CollisionPolicySnapshot> snapshot,
      std::shared_ptr<const CollisionPolicyIndex> index = nullptr);

  /// Clear support-contact state machine entries. Call between
  /// independent validation passes.
  void reset();

  /// Replace the complete trajectory-scoped support-contact state. The input
  /// must contain exactly the canonical support-contact identities from the
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
  ///   `observed_pairs` were produced. Used for the
  ///   `kAllowedStaticStatic` rule ("truly static under the active
  ///   revision").
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
