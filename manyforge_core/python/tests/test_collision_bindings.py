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
    # Note: kRobotLink collides with ReferenceType.kRobotLink at module scope.
    # Check membership on the enum class itself, not the module-top alias.
    e = manyforge_core_py.CollisionOwnerKind
    for name in ("kRobotLink", "kAttachedObject", "kDetachedObject", "kFixture"):
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
        "kStreamingTargetValidation",
        "kStreamingPredictedMonitor",
        "kStreamingObservedMonitor",
    ):
        assert getattr(e, name) is not None


def test_collision_sampling_mode_enum():
    e = manyforge_core_py.CollisionSamplingMode
    for name in ("kWaypointOnly", "kInterpolatedDiscrete", "kCcd", "kEngineDefault"):
        assert getattr(e, name) is not None


def test_collision_engine_status_enum():
    e = manyforge_core_py.CollisionEngineStatus
    for name in (
        "kOk",
        "kTimeout",
        "kUnsupportedGeometry",
        "kDiagnosticIncomplete",
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


def test_collision_world_mutation_source_enum():
    e = manyforge_core_py.CollisionWorldMutationSource
    for name in (
        "kProgramLoad",
        "kSceneEdit",
        "kAttach",
        "kDetach",
        "kPoseSourceApply",
        "kDeploymentLoad",
        "kPolicyUpdate",
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
        "kDiagnosticIncomplete",
    ):
        assert getattr(e, name) is not None


# ---------------------------------------------------------------------------
# CollisionWorldLedgerEntry (Chunk 2)
# ---------------------------------------------------------------------------


def test_collision_world_ledger_entry_default():
    e = manyforge_core_py.CollisionWorldLedgerEntry()
    assert e.revision.revision_id == 0
    assert e.revision.content_hash == ""
    assert e.previous_revision_id == 0
    assert (
        e.mutation_source
        == manyforge_core_py.CollisionWorldMutationSource.kProgramLoad
    )
    assert e.affected_body_ids == []
    assert e.timestamp_ns == 0
    assert e.authority_source == ""
    assert e.session_id is None


def test_collision_world_ledger_entry_round_trip():
    e = manyforge_core_py.CollisionWorldLedgerEntry()
    e.revision = manyforge_core_py.CollisionWorldRevision(
        revision_id=3, content_hash="rev-3"
    )
    e.previous_revision_id = 2
    e.mutation_source = manyforge_core_py.CollisionWorldMutationSource.kAttach
    e.affected_body_ids = ["object/box_a/shape/0"]
    e.timestamp_ns = 1_700_000_000_000_000_000
    e.authority_source = "scene_edit.attach_object"
    e.session_id = "operator-42"

    assert e.revision.revision_id == 3
    assert e.revision.content_hash == "rev-3"
    assert e.previous_revision_id == 2
    assert (
        e.mutation_source
        == manyforge_core_py.CollisionWorldMutationSource.kAttach
    )
    assert e.affected_body_ids == ["object/box_a/shape/0"]
    assert e.timestamp_ns == 1_700_000_000_000_000_000
    assert e.authority_source == "scene_edit.attach_object"
    assert e.session_id == "operator-42"
    assert "revision_id=3" in repr(e)


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
        body_a="object/cheese_box/shape/0",
        body_b="robot/ur10e/link/wrist_3_link/collision/0",
    )
    assert p.body_a == "object/cheese_box/shape/0"
    assert p.body_b == "robot/ur10e/link/wrist_3_link/collision/0"
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
        body_a="object/a/shape/0", body_b="object/b/shape/0"
    )
    r.classification = (
        manyforge_core_py.CollisionPairClassification.kAllowedStaticStatic
    )
    r.is_first_failure = True
    assert r.pair.body_a == "object/a/shape/0"
    assert (
        r.classification
        == manyforge_core_py.CollisionPairClassification.kAllowedStaticStatic
    )
    assert r.is_first_failure is True


def test_support_contact_policy_entry_round_trip():
    e = manyforge_core_py.SupportContactPolicyEntry()
    e.pair = manyforge_core_py.CollisionPairId(
        body_a="object/part/shape/0",
        body_b="fixture/table/shape/0",
    )
    e.max_start_penetration_m = 0.001
    e.require_separation_by_end = False
    e.allow_recontact = True

    assert e.pair.body_b == "fixture/table/shape/0"
    assert e.max_start_penetration_m == 0.001
    assert e.require_separation_by_end is False
    assert e.allow_recontact is True


