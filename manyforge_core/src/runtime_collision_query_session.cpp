#include <algorithm>
#include <cmath>
#include <manyforge_core/runtime_collision_query_session.hpp>
#include <map>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

bool contact_grasp_policy_valid(const ContactGraspPolicyEntry& policy) noexcept {
  return !policy.policy_id.empty() && !policy.selected_object_id.empty() &&
         !policy.allowed_robot_links.empty() && std::isfinite(policy.max_penetration_m) &&
         policy.max_penetration_m >= 0.0 && std::isfinite(policy.terminal_window_start_fraction) &&
         policy.terminal_window_start_fraction >= 0.0 &&
         policy.terminal_window_start_fraction <= 1.0;
}

RuntimeCollisionTickReason transition_failure_reason(
    ContactReleaseTransitionFailure failure) noexcept {
  switch (failure) {
    case ContactReleaseTransitionFailure::kNone:
      return RuntimeCollisionTickReason::kNone;
    case ContactReleaseTransitionFailure::kInvalidPolicy:
      return RuntimeCollisionTickReason::kContactReleasePolicyInvalid;
    case ContactReleaseTransitionFailure::kContactProcessNotImplemented:
      return RuntimeCollisionTickReason::kContactReleaseProcessUnsupported;
    case ContactReleaseTransitionFailure::kMissingOrInvalidSignedPenetration:
      return RuntimeCollisionTickReason::kContactReleaseMissingClearance;
    case ContactReleaseTransitionFailure::kNewContactAfterClearStart:
      return RuntimeCollisionTickReason::kContactReleaseNewContact;
    case ContactReleaseTransitionFailure::kInitialPenetrationExceedsPolicy:
      return RuntimeCollisionTickReason::kContactReleaseInitialPenetrationExceeded;
    case ContactReleaseTransitionFailure::kRecontactAfterRelease:
      return RuntimeCollisionTickReason::kContactReleaseRecontact;
    case ContactReleaseTransitionFailure::kRecontactPenetrationExceedsPolicy:
      return RuntimeCollisionTickReason::kContactReleaseRecontactPenetrationExceeded;
    case ContactReleaseTransitionFailure::kPenetrationIncreased:
      return RuntimeCollisionTickReason::kContactReleasePenetrationIncreased;
    case ContactReleaseTransitionFailure::kContactPenetrationExceedsPolicy:
      return RuntimeCollisionTickReason::kBoundedContactPenetrationExceeded;
  }
  return RuntimeCollisionTickReason::kContactReleasePolicyInvalid;
}

}  // namespace

const char* runtime_collision_tick_reason_name(RuntimeCollisionTickReason reason) noexcept {
  switch (reason) {
    case RuntimeCollisionTickReason::kNone:
      return "none";
    case RuntimeCollisionTickReason::kBundleUnavailable:
      return "runtime_collision_snapshot_not_published";
    case RuntimeCollisionTickReason::kBundleCapacityExceeded:
      return "runtime_collision_bundle_capacity_exceeded";
    case RuntimeCollisionTickReason::kBundleLookupOrderInvalid:
      return "runtime_collision_bundle_lookup_order_invalid";
    case RuntimeCollisionTickReason::kPolicyUnavailable:
      return "missing_collision_policy_snapshot";
    case RuntimeCollisionTickReason::kPolicyIndexMismatch:
      return "runtime_collision_policy_index_mismatch";
    case RuntimeCollisionTickReason::kPolicyCapacityExceeded:
      return "runtime_collision_policy_capacity_exceeded";
    case RuntimeCollisionTickReason::kDistanceRequestModeUnsupported:
      return "runtime_collision_distance_request_mode_unsupported";
    case RuntimeCollisionTickReason::kRequiredCapabilityUnsupported:
      return "runtime_collision_required_capability_unsupported";
    case RuntimeCollisionTickReason::kActiveContactPolicyUnknown:
      return "runtime_contact_grasp_unknown_policy";
    case RuntimeCollisionTickReason::kActiveContactPolicyInvalid:
      return "runtime_contact_grasp_invalid_policy";
    case RuntimeCollisionTickReason::kObservedStateShapeMismatch:
      return "runtime_collision_projection_observed_shape_mismatch";
    case RuntimeCollisionTickReason::kObservedStateNotFinite:
      return "runtime_collision_projection_observed_value_not_finite";
    case RuntimeCollisionTickReason::kObservationCapacityExceeded:
      return "runtime_collision_query_observation_capacity_exceeded";
    case RuntimeCollisionTickReason::kEngineInconclusive:
      return "runtime_collision_monitor_inconclusive";
    case RuntimeCollisionTickReason::kBlockingCollision:
      return "runtime_collision_detected";
    case RuntimeCollisionTickReason::kContactReleasePolicyInvalid:
      return "runtime_contact_release_policy_invalid";
    case RuntimeCollisionTickReason::kContactReleaseProcessUnsupported:
      return "runtime_contact_process_policy_not_implemented";
    case RuntimeCollisionTickReason::kContactReleaseMissingClearance:
      return "runtime_contact_release_missing_clearance";
    case RuntimeCollisionTickReason::kContactReleaseNewContact:
      return "runtime_contact_release_new_contact";
    case RuntimeCollisionTickReason::kContactReleaseInitialPenetrationExceeded:
      return "runtime_contact_release_initial_penetration_exceeds_policy";
    case RuntimeCollisionTickReason::kContactReleaseRecontact:
      return "runtime_contact_release_recontact";
    case RuntimeCollisionTickReason::kContactReleaseRecontactPenetrationExceeded:
      return "runtime_contact_release_recontact_penetration_exceeds_policy";
    case RuntimeCollisionTickReason::kContactReleasePenetrationIncreased:
      return "runtime_contact_release_penetration_increased";
    case RuntimeCollisionTickReason::kBoundedContactPenetrationExceeded:
      return "runtime_bounded_contact_penetration_exceeds_policy";
  }
  return "runtime_collision_monitor_unknown";
}

