#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/robot_defaults.generated.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

// Normalized collision DTOs.
//
// These DTOs shipped the ManyForge-owned collision contract. They were
// populated alongside CollisionCheckRequestDTO / CollisionCheckResultDTO
// during the rollout, before the kernel migrated to consuming them.

/// Stable, engine-independent collision body id.
///
/// Normative formats (one of):
///   component/<component>/model/<model>/body/<role>/collision/<index>
///   scene/<scene_resource>/shape/<index>
///   runtime_object/<runtime_object>/shape/<index>
///
/// Ids MUST be stable across process restarts for the same artifacts.
/// Ids MUST NOT depend on backend-native geometry object or generated link
/// names. Multi-component scenes carry exact component and model identity.
using CollisionBodyId = std::string;

/// Collision-world revision.
///
/// The unit of traceability for authoritative collision decisions.
/// Diagnostic wrappers may leave CollisionResult::collision_world_revision
/// empty.
struct CollisionWorldRevision {
  /// Monotonic counter; never decreases within a process lifetime.
  std::uint64_t revision_id = 0;

  /// Content digest covering the body inventory and any policy overlays
  /// affecting collision decisions. Stable for identical world content.
  std::string content_hash;
};

/// Canonical unordered pair of collision body ids.
///
/// Canonical form: body_a <= body_b in lexicographic order. The canonical
/// form is the on-the-wire representation so equality and sorting are
/// deterministic across engines and workers.
struct CollisionPairId {
  CollisionBodyId body_a;
  CollisionBodyId body_b;

  friend bool operator==(const CollisionPairId& lhs,
                         const CollisionPairId& rhs) {
    return lhs.body_a == rhs.body_a && lhs.body_b == rhs.body_b;
  }
  friend bool operator!=(const CollisionPairId& lhs,
                         const CollisionPairId& rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const CollisionPairId& lhs,
                        const CollisionPairId& rhs) {
    if (lhs.body_a != rhs.body_a) return lhs.body_a < rhs.body_a;
    return lhs.body_b < rhs.body_b;
  }
};

/// Per-pair classification produced by the policy evaluator.
enum class CollisionPairClassification {
  kBlockingCollision,
  kAllowedStaticStatic,
  kSupportContactStart,
  kSupportContactContinued,
  kSupportContactSeparated,
  kSupportContactRecontactViolation,
  kAllowedTouchLink,
  kAllowedContactRule,
  kDiagnosticIncomplete,
};

/// Per-pair output bundle emitted alongside CollisionResult::normalized_pairs.
struct CollisionPairResult {
  CollisionPairId pair;
  CollisionPairClassification classification =
      CollisionPairClassification::kBlockingCollision;
  bool is_first_failure = false;
};

/// Signed clearance / penetration diagnostic for one canonical collision pair.
///
/// Positive values mean separation clearance. Zero means touching within
/// engine tolerance. Negative values mean penetration depth. Engines populate
/// this vector only for pairs in the query's requested distance scope. Sparse
/// results are intentional: selected-colliding-pair queries omit selected pairs
/// that are not colliding. Consumers MUST use `normalized_pairs` as contact
/// truth and require numeric evidence only for a colliding pair whose policy
/// requests signed release evidence or a bounded-penetration check.
struct CollisionPairClearance {
  CollisionPairId pair;
  double signed_clearance_m = 0.0;
};

/// Logical owner of a collision body.
enum class CollisionOwnerKind {
  kComponentModelBody,
  kSceneResource,
  kRuntimeObject,
};

/// Geometry kind for a collision body.
enum class CollisionGeometryKind {
  kBox,
  kSphere,
  kCylinder,
  kCapsule,
  kConvexMesh,
  kTriangleMesh,
  kCompound,
};

/// Normalized descriptor for one geometry element.
struct CollisionGeometryDescriptor {
  CollisionBodyId body_id;
  CollisionOwnerKind owner_kind = CollisionOwnerKind::kSceneResource;
  CollisionGeometryKind geometry_kind = CollisionGeometryKind::kBox;
  Pose3DTO local_pose;
  /// Full box dimensions in meters. Required when `geometry_kind == kBox`.
  std::optional<Vector3DTO> box_dimensions_m;
  /// Sphere radius in meters. Required when `geometry_kind == kSphere`.
  std::optional<double> sphere_radius_m;
  /// Cylinder radius / height in meters. Required when
  /// `geometry_kind == kCylinder`.
  std::optional<double> cylinder_radius_m;
  std::optional<double> cylinder_height_m;
  /// Capsule radius / height in meters. Required when
  /// `geometry_kind == kCapsule`.
  std::optional<double> capsule_radius_m;
  std::optional<double> capsule_height_m;
  /// Mesh scale applied by the scene publisher before collision evaluation.
  /// Optional for mesh-like geometry; absent means unit scale. Publishers
  /// SHOULD normalize authored "zero means default" scale values to 1.0 here.
  std::optional<Vector3DTO> mesh_scale;
  std::optional<std::string> source_uri;
  std::optional<std::string> source_hash;
  /// Capability labels such as "convex", "watertight", "visual_mesh",
  /// "collision_mesh", "conservative_proxy". Free-form strings; the
  /// vocabulary is documented in the collision-ownership plan.
  std::vector<std::string> quality_labels;
  std::optional<std::int64_t> triangle_count;
  std::optional<std::int64_t> convex_part_count;
  std::optional<std::int64_t> memory_estimate_bytes;
};

