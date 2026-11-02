"""Round-trip binding tests for normalized collision DTOs (spec §3.8.1).

Chunk 1 of the collision-ownership plan. These tests assert that every new
DTO can be constructed from Python, every field is bindable, and every
typed enum exposes the same value names as the spec.
"""

import pytest

manyforge_core_py = pytest.importorskip("manyforge_core_py")


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------


def test_collision_owner_kind_enum():
    e = manyforge_core_py.CollisionOwnerKind
    for name in ("kComponentModelBody", "kSceneResource", "kRuntimeObject"):
        assert getattr(e, name) is not None


def test_collision_geometry_kind_enum():
    e = manyforge_core_py.CollisionGeometryKind
    for name in (
        "kBox",
        "kSphere",
        "kCylinder",
        "kCapsule",
        "kConvexMesh",
        "kTriangleMesh",
        "kCompound",
    ):
        assert getattr(e, name) is not None


def test_collision_surface_enum():
    e = manyforge_core_py.CollisionSurface
    for name in (
        "kPreExecutionValidation",
        "kPredictedRuntimeMonitor",
        "kObservedRuntimeMonitor",
        "kDiagnosticCompare",
        "kPlannerHint",
    ):
        assert getattr(e, name) is not None


def test_collision_sampling_mode_enum():
    e = manyforge_core_py.CollisionSamplingMode
    for name in ("kWaypointOnly", "kInterpolatedDiscrete", "kCcd", "kEngineDefault"):
        assert getattr(e, name) is not None


def test_collision_distance_request_mode_enum():
    e = manyforge_core_py.CollisionDistanceRequestMode
    for name in (
        "kNone",
        "kSelectedCollidingPairs",
        "kAllCollidingPairs",
        "kAllActivePairs",
    ):
        assert getattr(e, name) is not None


def test_collision_engine_status_enum():
    e = manyforge_core_py.CollisionEngineStatus
    for name in (
        "kOk",
        "kTimeout",
        "kUnsupportedGeometry",
        "kDiagnosticIncomplete",
        "kCancelled",
        "kEngineError",
    ):
        assert getattr(e, name) is not None


def test_collision_decision_result_enum():
    e = manyforge_core_py.CollisionDecisionResult
    for name in ("kPass", "kFail", "kWarn", "kStopRequested", "kInconclusiveFail"):
        assert getattr(e, name) is not None


def test_collision_decision_severity_enum():
    e = manyforge_core_py.CollisionDecisionSeverity
    for name in ("kNone", "kWarn", "kBlock"):
        assert getattr(e, name) is not None


def test_collision_decision_action_enum():
    e = manyforge_core_py.CollisionDecisionAction
    for name in ("kDispatch", "kReject", "kWarnOnly", "kInconclusive"):
        assert getattr(e, name) is not None


def test_collision_failure_kind_enum():
    e = manyforge_core_py.CollisionFailureKind
    for name in (
        "kBlockingCollision",
        "kSupportContactViolation",
        "kDiagnosticIncomplete",
        "kEngineError",
        "kCancelled",
        "kSceneChangedDuringValidation",
    ):
        assert getattr(e, name) is not None


def test_collision_pair_classification_enum():
    e = manyforge_core_py.CollisionPairClassification
    for name in (
        "kBlockingCollision",
        "kAllowedStaticStatic",
        "kSupportContactStart",
        "kSupportContactContinued",
        "kSupportContactSeparated",
        "kSupportContactRecontactViolation",
        "kAllowedTouchLink",
        "kAllowedContactGrasp",
        "kDiagnosticIncomplete",
    ):
        assert getattr(e, name) is not None


# ---------------------------------------------------------------------------
# CollisionWorldRevision
# ---------------------------------------------------------------------------


def test_collision_world_revision_default():
    r = manyforge_core_py.CollisionWorldRevision()
    assert r.revision_id == 0
    assert r.content_hash == ""


