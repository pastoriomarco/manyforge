#include <manyforge_core/collision/collision_policy_evaluator.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace manyforge::core {

namespace {

/// Build the canonical form of a CollisionPairId so map lookups are
/// order-independent across producers.
CollisionPairId canonicalize(const CollisionPairId& pair) {
  if (pair.body_b < pair.body_a) {
    return CollisionPairId{pair.body_b, pair.body_a};
  }
  return pair;
}

bool object_body_matches(const std::string& body,
                         const std::string& object_id) noexcept {
  constexpr std::string_view prefix = "object/";
  if (body.size() <= prefix.size() + object_id.size() ||
      body.compare(0, prefix.size(), prefix) != 0 ||
      body.compare(prefix.size(), object_id.size(), object_id) != 0) {
    return false;
  }
  return body[prefix.size() + object_id.size()] == '/';
}

bool robot_link_body_matches(const std::string& body,
                             const std::string& link) noexcept {
  constexpr std::string_view marker = "/link/";
  const auto begin = body.find(marker);
  if (begin == std::string::npos) {
    return false;
  }
  const auto name_begin = begin + marker.size();
  return body.size() > name_begin + link.size() &&
         body.compare(name_begin, link.size(), link) == 0 &&
         body[name_begin + link.size()] == '/';
}

}  // namespace

bool contact_grasp_pair_matches(const CollisionPairId& pair,
                                const ContactGraspPolicyEntry& policy) noexcept {
  auto link_matches = [&](const std::string& body) {
    return std::any_of(policy.allowed_robot_links.begin(),
                       policy.allowed_robot_links.end(),
                       [&](const auto& link) {
                         return robot_link_body_matches(body, link);
                       });
  };
  return (object_body_matches(pair.body_a, policy.selected_object_id) &&
          link_matches(pair.body_b)) ||
         (object_body_matches(pair.body_b, policy.selected_object_id) &&
          link_matches(pair.body_a));
}

std::vector<CollisionPairId> build_selected_collision_distance_pairs(
    const CollisionPolicySnapshot& policy,
    const CollisionWorldSnapshot& world,
    std::string_view active_contact_policy_id) {
  std::vector<CollisionPairId> selected;
  selected.reserve(policy.support_contacts.size());
  for (const auto& entry : policy.support_contacts) {
    if (entry.mode == CollisionContactPolicyMode::kBoundedPenetration ||
        (entry.mode == CollisionContactPolicyMode::kContactRelease &&
         entry.release_evidence_mode ==
             ContactReleaseEvidenceMode::kSignedPenetration)) {
      selected.push_back(canonicalize(entry.pair));
    }
  }

  const auto grasp =
      std::find_if(policy.contact_grasps.begin(), policy.contact_grasps.end(),
                   [&](const ContactGraspPolicyEntry& entry) {
                     return entry.policy_id == active_contact_policy_id;
                   });
  if (!active_contact_policy_id.empty() && grasp != policy.contact_grasps.end()) {
    for (std::size_t i = 0; i < world.bodies.size(); ++i) {
      for (std::size_t j = i + 1; j < world.bodies.size(); ++j) {
        CollisionPairId pair{world.bodies[i].body_id,
                             world.bodies[j].body_id};
        pair = canonicalize(pair);
        if (contact_grasp_pair_matches(pair, *grasp)) {
          selected.push_back(std::move(pair));
        }
      }
    }
  }

  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  return selected;
}

bool support_contact_policy_entry_valid(
    const SupportContactPolicyEntry& policy) noexcept {
  const auto nonnegative_finite = [](double value) {
    return std::isfinite(value) && value >= 0.0;
  };
  const bool release_defaults =
      policy.enforce_max_start_penetration &&
      policy.max_start_penetration_m == 0.0 &&
      policy.release_tolerance_m == 1e-5 &&
      policy.min_release_progress_m == 0.0 &&
      policy.require_separation_by_end &&
      policy.require_strictly_decreasing_penetration &&
      !policy.allow_recontact;
  const bool release_neutral =
      !policy.enforce_max_start_penetration &&
      policy.max_start_penetration_m == 0.0 &&
      policy.release_tolerance_m == 0.0 &&
      policy.min_release_progress_m == 0.0 &&
      !policy.require_separation_by_end &&
      !policy.require_strictly_decreasing_penetration &&
      !policy.allow_recontact;

  switch (policy.mode) {
    case CollisionContactPolicyMode::kBoundedPenetration:
      return nonnegative_finite(policy.max_contact_penetration_m) &&
             policy.release_evidence_mode ==
                 ContactReleaseEvidenceMode::kSignedPenetration &&
             (release_defaults || release_neutral);
    case CollisionContactPolicyMode::kContactProcess:
      return policy.max_contact_penetration_m == 0.0 &&
             policy.release_evidence_mode ==
                 ContactReleaseEvidenceMode::kSignedPenetration &&
             (release_defaults || release_neutral);
    case CollisionContactPolicyMode::kContactRelease:
      break;
    default:
      return false;
  }

  if (policy.max_contact_penetration_m != 0.0 ||
      !nonnegative_finite(policy.max_start_penetration_m) ||
      !nonnegative_finite(policy.release_tolerance_m) ||
      !nonnegative_finite(policy.min_release_progress_m)) {
    return false;
  }
  switch (policy.release_evidence_mode) {
    case ContactReleaseEvidenceMode::kSignedPenetration:
      return true;
    case ContactReleaseEvidenceMode::kBinaryContact:
      return (policy.enforce_max_start_penetration &&
              policy.max_start_penetration_m == 0.0 &&
              policy.release_tolerance_m == 1e-5 &&
              policy.min_release_progress_m == 0.0 &&
              policy.require_strictly_decreasing_penetration) ||
             (!policy.enforce_max_start_penetration &&
              policy.max_start_penetration_m == 0.0 &&
              policy.release_tolerance_m == 0.0 &&
              policy.min_release_progress_m == 0.0 &&
              !policy.require_strictly_decreasing_penetration);
    default:
      return false;
  }
}