/// Query surface. Names which subsystem issued the query.
enum class CollisionSurface {
  kPreExecutionValidation,
  kPredictedRuntimeMonitor,
  kObservedRuntimeMonitor,
  kObservedRuntimeLookahead,
  kDiagnosticCompare,
  kPlannerHint,
};

/// Engine sampling strategy for the query.
enum class CollisionSamplingMode {
  kWaypointOnly,
  kInterpolatedDiscrete,
  kCcd,
  kEngineDefault,
};

/// Scope of signed-distance work requested from a collision engine.
///
/// Collision enumeration is always complete for the active pair graph. This
/// enum controls only which pairs receive the additional signed-distance
/// calculation after collision detection.
enum class CollisionDistanceRequestMode {
  kNone,
  kSelectedCollidingPairs,
  kAllCollidingPairs,
  kAllActivePairs,
};

[[nodiscard]] constexpr const char* collision_distance_request_mode_name(
    CollisionDistanceRequestMode mode) noexcept {
  switch (mode) {
    case CollisionDistanceRequestMode::kNone:
      return "none";
    case CollisionDistanceRequestMode::kSelectedCollidingPairs:
      return "selected_colliding_pairs";
    case CollisionDistanceRequestMode::kAllCollidingPairs:
      return "all_colliding_pairs";
    case CollisionDistanceRequestMode::kAllActivePairs:
      return "all_active_pairs";
  }
  return "unknown";
}

/// Collision settings (the Deployment's `collision_rules` and
/// `collision_check_step_rad`; the defaults kDefaultCollisionClearanceM and
/// kDefaultCollisionCheckStepRad are in manyforge_core/resources/robot_defaults.tsv).
/// - Clearance: a validated or planned move keeps at least its rule's
///   clearance from every body its rules do not allow it to touch. The
///   Deployment's first rule holds every robot x environment pair, 1 mm by
///   default (it also covers the executed spline's deviation from the planned
///   chords); 0 disables it: contact-only checks. A motion's own row without
///   selections replaces that value for the move. Every clearance is within
///   [0, kMaxCollisionClearanceM].
/// - Check step: the joint-space distance (Euclidean, as RoboPlan's
///   `collision_check_step_size`) between two collision-checked
///   configurations of the planner's first attempt. Pre-dispatch validation
///   checks the trajectory's own waypoints only.
inline constexpr double kMaxCollisionClearanceM = 0.05;
inline constexpr double kCollisionRetryCheckStepRad = 0.01;

/// The planner's collision-check step for one planning attempt: the
/// configured step on the first attempt, the finer of it and
/// kCollisionRetryCheckStepRad on a retry (requested only after validation
/// rejected a candidate; the same resolution could reproduce the same missed
/// narrow contact).
[[nodiscard]] inline constexpr double collision_check_step_for_attempt(
    const double configured_step_rad, const std::uint32_t planning_attempt_index) noexcept {
  return planning_attempt_index == 0U || configured_step_rad < kCollisionRetryCheckStepRad
             ? configured_step_rad
             : kCollisionRetryCheckStepRad;
}

/// Collision clearance (coal security margin) of one canonical logical pair.
/// `rule_index` names the collision rule that set it, in
/// CollisionClearanceMargins::rule_ids.
struct CollisionPairMargin {
  CollisionPairId pair;
  double margin_m = 0.0;
  std::uint32_t rule_index = 0U;

  friend bool operator==(const CollisionPairMargin&,
                         const CollisionPairMargin&) = default;
};

/// Clearance half of one move's resolved collision-rule table: two bodies of
/// a listed pair closer than its margin collide. Pairs not listed keep margin
/// 0. Built once at admission by `resolve_collision_rules`; immutable and
/// shared by every query of the move.
struct CollisionClearanceMargins {
  /// Sorted by pair, unique, every margin in (0, kMaxCollisionClearanceM].
  std::vector<CollisionPairMargin> pairs;
  /// Ids of the rules the listed margins come from.
  std::vector<std::string> rule_ids;