def test_collision_world_revision_keyword_ctor():
    r = manyforge_core_py.CollisionWorldRevision(revision_id=42, content_hash="abc123")
    assert r.revision_id == 42
    assert r.content_hash == "abc123"
    assert "revision_id=42" in repr(r)


def test_collision_world_revision_writable():
    r = manyforge_core_py.CollisionWorldRevision()
    r.revision_id = 7
    r.content_hash = "deadbeef"
    assert r.revision_id == 7
    assert r.content_hash == "deadbeef"


# ---------------------------------------------------------------------------
# CollisionPairId
# ---------------------------------------------------------------------------


def test_collision_pair_id_default():
    p = manyforge_core_py.CollisionPairId()
    assert p.body_a == ""
    assert p.body_b == ""


def test_collision_pair_id_keyword_ctor():
    p = manyforge_core_py.CollisionPairId(
        body_a="component/ur10e/model/ur10e_model/body/wrist/collision/0",
        body_b="scene/cheese_box/shape/0",
    )
    assert p.body_a == "component/ur10e/model/ur10e_model/body/wrist/collision/0"
    assert p.body_b == "scene/cheese_box/shape/0"
    assert "body_a" in repr(p)


# ---------------------------------------------------------------------------
# Collision policy DTOs (Chunk 3)
# ---------------------------------------------------------------------------


def test_collision_pair_result_default_and_round_trip():
    r = manyforge_core_py.CollisionPairResult()
    assert r.pair.body_a == ""
    assert (
        r.classification
        == manyforge_core_py.CollisionPairClassification.kBlockingCollision
    )
    assert r.is_first_failure is False

    r.pair = manyforge_core_py.CollisionPairId(
        body_a="scene/a/shape/0", body_b="scene/b/shape/0"
    )
    r.classification = (
        manyforge_core_py.CollisionPairClassification.kAllowedStaticStatic
    )
    r.is_first_failure = True
    assert r.pair.body_a == "scene/a/shape/0"
    assert (
        r.classification
        == manyforge_core_py.CollisionPairClassification.kAllowedStaticStatic
    )
    assert r.is_first_failure is True


def test_support_contact_policy_entry_round_trip():
    e = manyforge_core_py.SupportContactPolicyEntry()
    e.pair = manyforge_core_py.CollisionPairId(
        body_a="scene/part/shape/0",
        body_b="scene/table/shape/0",
    )
    e.mode = manyforge_core_py.CollisionContactPolicyMode.kContactRelease
    e.release_evidence_mode = (
        manyforge_core_py.ContactReleaseEvidenceMode.kBinaryContact
    )
    e.enforce_max_start_penetration = False
    e.max_start_penetration_m = 0.001
    e.release_tolerance_m = 0.00001
    e.min_release_progress_m = 0.0005
    e.max_contact_penetration_m = 0.0
    e.require_separation_by_end = False
    e.require_strictly_decreasing_penetration = True
    e.allow_recontact = True

    assert e.pair.body_b == "scene/table/shape/0"
    assert e.mode == manyforge_core_py.CollisionContactPolicyMode.kContactRelease
    assert (
        e.release_evidence_mode
        == manyforge_core_py.ContactReleaseEvidenceMode.kBinaryContact
    )
    assert e.enforce_max_start_penetration is False
    assert e.max_start_penetration_m == 0.001
    assert e.release_tolerance_m == 0.00001
    assert e.min_release_progress_m == 0.0005
    assert e.max_contact_penetration_m == 0.0
    assert e.require_separation_by_end is False
    assert e.require_strictly_decreasing_penetration is True
    assert e.allow_recontact is True


def test_bounded_penetration_policy_entry_round_trip():
    e = manyforge_core_py.SupportContactPolicyEntry()
    e.mode = manyforge_core_py.CollisionContactPolicyMode.kBoundedPenetration
    e.max_contact_penetration_m = 0.002

    assert (
        e.mode
        == manyforge_core_py.CollisionContactPolicyMode.kBoundedPenetration
    )
    assert e.max_contact_penetration_m == 0.002


