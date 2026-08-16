from __future__ import annotations

import json
from pathlib import Path
import threading
import time

import pytest
import rclpy
from rclpy.executors import SingleThreadedExecutor
from rclpy.context import Context
from rclpy.node import Node
from sensor_msgs.msg import JointState

from manyforge_ros.isaac_pose_observer import (
    ObservationDispatchSnapshot,
    SITE_CONFIG_VERSION,
    load_isaac_pose_observer_site_config,
)
from manyforge_ros.manager_scene_runtime import PoseObservationIntent
from manyforge_ros_bringup import pose_source_snapshot_publisher


_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


class _FakeDispatcher:
    def __init__(self) -> None:
        self.started = False
        self.closed = False
        self.intents: list[PoseObservationIntent] = []

    def start(self) -> None:
        self.started = True

    def submit(self, intent: PoseObservationIntent) -> None:
        self.intents.append(intent)

    def close(self, *, timeout_s: float = 6.0) -> bool:
        del timeout_s
        self.closed = True
        return True

    def snapshot(self) -> ObservationDispatchSnapshot:
        return ObservationDispatchSnapshot(
            running=self.started and not self.closed,
            pending=0,
            inflight=False,
            submitted=len(self.intents),
            superseded=0,
            delivered=len(self.intents),
            retries=0,
            expired=0,
            joins=1 if self.started else 0,
            last_error_code="",
        )


def _write_joint_config(path: Path) -> None:
    path.write_text(
        json.dumps(
            {
                "api_version": SITE_CONFIG_VERSION,
                "cell_id": "cell.1",
                "detection3d_sources": [],
                "joint_state": {
                    "eligible": False,
                    "lifetime_ns": 1_000_000_000,
                    "object_id": "robot.1",
                    "policy_id": "viewer.measured_robot",
                    "required_joint_names": ["joint_a", "joint_b"],
                    "topic": "/isaac_joint_states",
                },
                "simulation_entities": [],
                "simulation_poll_period_ms": 250,
                "source_id": "isaac.observer",
            },
            separators=(",", ":"),
            sort_keys=True,
        ),
        encoding="utf-8",
    )


def test_ros_joint_state_adapter_forwards_good_and_unknown_manager_intents(
    tmp_path: Path,
) -> None:
    config_path = tmp_path / "site.json"
    _write_joint_config(config_path)
    dispatcher = _FakeDispatcher()
    rclpy.init()
    publisher: pose_source_snapshot_publisher.PoseSourceSnapshotPublisher | None = (
        None
    )
    source: Node | None = None
    executor = SingleThreadedExecutor()
    try:
        publisher = pose_source_snapshot_publisher.PoseSourceSnapshotPublisher(
            site_config_path=config_path,
            manager_state_root=tmp_path,
            dispatcher_factory=lambda _root, _config, _callback: dispatcher,
        )
        source = Node("manyforge_pose_observer_joint_test_source")
        joint_publisher = source.create_publisher(
            JointState, "/isaac_joint_states", 1
        )
        executor.add_node(publisher)
        executor.add_node(source)
        publisher.start()

        valid = JointState()
        valid.name = ["joint_b", "extra", "joint_a"]
        valid.position = [0.2, 9.0, 0.1]
        joint_publisher.publish(valid)
        _spin_until(executor, lambda: len(dispatcher.intents) >= 1)

        observed = dispatcher.intents[0]
        assert observed.quality == "good"
        assert observed.eligible is False
        assert observed.payload_value()["joint_names"] == ["joint_a", "joint_b"]
        assert observed.payload_value()["positions"] == [0.1, 0.2]

        invalid = JointState()
        invalid.name = ["joint_a"]
        invalid.position = [0.1]
        joint_publisher.publish(invalid)
        _spin_until(executor, lambda: len(dispatcher.intents) >= 2)

        unknown = dispatcher.intents[1]
        assert unknown.quality == "unknown"
        assert unknown.eligible is False
        assert unknown.payload_value()["source_error"]["code"] == (
            "joint_set_incomplete"
        )
    finally:
        if publisher is not None:
            assert publisher.stop()
        for node in (source, publisher):
            if node is not None:
                executor.remove_node(node)
                node.destroy_node()
        executor.shutdown()
        if rclpy.ok():
            rclpy.shutdown()
    assert dispatcher.started is True
    assert dispatcher.closed is True


