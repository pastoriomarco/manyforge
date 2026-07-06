#include <manyforge_core/collision/collision_policy_evaluator.hpp>

#include <algorithm>
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

std::shared_ptr<const CollisionPolicyIndex> build_collision_policy_index(
    const CollisionPolicySnapshot& snapshot) {
  auto index = std::make_shared<CollisionPolicyIndex>();
  for (const auto& entry : snapshot.support_contacts) {
    index->support_contact_index[canonicalize(entry.pair)] = entry;
  }
  for (const auto& pair : snapshot.allowed_static_static_pairs) {
    index->static_static_index[canonicalize(pair)] = true;
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
  for (const auto& [pair, _] : index_->support_contact_index) {
    support_contact_state_[pair] = SupportContactState::kNotInContact;
  }
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
    if (state == SupportContactState::kInitialContact ||
        state == SupportContactState::kContactActive) {
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
        case SupportContactState::kNotInContact:
          state = SupportContactState::kInitialContact;
          entry.classification = CollisionPairClassification::kSupportContactStart;
          break;
        case SupportContactState::kInitialContact:
        case SupportContactState::kContactActive:
          state = SupportContactState::kContactActive;
          entry.classification = CollisionPairClassification::kSupportContactContinued;
          break;
        case SupportContactState::kSeparated:
          if (support_it->second.allow_recontact) {
            state = SupportContactState::kContactActive;
            entry.classification =
                CollisionPairClassification::kSupportContactContinued;
          } else {
            state = SupportContactState::kRecontactViolation;
            entry.classification =
                CollisionPairClassification::kSupportContactRecontactViolation;
          }
          break;
        case SupportContactState::kRecontactViolation:
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
    state_it->second = SupportContactState::kSeparated;

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