def test_collision_pair_clearance_round_trip():
    clearance = manyforge_core_py.CollisionPairClearance()
    clearance.pair = manyforge_core_py.CollisionPairId(
        body_a="component/arm/model/arm_model/body/tool/collision/0",
        body_b="scene/part/shape/0",
    )
    clearance.signed_clearance_m = -0.001

    result = manyforge_core_py.CollisionResult()
    result.pair_clearances = [clearance]
    result.min_clearance_m = -0.001
    result.min_clearance_pair = clearance.pair

    assert result.pair_clearances[0].signed_clearance_m == -0.001
    assert result.min_clearance_pair.body_b == "scene/part/shape/0"


def test_collision_policy_revision_and_snapshot_round_trip():
    snapshot = manyforge_core_py.CollisionPolicySnapshot()
    snapshot.policy_id = "pick_with_support"
    snapshot.revision = manyforge_core_py.CollisionPolicyRevision(
        revision_id=12, content_hash="policy-hash"
    )
    static_pair = manyforge_core_py.CollisionPairId(
        body_a="scene/a/shape/0", body_b="scene/b/shape/0"
    )
    touch_pair = manyforge_core_py.CollisionPairId(
        body_a="component/arm/model/arm_model/body/wrist/collision/0",
        body_b="scene/part/shape/0",
    )
    support = manyforge_core_py.SupportContactPolicyEntry()
    support.pair = manyforge_core_py.CollisionPairId(
        body_a="scene/part/shape/0", body_b="scene/table/shape/0"
    )
    contact_grasp = manyforge_core_py.ContactGraspPolicyEntry()
    contact_grasp.policy_id = "contact_grasp/part"
    contact_grasp.selected_object_id = "part"
    contact_grasp.allowed_robot_links = ["left_finger", "right_finger"]
    contact_grasp.max_penetration_m = 0.002
    contact_grasp.terminal_window_start_fraction = 0.75
    snapshot.allowed_static_static_pairs = [static_pair]
    snapshot.allowed_touch_link_pairs = [touch_pair]
    snapshot.support_contacts = [support]
    snapshot.contact_grasps = [contact_grasp]
    snapshot.require_full_diagnostics = False

    assert snapshot.policy_id == "pick_with_support"
    assert snapshot.revision.revision_id == 12
    assert snapshot.revision.content_hash == "policy-hash"
    assert snapshot.allowed_static_static_pairs[0].body_a == "scene/a/shape/0"
    assert snapshot.allowed_touch_link_pairs[0].body_a.startswith("component/")
    assert snapshot.support_contacts[0].pair.body_b == "scene/table/shape/0"
    assert snapshot.contact_grasps[0].policy_id == "contact_grasp/part"
    assert snapshot.contact_grasps[0].allowed_robot_links == [
        "left_finger",
        "right_finger",
    ]
    assert snapshot.require_full_diagnostics is False


# ---------------------------------------------------------------------------
# CollisionGeometryDescriptor
# ---------------------------------------------------------------------------