  [[nodiscard]] const CollisionPairMargin* find(const CollisionPairId& pair) const noexcept {
    const auto found = std::lower_bound(
        pairs.begin(), pairs.end(), pair,
        [](const CollisionPairMargin& entry, const CollisionPairId& selected) {
          return entry.pair < selected;
        });
    return found != pairs.end() && found->pair == pair ? &*found : nullptr;
  }

  [[nodiscard]] double margin_for(const CollisionPairId& pair) const noexcept {
    const auto* entry = find(pair);
    return entry != nullptr ? entry->margin_m : 0.0;
  }

  /// Rule id of a listed pair; empty for a pair that is not listed.
  [[nodiscard]] std::string_view rule_id_for(const CollisionPairId& pair) const noexcept {
    const auto* entry = find(pair);
    return entry != nullptr && entry->rule_index < rule_ids.size()
               ? std::string_view{rule_ids[entry->rule_index]}
               : std::string_view{};
  }
};

/// Normalized collision query.
///
/// Superset of CollisionCheckRequestDTO; mirrors the
/// CollisionCheckRequestDTO shape (RobotStateDTO only).
struct CollisionQuery {
  std::string query_id;
  std::optional<std::string> validation_id;
  CollisionSurface surface = CollisionSurface::kPreExecutionValidation;
  CollisionWorldRevision collision_world_revision;
  std::optional<std::string> policy_revision;
  RobotStateDTO robot_state;
  std::optional<std::int64_t> waypoint_index;
  std::optional<std::int64_t> state_time_ns;
  CollisionSamplingMode sampling_mode = CollisionSamplingMode::kEngineDefault;
  std::optional<std::int64_t> timeout_budget_ns;
  /// Required capability flags such as "distance", "contact_points",
  /// "penetration_depth", "body_pair_only". Engines that cannot supply a
  /// required capability MUST fail the query with kDiagnosticIncomplete.
  std::vector<std::string> required_capabilities;
  /// Typed signed-distance scope. The selected-pair mode intersects the sorted
  /// canonical `selected_distance_pairs` list with the complete colliding-pair
  /// result; a selected pair that is not colliding is intentionally omitted
  /// from CollisionResult::pair_clearances.
  CollisionDistanceRequestMode distance_request_mode =
      CollisionDistanceRequestMode::kNone;
  /// Sorted, unique canonical logical pairs used only with
  /// kSelectedCollidingPairs. Other modes require this list to be empty.
  std::vector<CollisionPairId> selected_distance_pairs;
  std::optional<std::int64_t> max_contacts;
  /// Clearance margins for this query. Null means margin 0 for every pair,
  /// as on the runtime surfaces.
  std::shared_ptr<const CollisionClearanceMargins> clearance_margins;
};

/// Engine result status.
enum class CollisionEngineStatus {
  kOk,
  kTimeout,
  kUnsupportedGeometry,
  kDiagnosticIncomplete,
  kCancelled,
  kEngineError,
};

/// Normalized decision derived from an engine result and the active policy.
/// Distinct from CollisionDecision, which is the
/// kernel-facing summary across one or more CollisionResult values.
enum class CollisionDecisionResult {
  kPass,
  kFail,
  kWarn,
  kStopRequested,
  kInconclusiveFail,
};

/// Normalized collision result.
struct CollisionResult {
  bool in_collision = false;

  /// Raw backend names. Kept during the transition for migration debugging;
  /// see the collision-ownership plan for the removal schedule.
  std::optional<std::string> contact_body_a;
  std::optional<std::string> contact_body_b;

  /// Canonical body-pair ids. Empty when in_collision is false. Sorted by
  /// (body_a, body_b) lex order for deterministic merge across workers.
  std::vector<CollisionPairId> normalized_pairs;

  std::optional<std::string> query_id;
  std::optional<CollisionWorldRevision> collision_world_revision;
  std::optional<std::string> policy_revision;
  CollisionSurface surface = CollisionSurface::kPreExecutionValidation;
  std::optional<std::int64_t> state_time_ns;
  CollisionEngineStatus engine_status = CollisionEngineStatus::kOk;
  CollisionDecisionResult decision = CollisionDecisionResult::kPass;
  std::string engine_id;
  std::string engine_version;
  std::optional<std::int64_t> query_duration_ns;
  CollisionSamplingMode sampling_mode = CollisionSamplingMode::kEngineDefault;

  /// Minimum signed clearance across the evaluated distance scope. Negative
  /// values denote penetration depth. Null when no pair in the requested
  /// scope was evaluated.
  std::optional<double> min_clearance_m;