const char* runtime_collision_query_failure_reason_name(
    RuntimeCollisionQueryFailureReason reason) noexcept {
  switch (reason) {
    case RuntimeCollisionQueryFailureReason::kNone:
      return "none";
    case RuntimeCollisionQueryFailureReason::kUnsupported:
      return "runtime_query_unsupported";
    case RuntimeCollisionQueryFailureReason::kEngineBusy:
      return "runtime_query_engine_busy";
    case RuntimeCollisionQueryFailureReason::kSnapshotBodyLimitExceeded:
      return "runtime_query_snapshot_body_limit_exceeded";
    case RuntimeCollisionQueryFailureReason::kSnapshotInvalid:
      return "runtime_query_snapshot_invalid";
    case RuntimeCollisionQueryFailureReason::kSnapshotSyncFailed:
      return "runtime_query_snapshot_sync_failed";
    case RuntimeCollisionQueryFailureReason::kObservedStateShapeMismatch:
      return "runtime_query_observed_state_shape_mismatch";
    case RuntimeCollisionQueryFailureReason::kObservedStateNotFinite:
      return "runtime_query_observed_state_not_finite";
    case RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete:
      return "runtime_query_pair_diagnostics_incomplete";
    case RuntimeCollisionQueryFailureReason::kObservationCapacityExceeded:
      return "runtime_query_observation_capacity_exceeded";
    case RuntimeCollisionQueryFailureReason::kTimeout:
      return "runtime_query_timeout";
    case RuntimeCollisionQueryFailureReason::kEngineException:
      return "runtime_query_engine_exception";
  }
  return "runtime_query_unknown";
}

const char* runtime_collision_query_prepare_status_name(
    RuntimeCollisionQueryPrepareStatus status) noexcept {
  switch (status) {
    case RuntimeCollisionQueryPrepareStatus::kReady:
      return "ready";
    case RuntimeCollisionQueryPrepareStatus::kUnsupported:
      return "unsupported";
    case RuntimeCollisionQueryPrepareStatus::kJointCountExceeded:
      return "joint_count_exceeded";
    case RuntimeCollisionQueryPrepareStatus::kInvalidJointOrder:
      return "invalid_joint_order";
    case RuntimeCollisionQueryPrepareStatus::kEngineBusy:
      return "engine_busy";
    case RuntimeCollisionQueryPrepareStatus::kEngineError:
      return "engine_error";
  }
  return "unknown";
}

const char* runtime_collision_engine_status_name(CollisionEngineStatus status) noexcept {
  switch (status) {
    case CollisionEngineStatus::kOk:
      return "ok";
    case CollisionEngineStatus::kTimeout:
      return "timeout";
    case CollisionEngineStatus::kUnsupportedGeometry:
      return "unsupported_geometry";
    case CollisionEngineStatus::kDiagnosticIncomplete:
      return "diagnostic_incomplete";
    case CollisionEngineStatus::kCancelled:
      return "cancelled";
    case CollisionEngineStatus::kEngineError:
      return "engine_error";
  }
  return "unknown";
}

RuntimeCollisionQuerySession::RuntimeCollisionQuerySession(
    RuntimeCollisionStateProjection projection, ICollisionWorldSnapshotPublisher& publisher,
    std::shared_ptr<ICollisionQueryEngine> engine, std::string active_contact_policy_id,
    std::int64_t query_timeout_ns)
    : projection_(std::move(projection)),
      publisher_(&publisher),
      engine_(std::move(engine)),
      capability_(engine_ ? &engine_->capability() : nullptr),
      active_contact_policy_id_(std::move(active_contact_policy_id)),
      query_timeout_ns_(query_timeout_ns),
      query_positions_rad_(projection_.template_positions_rad) {}

RuntimeCollisionQueryBundlePtr RuntimeCollisionQuerySession::acquire_bundle() const {
  return publisher_ != nullptr ? publisher_->published_runtime_collision_query_bundle() : nullptr;
}

