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

}  // namespace

std::size_t tracked_contact_rule_count(
    const CollisionPolicySnapshot& policy) noexcept {
  return static_cast<std::size_t>(
      std::count_if(policy.contact_rules.begin(), policy.contact_rules.end(),
                    [](const CollisionPairContactRule& rule) {
                      return contact_rule_is_tracked(rule);
                    }));
}

std::vector<CollisionPairId> build_selected_collision_distance_pairs(
    const CollisionPolicySnapshot& policy) {
  std::vector<CollisionPairId> selected;
  selected.reserve(policy.contact_rules.size());
  for (const auto& rule : policy.contact_rules) {
    if (contact_rule_needs_signed_distance(rule)) {
      selected.push_back(canonicalize(rule.pair));
    }
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  return selected;
}

bool contact_rule_valid(const CollisionPairContactRule& rule) noexcept {
  const auto nonnegative_finite = [](double value) {
    return std::isfinite(value) && value >= 0.0;
  };
  if (const auto* contact = rule.contact()) {
    return (!contact->max_penetration_m ||
            nonnegative_finite(*contact->max_penetration_m)) &&
           (!contact->max_compression_step_m ||
            (contact->max_penetration_m && std::isfinite(*contact->max_compression_step_m) &&
             *contact->max_compression_step_m > 0.0)) &&
           std::isfinite(contact->end_from_fraction) &&
           contact->end_from_fraction >= 0.0 &&
           contact->end_from_fraction <= 1.0;
  }
  const auto& release = *rule.release();
  if (!nonnegative_finite(release.max_start_penetration_m) ||
      !nonnegative_finite(release.release_tolerance_m) ||
      !nonnegative_finite(release.min_release_progress_m)) {
    return false;
  }
  switch (release.release_evidence_mode) {
    case ContactReleaseEvidenceMode::kSignedPenetration:
      return true;
    case ContactReleaseEvidenceMode::kBinaryContact:
      return (release.enforce_max_start_penetration &&
              release.max_start_penetration_m == 0.0 &&
              release.release_tolerance_m == 1e-5 &&
              release.min_release_progress_m == 0.0 &&
              release.require_strictly_decreasing_penetration) ||
             (!release.enforce_max_start_penetration &&
              release.max_start_penetration_m == 0.0 &&
              release.release_tolerance_m == 0.0 &&
              release.min_release_progress_m == 0.0 &&
              !release.require_strictly_decreasing_penetration);
    default:
      return false;
  }
}

ContactReleaseTransitionResult advance_contact_release_sample(
    const CollisionPairContactRule& rule,
    const ContactReleaseSampleEvidence& sample,
    ContactReleaseEvaluationState& state) noexcept {
  ContactReleaseTransitionResult result;
  if (!contact_rule_is_tracked(rule) || !contact_rule_valid(rule)) {
    result.failure = ContactReleaseTransitionFailure::kInvalidPolicy;
    return result;
  }

  const bool bounded_penetration =
      contact_rule_kind(rule) == ContactRuleKind::kBoundedContact;
  const bool signed_penetration = contact_rule_needs_signed_distance(rule);
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
    const bool first_sample = !state.initialized;
    if (!state.initialized) {
      state.initialized = true;
      state.started_in_contact = sample.in_contact;
    }
    if (!sample.in_contact && state.in_contact) {
      state.separated = true;
    }
    state.in_contact = sample.in_contact;
    const auto& contact = *rule.contact();
    // A spring: the depth grows smoothly; a step or an obstacle jumps.
    const double effective_depth_m = sample.in_contact ? depth_m : 0.0;
    const auto step_limit = pair_compression_step_limit_m(contact, sample.path_steps);
    const double previous_depth_m = state.previous_depth_m;
    state.previous_depth_m = effective_depth_m;
    if (sample.in_contact && depth_m > std::max(0.0, *contact.max_penetration_m)) {
      result.failure =
          ContactReleaseTransitionFailure::kContactPenetrationExceeded;
    } else if (!first_sample && step_limit &&
               effective_depth_m > previous_depth_m + *step_limit + 1e-9) {
      result.failure =
          ContactReleaseTransitionFailure::kContactCompressionStepExceeded;
    }
    return result;
  }

  const CollisionReleaseRule& policy = *rule.release();
  const double tolerance = std::max(0.0, policy.release_tolerance_m);
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
  for (const auto& rule : snapshot.contact_rules) {
    auto canonical = rule;
    canonical.pair = canonicalize(rule.pair);
    index->contact_rule_index[canonical.pair] = std::move(canonical);
  }
  for (const auto& pair : snapshot.allowed_touch_link_pairs) {
    index->touch_link_index[canonicalize(pair)] = true;
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
  for (const auto& [pair, rule] : index_->contact_rule_index) {
    if (contact_rule_is_tracked(rule)) {
      support_contact_state_[pair] = SupportContactEvaluationState::kNotInContact;
    }
  }
}

bool CollisionPolicyEvaluator::restore_support_contact_state(
    const SupportContactEvaluationStateMap& state) {
  std::size_t tracked = 0U;
  for (const auto& [pair, rule] : index_->contact_rule_index) {
    if (!contact_rule_is_tracked(rule)) {
      continue;
    }
    ++tracked;
    if (state.find(pair) == state.end()) {
      return false;
    }
  }
  if (state.size() != tracked) {
    return false;
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
  // These may transition contact state machine entries from
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

    // Two static bodies are never checked against each other. The
    // evaluator trusts the snapshot's static bodies; the Scene projector
    // lists only bodies that do not move under the active world revision
    // (an attached object is carried, so not static).
    if (snapshot_->static_pair(pair)) {
      entry.classification = CollisionPairClassification::kAllowedStaticStatic;
      out.pairs.push_back(entry);
      continue;
    }

    // Contact rules. A whole-move contact without a depth limit is allowed
    // as it is. A windowed contact stays blocking here: the surface that
    // knows the sample's position in the move applies the window and depth.
    // Release and whole-move limited contact run the state machine.
    auto rule_it = index_->contact_rule_index.find(pair);
    if (rule_it != index_->contact_rule_index.end() &&
        contact_rule_kind(rule_it->second) == ContactRuleKind::kFreeContact) {
      entry.classification = CollisionPairClassification::kAllowedContactRule;
      out.pairs.push_back(entry);
      continue;
    }
    if (rule_it != index_->contact_rule_index.end() &&
        contact_rule_is_tracked(rule_it->second)) {
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
          if (rule_it->second.release() == nullptr ||
              rule_it->second.release()->allow_recontact) {
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

  // Now visit previously-active tracked contacts that are no longer in
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