  /// Pair corresponding to min_clearance_m. Null when min_clearance_m is
  /// null.
  std::optional<CollisionPairId> min_clearance_pair;

  /// Per-pair signed clearance / penetration diagnostics. Sorted by pair and
  /// intentionally sparse according to CollisionQuery::distance_request_mode.
  /// A missing entry is fail-closed only when the pair is colliding and the
  /// active policy requested signed release evidence or bounded penetration
  /// for that pair.
  std::vector<CollisionPairClearance> pair_clearances;

  /// Colliding pairs reported only because of their clearance margin: the
  /// engine's signed clearance is positive but below the pair's margin.
  /// Sorted by pair; a subset of `normalized_pairs`.
  std::vector<CollisionPairClearance> clearance_violations;

  /// Per-pair classification produced by the policy evaluator.
  /// Includes observed-pair classifications and may include policy-state
  /// transition entries (for example kSupportContactSeparated) for pairs absent
  /// from normalized_pairs at the current sample. Sorted by (body_a, body_b)
  /// lexicographic order. Empty when the evaluator has not been invoked.
  std::vector<CollisionPairResult> normalized_pairs_classified;

  /// Identifier of the policy revision whose snapshot produced
  /// normalized_pairs_classified. When the evaluator runs, this MUST
  /// equal the snapshot's revision.revision_id rendered as a string.
  std::optional<std::string> policy_revision_used;

  /// Free-form per-engine diagnostic payload. Required for fail-closed
  /// statuses where operators need the concrete reason, for example
  /// unsupported snapshot geometry, stale revisions, cancellation, unknown
  /// backend contact names, or an unsupported snapshot_format_version.
  /// Stable consumers SHOULD treat unknown keys as opaque.
  std::map<std::string, std::string> raw_diagnostics;

  /// Exact signed-distance scope requested for this result and the number of
  /// logical pairs for which the engine returned distance evidence. These are
  /// stamped by the coordinator from the typed query/result boundary; they do
  /// not infer or synthesize a metric for absent or binary-only pairs.
  CollisionDistanceRequestMode distance_request_mode =
      CollisionDistanceRequestMode::kNone;
  std::size_t evaluated_distance_pair_count = 0;
};

/// Evidence used to prove a release rule.
///
/// Presentation surfaces may label these values "Precise" and "Lightweight",
/// but authored and published contracts use the durable semantic names below.
enum class ContactReleaseEvidenceMode {
  kSignedPenetration,
  kBinaryContact,
};

/// `end_from_fraction` of a `contact` rule that does not give one.
inline constexpr double kDefaultContactEndFromFraction = 0.8;
/// `max_compression_step_m` a spring element declares when it gives none.
inline constexpr double kDefaultMaxCompressionStepM = 0.002;
/// Largest tool travel between two validation samples of a move with a
/// spring rule (OD-356).
inline constexpr double kSpringSampleTravelM = 0.001;

/// The spring slider a contact rule belongs to (OD-371): the slider element
/// `element_id` of generated tool `component_id`, whose rule holds the slider
/// body and the objects it carries against side `b`.
struct CollisionSpringGroup {
  ControllableComponentId component_id;
  std::string element_id;
  /// The slider's axis, a unit vector in the slider link's frame.
  std::array<double, 3> axis{};

  friend bool operator==(const CollisionSpringGroup&,
                         const CollisionSpringGroup&) = default;
};

/// Value `contact` of a collision rule: the pair may touch.
struct CollisionContactRule {
  /// Deepest penetration allowed at any colliding sample. Absent: the depth
  /// is not checked.
  std::optional<double> max_penetration_m;
  /// In [0, 1]. Above 0: the pair may touch only from this fraction of the
  /// move on; contact at the first sample or before the fraction is refused.
  /// 0: the pair may touch during the whole move, from the first sample.
  double end_from_fraction = kDefaultContactEndFromFraction;
  /// A spring (OD-356): the deepest increase of the penetration between two
  /// consecutive samples, at most kSpringSampleTravelM of tool travel apart
  /// while in contact. Absent: no spring, the increase is not checked. Set
  /// only together with `max_penetration_m` (the spring's stroke).
  std::optional<double> max_compression_step_m;
  /// A spring slider and its carried objects as one spring (OD-371): the
  /// rule's compression is the deepest penetration over all its pairs, and
  /// `max_penetration_m` and the step limit apply to that one value, with one
  /// previous depth per rule. Absent: each pair is checked on its own. Set
  /// only together with `max_compression_step_m`.
  std::optional<CollisionSpringGroup> spring_group{};