def test_collision_geometry_descriptor_default_and_writable():
    d = manyforge_core_py.CollisionGeometryDescriptor()
    assert d.body_id == ""
    assert d.owner_kind == manyforge_core_py.CollisionOwnerKind.kSceneResource
    assert d.geometry_kind == manyforge_core_py.CollisionGeometryKind.kBox
    assert d.box_dimensions_m is None
    assert d.sphere_radius_m is None
    assert d.cylinder_radius_m is None
    assert d.cylinder_height_m is None
    assert d.capsule_radius_m is None
    assert d.capsule_height_m is None
    assert d.mesh_scale is None
    assert d.source_uri is None
    assert d.source_hash is None
    assert d.triangle_count is None
    assert d.convex_part_count is None
    assert d.memory_estimate_bytes is None
    assert d.quality_labels == []

    d.body_id = "scene/cheese_box/shape/0"
    d.owner_kind = manyforge_core_py.CollisionOwnerKind.kSceneResource
    d.geometry_kind = manyforge_core_py.CollisionGeometryKind.kConvexMesh
    d.box_dimensions_m = manyforge_core_py.Vector3DTO(x_m=0.1, y_m=0.2, z_m=0.3)
    d.mesh_scale = manyforge_core_py.Vector3DTO(x_m=1.0, y_m=1.5, z_m=1.15)
    d.sphere_radius_m = 0.05
    d.cylinder_radius_m = 0.04
    d.cylinder_height_m = 0.2
    d.capsule_radius_m = 0.03
    d.capsule_height_m = 0.4
    d.source_uri = "file:///tmp/cheese.obj"
    d.source_hash = "sha256:0123"
    d.quality_labels = ["convex", "watertight"]
    d.triangle_count = 1024
    d.convex_part_count = 1
    d.memory_estimate_bytes = 4096

    assert d.body_id == "scene/cheese_box/shape/0"
    assert d.owner_kind == manyforge_core_py.CollisionOwnerKind.kSceneResource
    assert d.geometry_kind == manyforge_core_py.CollisionGeometryKind.kConvexMesh
    assert d.box_dimensions_m.x_m == 0.1
    assert d.sphere_radius_m == 0.05
    assert d.cylinder_radius_m == 0.04
    assert d.cylinder_height_m == 0.2
    assert d.capsule_radius_m == 0.03
    assert d.capsule_height_m == 0.4
    assert d.mesh_scale.x_m == 1.0
    assert d.mesh_scale.y_m == 1.5
    assert d.mesh_scale.z_m == 1.15
    assert d.source_uri == "file:///tmp/cheese.obj"
    assert d.source_hash == "sha256:0123"
    assert d.quality_labels == ["convex", "watertight"]
    assert d.triangle_count == 1024
    assert d.convex_part_count == 1
    assert d.memory_estimate_bytes == 4096


# ---------------------------------------------------------------------------
# CollisionQuery
# ---------------------------------------------------------------------------


def _make_robot_state():
    rs = manyforge_core_py.RobotStateDTO()
    rs.joint_names = ["j1", "j2"]
    rs.positions_rad = [0.0, 0.5]
    rs.velocities_rad_s = []
    return rs


def test_collision_query_default():
    q = manyforge_core_py.CollisionQuery()
    assert q.query_id == ""
    assert q.validation_id is None
    assert q.surface == manyforge_core_py.CollisionSurface.kPreExecutionValidation
    assert q.policy_revision is None
    assert q.waypoint_index is None
    assert q.state_time_ns is None
    assert q.sampling_mode == manyforge_core_py.CollisionSamplingMode.kEngineDefault
    assert q.timeout_budget_ns is None
    assert q.required_capabilities == []
    assert (
        q.distance_request_mode
        == manyforge_core_py.CollisionDistanceRequestMode.kNone
    )
    assert q.selected_distance_pairs == []
    assert q.max_contacts is None


