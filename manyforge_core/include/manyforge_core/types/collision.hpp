#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

// Normalized collision DTOs (spec §3.8.1).
//
// These DTOs shipped the ManyForge-owned collision contract. They were
// populated alongside CollisionCheckRequestDTO / CollisionCheckResultDTO
// during the rollout, before the kernel migrated to consuming them.

/// Stable, engine-independent collision body id (spec §3.8.1).
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

/// Collision-world revision (spec §3.8.1).
///
/// The unit of traceability for authoritative collision decisions. Chunk 1
/// diagnostic wrappers may leave CollisionResult::collision_world_revision
/// empty until the Chunk 2 ledger and atomic publisher land.
struct CollisionWorldRevision {
  /// Monotonic counter; never decreases within a process lifetime.
  std::uint64_t revision_id = 0;

  /// Content digest covering the body inventory and any policy overlays
  /// affecting collision decisions. Stable for identical world content.
  std::string content_hash;
};

/// Mutation source for a CollisionWorldLedgerEntry (spec §3.8.1).
///
/// Names which subsystem caused the revision bump. Audit consumers use
/// this to reconstruct the chain of changes that produced a given world
/// state.
enum class CollisionWorldMutationSource {
  kSceneInitialize,
  kSceneEdit,
  kCarriedObjectAttach,
  kCarriedObjectDetach,
  kComponentAttachmentApply,
  kPoseSourceApply,
  kDeploymentApply,
  kPolicyUpdate,
};

/// Bounded-ledger entry recording a single collision-world revision
/// transition (spec §3.8.1).
///
/// The publisher appends one entry per successful mutation, in trajectory
/// order. The ledger is read-only to consumers; failure archives and audit
/// tools use it to explain how the world state evolved.
struct CollisionWorldLedgerEntry {
  CollisionWorldRevision revision;

  /// Revision id immediately before this entry. Zero for the scene-initialize
  /// or deployment-apply entry that establishes the initial revision.
  std::uint64_t previous_revision_id = 0;

  CollisionWorldMutationSource mutation_source =
      CollisionWorldMutationSource::kSceneInitialize;

  /// Body ids whose identity or presence in the collision world is changed
  /// by this mutation. MAY be empty for scene-initialize / deployment-apply
  /// entries that affect the whole inventory.
  std::vector<std::string> affected_body_ids;

  /// Authoritative ordering within the collision-world publisher epoch.
  std::uint64_t owner_sequence = 0;

  /// Injected-clock observation timestamp. Ordering never falls back to this
  /// value when owner_sequence is available.
  std::int64_t observed_timestamp_ns = 0;

  /// Provenance stamped by the receiving transport adapter.
  RequestAttributionDTO attribution;

  /// Accepted request-context label, e.g. "scene_edit_route".
  std::string source_label;

  /// Optional cross-owner/request correlation token.
  std::optional<std::string> correlation_id;
};

/// Canonical unordered pair of collision body ids (spec §3.8.1).
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

/// Per-pair classification produced by the policy evaluator (spec §3.8.1,
/// added in Chunk 3 of the collision-ownership plan).
enum class CollisionPairClassification {
  kBlockingCollision,
  kAllowedStaticStatic,
  kSupportContactStart,
  kSupportContactContinued,
  kSupportContactSeparated,
  kSupportContactRecontactViolation,
  kAllowedTouchLink,
  kAllowedContactGrasp,
  kDiagnosticIncomplete,
};

/// Per-pair output bundle emitted alongside CollisionResult::normalized_pairs
/// (spec §3.8.1, added in Chunk 3).
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

/// Logical owner of a collision body (spec §3.8.1).
enum class CollisionOwnerKind {
  kComponentModelBody,
  kSceneResource,
  kRuntimeObject,
};

/// Geometry kind for a collision body (spec §3.8.1).
enum class CollisionGeometryKind {
  kBox,
  kSphere,
  kCylinder,
  kCapsule,
  kConvexMesh,
  kTriangleMesh,
  kCompound,
};

/// Normalized descriptor for one geometry element (spec §3.8.1).
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

/// Query surface (spec §3.8.1). Names which subsystem issued the query.
enum class CollisionSurface {
  kPreExecutionValidation,
  kPredictedRuntimeMonitor,
  kObservedRuntimeMonitor,
  kObservedRuntimeLookahead,
  kDiagnosticCompare,
  kPlannerHint,
};

/// Engine sampling strategy for the query (spec §3.8.1).
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