  /// True when the pair may touch during the whole move.
  [[nodiscard]] bool whole_move() const noexcept { return !(end_from_fraction > 0.0); }

  friend bool operator==(const CollisionContactRule&,
                         const CollisionContactRule&) = default;
};

/// Value `release` of a collision rule: a pair in contact at the start of the
/// move must separate. A pair that starts clear may not come into contact.
struct CollisionReleaseRule {
  ContactReleaseEvidenceMode release_evidence_mode =
      ContactReleaseEvidenceMode::kSignedPenetration;
  /// Initial penetration limit, with signed evidence.
  bool enforce_max_start_penetration = true;
  double max_start_penetration_m = 0.0;
  /// Numeric tolerance for monotonic release checks. A later sample may be
  /// deeper than the previous accepted sample by at most this amount.
  double release_tolerance_m = 1e-5;
  /// Minimum total reduction required when
  /// require_strictly_decreasing_penetration is true and the pair has not
  /// reached separation. Zero means any reduction beyond tolerance.
  double min_release_progress_m = 0.0;
  bool require_separation_by_end = true;
  bool require_strictly_decreasing_penetration = true;
  bool allow_recontact = false;

  friend bool operator==(const CollisionReleaseRule&,
                         const CollisionReleaseRule&) = default;
};

using CollisionContactRuleValue =
    std::variant<CollisionContactRule, CollisionReleaseRule>;

/// Rule id of the release the kernel adds by itself for a carried object
/// found touching a free Scene object at the first waypoint.
inline constexpr std::string_view kAutomaticStartReleaseRuleId =
    "automatic_start_release";

/// Contact half of one move's resolved collision-rule table: one canonical
/// pair that a `contact` or `release` rule holds, with the rule's id and
/// value. Pairs under a `clearance_m` rule are in CollisionClearanceMargins.
struct CollisionPairContactRule {
  CollisionPairId pair;
  std::string rule_id;
  CollisionContactRuleValue value{CollisionContactRule{}};
  /// The rule's side `a` selected `pair.body_b` (false: `pair.body_a`). Set
  /// by the rule resolver; it names the side a spring's published
  /// compression shifts (OD-358).
  bool side_a_is_body_b = false;

  [[nodiscard]] const CollisionContactRule* contact() const noexcept {
    return std::get_if<CollisionContactRule>(&value);
  }
  [[nodiscard]] const CollisionReleaseRule* release() const noexcept {
    return std::get_if<CollisionReleaseRule>(&value);
  }

  friend bool operator==(const CollisionPairContactRule&,
                         const CollisionPairContactRule&) = default;
};

/// Policy-revision twin of CollisionWorldRevision.
struct CollisionPolicyRevision {
  std::uint64_t revision_id = 0;
  std::string content_hash;

  friend bool operator==(const CollisionPolicyRevision&,
                         const CollisionPolicyRevision&) = default;
};

/// Immutable policy configuration consumed by the policy evaluator for one
/// validation / execution pass.
///
/// Snapshots are immutable for one validation or execution pass. A runtime
/// policy edit creates a new revision and cannot mutate an already running
/// move's effective policy.
struct CollisionPolicySnapshot {
  /// Application-level identifier. Defaults to "free_space" which classifies
  /// any unallowed collision as kBlockingCollision.
  std::string policy_id = "free_space";

  CollisionPolicyRevision revision;

  /// The bodies that never move in the policy's collision world: every shape
  /// of a cell-fixed Scene object (a carried object is never one) and every
  /// component body no dynamic coordinate moves. Sorted, unique. Two of them
  /// are never checked against each other (kAllowedStaticStatic): one rule by
  /// body class, not a pair list. The Scene projector derives the list from
  /// the logical scene and the retained model; the evaluator trusts it.
  std::vector<CollisionBodyId> static_body_ids;

  /// Grip pairs of the current attachments (carried object <-> the attach
  /// node's `touch_bodies`), valid until detach. Not rules: no depth check.
  std::vector<CollisionPairId> allowed_touch_link_pairs;

  /// Pairs under a `contact` or `release` collision rule of the move, sorted
  /// by pair and unique. Empty in the Scene projector's base policy; the
  /// kernel installs the move's resolved rules at admission.
  std::vector<CollisionPairContactRule> contact_rules;

  [[nodiscard]] const CollisionPairContactRule* contact_rule_for(
      const CollisionPairId& pair) const noexcept {
    const auto found = std::lower_bound(
        contact_rules.begin(), contact_rules.end(), pair,
        [](const CollisionPairContactRule& entry, const CollisionPairId& selected) {
          return entry.pair < selected;
        });
    return found != contact_rules.end() && found->pair == pair ? &*found : nullptr;
  }