namespace {

bool policy_revisions_equal(const CollisionPolicyRevision& lhs,
                            const CollisionPolicyRevision& rhs) noexcept {
  return lhs.revision_id == rhs.revision_id && lhs.content_hash == rhs.content_hash;
}

bool joint_names_unique(std::span<const std::string> names) noexcept {
  for (std::size_t i = 0; i < names.size(); ++i) {
    for (std::size_t j = i + 1; j < names.size(); ++j) {
      if (names[i] == names[j]) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::optional<ErrorDTO> RuntimeCollisionQuerySession::prepare(
    const RuntimeCollisionQueryBundlePtr& initial_bundle) {
  if (!engine_) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision query engine is missing",
                    {{"reason", "missing_runtime_collision_query_engine"}}};
  }
  const auto& capability = *capability_;
  const auto capability_status = runtime_collision_query_capability_status(capability);
  if (capability_status != RuntimeCollisionQueryCapabilityStatus::kAdmitted) {
    return ErrorDTO{
        kErrorValidationCollision,
        "Collision engine is not admitted for runtime-safety queries",
        {{"reason", "runtime_collision_query_capability_unsupported"},
         {"capability_status", runtime_collision_query_capability_status_name(capability_status)},
         {"engine_id", capability.engine_id}}};
  }
  if (query_timeout_ns_ <= 0 ||
      capability.runtime_query_qualified_wcet_ns > static_cast<std::uint64_t>(query_timeout_ns_) ||
      (capability.runtime_query_snapshot_change_qualified_wcet_ns > 0 &&
       capability.runtime_query_snapshot_change_qualified_wcet_ns >
           static_cast<std::uint64_t>(query_timeout_ns_))) {
    return ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision query budget is below the engine's qualified WCET",
        {{"reason", "runtime_collision_query_budget_below_qualified_wcet"},
         {"query_timeout_ns", std::to_string(query_timeout_ns_)},
         {"warm_qualified_wcet_ns", std::to_string(capability.runtime_query_qualified_wcet_ns)},
         {"snapshot_change_qualified_wcet_ns",
          std::to_string(capability.runtime_query_snapshot_change_qualified_wcet_ns)},
         {"engine_id", capability.engine_id}}};
  }
  if (projection_.query_joint_names.size() > capability.runtime_query_max_joints ||
      projection_.source_joint_names.size() > capability.runtime_query_max_joints) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision projection exceeds engine joint bound",
                    {{"reason", "runtime_collision_query_joint_bound_exceeded"},
                     {"query_joint_count", std::to_string(projection_.query_joint_names.size())},
                     {"source_joint_count", std::to_string(projection_.source_joint_names.size())},
                     {"joint_bound", std::to_string(capability.runtime_query_max_joints)}}};
  }
  if (projection_.query_joint_names.empty() || projection_.source_joint_names.empty() ||
      projection_.template_positions_rad.size() != projection_.query_joint_names.size() ||
      projection_.source_to_query_indices.size() != projection_.source_joint_names.size() ||
      !joint_names_unique(projection_.query_joint_names) ||
      !joint_names_unique(projection_.source_joint_names) ||
      !std::all_of(projection_.template_positions_rad.begin(),
                   projection_.template_positions_rad.end(),
                   [](double value) { return std::isfinite(value); })) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision projection shape is invalid",
                    {{"reason", "runtime_collision_projection_invalid_shape"}}};
  }
  for (std::size_t i = 0; i < projection_.source_joint_names.size(); ++i) {
    const auto query_index = projection_.source_to_query_indices[i];
    if (query_index >= projection_.query_joint_names.size() ||
        projection_.source_joint_names[i] != projection_.query_joint_names[query_index]) {
      return ErrorDTO{kErrorValidationCollision,
                      "Runtime collision projection joint mapping is invalid",
                      {{"reason", "runtime_collision_projection_invalid_mapping"}}};
    }
  }
  if (!initial_bundle || !initial_bundle->world) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision snapshot is unavailable",
                    {{"reason", "runtime_collision_snapshot_not_published"}}};
  }
  if (initial_bundle->world->bodies.size() > capability.runtime_query_max_world_bodies) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision bundle exceeds engine body bound",
                    {{"reason", "runtime_collision_bundle_capacity_exceeded"},
                     {"body_count", std::to_string(initial_bundle->world->bodies.size())},
                     {"body_bound", std::to_string(capability.runtime_query_max_world_bodies)}}};
  }
  if (!initial_bundle->lookup_order_valid) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision bundle lookup order is invalid",
                    {{"reason", "runtime_collision_bundle_lookup_order_invalid"}}};
  }
  if (!bind_policy(initial_bundle)) {
    std::map<std::string, std::string> details{
        {"reason", runtime_collision_tick_reason_name(policy_binding_error_)},
        {"engine_id", capability.engine_id}};
    if (policy_binding_required_capability_ != nullptr) {
      details["required_capability"] = policy_binding_required_capability_;
    }
    return ErrorDTO{kErrorValidationCollision, "Runtime collision policy preparation failed",
                    std::move(details)};
  }
  const auto query_prepare = engine_->prepare_runtime_query(projection_.query_joint_names);
  if (query_prepare != RuntimeCollisionQueryPrepareStatus::kReady) {
    return ErrorDTO{kErrorValidationCollision,
                    "Runtime collision query preparation failed",
                    {{"reason", "runtime_collision_query_prepare_failed"},
                     {"prepare_status", runtime_collision_query_prepare_status_name(query_prepare)},
                     {"engine_id", capability.engine_id}}};
  }
  const auto snapshot_prepare = engine_->prepare_runtime_snapshot(*initial_bundle->world);
  if (snapshot_prepare != RuntimeCollisionQueryPrepareStatus::kReady) {
    return ErrorDTO{
        kErrorValidationCollision,
        "Runtime collision snapshot preparation failed",
        {{"reason", "runtime_collision_snapshot_prepare_failed"},
         {"prepare_status", runtime_collision_query_prepare_status_name(snapshot_prepare)},
         {"engine_id", capability.engine_id}}};
  }
  return std::nullopt;
}