ContactReleaseTransitionResult advance_contact_release_sample(
    const SupportContactPolicyEntry& policy,
    const ContactReleaseSampleEvidence& sample,
    ContactReleaseEvaluationState& state) noexcept {
  ContactReleaseTransitionResult result;
  if (policy.mode == CollisionContactPolicyMode::kContactProcess) {
    result.failure =
        ContactReleaseTransitionFailure::kContactProcessNotImplemented;
    return result;
  }

  if (!support_contact_policy_entry_valid(policy)) {
    result.failure = ContactReleaseTransitionFailure::kInvalidPolicy;
    return result;
  }

  const bool bounded_penetration =
      policy.mode == CollisionContactPolicyMode::kBoundedPenetration;
  const bool signed_penetration =
      bounded_penetration ||
      policy.release_evidence_mode ==
          ContactReleaseEvidenceMode::kSignedPenetration;
  const double tolerance = std::max(0.0, policy.release_tolerance_m);
  double depth_m = 0.0;
  if (sample.in_contact && signed_penetration) {
    if (!sample.signed_clearance_m ||
        !std::isfinite(*sample.signed_clearance_m) ||
        *sample.signed_clearance_m > 0.0) {
      result.failure = ContactReleaseTransitionFailure::
          kMissingOrInvalidSignedPenetration;
      return result;
    }
    depth_m = std::max(0.0, -*sample.signed_clearance_m);
    result.penetration_depth_m = depth_m;
  }

  if (bounded_penetration) {
    if (!state.initialized) {
      state.initialized = true;
      state.started_in_contact = sample.in_contact;
    }
    if (!sample.in_contact && state.in_contact) {
      state.separated = true;
    }
    state.in_contact = sample.in_contact;
    if (sample.in_contact &&
        depth_m > std::max(0.0, policy.max_contact_penetration_m)) {
      result.failure = ContactReleaseTransitionFailure::
          kContactPenetrationExceedsPolicy;
    }
    return result;
  }

  if (!state.initialized) {
    state.initialized = true;
    state.started_in_contact = sample.in_contact;
    state.in_contact = sample.in_contact;
    state.separated = false;
    if (!sample.in_contact) {
      return result;
    }
    if (signed_penetration) {
      state.initial_depth_m = depth_m;
      state.previous_depth_m = depth_m;
      state.best_depth_m = depth_m;
      if (policy.enforce_max_start_penetration &&
          depth_m > std::max(0.0, policy.max_start_penetration_m) +
                        tolerance) {
        result.failure = ContactReleaseTransitionFailure::
            kInitialPenetrationExceedsPolicy;
      }
    }
    return result;
  }

  if (!sample.in_contact) {
    state.in_contact = false;
    if (state.started_in_contact) {
      state.separated = true;
      if (signed_penetration && state.initial_depth_m) {
        state.best_depth_m = 0.0;
      }
    }
    return result;
  }

  const bool recontact = !state.in_contact;
  state.in_contact = true;
  if (!state.started_in_contact) {
    result.failure =
        ContactReleaseTransitionFailure::kNewContactAfterClearStart;
    return result;
  }
  if (recontact && !policy.allow_recontact) {
    result.failure = ContactReleaseTransitionFailure::kRecontactAfterRelease;
    return result;
  }

  if (signed_penetration) {
    if (recontact && policy.enforce_max_start_penetration &&
        depth_m > std::max(0.0, policy.max_start_penetration_m) +
                      tolerance) {
      result.failure = ContactReleaseTransitionFailure::
          kRecontactPenetrationExceedsPolicy;
      return result;
    }
    if (state.initial_depth_m &&
        depth_m > state.previous_depth_m + tolerance) {
      result.failure =
          ContactReleaseTransitionFailure::kPenetrationIncreased;
      return result;
    }
    state.previous_depth_m = depth_m;
    state.best_depth_m =
        state.initial_depth_m ? std::min(state.best_depth_m, depth_m) : depth_m;
  }
  return result;
}

