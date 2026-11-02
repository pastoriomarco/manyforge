"""Focused tests for the retained owner-neutral pose adapter mechanics.

Historical acceptance map (exact blob
``453edb0ba3aee414bbe44d26a310465350d0c8f4``):
the ordered 14-definition-name cohort has SHA-256
``02d3753049b0628010f76bc820c559142ebf9729684e50df8858ea2f57cfa7af``.

* lines 9-25: fixed-offset finite-input validation -> retained here;
* lines 28-151, 220-397: simulator capture/pose/TCP mapping -> retained as
  stateless capture tests; latest/freshness/status/payload assertions move to
  H0/I5 and concrete simulator wiring moves to I9/I;
* lines 154-217: JointState translation -> retained without the angular-only
  or latest-cache assumptions; receiver-owned state/freshness moves to H0/N;
* lines 400-682: PoseStamped/PoseArray translation and depth-one transport ->
  retained without object association, status, policy, or cache ownership;
* lines 685-766: TF lookup/translation -> retained as a stateless batch;
* lines 769-1037: Detection3D association, confidence, covariance, and TF ->
  retained without scene-object/apply-policy/latest-state ownership; and
* lines 1040-1136: fake ROS helpers -> replaced by message-shaped pure fakes.

All 14 historical test definitions remain recoverable at that blob.  These
focused cases are current source tests, while the removed assertions remain
future acceptance input rather than commented-out legacy code.
"""

from __future__ import annotations

import ast
import inspect
from math import nan
from types import SimpleNamespace

import pytest

from manyforge_ros import pose_source_sidecar
from manyforge_ros.pose_source_sidecar import (
    DepthOneSubscriptionAdapter,
    DetectionSelector,
    ObservationNormalizationError,
    SimulationEntityPoseRequest,
    TfPoseRequest,
    capture_simulation_entity_poses,
    capture_tf_poses,
    compose_pose_with_local_offset,
    normalize_detection3d_array_message,
    normalize_joint_state_message,
    normalize_pose_array_message,
    normalize_pose_stamped_message,
    read_simulation_entity_states,
    relative_pose,
    transform_pose_snapshot,
)
from manyforge_ros.scene_mirror_probe import PoseSnapshot