bool RuntimeCollisionQuerySession::bind_distance_selection(
    const RuntimeCollisionQueryBundlePtr& bundle) noexcept {
  policy_binding_required_capability_ = nullptr;
  if (!bundle || !capability_) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyUnavailable;
    return false;
  }
  if (!bundle->distance_selection_order_valid) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyIndexMismatch;
    return false;
  }

  const std::vector<CollisionPairId>* candidate = &bundle->selected_distance_pairs;
  if (!active_contact_policy_id_.empty()) {
    const auto grasp =
        bundle->contact_grasp_selected_distance_pairs.find(active_contact_policy_id_);
    if (grasp == bundle->contact_grasp_selected_distance_pairs.end()) {
      policy_binding_error_ = RuntimeCollisionTickReason::kActiveContactPolicyUnknown;
      return false;
    }
    candidate = &grasp->second;
  }

  if (candidate->size() > capability_->runtime_query_max_collision_pairs) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyCapacityExceeded;
    return false;
  }
  const auto requested_mode = candidate->empty()
                                  ? CollisionDistanceRequestMode::kNone
                                  : CollisionDistanceRequestMode::kSelectedCollidingPairs;
  if (!collision_engine_supports_distance_request_mode(*capability_, requested_mode)) {
    policy_binding_error_ = RuntimeCollisionTickReason::kDistanceRequestModeUnsupported;
    return false;
  }
  if (requested_mode != CollisionDistanceRequestMode::kNone &&
      !capability_->supports_penetration_depth) {
    policy_binding_error_ = RuntimeCollisionTickReason::kRequiredCapabilityUnsupported;
    policy_binding_required_capability_ = "penetration_depth";
    return false;
  }
  return true;
}

bool RuntimeCollisionQuerySession::bind_policy(
    const RuntimeCollisionQueryBundlePtr& bundle) noexcept {
  policy_binding_error_ = RuntimeCollisionTickReason::kPolicyUnavailable;
  policy_binding_required_capability_ = nullptr;
  if (!bundle || !bundle->policy || !bundle->policy_index) {
    return false;
  }
  if (!policy_revisions_equal(bundle->policy->revision,
                              bundle->policy_index->source_policy_revision)) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyIndexMismatch;
    return false;
  }
  // Policy revision is the immutable semantic identity. Freshness-only
  // publications may rebuild equivalent shared handles; retaining the existing
  // binding preserves execution-bound contact-release history. A publisher
  // that changes policy content without advancing this identity violates the
  // publication contract and must not replace the already-bound policy.
  if (bound_policy_ && policy_revisions_equal(bound_policy_->revision, bundle->policy->revision)) {
    if (policy_binding_valid_ && !bind_distance_selection(bundle)) {
      return false;
    }
    policy_binding_error_ =
        policy_binding_valid_ ? RuntimeCollisionTickReason::kNone : policy_binding_error_;
    return policy_binding_valid_;
  }
  const auto& capability = *capability_;
  const auto pair_bound = capability.runtime_query_max_collision_pairs;
  const bool policy_capacity_exceeded =
      bundle->policy->allowed_static_static_pairs.size() > pair_bound ||
      bundle->policy->allowed_touch_link_pairs.size() > pair_bound ||
      bundle->policy->support_contacts.size() > pair_bound ||
      bundle->policy->contact_grasps.size() > pair_bound ||
      bundle->policy_index->static_static_index.size() > pair_bound ||
      bundle->policy_index->touch_link_index.size() > pair_bound ||
      bundle->policy_index->support_contact_index.size() > pair_bound ||
      bundle->policy_index->contact_grasp_index.size() > pair_bound ||
      bundle->policy->support_contacts.size() > release_states_.size();
  if (policy_capacity_exceeded) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyCapacityExceeded;
    return false;
  }
  const bool index_content_mismatch =
      bundle->policy_index->static_static_index.size() !=
          bundle->policy->allowed_static_static_pairs.size() ||
      bundle->policy_index->touch_link_index.size() !=
          bundle->policy->allowed_touch_link_pairs.size() ||
      bundle->policy_index->support_contact_index.size() !=
          bundle->policy->support_contacts.size() ||
      bundle->policy_index->contact_grasp_index.size() != bundle->policy->contact_grasps.size();
  if (index_content_mismatch) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyIndexMismatch;
    return false;
  }
  const ContactGraspPolicyEntry* candidate_active_contact_policy = nullptr;
  if (!active_contact_policy_id_.empty()) {
    const auto active = bundle->policy_index->contact_grasp_index.find(active_contact_policy_id_);
    if (active == bundle->policy_index->contact_grasp_index.end()) {
      policy_binding_error_ = RuntimeCollisionTickReason::kActiveContactPolicyUnknown;
      return false;
    }
    if (!contact_grasp_policy_valid(active->second)) {
      policy_binding_error_ = RuntimeCollisionTickReason::kActiveContactPolicyInvalid;
      return false;
    }
    if (active->second.allowed_robot_links.size() > capability.runtime_query_max_joints) {
      policy_binding_error_ = RuntimeCollisionTickReason::kPolicyCapacityExceeded;
      return false;
    }
    candidate_active_contact_policy = &active->second;
  }
  for (const auto& [pair, policy] : bundle->policy_index->support_contact_index) {
    (void)pair;
    if (!support_contact_policy_entry_valid(policy)) {
      policy_binding_error_ = RuntimeCollisionTickReason::kContactReleasePolicyInvalid;
      return false;
    }
  }
  if (!bind_distance_selection(bundle)) {
    return false;
  }

  // Commit the new binding only after every candidate check passes. A malformed
  // publication cannot partially erase the previous generation's contact
  // history before the monitor faults closed.
  //
  // A rule change during execution keeps the observed history of every
  // support-contact pair whose rule is unchanged; new or changed pairs start
  // fresh and removed pairs drop out. The previous entries point into the
  // previous bundle, which stays pinned until the copy completes. The
  // start-of-motion contact baseline is not re-counted.
  const auto previous_bundle = std::move(bound_bundle_);
  const auto previous_states = release_states_;
  const auto previous_states_end = previous_states.begin() + release_state_count_;
  bound_bundle_ = bundle;
  bound_policy_ = bound_bundle_->policy.get();
  bound_policy_index_ = bound_bundle_->policy_index.get();
  active_contact_policy_ = candidate_active_contact_policy;
  release_state_count_ = 0;
  for (const auto& [pair, policy] : bundle->policy_index->support_contact_index) {
    auto& state = release_states_[release_state_count_++];
    state = ContactReleaseState{&pair, &policy, {}, false, std::nullopt};
    const auto previous =
        std::lower_bound(previous_states.begin(), previous_states_end, pair,
                         [](const ContactReleaseState& entry, const CollisionPairId& key) {
                           return *entry.pair < key;
                         });
    if (previous != previous_states_end && *previous->pair == pair &&
        *previous->policy == policy) {
      state.transition_state = previous->transition_state;
      state.observed_in_contact = previous->observed_in_contact;
      state.observed_clearance_m = previous->observed_clearance_m;
    }
  }
  policy_binding_valid_ = true;
  policy_binding_error_ = RuntimeCollisionTickReason::kNone;
  return true;
}