def test_collision_query_round_trip():
    q = manyforge_core_py.CollisionQuery()
    q.query_id = "q-1"
    q.validation_id = "v-7"
    q.surface = manyforge_core_py.CollisionSurface.kPreExecutionValidation
    rev = manyforge_core_py.CollisionWorldRevision(
        revision_id=3, content_hash="rev-content"
    )
    q.collision_world_revision = rev
    q.policy_revision = "policy.v0.1"
    q.robot_state = _make_robot_state()
    q.waypoint_index = 12
    q.state_time_ns = 1_700_000_000_000_000_000
    q.sampling_mode = manyforge_core_py.CollisionSamplingMode.kInterpolatedDiscrete
    q.timeout_budget_ns = 25_000_000
    q.required_capabilities = ["distance", "contact_points"]
    q.distance_request_mode = (
        manyforge_core_py.CollisionDistanceRequestMode.kSelectedCollidingPairs
    )
    q.selected_distance_pairs = [
        manyforge_core_py.CollisionPairId(
            body_a="component/arm/model/model/body/a/collision/0",
            body_b="scene/b/shape/0",
        )
    ]
    q.max_contacts = 16

    assert q.query_id == "q-1"
    assert q.validation_id == "v-7"
    assert q.surface == manyforge_core_py.CollisionSurface.kPreExecutionValidation
    assert q.collision_world_revision.revision_id == 3
    assert q.collision_world_revision.content_hash == "rev-content"
    assert q.policy_revision == "policy.v0.1"
    assert q.robot_state.joint_names == ["j1", "j2"]
    assert q.waypoint_index == 12
    assert q.state_time_ns == 1_700_000_000_000_000_000
    assert q.sampling_mode == manyforge_core_py.CollisionSamplingMode.kInterpolatedDiscrete
    assert q.timeout_budget_ns == 25_000_000
    assert q.required_capabilities == ["distance", "contact_points"]
    assert (
        q.distance_request_mode
        == manyforge_core_py.CollisionDistanceRequestMode.kSelectedCollidingPairs
    )
    assert q.selected_distance_pairs[0].body_a.startswith("component/")
    assert q.max_contacts == 16


# ---------------------------------------------------------------------------
# CollisionResult
# ---------------------------------------------------------------------------


def test_collision_result_default():
    r = manyforge_core_py.CollisionResult()
    assert r.in_collision is False
    assert r.contact_body_a is None
    assert r.contact_body_b is None
    assert r.normalized_pairs == []
    assert r.query_id is None
    assert r.collision_world_revision is None
    assert r.policy_revision is None
    assert r.surface == manyforge_core_py.CollisionSurface.kPreExecutionValidation
    assert r.state_time_ns is None
    assert r.engine_status == manyforge_core_py.CollisionEngineStatus.kOk
    assert r.decision == manyforge_core_py.CollisionDecisionResult.kPass
    assert r.engine_id == ""
    assert r.engine_version == ""
    assert r.query_duration_ns is None
    assert r.sampling_mode == manyforge_core_py.CollisionSamplingMode.kEngineDefault
    assert r.min_clearance_m is None
    assert r.min_clearance_pair is None
    assert r.normalized_pairs_classified == []
    assert r.policy_revision_used is None
    assert r.raw_diagnostics == {}


def test_collision_result_round_trip():
    r = manyforge_core_py.CollisionResult()
    r.in_collision = True
    r.contact_body_a = "raw_a"
    r.contact_body_b = "raw_b"
    pair = manyforge_core_py.CollisionPairId(
        body_a="scene/box_a/shape/0", body_b="scene/box_b/shape/0"
    )
    r.normalized_pairs = [pair]
    r.query_id = "q-2"
    r.collision_world_revision = manyforge_core_py.CollisionWorldRevision(
        revision_id=5, content_hash="hh"
    )
    r.policy_revision = "policy.v0.1"
    r.surface = manyforge_core_py.CollisionSurface.kPreExecutionValidation
    r.state_time_ns = 1_700_000_000_000_000_001
    r.engine_status = manyforge_core_py.CollisionEngineStatus.kOk
    r.decision = manyforge_core_py.CollisionDecisionResult.kFail
    r.engine_id = "roboplan_scene"
    r.engine_version = "unknown"
    r.query_duration_ns = 1_234_567
    r.sampling_mode = manyforge_core_py.CollisionSamplingMode.kEngineDefault
    classified = manyforge_core_py.CollisionPairResult()
    classified.pair = pair
    classified.classification = (
        manyforge_core_py.CollisionPairClassification.kBlockingCollision
    )
    classified.is_first_failure = True
    r.normalized_pairs_classified = [classified]
    r.policy_revision_used = "12"
    r.raw_diagnostics = {"snapshot_sync_error": "unsupported mesh"}
    # Chunk 1: distance fields stay null even on a populated result.
    assert r.min_clearance_m is None
    assert r.min_clearance_pair is None

    assert r.in_collision is True
    assert r.normalized_pairs[0].body_a == "scene/box_a/shape/0"
    assert r.collision_world_revision.revision_id == 5
    assert r.decision == manyforge_core_py.CollisionDecisionResult.kFail
    assert r.engine_id == "roboplan_scene"
    assert r.engine_version == "unknown"
    assert r.query_duration_ns == 1_234_567
    assert r.normalized_pairs_classified[0].is_first_failure is True
    assert r.policy_revision_used == "12"
    assert r.raw_diagnostics == {"snapshot_sync_error": "unsupported mesh"}