/// Normalized collision query (spec §3.8.1).
///
/// Superset of CollisionCheckRequestDTO. Chunk 1 mirrors the existing
/// CollisionCheckRequestDTO shape (RobotStateDTO only); trajectory-segment
/// queries land in Chunk 5 when the validation coordinator becomes the
/// entry point.
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
};

/// Engine result status (spec §3.8.1).
enum class CollisionEngineStatus {
  kOk,
  kTimeout,
  kUnsupportedGeometry,
  kDiagnosticIncomplete,
  kCancelled,
  kEngineError,
};

/// Normalized decision derived from an engine result and the active policy
/// (spec §3.8.1). Distinct from CollisionDecision, which is the
/// kernel-facing summary across one or more CollisionResult values.
enum class CollisionDecisionResult {
  kPass,
  kFail,
  kWarn,
  kStopRequested,
  kInconclusiveFail,
};

/// Normalized collision result (spec §3.8.1).
///
/// Populated alongside CollisionCheckResultDTO during Chunks 1-4 of the
/// collision-ownership plan. The kernel migrates to consuming this DTO in
/// Chunk 5.
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

  /// Per-pair classification produced by the Chunk 3 policy evaluator.
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

/// Contact-policy mode for entries in CollisionPolicySnapshot::support_contacts.
///
/// kContactRelease is the production mode for configured release-set
/// transitions: initially present pairs must satisfy the authored evidence and
/// final-separation rules, while an initially absent configured pair may not
/// become a new active contact later in the trajectory.
/// kBoundedPenetration has no initial-contact prerequisite: the declared pair
/// may first collide at any sample, persist, disappear, reappear, or remain at
/// trajectory end, but every colliding sample must remain within
/// max_contact_penetration_m.
/// kContactProcess is reserved for future force/controller-backed contact-rich
/// applications and intentionally fails closed until sensing envelopes and
/// adapter freshness gates are implemented.
enum class CollisionContactPolicyMode {
  kContactRelease,
  kBoundedPenetration,
  kContactProcess,
};

/// Evidence used to prove a contact-release policy.
///
/// Presentation surfaces may label these values "Precise" and "Lightweight",
/// but authored and published contracts use the durable semantic names below.
enum class ContactReleaseEvidenceMode {
  kSignedPenetration,
  kBinaryContact,
};

/// One entry in CollisionPolicySnapshot::support_contacts (spec §3.8.1,
/// added in Chunk 3).
struct SupportContactPolicyEntry {
  CollisionPairId pair;
  CollisionContactPolicyMode mode =
      CollisionContactPolicyMode::kContactRelease;
  ContactReleaseEvidenceMode release_evidence_mode =
      ContactReleaseEvidenceMode::kSignedPenetration;
  /// Initial penetration envelope for kContactRelease with signed evidence.
  /// Other modes keep these release-only fields neutral after typed-boundary
  /// canonicalization; arbitrary mode-scoped mixtures fail core admission.
  bool enforce_max_start_penetration = true;
  double max_start_penetration_m = 0.0;
  /// Numeric tolerance for monotonic release checks. A later sample may be
  /// deeper than the previous accepted sample by at most this amount.
  double release_tolerance_m = 1e-5;
  /// Minimum total reduction required when
  /// require_strictly_decreasing_penetration is true and the pair has not
  /// reached separation. Zero means any reduction beyond tolerance.
  double min_release_progress_m = 0.0;
  /// Maximum penetration permitted at every colliding sample when `mode` is
  /// kBoundedPenetration. Other policy modes keep this neutral at zero.
  double max_contact_penetration_m = 0.0;
  /// kContactRelease trajectory-end obligation. Canonical bounded entries keep
  /// this false and permit final contact within their cap.
  bool require_separation_by_end = true;
  /// kContactRelease net-progress obligation. Canonical bounded entries keep
  /// this false.
  bool require_strictly_decreasing_penetration = true;
  bool allow_recontact = false;
};

/// Geometry-only grasp-contact policy for detached pick approaches.
///
/// This is intentionally narrower than contact_process. It allows only the
/// configured robot gripper links to contact one selected detached object, and
/// only in the terminal portion of a trajectory. Post-attach contact is handled
/// by attached-object touch links, not by this policy.
struct ContactGraspPolicyEntry {
  std::string policy_id;
  std::string selected_object_id;
  std::vector<std::string> allowed_robot_links;
  double max_penetration_m = 0.0;
  double terminal_window_start_fraction = 0.8;
};

/// Policy-revision twin of CollisionWorldRevision (spec §3.8.1, added in
/// Chunk 3).
struct CollisionPolicyRevision {
  std::uint64_t revision_id = 0;
  std::string content_hash;
};