RuntimeCollisionTickOutcome RuntimeCollisionQuerySession::project_observed_state(
    std::span<const double> observed_positions_rad) noexcept {
  RuntimeCollisionTickOutcome out;
  if (observed_positions_rad.size() != projection_.source_joint_names.size()) {
    out.reason = RuntimeCollisionTickReason::kObservedStateShapeMismatch;
    return out;
  }
  if (!std::all_of(observed_positions_rad.begin(), observed_positions_rad.end(),
                   [](double value) { return std::isfinite(value); })) {
    out.reason = RuntimeCollisionTickReason::kObservedStateNotFinite;
    return out;
  }
  if (projection_.source_joint_names == projection_.query_joint_names) {
    std::copy(observed_positions_rad.begin(), observed_positions_rad.end(),
              query_positions_rad_.begin());
  } else {
    std::copy(projection_.template_positions_rad.begin(), projection_.template_positions_rad.end(),
              query_positions_rad_.begin());
    for (std::size_t i = 0; i < observed_positions_rad.size(); ++i) {
      query_positions_rad_[projection_.source_to_query_indices[i]] = observed_positions_rad[i];
    }
  }
  out.status = RuntimeCollisionTickStatus::kClear;
  out.reason = RuntimeCollisionTickReason::kNone;
  return out;
}

