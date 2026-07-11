#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/collision.hpp>

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

std::shared_ptr<const CollisionPolicyIndex> build_collision_policy_index(
    const CollisionPolicySnapshot& snapshot);

/// CollisionPolicyEvaluator (spec §3.8.1, added in Chunk 3 of the
/// collision-ownership plan).
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
  /// Support-contact state machine states (per the plan's explicit
  /// transition spec). Mapped by canonical CollisionPairId.
  enum class SupportContactState {
    kNotInContact,
    kInitialContact,
    kContactActive,
    kSeparated,
    kRecontactViolation,
  };

  std::shared_ptr<const CollisionPolicySnapshot> snapshot_;
  std::shared_ptr<const CollisionPolicyIndex> index_;

  /// Pair lookups built from `snapshot_` at construction so classify()
  /// runs in O(N log N) where N is the number of observed pairs.
  std::map<CollisionPairId, SupportContactState, std::less<>>
      support_contact_state_;
};

}  // namespace manyforge::core
