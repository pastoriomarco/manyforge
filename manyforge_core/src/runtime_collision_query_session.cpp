#include <algorithm>
#include <cmath>
#include <manyforge_core/runtime_collision_query_session.hpp>
#include <map>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

RuntimeCollisionTickReason transition_failure_reason(
    ContactReleaseTransitionFailure failure) noexcept {
  switch (failure) {
    case ContactReleaseTransitionFailure::kNone:
      return RuntimeCollisionTickReason::kNone;
    case ContactReleaseTransitionFailure::kInvalidPolicy:
      return RuntimeCollisionTickReason::kContactReleasePolicyInvalid;
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
    case ContactReleaseTransitionFailure::kContactPenetrationExceeded:
      return RuntimeCollisionTickReason::kContactPenetrationExceeded;
    case ContactReleaseTransitionFailure::kContactCompressionStepExceeded:
      return RuntimeCollisionTickReason::kContactCompressionStepExceeded;
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
    case RuntimeCollisionTickReason::kContactRuleInvalid:
      return "runtime_contact_rule_invalid";
    case RuntimeCollisionTickReason::kObservedStateShapeMismatch:
      return "runtime_collision_projection_observed_shape_mismatch";
    case RuntimeCollisionTickReason::kObservedStateNotFinite:
      return "runtime_collision_projection_observed_value_not_finite";
    // That component's stream condition; `peer_component_id` names it. The
    // Stop is the one of the moving robot's own lost observation.
    case RuntimeCollisionTickReason::kOtherComponentStateStale:
      return "component_stream_stale";
    case RuntimeCollisionTickReason::kOtherComponentStateAbsent:
      return "component_stream_absent";
    case RuntimeCollisionTickReason::kObservationCapacityExceeded:
      return "runtime_collision_query_observation_capacity_exceeded";
    case RuntimeCollisionTickReason::kEngineInconclusive:
      return "runtime_collision_monitor_inconclusive";
    case RuntimeCollisionTickReason::kBlockingCollision:
      return "runtime_collision_detected";
    case RuntimeCollisionTickReason::kContactReleasePolicyInvalid:
      return "runtime_contact_release_policy_invalid";
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
    case RuntimeCollisionTickReason::kContactPenetrationExceeded:
      return "runtime_contact_penetration_exceeded";
    case RuntimeCollisionTickReason::kContactCompressionStepExceeded:
      return "runtime_contact_compression_step_exceeded";
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
    case RuntimeCollisionQueryFailureReason::kMeshNotClosed:
      return "runtime_query_mesh_not_closed";
    case RuntimeCollisionQueryFailureReason::kMeshDepthVertexCapReached:
      return "runtime_query_mesh_depth_vertex_cap_reached";
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
    std::shared_ptr<ICollisionQueryEngine> engine, std::int64_t query_timeout_ns)
    : projection_(std::move(projection)),
      publisher_(&publisher),
      engine_(std::move(engine)),
      capability_(engine_ ? &engine_->capability() : nullptr),
      query_timeout_ns_(query_timeout_ns),
      query_positions_rad_(projection_.template_positions_rad) {}

RuntimeCollisionQueryBundlePtr RuntimeCollisionQuerySession::acquire_bundle() {
  auto published =
      publisher_ != nullptr ? publisher_->published_runtime_collision_query_bundle() : nullptr;
  if (!admitted_policy_ || !published || !published->world) {
    return published;
  }
  // The world follows the latest publication (last arrival wins); the policy
  // stays the one admitted for this move. Rebuild only when the world changes.
  if (!admitted_policy_bundle_ || admitted_policy_bundle_->world != published->world) {
    auto rebuilt = build_runtime_collision_query_bundle(
        CollisionWorldPolicySnapshotBundle{published->world, *admitted_policy_},
        published->projection_revision);
    auto own = std::make_shared<RuntimeCollisionQueryBundle>(*rebuilt);
    own->admitted_operation_policy = true;
    admitted_policy_bundle_ = std::move(own);
  }
  return admitted_policy_bundle_;
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
    const RuntimeCollisionQueryBundlePtr& initial_bundle, bool keep_admitted_policy) {
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
  if (keep_admitted_policy && initial_bundle->policy) {
    auto own = std::make_shared<RuntimeCollisionQueryBundle>(*initial_bundle);
    own->admitted_operation_policy = true;
    admitted_policy_ = own->policy;
    admitted_policy_bundle_ = std::move(own);
  }
  if (!bind_policy(admitted_policy_bundle_ ? admitted_policy_bundle_ : initial_bundle)) {
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
  std::size_t tracked_rule_count = 0U;
  for (const auto& [pair, rule] : bundle->policy_index->contact_rule_index) {
    static_cast<void>(pair);
    if (contact_rule_is_tracked(rule)) {
      ++tracked_rule_count;
    }
  }
  const bool policy_capacity_exceeded =
      bundle->policy->allowed_touch_link_pairs.size() > pair_bound ||
      bundle->policy->contact_rules.size() > pair_bound ||
      bundle->policy_index->touch_link_index.size() > pair_bound ||
      bundle->policy_index->contact_rule_index.size() > pair_bound ||
      tracked_rule_count > release_states_.size();
  if (policy_capacity_exceeded) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyCapacityExceeded;
    return false;
  }
  const bool index_content_mismatch =
      bundle->policy_index->touch_link_index.size() !=
          bundle->policy->allowed_touch_link_pairs.size() ||
      bundle->policy_index->contact_rule_index.size() !=
          bundle->policy->contact_rules.size();
  if (index_content_mismatch) {
    policy_binding_error_ = RuntimeCollisionTickReason::kPolicyIndexMismatch;
    return false;
  }
  for (const auto& [pair, rule] : bundle->policy_index->contact_rule_index) {
    static_cast<void>(pair);
    if (!contact_rule_valid(rule)) {
      policy_binding_error_ = contact_rule_is_tracked(rule)
                                  ? RuntimeCollisionTickReason::kContactReleasePolicyInvalid
                                  : RuntimeCollisionTickReason::kContactRuleInvalid;
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
  // tracked contact-rule pair whose value is unchanged; new or changed pairs start
  // fresh and removed pairs drop out. The previous entries point into the
  // previous bundle, which stays pinned until the copy completes. The
  // start-of-motion contact baseline is not re-counted.
  const auto previous_bundle = std::move(bound_bundle_);
  const auto previous_states = release_states_;
  const auto previous_states_end = previous_states.begin() + release_state_count_;
  bound_bundle_ = bundle;
  bound_policy_ = bound_bundle_->policy.get();
  bound_policy_index_ = bound_bundle_->policy_index.get();
  release_state_count_ = 0;
  for (const auto& [pair, policy] : bundle->policy_index->contact_rule_index) {
    if (!contact_rule_is_tracked(policy)) {
      continue;
    }
    auto& state = release_states_[release_state_count_++];
    state = ContactReleaseState{&pair, &policy, {}, false, std::nullopt, std::nullopt};
    const auto previous =
        std::lower_bound(previous_states.begin(), previous_states_end, pair,
                         [](const ContactReleaseState& entry, const CollisionPairId& key) {
                           return *entry.pair < key;
                         });
    if (previous != previous_states_end && *previous->pair == pair &&
        previous->policy->value == policy.value) {
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
    std::span<const double> observed_positions_rad, const std::int64_t now_ns) noexcept {
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
  // The other components of the cell, where they are now.
  if (const auto unavailable =
          overlay_cell_model_state(projection_.live_sources, now_ns, query_positions_rad_)) {
    out.reason = unavailable.verdict == PeerStateVerdict::kStale
                     ? RuntimeCollisionTickReason::kOtherComponentStateStale
                     : RuntimeCollisionTickReason::kOtherComponentStateAbsent;
    out.other_component = &unavailable.source->component_id;
    return out;
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
    // A mesh pair without a depth (OD-363) lacks its metric like any other
    // incomplete pair, so the rule that needs it is named the same way.
    const bool production_pair_diagnostics_incomplete =
        (query_result.failure_reason ==
             RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete ||
         query_result.failure_reason == RuntimeCollisionQueryFailureReason::kMeshNotClosed ||
         query_result.failure_reason ==
             RuntimeCollisionQueryFailureReason::kMeshDepthVertexCapReached) &&
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

      // Every tracked contact-rule pair is an active allow-list candidate.
      // A whole-move depth limit therefore applies when a pair first appears
      // as well as when it was already touching at the execution baseline.
      for (std::size_t index = 0; index < release_state_count_; ++index) {
        const auto& state = release_states_[index];
        if (state.pair == nullptr || state.policy == nullptr) {
          continue;
        }
        const auto& policy = *state.policy;
        const bool metric_required = contact_rule_needs_signed_distance(policy);
        if (!metric_required || !pair_is_colliding(*state.pair) ||
            pair_has_complete_penetration(*state.pair)) {
          continue;
        }
        out.reason = RuntimeCollisionTickReason::kContactReleaseMissingClearance;
        out.pair = state.pair;
        out.pair_classification = CollisionPairClassification::kDiagnosticIncomplete;
        out.contact_rule = &policy;
        out.contact_present = true;
        return out;
      }

      // A colliding pair under a contact rule with an end window and a
      // depth limit needs its depth too.
      for (const auto& observation : query_result.collision_pairs) {
        if (observation.pair == nullptr) {
          continue;
        }
        const auto rule = bound_policy_index_->contact_rule_index.find(*observation.pair);
        if (rule == bound_policy_index_->contact_rule_index.end() ||
            contact_rule_kind(rule->second) != ContactRuleKind::kWindowedContact ||
            !contact_rule_needs_signed_distance(rule->second) ||
            pair_has_complete_penetration(*observation.pair)) {
          continue;
        }
        out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
        out.pair = observation.pair;
        out.pair_classification = CollisionPairClassification::kDiagnosticIncomplete;
        out.contact_rule = &rule->second;
        out.contact_present = true;
        out.contact_rule_violation = RuntimeContactRuleViolation::kMissingClearance;
        return out;
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
    } else if (bound_policy_->static_pair(pair)) {
      classification = CollisionPairClassification::kAllowedStaticStatic;
    } else if (const auto rule = bound_policy_index_->contact_rule_index.find(pair);
               rule != bound_policy_index_->contact_rule_index.end()) {
      const auto kind = contact_rule_kind(rule->second);
      if (kind == ContactRuleKind::kRelease || kind == ContactRuleKind::kBoundedContact) {
        classification = CollisionPairClassification::kSupportContactContinued;
      } else if (kind == ContactRuleKind::kFreeContact ||
                 !rule->second.contact()->max_penetration_m) {
        // Allowed without a depth check. The monitor does not apply the
        // window of a rule with an end window.
        classification = CollisionPairClassification::kAllowedContactRule;
      } else {
        const auto clearance = observation.signed_clearance_m;
        if (!clearance.has_value() || !std::isfinite(*clearance) || *clearance > 0.0) {
          out.status = RuntimeCollisionTickStatus::kInconclusive;
          out.reason = RuntimeCollisionTickReason::kEngineInconclusive;
          out.engine_failure_reason = RuntimeCollisionQueryFailureReason::kPairDiagnosticsIncomplete;
          out.engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
          out.pair = &pair;
          out.signed_clearance_m = clearance;
          out.contact_rule = &rule->second;
          out.contact_rule_violation = RuntimeContactRuleViolation::kMissingClearance;
          return out;
        }
        if (std::max(0.0, -*clearance) <=
            *rule->second.contact()->max_penetration_m + 1e-9) {
          classification = CollisionPairClassification::kAllowedContactRule;
        } else {
          out.contact_rule = &rule->second;
          out.contact_rule_violation = RuntimeContactRuleViolation::kPenetrationExceeded;
        }
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
    release_states_[i].observed_normal_a_to_b.reset();
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
      state_it->observed_normal_a_to_b = observation.normal_a_to_b;
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
        policy, ContactReleaseSampleEvidence{state.observed_in_contact, state.observed_clearance_m, std::nullopt},
        state.transition_state);
    if (transition.accepted()) {
      continue;
    }

    clear.reason = transition_failure_reason(transition.failure);
    clear.status = transition.failure == ContactReleaseTransitionFailure::kInvalidPolicy ||
                           transition.failure ==
                               ContactReleaseTransitionFailure::kMissingOrInvalidSignedPenetration
                       ? RuntimeCollisionTickStatus::kInconclusive
                       : RuntimeCollisionTickStatus::kCollision;
    clear.pair_classification = clear.status == RuntimeCollisionTickStatus::kInconclusive
                                    ? CollisionPairClassification::kDiagnosticIncomplete
                                    : CollisionPairClassification::kBlockingCollision;
    clear.pair = state.pair;
    clear.signed_clearance_m = state.observed_clearance_m;
    clear.contact_rule = &policy;
    clear.contact_present = state.observed_in_contact;
    clear.initial_penetration_m = state.transition_state.initial_depth_m;
    if (previous_state.initialized && previous_state.started_in_contact) {
      clear.previous_penetration_m = previous_state.previous_depth_m;
    }
    clear.current_penetration_m = transition.penetration_depth_m;
    return clear;
  }
  clear.pair = nullptr;
  clear.signed_clearance_m.reset();
  clear.initial_penetration_m.reset();
  clear.previous_penetration_m.reset();
  clear.current_penetration_m.reset();
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
    if (source.policy != nullptr && source.policy->release() == nullptr) {
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

RuntimeSpringCompressionSnapshot RuntimeCollisionQuerySession::spring_compression_snapshot()
    const noexcept {
  RuntimeSpringCompressionSnapshot snapshot;
  snapshot.binding_owner = bound_bundle_;
  for (std::size_t index = 0; index < release_state_count_; ++index) {
    const auto& state = release_states_[index];
    const auto* contact = state.policy == nullptr ? nullptr : state.policy->contact();
    if (contact == nullptr || !contact->max_compression_step_m || !state.observed_in_contact ||
        !state.observed_clearance_m || !(*state.observed_clearance_m < 0.0) ||
        !state.observed_normal_a_to_b) {
      continue;
    }
    // The normal points from body_a to body_b: side a leaves side b against
    // it when side a is body_a, along it when side a is body_b.
    const double sign = state.policy->side_a_is_body_b ? 1.0 : -1.0;
    const auto& normal = *state.observed_normal_a_to_b;
    snapshot.entries[snapshot.count++] = RuntimeSpringCompressionEntry{
        state.policy, -*state.observed_clearance_m,
        {sign * normal[0], sign * normal[1], sign * normal[2]}};
  }
  return snapshot;
}

RuntimeCollisionTickOutcome RuntimeCollisionQuerySession::tick(
    const RuntimeCollisionQueryBundlePtr& bundle, std::span<const double> observed_positions_rad,
    std::int64_t state_time_ns, const std::int64_t now_ns) noexcept {
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
  auto projection = project_observed_state(observed_positions_rad, now_ns);
  if (projection.status == RuntimeCollisionTickStatus::kInconclusive) {
    return projection;
  }
  CollisionDistanceRequestMode distance_request_mode = CollisionDistanceRequestMode::kNone;
  std::span<const CollisionPairId> selected_distance_pairs;
  const std::vector<CollisionPairId>* prepared_selection = &bundle->selected_distance_pairs;
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