def test_collision_policy_revision_and_snapshot_round_trip():
    snapshot = manyforge_core_py.CollisionPolicySnapshot()
    snapshot.policy_id = "pick_with_support"
    snapshot.revision = manyforge_core_py.CollisionPolicyRevision(
        revision_id=12, content_hash="policy-hash"
    )
    static_pair = manyforge_core_py.CollisionPairId(
        body_a="fixture/a/shape/0", body_b="fixture/b/shape/0"
    )
    touch_pair = manyforge_core_py.CollisionPairId(
        body_a="object/part/shape/0",
        body_b="robot/robot0/link/wrist_3_link/collision/0",
    )
    support = manyforge_core_py.SupportContactPolicyEntry()
    support.pair = manyforge_core_py.CollisionPairId(
        body_a="object/part/shape/0", body_b="fixture/table/shape/0"
    )
    snapshot.allowed_static_static_pairs = [static_pair]
    snapshot.allowed_touch_link_pairs = [touch_pair]
    snapshot.support_contacts = [support]
    snapshot.require_full_diagnostics = False

    assert snapshot.policy_id == "pick_with_support"
    assert snapshot.revision.revision_id == 12
    assert snapshot.revision.content_hash == "policy-hash"
    assert snapshot.allowed_static_static_pairs[0].body_a == "fixture/a/shape/0"
    assert snapshot.allowed_touch_link_pairs[0].body_b.startswith("robot/")
    assert snapshot.support_contacts[0].pair.body_b == "fixture/table/shape/0"
    assert snapshot.require_full_diagnostics is False


# ---------------------------------------------------------------------------
# CollisionGeometryDescriptor
# ---------------------------------------------------------------------------


def test_collision_geometry_descriptor_default_and_writable():
    d = manyforge_core_py.CollisionGeometryDescriptor()
    assert d.body_id == ""
    assert d.owner_kind == manyforge_core_py.CollisionOwnerKind.kFixture
    assert d.geometry_kind == manyforge_core_py.CollisionGeometryKind.kBox
    assert d.source_uri is None
    assert d.source_hash is None
    assert d.triangle_count is None
    assert d.convex_part_count is None
    assert d.memory_estimate_bytes is None
    assert d.quality_labels == []

    d.body_id = "object/cheese_box/shape/0"
    d.owner_kind = manyforge_core_py.CollisionOwnerKind.kDetachedObject
    d.geometry_kind = manyforge_core_py.CollisionGeometryKind.kConvexMesh
    d.source_uri = "file:///tmp/cheese.obj"
    d.source_hash = "sha256:0123"
    d.quality_labels = ["convex", "watertight"]
    d.triangle_count = 1024
    d.convex_part_count = 1
    d.memory_estimate_bytes = 4096

    assert d.body_id == "object/cheese_box/shape/0"
    assert d.owner_kind == manyforge_core_py.CollisionOwnerKind.kDetachedObject
    assert d.geometry_kind == manyforge_core_py.CollisionGeometryKind.kConvexMesh
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
    assert q.max_contacts is None


def test_collision_query_round_trip():
    q = manyforge_core_py.CollisionQuery()
    q.query_id = "q-1"
    q.validation_id = "v-7"
    q.surface = manyforge_core_py.CollisionSurface.kStreamingTargetValidation
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
    q.max_contacts = 16

    assert q.query_id == "q-1"
    assert q.validation_id == "v-7"
    assert q.surface == manyforge_core_py.CollisionSurface.kStreamingTargetValidation
    assert q.collision_world_revision.revision_id == 3
    assert q.collision_world_revision.content_hash == "rev-content"
    assert q.policy_revision == "policy.v0.1"
    assert q.robot_state.joint_names == ["j1", "j2"]
    assert q.waypoint_index == 12
    assert q.state_time_ns == 1_700_000_000_000_000_000
    assert q.sampling_mode == manyforge_core_py.CollisionSamplingMode.kInterpolatedDiscrete
    assert q.timeout_budget_ns == 25_000_000
    assert q.required_capabilities == ["distance", "contact_points"]
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


def test_collision_result_round_trip():
    r = manyforge_core_py.CollisionResult()
    r.in_collision = True
    r.contact_body_a = "raw_a"
    r.contact_body_b = "raw_b"
    pair = manyforge_core_py.CollisionPairId(
        body_a="object/box_a/shape/0", body_b="object/box_b/shape/0"
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
    # Chunk 1: distance fields stay null even on a populated result.
    assert r.min_clearance_m is None
    assert r.min_clearance_pair is None

    assert r.in_collision is True
    assert r.normalized_pairs[0].body_a == "object/box_a/shape/0"
    assert r.collision_world_revision.revision_id == 5
    assert r.decision == manyforge_core_py.CollisionDecisionResult.kFail
    assert r.engine_id == "roboplan_scene"
    assert r.engine_version == "unknown"
    assert r.query_duration_ns == 1_234_567
    assert r.normalized_pairs_classified[0].is_first_failure is True
    assert r.policy_revision_used == "12"


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
