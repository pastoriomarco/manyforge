from __future__ import annotations

import json
from pathlib import Path
import time

import pytest

from manyforge_ros.isaac_pose_observer import (
    SITE_CONFIG_VERSION,
    IsaacPoseObserverConfigError,
    ObservationDispatcher,
    detection3d_observation_intent,
    joint_state_observation_intent,
    load_isaac_pose_observer_site_config,
    manager_source_epoch_factory,
    simulation_entity_observation_intent,
    unknown_observation_intent,
)
from manyforge_ros.manager_scene_runtime import (
    PoseObservationIntent,
    PoseObservationResult,
    PoseObserverBinding,
    PoseObserverSession,
    SceneRuntimeIdentity,
    SceneRuntimeManagerError,
    SceneRuntimeOwnerSnapshot,
)
from manyforge_ros.pose_source_sidecar import (
    NormalizedDetectionPose,
    NormalizedJointState,
    ObservationNormalizationError,
    SimulationEntityPoseObservation,
)
from manyforge_ros.scene_mirror_probe import PoseSnapshot


def _config_value() -> dict[str, object]:
    return {
        "api_version": SITE_CONFIG_VERSION,
        "cell_id": "isaac_ur10e_light_cell",
        "detection3d_sources": [
            {
                "eligible": True,
                "fallback_frame_id": "link_base",
                "frame_kind": "cell_frame",
                "lifetime_ns": 5_000_000_000,
                "object_id": "graspable",
                "policy_id": "before_planning",
                "selector": {
                    "class_id": "",
                    "confidence_min": 0.0,
                    "detection_id": "",
                    "max_position_variance_m2": 0.0025,
                    "selection": "first",
                },
                "target_frame_id": "link_base",
                "topic": "/output",
            }
        ],
        "joint_state": {
            "eligible": False,
            "lifetime_ns": 1_000_000_000,
            "object_id": "isaac_ur10e_light_robot",
            "policy_id": "viewer.measured_robot",
            "required_joint_names": ["joint_a", "joint_b"],
            "topic": "/isaac_joint_states",
        },
        "simulation_entities": [
            {
                "eligible": True,
                "entity": "/mac_n_cheese_centered",
                "frame_kind": "cell_frame",
                "lifetime_ns": 1_000_000_000,
                "local_orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                "local_position_xyz": [0.0, 0.0, 0.0],
                "object_id": "isaac_cheese_box",
                "policy_id": "continuous_unattached",
                "source_coordinate_frame_id": "world",
                "source_header_frame_id": "mac_n_cheese_centered",
                "target_frame_id": "universe",
            }
        ],
        "simulation_poll_period_ms": 250,
        "source_id": "isaac.observer",
    }


def _write_config(path: Path, value: dict[str, object]) -> None:
    path.write_text(
        json.dumps(value, allow_nan=False, separators=(",", ":"), sort_keys=True),
        encoding="utf-8",
    )


def test_site_config_is_closed_bounded_and_preserves_exact_bindings(
    tmp_path: Path,
) -> None:
    path = tmp_path / "site.json"
    _write_config(path, _config_value())

    config = load_isaac_pose_observer_site_config(path)

    assert config.cell_id == "isaac_ur10e_light_cell"
    assert config.source_id == "isaac.observer"
    assert config.simulation_poll_period_ms == 250
    assert config.joint_state is not None
    assert config.joint_state.required_joint_names == ("joint_a", "joint_b")
    assert config.simulation_entities[0].request.entity == (
        "/mac_n_cheese_centered"
    )
    assert config.simulation_entities[0].request.source_header_frame_id == (
        "mac_n_cheese_centered"
    )
    assert config.simulation_entities[0].request.source_coordinate_frame_id == (
        "world"
    )
    assert config.detection3d_sources[0].selector.selection == "first"
    assert config.keys == {
        ("isaac_ur10e_light_robot", "viewer.measured_robot"),
        ("isaac_cheese_box", "continuous_unattached"),
        ("graspable", "before_planning"),
    }
    assert config.observer_bindings == (
        PoseObserverBinding("graspable", "before_planning", True),
        PoseObserverBinding(
            "isaac_cheese_box", "continuous_unattached", True
        ),
        PoseObserverBinding(
            "isaac_ur10e_light_robot", "viewer.measured_robot", False
        ),
    )