  /// True when both bodies of `pair` are static bodies: the pair is never
  /// checked. Two binary searches, no allocation.
  [[nodiscard]] bool static_pair(const CollisionPairId& pair) const noexcept {
    return std::binary_search(static_body_ids.begin(), static_body_ids.end(), pair.body_a) &&
           std::binary_search(static_body_ids.begin(), static_body_ids.end(), pair.body_b);
  }

  /// When true, missing diagnostic fields produce kDiagnosticIncomplete;
  /// when false, the evaluator classifies opportunistically with the
  /// available data.
  bool require_full_diagnostics = true;

  friend bool operator==(const CollisionPolicySnapshot&,
                         const CollisionPolicySnapshot&) = default;
};

/// Severity grade of a CollisionDecision.
enum class CollisionDecisionSeverity {
  kNone,
  kWarn,
  kBlock,
};

/// Recommended kernel action derived from one or more CollisionResult
/// values.
enum class CollisionDecisionAction {
  kDispatch,
  kReject,
  kWarnOnly,
  kInconclusive,
};

/// Kernel-facing summary across one or more CollisionResult values.
///
/// The kernel consumes CollisionDecision, not raw CollisionResult values.
struct CollisionDecision {
  bool accepted = true;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;
  CollisionDecisionSeverity severity = CollisionDecisionSeverity::kNone;
  CollisionDecisionAction action = CollisionDecisionAction::kDispatch;
  std::optional<CollisionWorldRevision> collision_world_revision;
  /// Null until the failure-archive plumbing lands in a later chunk.
  std::optional<std::string> failure_archive_id;
};

/// Thread-safety class for a collision engine adapter.
enum class CollisionEngineThreadSafetyClass {
  /// Each adapter instance owns mutable state and MUST be called from one
  /// thread (or under an external mutex). Calling concurrently from
  /// multiple threads is undefined behavior.
  kSingleContextExclusive,
  /// The adapter holds an immutable model + immutable geometry shared across
  /// workers and per-worker mutable Data. Multiple workers can call concurrently
  /// as long as each owns its own Data view.
  kImmutableModelWorkerData,
  /// Each adapter instance lives in its own process; concurrency is enforced
  /// by OS isolation.
  kProcessIsolated,
};

/// Memory/latency contract for the dedicated observed-runtime query surface.
/// General trajectory validation support does not imply runtime suitability.
enum class RuntimeCollisionQueryMemoryModel {
  /// The engine must not be admitted as the runtime-monitor authority.
  kUnsupported,
  /// Warm, revision-stable queries use storage prepared before monitor arm.
  /// Snapshot-revision preparation may allocate off the clean steady-state path.
  kPreallocated,
  /// Warm queries have a measured finite allocation/WCET envelope recorded by
  /// qualification. This is firm-RT evidence, not a hard-RT claim.
  kMeasuredBounded,
};