std::shared_ptr<const CollisionPolicyIndex> build_collision_policy_index(
    const CollisionPolicySnapshot& snapshot) {
  auto index = std::make_shared<CollisionPolicyIndex>();
  index->source_policy_revision = snapshot.revision;
  for (const auto& entry : snapshot.support_contacts) {
    index->support_contact_index[canonicalize(entry.pair)] = entry;
  }
  for (const auto& pair : snapshot.allowed_static_static_pairs) {
    index->static_static_index[canonicalize(pair)] = true;
  }
  for (const auto& pair : snapshot.allowed_touch_link_pairs) {
    index->touch_link_index[canonicalize(pair)] = true;
  }
  for (const auto& entry : snapshot.contact_grasps) {
    index->contact_grasp_index[entry.policy_id] = entry;
  }
  return index;
}

CollisionPolicyEvaluator::CollisionPolicyEvaluator(
    CollisionPolicySnapshot snapshot)
    : CollisionPolicyEvaluator(
          std::make_shared<CollisionPolicySnapshot>(std::move(snapshot))) {}

CollisionPolicyEvaluator::CollisionPolicyEvaluator(
    std::shared_ptr<const CollisionPolicySnapshot> snapshot,
    std::shared_ptr<const CollisionPolicyIndex> index)
    : snapshot_(std::move(snapshot)),
      index_(std::move(index)) {
  if (!snapshot_) {
    snapshot_ = std::make_shared<CollisionPolicySnapshot>();
  }
  if (!index_) {
    index_ = build_collision_policy_index(*snapshot_);
  }
  reset();
}

void CollisionPolicyEvaluator::reset() {
  support_contact_state_.clear();
  for (const auto& [pair, _] : index_->support_contact_index) {
    support_contact_state_[pair] = SupportContactEvaluationState::kNotInContact;
  }
}

bool CollisionPolicyEvaluator::restore_support_contact_state(
    const SupportContactEvaluationStateMap& state) {
  if (state.size() != index_->support_contact_index.size()) {
    return false;
  }
  for (const auto& [pair, _] : index_->support_contact_index) {
    if (state.find(pair) == state.end()) {
      return false;
    }
  }
  for (const auto& [pair, _] : state) {
    if (index_->support_contact_index.find(pair) == index_->support_contact_index.end()) {
      return false;
    }
  }
  support_contact_state_ = state;
  return true;
}