def test_site_config_rejects_symlink_duplicates_unknown_fields_and_key_aliases(
    tmp_path: Path,
) -> None:
    path = tmp_path / "site.json"
    _write_config(path, _config_value())
    alias = tmp_path / "alias.json"
    alias.symlink_to(path)
    with pytest.raises(IsaacPoseObserverConfigError, match="non-symlink"):
        load_isaac_pose_observer_site_config(alias)

    duplicate = tmp_path / "duplicate.json"
    duplicate.write_text(
        '{"api_version":"x","api_version":"y"}', encoding="utf-8"
    )
    with pytest.raises(IsaacPoseObserverConfigError, match="duplicate"):
        load_isaac_pose_observer_site_config(duplicate)

    unknown_value = _config_value()
    unknown_value["unknown"] = True
    _write_config(path, unknown_value)
    with pytest.raises(IsaacPoseObserverConfigError, match="unknown or missing"):
        load_isaac_pose_observer_site_config(path)

    alias_value = _config_value()
    simulation = alias_value["simulation_entities"]
    assert isinstance(simulation, list)
    simulation[0]["object_id"] = "isaac_ur10e_light_robot"
    simulation[0]["policy_id"] = "viewer.measured_robot"
    _write_config(path, alias_value)
    with pytest.raises(IsaacPoseObserverConfigError, match="must be unique"):
        load_isaac_pose_observer_site_config(path)

    invalid_frame = _config_value()
    simulations = invalid_frame["simulation_entities"]
    assert isinstance(simulations, list)
    simulations[0]["frame_kind"] = "world_frame"
    _write_config(path, invalid_frame)
    with pytest.raises(IsaacPoseObserverConfigError, match="cell_frame"):
        load_isaac_pose_observer_site_config(path)