/// Engine capability descriptor.
///
/// Every adapter (RoboPlan / manyforge_collision / future MoveIt2 /
/// future direct-Pinocchio) publishes one of these so callers can decide
/// whether to ask for `distance`, `contact_points`, etc., before issuing
/// the query, and so qualification can record what an engine claimed it
/// could do.
struct CollisionEngineCapabilityDescriptor {
  std::string engine_id;
  std::string engine_version;
  /// Underlying library lineage: e.g. "coal" / "hpp-fcl" / "moveit_planning_scene".
  std::string library_lineage;
  /// Identity of the mutable backend/query context, when one exists. Distinct
  /// C++ wrappers sharing this id must be treated as one
  /// `kSingleContextExclusive` resource for admission; missing ids fail closed
  /// for concurrent runtime-monitor/validation use.
  std::string mutable_context_id;
  std::vector<std::string> supported_geometry_kinds;
  bool supports_distance_query = false;
  /// Exact, sorted, duplicate-free distance scopes implemented by this
  /// adapter.  A broader scope never satisfies a narrower request: callers
  /// must fail capability admission instead of silently doing more work.
  std::vector<CollisionDistanceRequestMode> supported_distance_request_modes;
  bool supports_contact_points = false;
  bool supports_penetration_depth = false;
  bool supports_continuous_collision = false;
  CollisionEngineThreadSafetyClass thread_safety =
      CollisionEngineThreadSafetyClass::kSingleContextExclusive;
  bool deterministic_ordering = false;
  RuntimeCollisionQueryMemoryModel runtime_query_memory_model =
      RuntimeCollisionQueryMemoryModel::kUnsupported;
  /// Hard admission bounds enforced before a runtime query. Zero means the
  /// corresponding dimension is unsupported, not unbounded.
  std::size_t runtime_query_max_joints = 0;
  std::size_t runtime_query_max_world_bodies = 0;
  std::size_t runtime_query_max_collision_pairs = 0;
  /// Upper bound for engine-owned heap allocations on a warm, unchanged-world
  /// clean query. kPreallocated engines must publish zero here.
  std::uint64_t runtime_query_clean_allocation_upper_bound = 0;
  /// True when ingesting a new immutable world revision may allocate/rebuild
  /// engine scratch. Qualification records this cold-path tail separately.
  bool runtime_query_snapshot_change_may_allocate = false;
  /// Qualified upper bound for allocations while ingesting one changed
  /// immutable world revision. This is distinct from the warm query bound.
  std::uint64_t runtime_query_snapshot_change_allocation_upper_bound = 0;
  /// Qualified warm-query duration envelope. Zero means no runtime admission.
  /// The per-call timeout may be tighter; this value documents the engine's
  /// declared product/fixture envelope rather than replacing that timeout.
  std::uint64_t runtime_query_qualified_wcet_ns = 0;
  /// Qualified duration envelope for one changed-world ingestion/query. Every
  /// admitted runtime engine must publish a nonzero bound, including engines
  /// whose bounded response is to reject changed snapshots without allocation.
  std::uint64_t runtime_query_snapshot_change_qualified_wcet_ns = 0;
  /// Free-form notes — e.g. "octree collision not yet supported",
  /// "release rules require the policy snapshot attached".
  std::vector<std::string> known_unsupported_cases;
  /// Diagnostic geometry queries enumerate colliding pairs without applying
  /// policy exemptions. Absent evidence must not be displayed as clear.
  bool diagnostic_reports_all_collision_pairs = false;
};

/// Validate the exact distance-scope capability declaration.  Empty,
/// unordered, duplicate, or internally contradictory declarations are not
/// admissible.  `kNone` remains a real declared scope: collision-only callers
/// must not rely on an omitted/legacy interpretation.
[[nodiscard]] inline bool collision_distance_request_modes_valid(
    const CollisionEngineCapabilityDescriptor& capability) noexcept {
  const auto& modes = capability.supported_distance_request_modes;
  if (modes.empty()) {
    return false;
  }
  for (std::size_t index = 1; index < modes.size(); ++index) {
    if (static_cast<int>(modes[index - 1]) >=
        static_cast<int>(modes[index])) {
      return false;
    }
  }
  if (!capability.supports_distance_query &&
      std::any_of(modes.begin(), modes.end(), [](const auto mode) {
        return mode != CollisionDistanceRequestMode::kNone;
      })) {
    return false;
  }
  return true;
}

/// Exact-scope admission.  `all_active_pairs` and `all_colliding_pairs` are
/// deliberately not fallbacks for `selected_colliding_pairs`.
[[nodiscard]] inline bool collision_engine_supports_distance_request_mode(
    const CollisionEngineCapabilityDescriptor& capability,
    CollisionDistanceRequestMode requested_mode) noexcept {
  if (!collision_distance_request_modes_valid(capability)) {
    return false;
  }
  if (requested_mode != CollisionDistanceRequestMode::kNone &&
      !capability.supports_distance_query) {
    return false;
  }
  return std::binary_search(capability.supported_distance_request_modes.begin(),
                            capability.supported_distance_request_modes.end(),
                            requested_mode,
                            [](const auto lhs, const auto rhs) {
                              return static_cast<int>(lhs) <
                                     static_cast<int>(rhs);
                            });
}

/// Compare-mode policy.
enum class CollisionEngineCompareMode {
  /// Secondary engine records mismatches but never blocks execution.
  kShadowCompare,
  /// Secondary engine's blocking-collision result is converted to
  /// kEngineMismatchBlocking on the authoritative path.
  kConservativeCompare,
  /// Mismatches fail the qualification run only; live execution is unaffected.
  kQualificationCompare,
};

/// Disposition of one mismatch record.
enum class CollisionEngineMismatchDisposition {
  kTolerated,
  kBlocking,
  kQualificationOnly,
};