RuntimeCollisionTickOutcome RuntimeCollisionQuerySession::classify(
    const RuntimeCollisionQueryResultView& query_result) noexcept {
  RuntimeCollisionTickOutcome out;
  out.query_duration_ns = query_result.query_duration_ns;
  out.query_executed = true;
  out.engine_status = query_result.engine_status;
  out.engine_failure_reason = query_result.failure_reason;
  const auto pair_bound = capability_->runtime_query_max_collision_pairs;
  if (query_result.collision_pairs.size() > pair_bound ||
      query_result.clearance_pairs.size() > pair_bound) {
    out.reason = RuntimeCollisionTickReason::kObservationCapacityExceeded;
    out.engine_failure_reason = RuntimeCollisionQueryFailureReason::kObservationCapacityExceeded;
    out.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
    return out;
  }
  if (query_result.status == RuntimeCollisionQueryStatus::kInconclusive) {
    const bool production_pair_diagnostics_incomplete =
        query_result.failure_reason ==
            RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete &&
        query_result.engine_status == CollisionEngineStatus::kDiagnosticIncomplete;
    if (production_pair_diagnostics_incomplete) {
      const auto pair_is_colliding = [&](const CollisionPairId& pair) noexcept {
        return std::any_of(query_result.collision_pairs.begin(), query_result.collision_pairs.end(),
                           [&](const RuntimeCollisionPairObservation& observation) {
                             return observation.pair != nullptr && *observation.pair == pair;
                           });
      };
      const auto pair_has_complete_penetration = [&](const CollisionPairId& pair) noexcept {
        bool saw_metric = false;
        bool saw_invalid_metric = false;
        const auto inspect = [&](const auto& observations) noexcept {
          for (const auto& observation : observations) {
            if (observation.pair == nullptr || *observation.pair != pair ||
                !observation.signed_clearance_m.has_value()) {
              continue;
            }
            saw_metric = true;
            const double clearance = *observation.signed_clearance_m;
            saw_invalid_metric = saw_invalid_metric || !std::isfinite(clearance) || clearance > 0.0;
          }
        };
        inspect(query_result.collision_pairs);
        inspect(query_result.clearance_pairs);
        return saw_metric && !saw_invalid_metric;
      };

      // All configured support-contact identities are active allow-list
      // candidates. Bounded penetration therefore applies when a configured
      // pair first appears as well as when it was already touching at the
      // execution baseline.
      for (std::size_t index = 0; index < release_state_count_; ++index) {
        const auto& state = release_states_[index];
        if (state.pair == nullptr || state.policy == nullptr) {
          continue;
        }
        const auto& policy = *state.policy;
        const bool metric_required =
            policy.mode == CollisionContactPolicyMode::kBoundedPenetration ||
            (policy.mode == CollisionContactPolicyMode::kContactRelease &&
             policy.release_evidence_mode == ContactReleaseEvidenceMode::kSignedPenetration);
        if (!metric_required || !pair_is_colliding(*state.pair) ||
            pair_has_complete_penetration(*state.pair)) {
          continue;
        }
        out.reason = RuntimeCollisionTickReason::kContactReleaseMissingClearance;
        out.pair = state.pair;
        out.pair_classification = CollisionPairClassification::kDiagnosticIncomplete;
        out.contact_policy_mode = policy.mode;
        if (policy.mode == CollisionContactPolicyMode::kContactRelease) {
          out.release_evidence_mode = policy.release_evidence_mode;
        } else {
          out.max_contact_penetration_m = policy.max_contact_penetration_m;
        }
        out.contact_present = true;
        return out;
      }

      if (active_contact_policy_ != nullptr) {
        const auto missing_grasp = std::find_if(
            query_result.collision_pairs.begin(), query_result.collision_pairs.end(),
            [&](const RuntimeCollisionPairObservation& observation) {
              return observation.pair != nullptr &&
                     contact_grasp_pair_matches(*observation.pair, *active_contact_policy_) &&
                     !pair_has_complete_penetration(*observation.pair);
            });
        if (missing_grasp != query_result.collision_pairs.end()) {
          out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
          out.pair = missing_grasp->pair;
          out.pair_classification = CollisionPairClassification::kDiagnosticIncomplete;
          out.contact_present = true;
          out.contact_grasp_violation = RuntimeContactGraspViolation::kMissingClearance;
          return out;
        }
      }
    }
    out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
    return out;
  }
  if (query_result.engine_status != CollisionEngineStatus::kOk ||
      query_result.failure_reason != RuntimeCollisionQueryFailureReason::kNone ||
      (query_result.status == RuntimeCollisionQueryStatus::kCollision &&
       query_result.collision_pairs.empty()) ||
      (query_result.status == RuntimeCollisionQueryStatus::kClear &&
       !query_result.collision_pairs.empty())) {
    out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
    out.engine_failure_reason = RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
    out.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
    return out;
  }
  for (const auto& observation : query_result.clearance_pairs) {
    if (observation.pair == nullptr || observation.pair->body_a.empty() ||
        observation.pair->body_b.empty() || observation.pair->body_b < observation.pair->body_a ||
        !observation.signed_clearance_m.has_value() ||
        !std::isfinite(*observation.signed_clearance_m)) {
      out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
      out.engine_failure_reason = RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
      out.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
      return out;
    }
  }
  if (query_result.status == RuntimeCollisionQueryStatus::kClear) {
    out.status = RuntimeCollisionTickStatus::kClear;
    out.reason = RuntimeCollisionTickReason::kNone;
    return out;
  }

  for (const auto& observation : query_result.collision_pairs) {
    if (observation.pair == nullptr || observation.pair->body_a.empty() ||
        observation.pair->body_b.empty() || observation.pair->body_b < observation.pair->body_a ||
        (observation.signed_clearance_m.has_value() &&
         !std::isfinite(*observation.signed_clearance_m))) {
      out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
      out.engine_failure_reason = RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
      return out;
    }
    const auto& pair = *observation.pair;
    CollisionPairClassification classification = CollisionPairClassification::kBlockingCollision;
    if (bound_policy_index_->touch_link_index.find(pair) !=
        bound_policy_index_->touch_link_index.end()) {
      classification = CollisionPairClassification::kAllowedTouchLink;
    } else if (bound_policy_index_->static_static_index.find(pair) !=
               bound_policy_index_->static_static_index.end()) {
      classification = CollisionPairClassification::kAllowedStaticStatic;
    } else if (bound_policy_index_->support_contact_index.find(pair) !=
               bound_policy_index_->support_contact_index.end()) {
      classification = CollisionPairClassification::kSupportContactContinued;
    } else if (active_contact_policy_ != nullptr &&
               contact_grasp_pair_matches(pair, *active_contact_policy_)) {
      const auto clearance = observation.signed_clearance_m;
      if (!clearance.has_value() || !std::isfinite(*clearance) || *clearance > 0.0) {
        out.status = RuntimeCollisionTickStatus::kInconclusive;
        out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
        out.engine_failure_reason = RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
        out.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
        out.pair = &pair;
        out.signed_clearance_m = clearance;
        out.contact_grasp_violation = RuntimeContactGraspViolation::kMissingClearance;
        return out;
      }
      if (std::max(0.0, -*clearance) <= active_contact_policy_->max_penetration_m + 1e-9) {
        classification = CollisionPairClassification::kAllowedContactGrasp;
      } else {
        out.contact_grasp_violation = RuntimeContactGraspViolation::kPenetrationExceeded;
      }
    }

    if (classification == CollisionPairClassification::kBlockingCollision) {
      out.status = RuntimeCollisionTickStatus::kCollision;
      out.reason = RuntimeCollisionTickReason::kBlockingCollision;
      out.pair = &pair;
      out.pair_classification = classification;
      out.signed_clearance_m = observation.signed_clearance_m;
      return out;
    }
  }
  out.status = RuntimeCollisionTickStatus::kClear;
  out.reason = RuntimeCollisionTickReason::kNone;
  return out;
}