def test_normalized_observations_map_to_stable_manager_payloads(
    tmp_path: Path,
) -> None:
    path = tmp_path / "site.json"
    _write_config(path, _config_value())
    config = load_isaac_pose_observer_site_config(path)
    assert config.joint_state is not None

    joint = joint_state_observation_intent(
        config.joint_state,
        NormalizedJointState(
            topic="/isaac_joint_states",
            names=("extra", "joint_b", "joint_a"),
            positions=(9.0, 0.2, 0.1),
            source_stamp_ns=12500000000,
        ),
    )
    assert joint.quality == "good"
    assert joint.eligible is False
    assert joint.payload_value()["joint_names"] == ["joint_a", "joint_b"]
    assert joint.payload_value()["positions"] == [0.1, 0.2]

    pose = PoseSnapshot(
        frame_id="universe",
        stamp_ns=13000000000,
        position_xyz=(0.4, -0.2, 0.1),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    simulation = simulation_entity_observation_intent(
        config.simulation_entities[0],
        SimulationEntityPoseObservation(
            entity="/mac_n_cheese_centered",
            pose=pose,
        ),
    )
    assert simulation.payload_value()["kind"] == "simulation_entity_pose"
    assert simulation.payload_value()["pose"]["frame_kind"] == "cell_frame"
    assert simulation.payload_value()["pose"]["position_m"] == [0.4, -0.2, 0.1]
    with pytest.raises(ObservationNormalizationError, match="configured cell frame"):
        simulation_entity_observation_intent(
            config.simulation_entities[0],
            SimulationEntityPoseObservation(
                entity="/mac_n_cheese_centered",
                pose=PoseSnapshot(
                    frame_id="world",
                    stamp_ns=13000000000,
                    position_xyz=(0.4, -0.2, 0.1),
                    orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
                ),
            ),
        )

    detection = detection3d_observation_intent(
        config.detection3d_sources[0],
        NormalizedDetectionPose(
            detection_id="detection.1",
            class_id="trocar",
            confidence=0.9,
            max_position_variance_m2=0.001,
            pose=PoseSnapshot(
                frame_id="link_base",
                stamp_ns=13000000000,
                position_xyz=pose.position_xyz,
                orientation_xyzw=pose.orientation_xyzw,
            ),
        ),
    )
    assert detection.payload_value()["source"]["confidence"] == 0.9

    unknown = unknown_observation_intent(
        config.simulation_entities[0],
        code="source_error",
        detail="service unavailable",
        source_key="",
    )
    assert unknown.quality == "unknown"
    assert unknown.eligible is False
    assert unknown.payload_value()["source_error"]["source_key"] == "<source>"

    with pytest.raises(ObservationNormalizationError) as raised:
        joint_state_observation_intent(
            config.joint_state,
            NormalizedJointState(
                topic="/isaac_joint_states",
                names=("joint_a",),
                positions=(0.1,),
                source_stamp_ns=None,
            ),
        )
    assert raised.value.code == "joint_set_incomplete"


class _FakeSceneRuntimeClient:
    def __init__(self, *, fail_first: bool = False, clock=None) -> None:
        self.fail_first = fail_first
        self.clock = clock
        self.owner_reads = 0
        self.opens: list[tuple[str, str]] = []
        self.observations: list[tuple[int, PoseObservationIntent]] = []
        self.identity = SceneRuntimeIdentity(
            retained_bundle_id="sha256:" + "a" * 64,
            graph_revision="sha256:" + "b" * 64,
            deployment_generation=1,
            program_generation=0,
            runtime_generation=1,
        )

    def read_owner(self) -> SceneRuntimeOwnerSnapshot:
        self.owner_reads += 1
        return SceneRuntimeOwnerSnapshot(
            cell_id="cell.1",
            owner_epoch=f"owner.{self.owner_reads}",
            runtime_identity=self.identity,
            initialization_generation=1,
            initialization_state="open",
        )

    def open_observer(
        self,
        owner: SceneRuntimeOwnerSnapshot,
        *,
        bindings: tuple[PoseObserverBinding, ...],
        source_id: str,
        source_epoch: str,
    ) -> PoseObserverSession:
        assert bindings
        self.opens.append((source_id, source_epoch))
        return PoseObserverSession(
            cell_id=owner.cell_id,
            runtime_identity=owner.runtime_identity,
            source_id=source_id,
            source_epoch=source_epoch,
            source_epoch_order=len(self.opens),
        )

    def observe(
        self,
        session: PoseObserverSession,
        *,
        source_sequence: int,
        intent: PoseObservationIntent,
    ) -> PoseObservationResult:
        self.observations.append((source_sequence, intent))
        if self.fail_first and len(self.observations) == 1:
            if self.clock is not None:
                self.clock.value = intent.lifetime_ns + 1
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_unavailable", "provider unavailable"
            )
        return PoseObservationResult(
            disposition="observed_only",
            fresh=True,
            initialization_generation=1,
            scene_runtime_source_generation=len(self.observations),
            source_epoch_order=session.source_epoch_order,
            source_sequence=source_sequence,
            scene_application_status="not_requested",
            scene_projection_revision=None,
            scene_request_id=None,
            scene_update_status=None,
        )


class _Clock:
    value = 0

    def __call__(self) -> int:
        return self.value


def _intent(
    position: float,
    *,
    lifetime_ns: int = 1_000_000_000,
) -> PoseObservationIntent:
    return PoseObservationIntent(
        object_id="robot",
        policy_id="viewer.measured_robot",
        lifetime_ns=lifetime_ns,
        quality="good",
        eligible=False,
        payload={"kind": "joint_state", "position": position},
    )


def _wait_for(predicate, *, timeout_s: float = 2.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.005)
    raise AssertionError("dispatcher did not reach the expected state")