# ---------------------------------------------------------------------------
# CollisionDecision
# ---------------------------------------------------------------------------


def test_collision_decision_default():
    d = manyforge_core_py.CollisionDecision()
    assert d.accepted is True
    assert d.first_failing_waypoint is None
    assert d.first_failing_time_ns is None
    assert d.severity == manyforge_core_py.CollisionDecisionSeverity.kNone
    assert d.action == manyforge_core_py.CollisionDecisionAction.kDispatch
    assert d.collision_world_revision is None
    assert d.failure_archive_id is None


def test_collision_decision_round_trip():
    d = manyforge_core_py.CollisionDecision()
    d.accepted = False
    d.first_failing_waypoint = 4
    d.first_failing_time_ns = 1_500_000
    d.severity = manyforge_core_py.CollisionDecisionSeverity.kBlock
    d.action = manyforge_core_py.CollisionDecisionAction.kReject
    d.collision_world_revision = manyforge_core_py.CollisionWorldRevision(
        revision_id=9, content_hash="hh"
    )
    d.failure_archive_id = "archive-001"

    assert d.accepted is False
    assert d.first_failing_waypoint == 4
    assert d.first_failing_time_ns == 1_500_000
    assert d.severity == manyforge_core_py.CollisionDecisionSeverity.kBlock
    assert d.action == manyforge_core_py.CollisionDecisionAction.kReject
    assert d.collision_world_revision.revision_id == 9
    assert d.failure_archive_id == "archive-001"


# ---------------------------------------------------------------------------
# CollisionFailureArchiveEntry
# ---------------------------------------------------------------------------


def test_collision_failure_archive_entry_default():
    e = manyforge_core_py.CollisionFailureArchiveEntry()
    assert e.entry_id == ""
    assert e.emitted_at_ns == 0
    assert e.engine_id == ""
    assert e.engine_version == ""
    assert e.collision_world_revision.revision_id == 0
    assert e.policy_revision is None
    assert e.failure_kind == manyforge_core_py.CollisionFailureKind.kBlockingCollision
    assert e.decision_action == manyforge_core_py.CollisionDecisionAction.kInconclusive
    assert e.decision_severity == manyforge_core_py.CollisionDecisionSeverity.kBlock
    assert e.normalized_pairs == []
    assert e.first_failing_waypoint is None
    assert e.first_failing_time_ns is None
    assert e.trajectory_waypoint_count == 0
    assert e.query_duration_ns is None
    assert e.worker_count_actually_used == 1
    assert e.fallback_reason == ""
    assert e.raw_diagnostics == {}
    assert e.correlation_id is None