/// Canonical engine-mismatch record. Mismatch records must include both engines, the world / policy revisions
/// active at the time of the mismatch, normalized body pairs, the trajectory
/// coordinate (waypoint / time), and the final disposition.
struct CollisionEngineMismatchRecord {
  std::string mismatch_id;
  std::int64_t emitted_at_ns = 0;
  std::string authoritative_engine_id;
  std::string authoritative_engine_version;
  std::string secondary_engine_id;
  std::string secondary_engine_version;
  std::optional<CollisionWorldRevision> collision_world_revision;
  std::optional<std::string> policy_revision;
  std::vector<CollisionPairId> authoritative_normalized_pairs;
  std::vector<CollisionPairId> secondary_normalized_pairs;
  std::optional<std::int64_t> waypoint_index;
  std::optional<std::int64_t> state_time_ns;
  /// Free-form per-engine diagnostic payload that was emitted alongside the
  /// mismatch (engine_status text, raw contact names, etc.).
  std::map<std::string, std::string> raw_diagnostics;
  std::optional<std::string> query_id;
  CollisionEngineMismatchDisposition disposition =
      CollisionEngineMismatchDisposition::kTolerated;
};

/// Categorical reason a collision validation failed. Distinct from
/// `CollisionDecisionResult` because archive consumers want to slice by
/// *why* the kernel rejected dispatch, not just the resulting action.
enum class CollisionFailureKind {
  /// Authoritative engine reported an in-collision pair that the policy
  /// classified as `kBlockingCollision`.
  kBlockingCollision,
  /// A configured `support_contact` constraint was violated (initial
  /// penetration, separation, or recontact).
  kSupportContactViolation,
  /// Engine returned `kDiagnosticIncomplete` for a query that required
  /// full diagnostics.
  kDiagnosticIncomplete,
  /// Engine returned `kEngineError` (raised exception, internal
  /// inconsistency, backend unavailability).
  kEngineError,
  /// Cancellation observed via `CollisionCancellationToken`.
  kCancelled,
  /// Workers borrowed different `CollisionWorldRevision`s, or the
  /// revision drifted across the validation pass.
  kSceneChangedDuringValidation,
};

/// One archived collision-validation failure.
///
/// Pushed by the `CollisionValidationCoordinator` (or by callers wrapping
/// it) only when a validation pass produces a non-`kDispatch` decision.
/// The archive does NOT record passing trajectories — that would make it
/// useless as a triage surface and would silently inflate ingest cost.
///
/// Persistence shape is intentionally deferred for now: the v0 archive
/// is an in-process ring buffer (`CollisionFailureArchive`) with an
/// optional sink callback that external integrations (JSONL writer,
/// telemetry bus, qualification harness) can wire up. The on-disk /
/// long-retention persistence format will be decided once we have data
/// on retention duration, blob sizes, and query patterns (per the
/// defer-design-until-data rule).
struct CollisionFailureArchiveEntry {
  /// Stable identifier for cross-system correlation (UUID-ish). The
  /// archive does NOT generate this; the caller (coordinator) supplies it.
  std::string entry_id;
  std::int64_t emitted_at_ns = 0;

  /// Authoritative engine that produced the failing result.
  std::string engine_id;
  std::string engine_version;

  /// World + policy revisions active when the failure was emitted.
  CollisionWorldRevision collision_world_revision;
  std::optional<std::string> policy_revision;

  /// Categorical failure kind. See `CollisionFailureKind`.
  CollisionFailureKind failure_kind = CollisionFailureKind::kBlockingCollision;

  /// Kernel-facing decision recorded alongside the failure.
  CollisionDecisionAction decision_action =
      CollisionDecisionAction::kInconclusive;
  CollisionDecisionSeverity decision_severity =
      CollisionDecisionSeverity::kBlock;

  /// Normalized colliding pairs at the point of failure (may be empty
  /// for `kEngineError`, `kCancelled`, `kDiagnosticIncomplete`).
  std::vector<CollisionPairId> normalized_pairs;

  /// Trajectory-level coordinates of the failure. Optional because
  /// some failures (e.g. cancellation) are not tied to a specific
  /// waypoint.
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;

  /// Total waypoints in the trajectory the failure was found in.
  /// Useful for "failure at 3/20" vs "failure at 19/20" triage.
  std::int64_t trajectory_waypoint_count = 0;

  /// Engine query duration that produced the failure, ns.
  std::optional<std::int64_t> query_duration_ns;

  /// Parallel pool snapshot: how many workers ran (1 means serial
  /// fallback) and the fallback reason if any. Empty `fallback_reason`
  /// with `worker_count_actually_used == 1` is also possible when the
  /// coordinator was invoked directly without the pool.
  int worker_count_actually_used = 1;
  std::string fallback_reason;

  /// Free-form per-engine diagnostic payload (engine_status text, raw
  /// contact names, internal counters). Keys are engine-specific;
  /// stable archive consumers SHOULD treat unknown keys as opaque.
  std::map<std::string, std::string> raw_diagnostics;

  /// Optional correlation identifier for downstream systems (program
  /// id, move id, cycle id). Free-form.
  std::optional<std::string> correlation_id;
};

}  // namespace manyforge::core