EvaluatedPairs CollisionPolicyEvaluator::classify(
    const std::vector<CollisionPairId>& observed_pairs,
    const CollisionWorldRevision& world_revision,
    bool diagnostics_complete) {
  EvaluatedPairs out;

  // Build a sorted unique view so classify() is deterministic for fixed
  // inputs and order-invariant across producers.
  std::vector<CollisionPairId> canonical;
  canonical.reserve(observed_pairs.size());
  for (const auto& p : observed_pairs) {
    canonical.push_back(canonicalize(p));
  }
  std::sort(canonical.begin(), canonical.end());
  canonical.erase(std::unique(canonical.begin(), canonical.end()),
                  canonical.end());

  // Pairs that were in contact on a previous call but are absent now.
  // These may transition support-contact state machine entries from
  // kContactActive to kSeparated.
  std::vector<CollisionPairId> previously_active;
  for (const auto& [pair, state] : support_contact_state_) {
    if (state == SupportContactEvaluationState::kInitialContact ||
        state == SupportContactEvaluationState::kContactActive) {
      previously_active.push_back(pair);
    }
  }

  out.pairs.reserve(canonical.size());

  const bool fail_closed_diagnostics =
      snapshot_->require_full_diagnostics && !diagnostics_complete;

  for (const auto& pair : canonical) {
    CollisionPairResult entry;
    entry.pair = pair;

    if (fail_closed_diagnostics) {
      entry.classification = CollisionPairClassification::kDiagnosticIncomplete;
      out.pairs.push_back(entry);
      continue;
    }

    // Touch-link allowances classify first: they are the explicit
    // attached-object exemption that "do not mask unrelated collisions"
    // applies to — only the listed pairs are touched.
    auto touch_it = index_->touch_link_index.find(pair);
    if (touch_it != index_->touch_link_index.end()) {
      entry.classification = CollisionPairClassification::kAllowedTouchLink;
      out.pairs.push_back(entry);
      continue;
    }

    // Static-static filtering only applies when both bodies are listed
    // as static under the active world revision. The evaluator trusts
    // the snapshot's listing; "truly static under the active revision"
    // is a guarantee the snapshot publisher must honor (a body that
    // becomes attached or pose-source driven gets a new body id and so
    // does not match a stale allowance).
    auto static_it = index_->static_static_index.find(pair);
    if (static_it != index_->static_static_index.end()) {
      entry.classification = CollisionPairClassification::kAllowedStaticStatic;
      out.pairs.push_back(entry);
      continue;
    }

    // Support contact: stateful transitions per the plan's explicit
    // state machine spec.
    auto support_it = index_->support_contact_index.find(pair);
    if (support_it != index_->support_contact_index.end()) {
      auto& state = support_contact_state_[pair];
      switch (state) {
        case SupportContactEvaluationState::kNotInContact:
          state = SupportContactEvaluationState::kInitialContact;
          entry.classification = CollisionPairClassification::kSupportContactStart;
          break;
        case SupportContactEvaluationState::kInitialContact:
        case SupportContactEvaluationState::kContactActive:
          state = SupportContactEvaluationState::kContactActive;
          entry.classification = CollisionPairClassification::kSupportContactContinued;
          break;
        case SupportContactEvaluationState::kSeparated:
          if (support_it->second.mode ==
                  CollisionContactPolicyMode::kBoundedPenetration ||
              support_it->second.allow_recontact) {
            state = SupportContactEvaluationState::kContactActive;
            entry.classification =
                CollisionPairClassification::kSupportContactContinued;
          } else {
            state = SupportContactEvaluationState::kRecontactViolation;
            entry.classification =
                CollisionPairClassification::kSupportContactRecontactViolation;
          }
          break;
        case SupportContactEvaluationState::kRecontactViolation:
          entry.classification =
              CollisionPairClassification::kSupportContactRecontactViolation;
          break;
      }
      out.pairs.push_back(entry);
      continue;
    }

    // Default: blocking collision.
    entry.classification = CollisionPairClassification::kBlockingCollision;
    out.pairs.push_back(entry);
  }

  // Now visit previously-active support contacts that are no longer in
  // the observed set — transition them to kSeparated and emit a
  // kSupportContactSeparated entry so the trajectory record shows
  // separation deterministically.
  for (const auto& prev : previously_active) {
    bool still_in_contact = std::binary_search(canonical.begin(),
                                                canonical.end(), prev);
    if (still_in_contact) continue;
    auto state_it = support_contact_state_.find(prev);
    if (state_it == support_contact_state_.end()) continue;
    state_it->second = SupportContactEvaluationState::kSeparated;

    CollisionPairResult sep_entry;
    sep_entry.pair = prev;
    sep_entry.classification =
        CollisionPairClassification::kSupportContactSeparated;
    out.pairs.push_back(sep_entry);
  }

  // Re-sort the combined list so consumers always see deterministic
  // ordering regardless of how the separation entries were appended.
  std::sort(out.pairs.begin(), out.pairs.end(),
            [](const CollisionPairResult& a, const CollisionPairResult& b) {
              return a.pair < b.pair;
            });

  // Build the overall CollisionDecision. The first blocking /
  // recontact-violation / inconclusive entry (in sorted order) is the
  // first_failure marker.
  CollisionPairResult* first_failure = nullptr;
  bool any_block = false;
  bool any_inconclusive = fail_closed_diagnostics;
  for (auto& entry : out.pairs) {
    switch (entry.classification) {
      case CollisionPairClassification::kBlockingCollision:
      case CollisionPairClassification::kSupportContactRecontactViolation:
        any_block = true;
        if (first_failure == nullptr) first_failure = &entry;
        break;
      case CollisionPairClassification::kDiagnosticIncomplete:
        any_inconclusive = true;
        if (first_failure == nullptr) first_failure = &entry;
        break;
      default:
        break;
    }
  }
  if (first_failure != nullptr) {
    first_failure->is_first_failure = true;
  }

  out.decision.accepted = !any_block && !any_inconclusive;
  out.decision.collision_world_revision = world_revision;
  if (any_block) {
    out.decision.severity = CollisionDecisionSeverity::kBlock;
    out.decision.action = CollisionDecisionAction::kReject;
  } else if (any_inconclusive) {
    out.decision.severity = CollisionDecisionSeverity::kBlock;
    out.decision.action = CollisionDecisionAction::kInconclusive;
  } else {
    out.decision.severity = CollisionDecisionSeverity::kNone;
    out.decision.action = CollisionDecisionAction::kDispatch;
  }

  return out;
}

}  // namespace manyforge::core