/// Immutable policy configuration consumed by the policy evaluator for one
/// validation / execution pass (spec §3.8.1, added in Chunk 3).
///
/// Snapshots are immutable for one validation or execution pass. A runtime
/// policy edit creates a new revision and cannot mutate an already running
/// move's effective policy.
struct CollisionPolicySnapshot {
  /// Application-level identifier, e.g. "free_space", "pick_with_object".
  /// Defaults to "free_space" which classifies any unallowed collision as
  /// kBlockingCollision.
  std::string policy_id = "free_space";

  CollisionPolicyRevision revision;

  /// Pairs that may overlap because both bodies are static. The policy resolver
  /// must derive this list from the active CollisionWorldRevision; the evaluator
  /// treats the snapshot as authoritative.
  std::vector<CollisionPairId> allowed_static_static_pairs;

  /// Explicit pair-level touch-link allowances (typically
  /// attached-object <-> parent-link contacts).
  std::vector<CollisionPairId> allowed_touch_link_pairs;

  std::vector<SupportContactPolicyEntry> support_contacts;

  /// Declared contact_grasp policies. At validation time the execute request's
  /// concrete contact_policy_id selects at most one of these entries.
  std::vector<ContactGraspPolicyEntry> contact_grasps;

  /// When true, missing diagnostic fields produce kDiagnosticIncomplete;
  /// when false, the evaluator classifies opportunistically with the
  /// available data.
  bool require_full_diagnostics = true;
};

/// Severity grade of a CollisionDecision (spec §3.8.1).
enum class CollisionDecisionSeverity {
  kNone,
  kWarn,
  kBlock,
};

/// Recommended kernel action derived from one or more CollisionResult
/// values (spec §3.8.1).
enum class CollisionDecisionAction {
  kDispatch,
  kReject,
  kWarnOnly,
  kInconclusive,
};

/// Kernel-facing summary across one or more CollisionResult values
/// (spec §3.8.1).
///
/// The kernel consumes CollisionDecision, not raw CollisionResult values,
/// starting in Chunk 5 of the collision-ownership plan. In Chunks 1-4
/// CollisionDecision may be constructed but is not the kernel's source of
/// truth.
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

/// Thread-safety class for a collision engine adapter (spec
/// COLLISION_CHECKING_OWNERSHIP_PLAN §"Engine Capability And Mismatch
/// Policy", added in Chunk 8).
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

/// Engine capability descriptor (spec COLLISION_CHECKING_OWNERSHIP_PLAN
/// §"Engine Capability And Mismatch Policy", added in Chunk 8 groundwork).
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
  /// "support-contact requires policy snapshot attached".
  std::vector<std::string> known_unsupported_cases;
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

/// Compare-mode policy (spec COLLISION_CHECKING_OWNERSHIP_PLAN §"Engine
/// Capability And Mismatch Policy", added in Chunk 8 groundwork).
enum class CollisionEngineCompareMode {
  /// Secondary engine records mismatches but never blocks execution.
  kShadowCompare,
  /// Secondary engine's blocking-collision result is converted to
  /// kEngineMismatchBlocking on the authoritative path.
  kConservativeCompare,
  /// Mismatches fail the qualification run only; live execution is unaffected.
  kQualificationCompare,
};

/// Disposition of one mismatch record (spec §"Engine Capability And
/// Mismatch Policy").
enum class CollisionEngineMismatchDisposition {
  kTolerated,
  kBlocking,
  kQualificationOnly,
};

/// Canonical engine-mismatch record (spec COLLISION_CHECKING_OWNERSHIP_PLAN
/// §"Engine Capability And Mismatch Policy", added in Chunk 8 groundwork).
/// Mismatch records must include both engines, the world / policy revisions
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

/// Categorical reason a collision validation failed (spec
/// COLLISION_CHECKING_OWNERSHIP_PLAN §"Failure archive groundwork",
/// added by the kernel-architecture sprint C-arch chunk). Distinct from
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
  /// Cancellation observed via `CollisionCancellationToken` (spec/260
  /// §8 handshake).
  kCancelled,
  /// Workers borrowed different `CollisionWorldRevision`s, or the
  /// revision drifted across the validation pass.
  kSceneChangedDuringValidation,
};

/// One archived collision-validation failure (spec
/// COLLISION_CHECKING_OWNERSHIP_PLAN §"Failure archive groundwork",
/// added by the kernel-architecture sprint C-arch chunk).
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