RuntimeCollisionTickOutcome RuntimeCollisionQuerySession::update_contact_release(
    const RuntimeCollisionQueryResultView& query_result) noexcept {
  RuntimeCollisionTickOutcome clear;
  clear.status = RuntimeCollisionTickStatus::kClear;
  clear.reason = RuntimeCollisionTickReason::kNone;
  clear.query_duration_ns = query_result.query_duration_ns;
  clear.query_executed = true;
  clear.engine_status = query_result.engine_status;
  for (std::size_t i = 0; i < release_state_count_; ++i) {
    release_states_[i].observed_in_contact = false;
    release_states_[i].observed_clearance_m.reset();
  }

  auto observe = [&](const RuntimeCollisionPairObservation& observation, bool in_contact) {
    if (observation.pair == nullptr) {
      return;
    }
    const auto states_end = release_states_.begin() + release_state_count_;
    const auto state_it =
        std::lower_bound(release_states_.begin(), states_end, *observation.pair,
                         [](const ContactReleaseState& state, const CollisionPairId& pair) {
                           return *state.pair < pair;
                         });
    if (state_it == states_end || *state_it->pair != *observation.pair) {
      return;
    }
    state_it->observed_in_contact = state_it->observed_in_contact || in_contact;
    if (!observation.signed_clearance_m.has_value()) {
      return;
    }
    auto& observed = state_it->observed_clearance_m;
    if (!observed || *observation.signed_clearance_m < *observed) {
      observed = observation.signed_clearance_m;
    }
  };

  for (const auto& observation : query_result.collision_pairs) {
    observe(observation, true);
  }
  for (const auto& observation : query_result.clearance_pairs) {
    observe(observation, false);
  }
  for (std::size_t i = 0; i < release_state_count_; ++i) {
    auto& state = release_states_[i];
    const auto& policy = *state.policy;
    const auto previous_state = state.transition_state;
    const auto transition = advance_contact_release_sample(
        policy, ContactReleaseSampleEvidence{state.observed_in_contact, state.observed_clearance_m},
        state.transition_state);
    if (transition.accepted()) {
      continue;
    }

    clear.reason = transition_failure_reason(transition.failure);
    clear.status = transition.failure == ContactReleaseTransitionFailure::kInvalidPolicy ||
                           transition.failure ==
                               ContactReleaseTransitionFailure::kContactProcessNotImplemented ||
                           transition.failure ==
                               ContactReleaseTransitionFailure::kMissingOrInvalidSignedPenetration
                       ? RuntimeCollisionTickStatus::kInconclusive
                       : RuntimeCollisionTickStatus::kCollision;
    clear.pair_classification = clear.status == RuntimeCollisionTickStatus::kInconclusive
                                    ? CollisionPairClassification::kDiagnosticIncomplete
                                    : CollisionPairClassification::kBlockingCollision;
    clear.pair = state.pair;
    clear.signed_clearance_m = state.observed_clearance_m;
    clear.contact_policy_mode = policy.mode;
    if (policy.mode == CollisionContactPolicyMode::kContactRelease) {
      clear.release_evidence_mode = policy.release_evidence_mode;
    }
    clear.contact_present = state.observed_in_contact;
    clear.initial_penetration_m = state.transition_state.initial_depth_m;
    if (previous_state.initialized && previous_state.started_in_contact) {
      clear.previous_penetration_m = previous_state.previous_depth_m;
    }
    clear.current_penetration_m = transition.penetration_depth_m;
    if (policy.mode == CollisionContactPolicyMode::kBoundedPenetration) {
      clear.max_contact_penetration_m = policy.max_contact_penetration_m;
    }
    return clear;
  }
  clear.pair = nullptr;
  clear.signed_clearance_m.reset();
  clear.initial_penetration_m.reset();
  clear.previous_penetration_m.reset();
  clear.current_penetration_m.reset();
  clear.max_contact_penetration_m.reset();
  return clear;
}

