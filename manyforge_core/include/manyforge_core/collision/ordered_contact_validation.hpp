#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/collision/collision_policy_evaluator.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

/// Hard bound shared with the runtime contact-release session. Ordered
/// summaries are O(chunks * identities), never O(chunks * waypoints).
inline constexpr std::size_t kMaxOrderedSupportContactIdentities = 64;

struct SupportContactTransition {
  SupportContactEvaluationState exit_state = SupportContactEvaluationState::kNotInContact;
  std::optional<std::int64_t> first_recontact_violation_waypoint;
};

struct SupportContactPairChunkSummary {
  CollisionPairId pair;
  std::array<SupportContactTransition, 5> transition_by_entry_state;
};

struct ContactReleaseSampleSummary {
  std::int64_t waypoint_index = -1;
  std::optional<std::int64_t> time_from_start_ns;
  /// Authoritative logical-pair presence from CollisionResult::normalized_pairs.
  bool in_contact = false;
  bool clearance_valid = false;
  double signed_clearance_m = 0.0;
  double penetration_depth_m = 0.0;
  bool separated = false;
  /// Path steps since the previous sample (see
  /// CollisionValidationPassConfig::sample_path_steps); absent: unknown.
  std::optional<double> path_steps;
};

struct ContactReleaseSegmentSummary {
  bool empty = true;
  std::optional<ContactReleaseSampleSummary> first_sample;
  std::optional<ContactReleaseSampleSummary> last_sample;
  std::optional<ContactReleaseSampleSummary> first_missing_clearance;
  std::optional<ContactReleaseSampleSummary> first_active_contact;
  std::optional<ContactReleaseSampleSummary> last_active_contact;
  std::optional<ContactReleaseSampleSummary> first_separation;
  std::optional<ContactReleaseSampleSummary> first_active_contact_after_separation;
  std::optional<ContactReleaseSampleSummary> first_active_contact_exceeding_max_start;
  std::optional<ContactReleaseSampleSummary>
      first_active_contact_exceeding_max_start_after_separation;
  std::optional<ContactReleaseSampleSummary>
      first_active_contact_exceeding_contact_bound;
  std::optional<ContactReleaseSampleSummary> first_internal_depth_increase;
  /// A spring (OD-356): the first sample after the segment's first whose
  /// depth grew by more than the step limit.
  std::optional<ContactReleaseSampleSummary> first_compression_step_exceeded;
  std::optional<double> minimum_effective_depth_m;
  double last_effective_depth_m = 0.0;
  bool saw_separation = false;
};

struct ContactReleasePairChunkSummary {
  CollisionPairId pair;
  ContactReleaseSegmentSummary full_segment;
  ContactReleaseSegmentSummary segment_after_first_sample;
};

/// One bounded worker result. It contains no trajectory-sized observation
/// history; waypoint records remain worker-local and are finalized there.
struct OrderedContactChunkSummary {
  std::size_t range_begin = 0;
  std::size_t range_end = 0;
  CollisionPolicyRevision policy_revision;
  bool valid = true;
  std::string invalid_reason;
  std::map<CollisionPairId, SupportContactPairChunkSummary, std::less<>>
      support_contact_transitions;
  std::map<CollisionPairId, ContactReleasePairChunkSummary, std::less<>> contact_release_summaries;
};

struct ContactReleaseViolation {
  /// Index into `CollisionPolicySnapshot::contact_rules`.
  std::size_t policy_entry_index = 0;
  std::int64_t waypoint_index = -1;
  std::optional<std::int64_t> time_from_start_ns;
  CollisionDecisionResult decision = CollisionDecisionResult::kFail;
  std::string reason;
  std::optional<double> initial_depth_m;
  std::optional<double> current_depth_m;
};

/// One step-limit violation of a grouped spring rule (OD-371).
struct SpringGroupStepViolation {
  /// Index into the records given.
  std::size_t record_index = 0;
  /// Index into `CollisionPolicySnapshot::contact_rules` of the rule's
  /// deepest pair at that record.
  std::size_t policy_entry_index = 0;
  double previous_depth_m = 0.0;
  double depth_m = 0.0;
  double step_limit_m = 0.0;
};

/// The step limit of the grouped spring rules (`CollisionContactRule::
/// spring_group`) of kind `kind` over one whole trajectory's records, in
/// order (OD-371). A grouped rule is one spring: its depth at a record is the
/// deepest penetration over its pairs that collide there with a signed
/// clearance (0 when none does), and its step limit applies to that depth
/// against the previous record's; the first record has no predecessor. At
/// most one violation per rule, the first, on the rule's deepest pair. The
/// depth limit and a missing clearance stay per pair. O(records x pairs).
[[nodiscard]] std::vector<SpringGroupStepViolation> spring_group_step_violations(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records,
    const CollisionValidationPassConfig& pass_config, ContactRuleKind kind);

/// The violations of `spring_group_step_violations` for the tracked kind
/// (whole-move contact with a depth limit), as contact-release violations
/// for `apply_contact_release_violations`. `records` are one whole
/// trajectory's, indexed as the pass is.
[[nodiscard]] std::vector<ContactReleaseViolation> spring_group_release_violations(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records,
    const CollisionValidationPassConfig& pass_config);

/// Build one bounded chunk summary from worker-local records. `global_offset`
/// maps local record indices to the trajectory's original index space.
/// `pass_config` gives the samples' path steps for a spring's step limit
/// (null: none checked).
OrderedContactChunkSummary summarize_ordered_contact_chunk(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records, std::size_t global_offset,
    std::size_t range_end, const CollisionValidationPassConfig* pass_config = nullptr);

SupportContactEvaluationStateMap initial_support_contact_state(
    const CollisionPolicySnapshot& policy);

/// Compose one tracked-contact transition summary into an exact prefix state.
/// Returns false on a policy/summary identity mismatch.
bool advance_support_contact_state(const OrderedContactChunkSummary& summary,
                                   SupportContactEvaluationStateMap& state,
                                   std::string* error_reason = nullptr);

/// Merge contact-release summaries in original chunk order. The function
/// returns at most one earliest violation per tracked contact-rule pair
/// (release rules and whole-move contact rules with a depth limit); it never reads
/// waypoint records or calls a collision engine.
std::vector<ContactReleaseViolation> merge_contact_release_summaries(
    const CollisionPolicySnapshot& policy, const std::vector<OrderedContactChunkSummary>& summaries,
    std::size_t trajectory_waypoint_count, std::string* error_reason = nullptr,
    const ContactReleaseEvaluationStateMap* entering_state = nullptr,
    bool enforce_trajectory_end_constraints = true,
    ContactReleaseEvaluationStateMap* exiting_state = nullptr);

/// Apply bounded merge conclusions to their selected diagnostic records and
/// recompute the bundle's globally earliest decision. Existing engine/policy
/// failures are preserved when they precede a contact-release violation.
bool apply_contact_release_violations(const CollisionPolicySnapshot& policy,
                                      const std::vector<ContactReleaseViolation>& violations,
                                      CollisionValidationDecisionBundle& bundle,
                                      std::string* error_reason = nullptr);

}  // namespace manyforge::core