def test_fixed_local_offset_rejects_non_finite_source_position() -> None:
    pose = PoseSnapshot(
        frame_id="world",
        stamp_ns=1000000000,
        position_xyz=(nan, 0.0, 0.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )

    with pytest.raises(
        ObservationNormalizationError,
        match=r"source pose position_xyz\[0\] must be finite",
    ):
        compose_pose_with_local_offset(
            pose,
            (0.0, 0.0, 0.2),
            (0.0, 0.0, 0.0, 1.0),
        )


def test_fixed_local_offset_rotates_translation_and_orientation() -> None:
    half_root = 2.0**-0.5
    pose = PoseSnapshot(
        frame_id="world",
        stamp_ns=1000000000,
        position_xyz=(0.5, 0.0, 0.25),
        orientation_xyzw=(0.0, 0.0, half_root, half_root),
    )

    composed = compose_pose_with_local_offset(
        pose,
        (0.2, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )

    assert composed.position_xyz == pytest.approx((0.5, 0.2, 0.25))
    assert composed.orientation_xyzw == pytest.approx(pose.orientation_xyzw)


def test_pose_stamped_normalizes_frame_stamp_and_quaternion() -> None:
    message = _pose_stamped("world", 12, 250_000_000, 1.0, 2.0, 3.0, w=2.0)

    pose = normalize_pose_stamped_message(message)

    assert pose.frame_id == "world"
    assert pose.stamp_ns == 12250000000
    assert pose.position_xyz == (1.0, 2.0, 3.0)
    assert pose.orientation_xyzw == (0.0, 0.0, 0.0, 1.0)


@pytest.mark.parametrize("nanoseconds", [
    (1 << 53) - 1, 1 << 53, (1 << 53) + 1,
    105 * 86400 * 1_000_000_000, 31 * 366 * 86400 * 1_000_000_000 + 1,
])
def test_pose_timestamp_is_exact_across_long_service_boundaries(nanoseconds: int) -> None:
    sec, nanos = divmod(nanoseconds, 1_000_000_000)
    pose = normalize_pose_stamped_message(
        _pose_stamped("world", sec, nanos, 0.0, 0.0, 0.0, w=1.0)
    )
    assert pose.stamp_ns == nanoseconds


@pytest.mark.parametrize(
    ("sec", "nanosec"),
    [("12", 0), (True, 0), (12, -1), (12, 1_000_000_000), (-1, 0), (1 << 63, 0)],
)
def test_pose_stamped_rejects_non_integral_or_out_of_range_timestamps(
    sec: object,
    nanosec: object,
) -> None:
    message = _namespace(
        header=_namespace(frame_id="world", stamp=_namespace(sec=sec, nanosec=nanosec)),
        pose=_pose(0.0, 0.0, 0.0),
    )

    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_pose_stamped_message(message)

    assert raised.value.code == "invalid_timestamp"


def test_pose_stamped_rejects_unbounded_frames_and_invalid_pose_numbers() -> None:
    long_frame = _pose_stamped("f" * 1025, 0, 0, 0.0, 0.0, 0.0, w=1.0)
    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_pose_stamped_message(long_frame)
    assert raised.value.code == "invalid_pose"

    non_finite = _pose_stamped("world", 0, 0, float("inf"), 0.0, 0.0, w=1.0)
    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_pose_stamped_message(non_finite)
    assert raised.value.code == "invalid_pose"

    zero_quaternion = _pose_stamped("world", 0, 0, 0.0, 0.0, 0.0, w=0.0)
    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_pose_stamped_message(zero_quaternion)
    assert raised.value.code == "invalid_quaternion"


def test_pose_array_preserves_order_without_object_identity() -> None:
    message = _namespace(
        header=_header("world", 41, 0),
        poses=[_pose(1.0, 2.0, 3.0), _pose(4.0, 5.0, 6.0)],
    )

    poses = normalize_pose_array_message(message)

    assert [pose.position_xyz for pose in poses] == [
        (1.0, 2.0, 3.0),
        (4.0, 5.0, 6.0),
    ]
    assert all(pose.frame_id == "world" for pose in poses)
    assert not any(
        hasattr(pose, name)
        for pose in poses
        for name in ("object_id", "status")
    )


def test_pose_array_normalization_rejects_first_item_over_bound() -> None:
    message = _namespace(
        header=_header("world", 41, 0),
        poses=[_pose(0.0, 0.0, 0.0)] * 4097,
    )

    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_pose_array_message(message)

    assert raised.value.code == "invalid_pose_array"


def test_joint_state_normalization_does_not_claim_angular_only_units() -> None:
    message = _namespace(
        header=_header("", 17, 250_000_000),
        name=["revolute_joint", "prismatic_joint"],
        position=[0.25, 0.012],
    )

    sample = normalize_joint_state_message(message, topic="/joint_states")

    assert sample.topic == "/joint_states"
    assert sample.names == ("revolute_joint", "prismatic_joint")
    assert sample.positions == (0.25, 0.012)
    assert sample.source_stamp_ns == 17250000000
    assert not hasattr(sample, "joint_positions_rad")


@pytest.mark.parametrize(
    ("names", "positions", "detail"),
    [
        ([], [], "non-empty and equal"),
        (["joint"], [nan], "must be finite"),
        (["joint", "joint"], [0.0, 1.0], "must be unique"),
    ],
)
def test_joint_state_rejects_invalid_samples(
    names: list[str],
    positions: list[float],
    detail: str,
) -> None:
    message = _namespace(header=_header("", 0, 0), name=names, position=positions)

    with pytest.raises(ObservationNormalizationError, match=detail):
        normalize_joint_state_message(message, topic="/joint_states")


def test_detection_normalization_selects_best_score_and_applies_real_tf() -> None:
    message = _namespace(
        header=_header("camera", 80, 0),
        detections=[
            _detection("first", "box", 0.7, 1.0, 2.0, 3.0, covariance=0.002),
            _detection("best", "box", 0.9, 4.0, 5.0, 6.0, covariance=0.001),
        ],
    )
    buffer = _FakeTfBuffer(
        {("world", "camera"): _transform("world", "camera", 10.0, 0.0, 0.0)}
    )

    observation = normalize_detection3d_array_message(
        message,
        selector=DetectionSelector(
            selection="best_score",
            confidence_min=0.8,
            max_position_variance_m2=0.01,
        ),
        target_frame_id="world",
        tf_buffer=buffer,
        lookup_time=object(),
    )

    assert observation.detection_id == "best"
    assert observation.class_id == "box"
    assert observation.confidence == pytest.approx(0.9)
    assert observation.max_position_variance_m2 == pytest.approx(0.001)
    assert observation.pose.frame_id == "world"
    assert observation.pose.position_xyz == pytest.approx((14.0, 5.0, 6.0))
    assert buffer.lookups == [("world", "camera")]


@pytest.mark.parametrize(
    ("score", "covariance", "code"),
    [
        (0.1, 0.001, "confidence_below_min"),
        (0.95, 0.5, "covariance_exceeded"),
        (0.95, -0.1, "invalid_covariance"),
    ],
)
def test_detection_quality_failure_has_no_status_ownership(
    score: float,
    covariance: float,
    code: str,
) -> None:
    message = _namespace(
        header=_header("world", 90, 0),
        detections=[
            _detection("box-01", "box", score, 1.0, 0.0, 0.0, covariance=covariance)
        ],
    )

    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_detection3d_array_message(
            message,
            selector=DetectionSelector(
                class_id="box",
                confidence_min=0.8,
                max_position_variance_m2=0.01,
            ),
        )

    assert raised.value.code == code


def test_detection_covariance_and_nested_results_are_consumed_with_hard_caps() -> None:
    detection = _detection(
        "box-01",
        "box",
        0.9,
        0.0,
        0.0,
        0.0,
        covariance=0.001,
    )
    detection.results[0].pose.covariance.append(0.0)
    message = _namespace(
        header=_header("world", 0, 0),
        detections=[detection],
    )
    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_detection3d_array_message(
            message,
            selector=DetectionSelector(class_id="box"),
        )
    assert raised.value.code == "invalid_covariance"

    detection.results = detection.results * 4097
    with pytest.raises(ObservationNormalizationError) as raised:
        normalize_detection3d_array_message(
            message,
            selector=DetectionSelector(class_id="box"),
        )
    assert raised.value.code == "invalid_detection_array"


def test_pose_transform_requires_tf_instead_of_relabeling_frame() -> None:
    pose = PoseSnapshot(
        frame_id="camera",
        stamp_ns=1000000000,
        position_xyz=(1.0, 2.0, 3.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )

    with pytest.raises(ObservationNormalizationError) as raised:
        transform_pose_snapshot(
            pose,
            target_frame_id="world",
            buffer=None,
            lookup_time=object(),
        )

    assert raised.value.code == "frame_unresolved"


def test_simulation_capture_is_stateless_and_reports_each_entity() -> None:
    snapshot = _namespace(
        entities=(
            _namespace(
                name="/World/tool0",
                pose=PoseSnapshot(
                    frame_id="world",
                    stamp_ns=100000000000,
                    position_xyz=(0.5, 0.0, 0.25),
                    orientation_xyzw=(0.0, 0.0, 2.0**-0.5, 2.0**-0.5),
                ),
                errors=(),
            ),
        ),
        errors=("source warning",),
    )
    client = _FakeSimulationClient(snapshot)

    batch = capture_simulation_entity_poses(
        client,
        [
            SimulationEntityPoseRequest(
                entity="/World/tool0",
                local_position_xyz=(0.2, 0.0, 0.0),
            ),
            SimulationEntityPoseRequest(entity="/World/missing"),
        ],
    )

    assert len(batch.observations) == 1
    assert batch.observations[0].entity == "/World/tool0"
    assert batch.observations[0].pose.position_xyz == pytest.approx((0.5, 0.2, 0.25))
    assert [(item.source_key, item.code) for item in batch.failures] == [
        ("", "source_error"),
        ("/World/missing", "missing_entity"),
    ]
    assert client.calls == [
        {
            "entity_names": ("/World/tool0", "/World/missing"),
            "include_info": False,
            "include_state": True,
            "include_bounds": False,
        }
    ]
    assert not hasattr(batch, "status")
    assert not hasattr(batch, "snapshot")


def test_coherent_read_is_one_batch_and_falls_back_as_non_coherent() -> None:
    snapshot = _namespace(entities=(), errors=())

    class _BatchClient(_FakeSimulationClient):
        def __init__(self, batch: object) -> None:
            super().__init__(snapshot)
            self.batch = batch

        def capture_entity_states(self, **kwargs: object) -> object:
            self.calls.append({"batch": kwargs})
            return self.batch

    batch_snapshot = _namespace(
        entities=(
            _namespace(
                name="/obj",
                pose=PoseSnapshot("world", None, (1.0, 1.0, 0.0), (0.0, 0.0, 0.0, 1.0)),
                errors=(),
            ),
        ),
        errors=(),
    )
    client = _BatchClient(batch_snapshot)
    read, coherent = read_simulation_entity_states(
        client, ("/obj", "/ref"), coherent=True
    )
    assert coherent and read is batch_snapshot
    batch = capture_simulation_entity_poses(
        client, [SimulationEntityPoseRequest(entity="/obj")], snapshot=read
    )
    assert [item.entity for item in batch.observations] == ["/obj"]
    assert client.calls == [{"batch": {"entity_names": ("/obj", "/ref")}}]

    capture_kwargs = {
        "entity_names": ("/obj",),
        "include_info": False,
        "include_state": True,
        "include_bounds": False,
    }
    for fallback in (_BatchClient(None), _FakeSimulationClient(snapshot)):
        read, coherent = read_simulation_entity_states(
            fallback, ("/obj",), coherent=True
        )
        assert not coherent and read is snapshot
        assert fallback.calls[-1] == capture_kwargs
    unrequested = _BatchClient(batch_snapshot)
    assert read_simulation_entity_states(unrequested, ("/obj",), coherent=False)[1] is False
    assert unrequested.calls == [capture_kwargs]


def test_relative_pose_expresses_source_in_reference_frame() -> None:
    quarter = (0.0, 0.0, 2.0**-0.5, 2.0**-0.5)
    reference = PoseSnapshot("world", 7, (1.0, 0.0, 0.0), quarter)
    source = PoseSnapshot("world", 9, (1.0, 1.0, 0.5), quarter)
    relative = relative_pose(reference, source)
    assert relative.stamp_ns == 9
    assert relative.position_xyz == pytest.approx((1.0, 0.0, 0.5))
    assert relative.orientation_xyzw == pytest.approx((0.0, 0.0, 0.0, 1.0))
    with pytest.raises(ObservationNormalizationError):
        relative_pose(
            PoseSnapshot("other", 7, (0.0, 0.0, 0.0), quarter), source
        )


def test_simulation_request_values_are_bounded_finite_and_normalized() -> None:
    request = SimulationEntityPoseRequest(
        entity="/World/tool0",
        target_frame_id=" world ",
        local_orientation_xyzw=(0.0, 0.0, 0.0, 2.0),
    )
    assert request.target_frame_id == "world"
    assert request.local_orientation_xyzw == (0.0, 0.0, 0.0, 1.0)

    with pytest.raises(ValueError, match="must be declared together"):
        SimulationEntityPoseRequest(
            entity="/World/tool0",
            source_header_frame_id="tool0",
        )

    with pytest.raises(ValueError, match="target_frame_id exceeds"):
        SimulationEntityPoseRequest(
            entity="/World/tool0",
            target_frame_id="f" * 1025,
        )
    with pytest.raises(ValueError, match="text or boolean"):
        SimulationEntityPoseRequest(
            entity="/World/tool0",
            local_position_xyz=(True, 0.0, 0.0),
        )
    with pytest.raises(ValueError, match="text or boolean"):
        SimulationEntityPoseRequest(
            entity="/World/tool0",
            local_position_xyz=("0.0", 0.0, 0.0),  # type: ignore[arg-type]
        )
    with pytest.raises(ObservationNormalizationError, match="invalid norm"):
        SimulationEntityPoseRequest(
            entity="/World/tool0",
            local_orientation_xyzw=(0.0, 0.0, 0.0, 0.0),
        )


def test_simulation_capture_requires_exact_declared_header_before_real_tf() -> None:
    snapshot = _namespace(
        entities=(
            _namespace(
                name="/World/tool0",
                pose=PoseSnapshot(
                    frame_id="tool0",
                    stamp_ns=1000000000,
                    position_xyz=(1.0, 2.0, 3.0),
                    orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
                ),
                errors=(),
            ),
        ),
        errors=(),
    )
    buffer = _FakeTfBuffer(
        {("universe", "world"): _transform("universe", "world", 10.0, 0.0, 0.0)}
    )
    request = SimulationEntityPoseRequest(
        entity="/World/tool0",
        target_frame_id="universe",
        source_header_frame_id="tool0",
        source_coordinate_frame_id="world",
    )

    batch = capture_simulation_entity_poses(
        _FakeSimulationClient(snapshot),
        [request],
        tf_buffer=buffer,
    )

    assert batch.failures == ()
    assert batch.observations[0].pose.frame_id == "universe"
    assert batch.observations[0].pose.position_xyz == pytest.approx(
        (11.0, 2.0, 3.0)
    )

    changed = _namespace(
        entities=(
            _namespace(
                name="/World/tool0",
                pose=PoseSnapshot(
                    frame_id="changed_tool0",
                    stamp_ns=1000000000,
                    position_xyz=(1.0, 2.0, 3.0),
                    orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
                ),
                errors=(),
            ),
        ),
        errors=(),
    )
    rejected = capture_simulation_entity_poses(
        _FakeSimulationClient(changed),
        [request],
        tf_buffer=buffer,
    )
    assert rejected.observations == ()
    assert rejected.failures[0].code == "frame_or_pose_invalid"
    assert "source_frame_mismatch" in rejected.failures[0].detail


def test_simulation_capture_rejects_duplicate_or_unbounded_source_rows() -> None:
    pose = PoseSnapshot(
        frame_id="world",
        stamp_ns=1000000000,
        position_xyz=(0.0, 0.0, 0.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    duplicate_snapshot = _namespace(
        entities=(
            _namespace(name="/World/tool0", pose=pose, errors=()),
            _namespace(name="/World/tool0", pose=pose, errors=()),
        ),
        errors=(),
    )
    batch = capture_simulation_entity_poses(
        _FakeSimulationClient(duplicate_snapshot),
        [SimulationEntityPoseRequest(entity="/World/tool0")],
    )
    assert batch.observations == ()
    assert [failure.code for failure in batch.failures] == [
        "invalid_source_snapshot"
    ]
    assert "duplicate entity" in batch.failures[0].detail

    unbounded_errors_snapshot = _namespace(
        entities=(),
        errors=["source error"] * 4097,
    )
    batch = capture_simulation_entity_poses(
        _FakeSimulationClient(unbounded_errors_snapshot),
        [SimulationEntityPoseRequest(entity="/World/tool0")],
    )
    assert batch.observations == ()
    assert [failure.code for failure in batch.failures] == [
        "invalid_source_snapshot"
    ]

    unbounded_snapshot = _namespace(
        entities=[
            _namespace(name=f"/World/entity-{index}", pose=pose, errors=())
            for index in range(4097)
        ],
        errors=(),
    )
    batch = capture_simulation_entity_poses(
        _FakeSimulationClient(unbounded_snapshot),
        [SimulationEntityPoseRequest(entity="/World/tool0")],
    )
    assert batch.observations == ()
    assert [failure.code for failure in batch.failures] == [
        "invalid_source_snapshot"
    ]


def test_transform_translation_rejects_unbounded_output_frame() -> None:
    transform = _transform("f" * 1025, "box", 0.0, 0.0, 0.0)
    with pytest.raises(ObservationNormalizationError) as raised:
        pose_source_sidecar.normalize_transform_stamped(transform)
    assert raised.value.code == "invalid_transform"


def test_tf_capture_is_stateless_and_preserves_request_identity() -> None:
    buffer = _FakeTfBuffer(
        {("world", "box"): _transform("world", "box", 1.0, 2.0, 3.0)}
    )
    requests = [
        TfPoseRequest("world", "box"),
        TfPoseRequest("world", "missing"),
    ]

    batch = capture_tf_poses(buffer, requests, lookup_time=object())

    assert len(batch.observations) == 1
    assert batch.observations[0].request == requests[0]
    assert batch.observations[0].pose.position_xyz == (1.0, 2.0, 3.0)
    assert len(batch.failures) == 1
    assert batch.failures[0].source_key == "world<-missing"
    assert batch.failures[0].code == "tf_lookup_failed"
    assert not hasattr(batch, "status")


def test_depth_one_subscription_forwards_without_latest_cache() -> None:
    node = _FakeNode()
    observations: list[int] = []
    errors: list[ObservationNormalizationError] = []
    adapter = DepthOneSubscriptionAdapter(
        node=node,
        message_type=object,
        topic="/observations",
        normalize=_normalize_integer_message,
        on_observation=observations.append,
        on_error=errors.append,
        qos_profile_factory=lambda depth: {"depth": depth},
    )

    node.callback(_namespace(value=7))
    node.callback(_namespace(value=-1))

    assert observations == [7]
    assert [error.code for error in errors] == ["negative_value"]
    assert node.qos == {"depth": 1}
    assert not hasattr(adapter, "latest")
    assert not hasattr(adapter, "state")
    assert not hasattr(adapter, "snapshot")

    adapter.close()
    assert node.destroyed is node.subscription


def test_module_has_no_legacy_pose_authority_or_composer_payload_surface() -> None:
    source = inspect.getsource(pose_source_sidecar)
    tree = ast.parse(source)
    class_names = {
        node.name for node in ast.walk(tree) if isinstance(node, ast.ClassDef)
    }
    identifiers = {
        node.id for node in ast.walk(tree) if isinstance(node, ast.Name)
    } | {
        node.attr for node in ast.walk(tree) if isinstance(node, ast.Attribute)
    }

    assert not {
        "PoseSourceLatestState",
        "PoseSourceSnapshot",
        "PoseSourceObjectState",
        "SimulationInterfacesPoseSource",
        "PoseStampedTopicPoseSource",
        "PoseArrayTopicPoseSource",
        "Detection3DArrayTopicPoseSource",
        "TfPoseSource",
        "MeasuredRobotJointStateTracker",
    } & class_names
    assert "apply_policy" not in identifiers
    assert "to_dict" not in identifiers
    assert "from_deployment_config" not in identifiers
    assert "time.monotonic" not in source
    assert "threading" not in source
    assert "POSE_SOURCE_STATUS_" not in source


class _FakeSimulationClient:
    def __init__(self, snapshot: object) -> None:
        self.snapshot = snapshot
        self.calls: list[dict[str, object]] = []

    def capture(self, **kwargs: object) -> object:
        self.calls.append(kwargs)
        return self.snapshot


class _FakeTfBuffer:
    def __init__(self, transforms: dict[tuple[str, str], object]) -> None:
        self.transforms = dict(transforms)
        self.lookups: list[tuple[str, str]] = []

    def lookup_transform(
        self,
        target_frame: str,
        source_frame: str,
        _lookup_time: object,
    ) -> object:
        self.lookups.append((target_frame, source_frame))
        try:
            return self.transforms[(target_frame, source_frame)]
        except KeyError as exc:
            raise RuntimeError(
                f"missing transform {target_frame} <- {source_frame}"
            ) from exc


class _FakeNode:
    def __init__(self) -> None:
        self.callback = lambda _message: None
        self.subscription = object()
        self.destroyed: object | None = None
        self.qos: object | None = None

    def create_subscription(
        self,
        _message_type: object,
        _topic: str,
        callback: object,
        qos: object,
    ) -> object:
        self.callback = callback
        self.qos = qos
        return self.subscription

    def destroy_subscription(self, subscription: object) -> None:
        self.destroyed = subscription


def _normalize_integer_message(message: object) -> int:
    value = int(getattr(message, "value"))
    if value < 0:
        raise ObservationNormalizationError("negative_value", "value is negative")
    return value


def _namespace(**kwargs: object) -> SimpleNamespace:
    return SimpleNamespace(**kwargs)


def _header(frame_id: str, sec: int, nanosec: int) -> SimpleNamespace:
    return _namespace(
        frame_id=frame_id,
        stamp=_namespace(sec=sec, nanosec=nanosec),
    )


def _pose(
    x: float,
    y: float,
    z: float,
    *,
    qx: float = 0.0,
    qy: float = 0.0,
    qz: float = 0.0,
    w: float = 1.0,
) -> SimpleNamespace:
    return _namespace(
        position=_namespace(x=x, y=y, z=z),
        orientation=_namespace(x=qx, y=qy, z=qz, w=w),
    )


def _pose_stamped(
    frame_id: str,
    sec: int,
    nanosec: int,
    x: float,
    y: float,
    z: float,
    *,
    w: float,
) -> SimpleNamespace:
    return _namespace(
        header=_header(frame_id, sec, nanosec),
        pose=_pose(x, y, z, w=w),
    )


def _detection(
    detection_id: str,
    class_id: str,
    score: float,
    x: float,
    y: float,
    z: float,
    *,
    covariance: float,
) -> SimpleNamespace:
    covariance_values = [0.0] * 36
    for index in (0, 7, 14):
        covariance_values[index] = covariance
    result = _namespace(
        hypothesis=_namespace(class_id=class_id, score=score),
        pose=_namespace(
            pose=_pose(x, y, z),
            covariance=covariance_values,
        ),
    )
    return _namespace(
        id=detection_id,
        header=_header("camera", 80, 0),
        results=[result],
    )


def _transform(
    frame_id: str,
    child_frame_id: str,
    x: float,
    y: float,
    z: float,
) -> SimpleNamespace:
    return _namespace(
        header=_header(frame_id, 61, 500_000_000),
        child_frame_id=child_frame_id,
        transform=_namespace(
            translation=_namespace(x=x, y=y, z=z),
            rotation=_namespace(x=0.0, y=0.0, z=0.0, w=2.0),
        ),
    )