def test_collision_failure_archive_entry_round_trip():
    e = manyforge_core_py.CollisionFailureArchiveEntry()
    e.entry_id = "failure-001"
    e.emitted_at_ns = 1_700_000_000_000_000_002
    e.engine_id = "manyforge_collision/ur5_gripper"
    e.engine_version = "v1"
    e.collision_world_revision = manyforge_core_py.CollisionWorldRevision(
        revision_id=12, content_hash="world-hash"
    )
    e.policy_revision = "policy-7"
    e.failure_kind = manyforge_core_py.CollisionFailureKind.kCancelled
    e.decision_action = manyforge_core_py.CollisionDecisionAction.kInconclusive
    e.decision_severity = manyforge_core_py.CollisionDecisionSeverity.kBlock
    pair = manyforge_core_py.CollisionPairId(
        body_a="component/arm/model/arm_model/body/tool/collision/0",
        body_b="scene/a/shape/0",
    )
    e.normalized_pairs = [pair]
    e.first_failing_waypoint = 4
    e.first_failing_time_ns = 123_000_000
    e.trajectory_waypoint_count = 10
    e.query_duration_ns = 55_000
    e.worker_count_actually_used = 4
    e.fallback_reason = ""
    e.raw_diagnostics = {"cancel_reason": "operator"}
    e.correlation_id = "move-42"

    assert e.entry_id == "failure-001"
    assert e.engine_id == "manyforge_collision/ur5_gripper"
    assert e.collision_world_revision.revision_id == 12
    assert e.policy_revision == "policy-7"
    assert e.failure_kind == manyforge_core_py.CollisionFailureKind.kCancelled
    assert e.normalized_pairs[0].body_a.endswith("/collision/0")
    assert e.normalized_pairs[0].body_b == "scene/a/shape/0"
    assert e.first_failing_waypoint == 4
    assert e.raw_diagnostics == {"cancel_reason": "operator"}
    assert e.correlation_id == "move-42"


# ---------------------------------------------------------------------------
# CollisionWorldSnapshot + builder
# ---------------------------------------------------------------------------


def _identity_pose():
    p = manyforge_core_py.Pose3DTO()
    p.position_m = manyforge_core_py.Vector3DTO(x_m=0.0, y_m=0.0, z_m=0.0)
    p.orientation = manyforge_core_py.QuaternionDTO(x=0.0, y=0.0, z=0.0, w=1.0)
    return p


def _box_geom(body_id, owner_kind):
    g = manyforge_core_py.CollisionGeometryDescriptor()
    g.body_id = body_id
    g.owner_kind = owner_kind
    g.geometry_kind = manyforge_core_py.CollisionGeometryKind.kBox
    g.local_pose = _identity_pose()
    g.box_dimensions_m = manyforge_core_py.Vector3DTO(x_m=0.1, y_m=0.1, z_m=0.1)
    return g


def _component_body(body_role):
    body_id = f"component/arm/model/arm_model/body/{body_role}/collision/0"
    b = manyforge_core_py.CollisionWorldBody()
    b.body_id = body_id
    b.owner_kind = manyforge_core_py.CollisionOwnerKind.kComponentModelBody
    b.frame_id = f"compiled_{body_role}"
    b.pose = _identity_pose()
    b.geometries = [
        _box_geom(
            body_id, manyforge_core_py.CollisionOwnerKind.kComponentModelBody
        )
    ]
    return b


def test_collision_world_snapshot_default():
    s = manyforge_core_py.CollisionWorldSnapshot()
    assert s.snapshot_format_version == 1
    assert s.publisher_id == ""
    assert s.revision.revision_id == 0
    assert s.component_model_frames == []
    assert s.bodies == []
    assert s.body_count == 0
    assert s.geometry_count == 0
    assert not s.contains_mesh_geometry
    assert s.all_bodies_have_geometry


def test_collision_world_body_round_trip():
    b = _component_body("link1")
    assert b.body_id == "component/arm/model/arm_model/body/link1/collision/0"
    assert b.owner_kind == manyforge_core_py.CollisionOwnerKind.kComponentModelBody
    assert b.parent_body_id is None
    assert b.frame_id == "compiled_link1"
    assert len(b.geometries) == 1


def _component_model_frame():
    return manyforge_core_py.CollisionComponentModelFrame(
        controllable_component_id=manyforge_core_py.ControllableComponentId("arm"),
        model_instance_id=manyforge_core_py.ModelInstanceId("arm_model"),
        frame_role_id=manyforge_core_py.FrameRoleId("base"),
        compiled_frame_name="base_link",
        model_revision=manyforge_core_py.ModelRevision("model-revision-1"),
        world_pose=_identity_pose(),
    )