std::size_t RuntimeCollisionQuerySession::current_contact_pair_count(
    const RuntimeCollisionQueryResultView& query_result) const noexcept {
  std::size_t count = 0;
  for (std::size_t index = 0; index < release_state_count_; ++index) {
    const auto* configured_pair = release_states_[index].pair;
    if (configured_pair == nullptr) {
      continue;
    }
    const bool present =
        std::any_of(query_result.collision_pairs.begin(), query_result.collision_pairs.end(),
                    [&](const RuntimeCollisionPairObservation& observation) {
                      return observation.pair != nullptr && *observation.pair == *configured_pair;
                    });
    if (present) {
      ++count;
    }
  }
  return count;
}

RuntimeContactReleaseStateSnapshot RuntimeCollisionQuerySession::contact_release_state_snapshot()
    const noexcept {
  RuntimeContactReleaseStateSnapshot snapshot;
  snapshot.count = release_state_count_;
  snapshot.valid = policy_binding_valid_;
  snapshot.binding_owner = bound_bundle_;
  for (std::size_t index = 0; index < release_state_count_; ++index) {
    const auto& source = release_states_[index];
    auto& target = snapshot.entries[index];
    target.pair = source.pair;
    target.release_state = source.transition_state;
    if (source.policy != nullptr &&
        source.policy->mode == CollisionContactPolicyMode::kBoundedPenetration) {
      target.support_state =
          source.transition_state.initialized && source.transition_state.in_contact
              ? SupportContactEvaluationState::kContactActive
              : SupportContactEvaluationState::kNotInContact;
    } else {
      target.support_state =
          !source.transition_state.initialized || !source.transition_state.started_in_contact
              ? SupportContactEvaluationState::kNotInContact
          : source.transition_state.in_contact ? SupportContactEvaluationState::kContactActive
                                               : SupportContactEvaluationState::kSeparated;
    }
  }
  return snapshot;
}

RuntimeCollisionTickOutcome RuntimeCollisionQuerySession::tick(
    const RuntimeCollisionQueryBundlePtr& bundle, std::span<const double> observed_positions_rad,
    std::int64_t state_time_ns) noexcept {
  if (!bundle || !bundle->world) {
    return {};
  }
  const auto& capability = *capability_;
  if (bundle->world->bodies.size() > capability.runtime_query_max_world_bodies) {
    RuntimeCollisionTickOutcome out;
    out.reason = RuntimeCollisionTickReason::kBundleCapacityExceeded;
    return out;
  }
  if (!bundle->lookup_order_valid) {
    RuntimeCollisionTickOutcome out;
    out.reason = RuntimeCollisionTickReason::kBundleLookupOrderInvalid;
    return out;
  }
  if (!bind_policy(bundle)) {
    RuntimeCollisionTickOutcome out;
    out.reason = policy_binding_error_;
    return out;
  }
  auto projection = project_observed_state(observed_positions_rad);
  if (projection.status == RuntimeCollisionTickStatus::kInconclusive) {
    return projection;
  }
  CollisionDistanceRequestMode distance_request_mode = CollisionDistanceRequestMode::kNone;
  std::span<const CollisionPairId> selected_distance_pairs;
  const std::vector<CollisionPairId>* prepared_selection = &bundle->selected_distance_pairs;
  if (!active_contact_policy_id_.empty()) {
    const auto grasp =
        bundle->contact_grasp_selected_distance_pairs.find(active_contact_policy_id_);
    if (grasp == bundle->contact_grasp_selected_distance_pairs.end()) {
      RuntimeCollisionTickOutcome out;
      out.reason = RuntimeCollisionTickReason::kPolicyIndexMismatch;
      return out;
    }
    prepared_selection = &grasp->second;
  }
  if (!prepared_selection->empty()) {
    distance_request_mode = CollisionDistanceRequestMode::kSelectedCollidingPairs;
    selected_distance_pairs = *prepared_selection;
  }
  const auto query_result = engine_->check_runtime(
      *bundle->world,
      RuntimeCollisionQueryRequest{query_positions_rad_, state_time_ns, query_timeout_ns_,
                                   distance_request_mode, selected_distance_pairs});
  const auto current_pair_count = current_contact_pair_count(query_result);
  if (!initial_contact_pair_count_initialized_ &&
      query_result.status != RuntimeCollisionQueryStatus::kInconclusive) {
    initial_contact_pair_count_ = current_pair_count;
    initial_contact_pair_count_initialized_ = true;
  }
  const auto annotate = [&](RuntimeCollisionTickOutcome out) {
    out.distance_request_mode = distance_request_mode;
    out.evaluated_distance_pair_count = query_result.clearance_pairs.size();
    out.initial_contact_pair_count = initial_contact_pair_count_;
    out.current_contact_pair_count = current_pair_count;
    if (out.pair != nullptr) {
      out.contact_present =
          std::any_of(query_result.collision_pairs.begin(), query_result.collision_pairs.end(),
                      [&](const RuntimeCollisionPairObservation& observation) {
                        return observation.pair != nullptr && *observation.pair == *out.pair;
                      });
    }
    return out;
  };
  auto classified = classify(query_result);
  if (classified.status != RuntimeCollisionTickStatus::kClear) {
    return annotate(std::move(classified));
  }
  return annotate(update_contact_release(query_result));
}

}  // namespace manyforge::core