def test_simulation_auxiliary_node_uses_the_entrypoint_private_context(
    tmp_path: Path,
) -> None:
    context = Context()
    rclpy.init(context=context)
    dispatcher = _FakeDispatcher()
    publisher: pose_source_snapshot_publisher.PoseSourceSnapshotPublisher | None = (
        None
    )
    try:
        publisher = pose_source_snapshot_publisher.PoseSourceSnapshotPublisher(
            site_config_path=(
                _REPOSITORY_ROOT
                / "manyforge_ros_bringup/config/isaac_ur10e_light.pose_observer.json"
            ),
            manager_state_root=tmp_path,
            dispatcher_factory=lambda _root, _config, _callback: dispatcher,
            simulation_client_factory=lambda node, _timeout_s: node,
            context=context,
        )

        assert publisher.context is context
        assert len(publisher.auxiliary_nodes) == 1
        assert publisher.auxiliary_nodes[0].context is context
    finally:
        if publisher is not None:
            for auxiliary in publisher.auxiliary_nodes:
                auxiliary.destroy_node()
            publisher.destroy_node()
        if context.ok():
            rclpy.shutdown(context=context)


def test_simulation_capture_waits_for_attached_private_executor_spin(
    tmp_path: Path,
) -> None:
    context = Context()
    rclpy.init(context=context)
    dispatcher = _FakeDispatcher()
    captures = threading.Event()

    class _SimulationClient:
        def capture(self, **_kwargs: object) -> object:
            captures.set()
            raise RuntimeError("bounded test capture")

    publisher: pose_source_snapshot_publisher.PoseSourceSnapshotPublisher | None = (
        None
    )
    executor = SingleThreadedExecutor(context=context)
    spin_thread: threading.Thread | None = None
    try:
        publisher = pose_source_snapshot_publisher.PoseSourceSnapshotPublisher(
            site_config_path=(
                _REPOSITORY_ROOT
                / "manyforge_ros_bringup/config/isaac_ur10e_light.pose_observer.json"
            ),
            manager_state_root=tmp_path,
            dispatcher_factory=lambda _root, _config, _callback: dispatcher,
            simulation_client_factory=lambda _node, _timeout_s: _SimulationClient(),
            context=context,
        )
        executor.add_node(publisher)
        for auxiliary in publisher.auxiliary_nodes:
            executor.add_node(auxiliary)
        publisher.start()

        assert not captures.wait(timeout=0.1)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        spin_thread.start()
        assert captures.wait(timeout=2.0)
    finally:
        if publisher is not None:
            assert publisher.stop()
            for auxiliary in publisher.auxiliary_nodes:
                executor.remove_node(auxiliary)
                auxiliary.destroy_node()
            executor.remove_node(publisher)
            publisher.destroy_node()
        executor.shutdown(timeout_sec=2.0)
        if spin_thread is not None:
            spin_thread.join(timeout=2.0)
        if context.ok():
            rclpy.shutdown(context=context)


def test_entrypoint_has_no_retired_composer_or_deployment_owner_surface() -> None:
    source = Path(pose_source_snapshot_publisher.__file__).read_text(
        encoding="utf-8"
    )
    for retired in (
        "composer_url",
        "urllib.request",
        "manyforge_behavior.deployment",
        "apply_policy",
        "isaac_demo_supervisor",
    ):
        assert retired not in source
    assert "ObservationDispatcher.for_manager_root" in source
    assert "DepthOneSubscriptionAdapter" in source
    assert "capture_simulation_entity_poses" in source