def test_collision_component_model_frame_round_trip():
    frame = _component_model_frame()

    assert frame.controllable_component_id.value == "arm"
    assert frame.model_instance_id.value == "arm_model"
    assert frame.frame_role_id.value == "base"
    assert frame.compiled_frame_name == "base_link"
    assert frame.model_revision.value == "model-revision-1"
    assert frame.world_pose.orientation.w == 1.0


def test_collision_world_snapshot_builder_happy_path():
    frame = _component_model_frame()
    builder = manyforge_core_py.CollisionWorldSnapshotBuilder()
    (
        builder.set_publisher_id("py_test")
        .set_revision_id(7)
        .set_emitted_at_ns(1234)
        .add_component_model_frame(frame)
        .add_body(_component_body("link1"))
        .add_body(_component_body("link2"))
    )
    snap = builder.build()
    assert snap.publisher_id == "py_test"
    assert snap.revision.revision_id == 7
    assert snap.revision.content_hash  # non-empty
    assert snap.emitted_at_ns == 1234
    assert len(snap.component_model_frames) == 1
    assert snap.component_model_frames[0].compiled_frame_name == "base_link"
    assert snap.body_count == 2
    assert snap.geometry_count == 2
    assert snap.component_model_body_count == 2
    assert snap.scene_resource_count == 0
    assert snap.runtime_object_count == 0
    assert not snap.contains_mesh_geometry
    assert snap.all_bodies_have_geometry
    # Bodies sorted lexicographically.
    assert snap.bodies[0].body_id < snap.bodies[1].body_id


def test_collision_world_snapshot_builder_raises_on_empty_publisher():
    builder = (
        manyforge_core_py.CollisionWorldSnapshotBuilder()
        .set_revision_id(1)
        .add_body(_component_body("link1"))
    )
    with pytest.raises(ValueError) as exc:
        builder.build()
    assert "publisher_id" in str(exc.value)


def test_collision_world_snapshot_builder_raises_on_zero_revision():
    builder = (
        manyforge_core_py.CollisionWorldSnapshotBuilder()
        .set_publisher_id("py_test")
        .add_body(_component_body("link1"))
    )
    with pytest.raises(ValueError):
        builder.build()


def test_collision_world_snapshot_builder_raises_on_zero_format_version():
    builder = (
        manyforge_core_py.CollisionWorldSnapshotBuilder()
        .set_publisher_id("py_test")
        .set_snapshot_format_version(0)
        .set_revision_id(1)
        .add_body(_component_body("link1"))
    )
    with pytest.raises(ValueError):
        builder.build()


def test_collision_world_snapshot_builder_raises_on_duplicate_body_ids():
    builder = (
        manyforge_core_py.CollisionWorldSnapshotBuilder()
        .set_publisher_id("py_test")
        .set_revision_id(1)
        .add_body(_component_body("link1"))
        .add_body(_component_body("link1"))
    )
    with pytest.raises(ValueError) as exc:
        builder.build()
    assert "duplicate" in str(exc.value).lower()


def test_collision_world_snapshot_builder_raises_on_owner_kind_id_mismatch():
    body = _component_body("link1")
    body.body_id = "scene/link1/shape/0"
    body.geometries = [
        _box_geom(
            body.body_id,
            manyforge_core_py.CollisionOwnerKind.kComponentModelBody,
        )
    ]
    builder = (
        manyforge_core_py.CollisionWorldSnapshotBuilder()
        .set_publisher_id("py_test")
        .set_revision_id(1)
        .add_body(body)
    )
    with pytest.raises(ValueError):
        builder.build()


def test_collision_world_snapshot_builder_determinism():
    def build():
        return (
            manyforge_core_py.CollisionWorldSnapshotBuilder()
            .set_publisher_id("py_test")
            .set_revision_id(3)
            .add_body(_component_body("link1"))
            .add_body(_component_body("link2"))
            .build()
        )

    a, b = build(), build()
    assert a.revision.content_hash == b.revision.content_hash