def test_manager_epoch_factory_keeps_full_base_and_bounds_rotation(monkeypatch) -> None:
    import manyforge_ros.isaac_pose_observer as module

    monkeypatch.setattr(module, "MAXIMUM_SAFE_INTEGER", 2)
    factory = manager_source_epoch_factory("manager.process.base.27")
    assert factory() == "manager.process.base.27.1"
    assert factory() == "manager.process.base.27.2"
    with pytest.raises(ValueError, match="rotation exhausted"):
        factory()
    with pytest.raises(IsaacPoseObserverConfigError):
        manager_source_epoch_factory("")
    with pytest.raises(IsaacPoseObserverConfigError):
        manager_source_epoch_factory("x" * 255)
    assert len(manager_source_epoch_factory("x" * 254)().encode()) == 256


def test_manager_epoch_base_reserves_full_counter_and_utf8_identity_budget() -> None:
    from manyforge_ros.manager_scene_runtime import MAXIMUM_SAFE_INTEGER

    base_bytes = 256 - len(f".{MAXIMUM_SAFE_INTEGER}".encode())
    for base in ("x" * base_bytes, "é" * (base_bytes // 2) + "x" * (base_bytes % 2)):
        factory = manager_source_epoch_factory(base)
        assert factory() == f"{base}.1"
        for rotation in range(2, 11):
            assert factory() == f"{base}.{rotation}"
        with pytest.raises(IsaacPoseObserverConfigError):
            manager_source_epoch_factory(base + "x")
    with pytest.raises(IsaacPoseObserverConfigError):
        manager_source_epoch_factory("x" * 254)


@pytest.mark.parametrize("rotation_reason", ["sequence_exhaustion", "request_conflict"])
def test_manager_owned_dispatcher_preserves_existing_rotation_paths(rotation_reason) -> None:
    class ConflictClient(_FakeSceneRuntimeClient):
        def observe(self, session, **kwargs):
            result = super().observe(session, **kwargs)
            if len(self.observations) == 1:
                raise SceneRuntimeManagerError(
                    "scene_runtime_observation_request_conflict", "conflicting payload"
                )
            return result

    client = ConflictClient() if rotation_reason == "request_conflict" else _FakeSceneRuntimeClient()
    dispatcher = ObservationDispatcher(
        client=client,
        expected_cell_id="cell.1",
        source_id="isaac.observer",
        allowed_keys=frozenset({("robot", "viewer.measured_robot")}),
        source_epoch_factory=manager_source_epoch_factory("manager.base.27"),
        retry_initial_s=0.001,
        retry_max_s=0.01,
    )
    if rotation_reason == "sequence_exhaustion":
        from manyforge_ros.manager_scene_runtime import MAXIMUM_SAFE_INTEGER
        dispatcher._next_sequence = MAXIMUM_SAFE_INTEGER + 1
    dispatcher.submit(_intent(0.1))
    dispatcher.start()
    try:
        _wait_for(lambda: dispatcher.snapshot().delivered == 1)
        assert client.opens == [("isaac.observer", "manager.base.27.1"), ("isaac.observer", "manager.base.27.2")]
        assert all(sequence == 1 for sequence, _ in client.observations)
    finally:
        assert dispatcher.close()


def test_for_manager_root_forwards_owner_factory_and_keeps_unmanaged_default(tmp_path) -> None:
    config_path = tmp_path / "observer.json"
    config_path.write_text(json.dumps(_config_value()), encoding="utf-8")
    config = load_isaac_pose_observer_site_config(config_path)
    owned = ObservationDispatcher.for_manager_root(
        manager_run_root=tmp_path,
        config=config,
        source_epoch_factory=manager_source_epoch_factory("manager.base.27"),
    )
    assert owned._source_epoch == "manager.base.27.1"
    assert owned._new_source_epoch() == "manager.base.27.2"
    unmanaged = ObservationDispatcher.for_manager_root(manager_run_root=tmp_path, config=config)
    assert unmanaged._source_epoch.startswith("isaac-observer-")
    assert unmanaged._new_source_epoch() != unmanaged._source_epoch


def test_dispatcher_is_latest_per_key_and_rejoins_with_exact_retry() -> None:
    client = _FakeSceneRuntimeClient(fail_first=True)
    events = []
    dispatcher = ObservationDispatcher(
        client=client,
        expected_cell_id="cell.1",
        source_id="isaac.observer",
        allowed_keys=frozenset({("robot", "viewer.measured_robot")}),
        event_callback=events.append,
        source_epoch_factory=lambda: "source.epoch.1",
        retry_initial_s=0.001,
        retry_max_s=0.01,
    )
    dispatcher.submit(_intent(0.1))
    dispatcher.submit(_intent(0.2))
    dispatcher.start()
    _wait_for(lambda: dispatcher.snapshot().delivered == 1)
    assert dispatcher.close()

    snapshot = dispatcher.snapshot()
    assert snapshot.submitted == 2
    assert snapshot.superseded == 1
    assert snapshot.delivered == 1
    assert snapshot.retries == 1
    assert snapshot.joins == 2
    assert len(client.observations) == 2
    assert client.observations[0][0] == client.observations[1][0] == 1
    assert client.observations[0][1] == client.observations[1][1]
    assert client.observations[1][1].payload_value()["position"] == 0.2
    assert [event.kind for event in events] == ["retry", "delivered"]
    assert events[0].scene_application_status is None
    assert events[1].scene_application_status == "not_requested"
    assert events[1].scene_runtime_source_generation == 2


def test_dispatcher_registers_its_exact_bindings_before_any_sample() -> None:
    client = _FakeSceneRuntimeClient()
    binding = PoseObserverBinding(
        object_id="robot",
        policy_id="viewer.measured_robot",
        eligible=False,
    )
    dispatcher = ObservationDispatcher(
        client=client,
        expected_cell_id="cell.1",
        source_id="isaac.observer",
        allowed_keys=frozenset({("robot", "viewer.measured_robot")}),
        observer_bindings=(binding,),
        source_epoch_factory=lambda: "source.epoch.1",
    )
    dispatcher.start()
    _wait_for(lambda: dispatcher.snapshot().joins == 1)
    assert dispatcher.close()
    assert dispatcher.snapshot().submitted == 0
    assert client.observations == []
    assert client.opens == [("isaac.observer", "source.epoch.1")]


def test_dispatcher_drops_an_undeliverable_sample_after_its_lifetime() -> None:
    clock = _Clock()
    client = _FakeSceneRuntimeClient(fail_first=True, clock=clock)
    dispatcher = ObservationDispatcher(
        client=client,
        expected_cell_id="cell.1",
        source_id="isaac.observer",
        allowed_keys=frozenset({("robot", "viewer.measured_robot")}),
        clock_ns=clock,
        source_epoch_factory=lambda: "source.epoch.1",
        retry_initial_s=0.001,
        retry_max_s=0.01,
    )
    dispatcher.start()
    dispatcher.submit(_intent(0.1, lifetime_ns=100))
    _wait_for(lambda: dispatcher.snapshot().expired == 1)
    assert dispatcher.close()
    snapshot = dispatcher.snapshot()
    assert snapshot.delivered == 0
    assert snapshot.retries == 1
    assert len(client.observations) == 1


def test_dispatcher_rejects_unconfigured_keys_without_growing_a_queue() -> None:
    dispatcher = ObservationDispatcher(
        client=_FakeSceneRuntimeClient(),
        expected_cell_id="cell.1",
        source_id="isaac.observer",
        allowed_keys=frozenset({("robot", "viewer.measured_robot")}),
        source_epoch_factory=lambda: "source.epoch.1",
    )
    with pytest.raises(ValueError, match="not configured"):
        dispatcher.submit(
            PoseObservationIntent(
                object_id="other",
                policy_id="viewer.measured_robot",
                lifetime_ns=1,
                quality="good",
                eligible=False,
                payload={},
            )
        )
    assert dispatcher.snapshot().pending == 0

    with pytest.raises(ValueError, match="exactly cover"):
        ObservationDispatcher(
            client=_FakeSceneRuntimeClient(),
            expected_cell_id="cell.1",
            source_id="isaac.observer",
            allowed_keys=frozenset({("robot", "viewer.measured_robot")}),
            observer_bindings=(),
            source_epoch_factory=lambda: "source.epoch.1",
        )