@pytest.mark.parametrize(
    (
        "config_name",
        "deployment_name",
        "robot_id",
        "expected_keys",
    ),
    [
        (
            "isaac_ur10e_light.pose_observer.json",
            "isaac_manipulator_scene_light_ros2_control.deployment.yaml",
            "isaac_ur10e_light_robot",
            {
                ("isaac_ur10e_light_robot", "viewer.measured_robot"),
                ("isaac_ur10e_light_robot", "viewer.measured_tcp"),
                ("isaac_cheese_box", "continuous_unattached"),
            },
        ),
        (
            "isaac_ur10e_collision.pose_observer.json",
            "isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml",
            "isaac_ur10e_collision_robot",
            {
                ("isaac_ur10e_collision_robot", "viewer.measured_robot"),
                ("isaac_ur10e_collision_robot", "viewer.measured_tcp"),
                ("isaac_cheese_box", "continuous_unattached"),
            },
        ),
        (
            "isaac_ur10e_assistant.pose_observer.json",
            "isaac_manipulator_scene_light_assistant.deployment.yaml",
            "isaac_ur10e_assistant_robot",
            {
                ("isaac_ur10e_assistant_robot", "viewer.measured_robot"),
                ("isaac_ur10e_assistant_robot", "viewer.measured_tcp"),
                ("isaac_cheese_box", "continuous_unattached"),
            },
        ),
        (
            "isaac_lite6_pick_place.pose_observer.json",
            "isaac_lite6_lite_gripper_pick_and_place.deployment.yaml",
            "isaac_lite6_robot",
            {("isaac_lite6_robot", "viewer.measured_robot")},
        ),
        (
            "isaac_lite6_trocar_foundationpose.pose_observer.json",
            "trocar_lite6_foundationpose_pick.deployment.yaml",
            "isaac_lite6_trocar_robot",
            {
                ("isaac_lite6_trocar_robot", "viewer.measured_robot"),
                ("graspable", "before_planning"),
            },
        ),
    ],
)
def test_retained_site_configs_match_exact_five_deployment_identities(
    config_name: str,
    deployment_name: str,
    robot_id: str,
    expected_keys: set[tuple[str, str]],
) -> None:
    config = load_isaac_pose_observer_site_config(
        _REPOSITORY_ROOT / "manyforge_ros_bringup" / "config" / config_name
    )
    deployment = json.loads(
        (_REPOSITORY_ROOT / "examples" / deployment_name).read_text(
            encoding="utf-8"
        )
    )

    assert config.cell_id == deployment["cell_id"]
    assert config.joint_state is not None
    assert config.joint_state.object_id == robot_id
    assert config.joint_state.topic == "/isaac_joint_states"
    assert expected_keys == set(config.keys)
    assert robot_id in {
        row["controllable_component_id"]
        for row in deployment["graph"]["controllable_components"]
    }


def test_retained_site_configs_preserve_historical_pose_source_contracts() -> None:
    config_root = _REPOSITORY_ROOT / "manyforge_ros_bringup" / "config"
    rigid_gripper = (
        "/World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/"
        "ee_link/robotiq_arg2f_base_link"
    )
    for config_name in (
        "isaac_ur10e_light.pose_observer.json",
        "isaac_ur10e_collision.pose_observer.json",
        "isaac_ur10e_assistant.pose_observer.json",
    ):
        ur_config = load_isaac_pose_observer_site_config(
            config_root / config_name
        )
        assert ur_config.simulation_poll_period_ms == 250
        assert [
            binding.request.entity for binding in ur_config.simulation_entities
        ] == [rigid_gripper, "/mac_n_cheese_centered"]
        assert ur_config.simulation_entities[0].request.local_position_xyz == (
            0.0,
            0.0,
            0.2,
        )
        assert ur_config.simulation_entities[0].lifetime_ns == 1_000_000_000
        assert ur_config.simulation_entities[1].eligible is True
        # The movable scene object crosses the Manager-owned observer and
        # scene-commit path. Keep its freshness lease bounded to eight polls so
        # one transient serialized delivery cycle cannot make a current sample
        # stale while motion is being validated.
        assert ur_config.simulation_entities[1].lifetime_ns == 2_000_000_000
        assert [
            (
                binding.request.source_header_frame_id,
                binding.request.source_coordinate_frame_id,
                binding.request.target_frame_id,
            )
            for binding in ur_config.simulation_entities
        ] == [
            ("robotiq_arg2f_base_link", "universe", "universe"),
            ("mac_n_cheese_centered", "universe", "universe"),
        ]

    foundation = load_isaac_pose_observer_site_config(
        config_root / "isaac_lite6_trocar_foundationpose.pose_observer.json"
    )
    assert len(foundation.detection3d_sources) == 1
    detection = foundation.detection3d_sources[0]
    assert detection.topic == "/output"
    assert detection.target_frame_id == "link_base"
    assert detection.lifetime_ns == 5_000_000_000
    assert detection.selector.selection == "first"
    assert detection.selector.confidence_min == 0.0
    assert detection.selector.max_position_variance_m2 == 0.0025


def _spin_until(
    executor: SingleThreadedExecutor,
    predicate,
    *,
    timeout_s: float = 2.0,
) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        executor.spin_once(timeout_sec=0.05)
        if predicate():
            return
    raise AssertionError("ROS pose observer did not receive the expected sample")
