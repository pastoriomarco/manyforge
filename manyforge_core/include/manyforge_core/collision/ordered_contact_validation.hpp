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
  std::size_t policy_entry_index = 0;
  std::int64_t waypoint_index = -1;
  std::optional<std::int64_t> time_from_start_ns;
  CollisionDecisionResult decision = CollisionDecisionResult::kFail;
  std::string reason;
  std::optional<double> initial_depth_m;
  std::optional<double> current_depth_m;
};

/// Build one bounded chunk summary from worker-local records. `global_offset`
/// maps local record indices to the trajectory's original index space.
OrderedContactChunkSummary summarize_ordered_contact_chunk(
    const CollisionPolicySnapshot& policy,
    const std::vector<CollisionValidationWaypointRecord>& records, std::size_t global_offset,
    std::size_t range_end);

SupportContactEvaluationStateMap initial_support_contact_state(
    const CollisionPolicySnapshot& policy);

/// Compose one support-contact transition summary into an exact prefix state.
/// Returns false on a policy/summary identity mismatch.
bool advance_support_contact_state(const OrderedContactChunkSummary& summary,
                                   SupportContactEvaluationStateMap& state,
                                   std::string* error_reason = nullptr);

/// Merge contact-release summaries in original chunk order. The function
/// returns at most one earliest violation per policy identity; it never reads
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
