from __future__ import annotations

import importlib.util
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


class _FakeSink:
    def __init__(self, _node: object, component: str, stream_id: str) -> None:
        self.component = component
        self.stream_id = stream_id
        self.stamps: list[int | None] = []

    def record(self, stamp_ns: int | None = None) -> None:
        self.stamps.append(stamp_ns)


class _FakeSinks:
    def __init__(self) -> None:
        self.by_stream: dict[str, _FakeSink] = {}

    def __call__(self, node: object, component: str, stream_id: str) -> _FakeSink:
        sink = _FakeSink(node, component, stream_id)
        self.by_stream[stream_id] = sink
        return sink


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


@pytest.mark.parametrize("epoch_base", ["", "manager.base.27"])
def test_publisher_forwards_ros_owner_base_and_keeps_unmanaged_default(
    tmp_path, monkeypatch, epoch_base
) -> None:
    config_path = tmp_path / "site.json"
    _write_joint_config(config_path)
    dispatcher = _FakeDispatcher()
    captured = []

    def for_manager_root(**kwargs):
        captured.append(kwargs)
        return dispatcher

    monkeypatch.setattr(
        pose_source_snapshot_publisher.ObservationDispatcher,
        "for_manager_root",
        for_manager_root,
    )
    rclpy.init(args=["--ros-args", "-p", f"source_epoch_base:={epoch_base}"] if epoch_base else [])
    publisher = None
    try:
        publisher = pose_source_snapshot_publisher.PoseSourceSnapshotPublisher(
            site_config_path=config_path,
            manager_run_root=tmp_path,
        )
        factory = captured[0]["source_epoch_factory"]
        if epoch_base:
            assert factory() == "manager.base.27.1"
            assert factory() == "manager.base.27.2"
        else:
            assert factory is None
        assert captured[0]["manager_run_root"] == tmp_path
    finally:
        if publisher is not None:
            publisher.destroy_node()
        rclpy.shutdown()


def test_ros_joint_state_adapter_forwards_good_and_unknown_manager_intents(
    tmp_path: Path,
) -> None:
    config_path = tmp_path / "site.json"
    _write_joint_config(config_path)
    dispatcher = _FakeDispatcher()
    sinks = _FakeSinks()
    rclpy.init()
    publisher: pose_source_snapshot_publisher.PoseSourceSnapshotPublisher | None = (
        None
    )
    source: Node | None = None
    executor = SingleThreadedExecutor()
    try:
        publisher = pose_source_snapshot_publisher.PoseSourceSnapshotPublisher(
            site_config_path=config_path,
            manager_run_root=tmp_path,
            dispatcher_factory=lambda _root, _config, _callback: dispatcher,
            stream_sink_factory=sinks,
        )
        # One sink per binding, on the component's (source_id) streams.
        assert set(sinks.by_stream) == {"poses/robot.1/viewer.measured_robot"}
        sink = sinks.by_stream["poses/robot.1/viewer.measured_robot"]
        assert sink.component == "isaac.observer"
        source = Node("manyforge_pose_observer_joint_test_source")
        joint_publisher = source.create_publisher(
            JointState, "/isaac_joint_states", 1
        )
        executor.add_node(publisher)
        executor.add_node(source)
        publisher.start()

        valid = JointState()
        valid.header.stamp.sec = 12
        valid.header.stamp.nanosec = 34
        valid.name = ["joint_b", "extra", "joint_a"]
        valid.position = [0.2, 9.0, 0.1]
        joint_publisher.publish(valid)
        _spin_until(executor, lambda: len(dispatcher.intents) >= 1)
        # The submitted good observation is one sample at its header stamp.
        assert sink.stamps == [12_000_000_034]

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
        # An unknown intent is no stream sample.
        assert sink.stamps == [12_000_000_034]
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
            manager_run_root=tmp_path,
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
    client_cancelled = threading.Event()
    client_closed = threading.Event()

    class _SimulationClient:
        def capture(self, **_kwargs: object) -> object:
            captures.set()
            assert client_cancelled.wait(timeout=2.0)
            raise RuntimeError("bounded cancelled test capture")

        def cancel(self) -> None:
            client_cancelled.set()

        def close(self) -> None:
            assert captures.is_set()
            assert client_cancelled.is_set()
            client_closed.set()

    simulation_client = _SimulationClient()

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
            manager_run_root=tmp_path,
            dispatcher_factory=lambda _root, _config, _callback: dispatcher,
            simulation_client_factory=lambda _node, _timeout_s: simulation_client,
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
            assert client_cancelled.is_set()
            assert client_closed.is_set()
            assert dispatcher.intents == []
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


class _RecordingStreamSinks:
    """Real ``ComponentStreamSink`` objects whose wire messages are kept."""

    def __init__(self) -> None:
        self.published: dict[str, list[int]] = {}

    def __call__(self, node: object, component: str, stream_id: str) -> object:
        from manyforge_ros.component_stream_health import ComponentStreamSink

        published = self.published.setdefault(stream_id, [])

        class _Sink(ComponentStreamSink):
            def _publish(self, level, code, stamp_ns, detail) -> None:
                del level, code, detail
                published.append(stamp_ns)

        return _Sink(node, component, stream_id, heartbeat_period_ns=0)


@pytest.mark.parametrize("entity_stamp_ns", [None, 0])
def test_relative_reference_is_read_with_its_object_in_one_batch(
    tmp_path: Path,
    entity_stamp_ns: int | None,
) -> None:
    from types import SimpleNamespace

    from manyforge_ros.scene_mirror_probe import PoseSnapshot

    gripper = (
        "/World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/"
        "ee_link/robotiq_arg2f_base_link"
    )
    quarter = (0.0, 0.0, 2.0**-0.5, 2.0**-0.5)
    batches: list[tuple[str, ...]] = []

    class _BatchClient:
        def capture_entity_states(self, *, entity_names):
            batches.append(tuple(entity_names))
            return SimpleNamespace(
                errors=(),
                entities=(
                    SimpleNamespace(
                        name=gripper,
                        pose=PoseSnapshot(
                            "robotiq_arg2f_base_link",
                            entity_stamp_ns,
                            (1.0, 0.0, 0.5),
                            quarter,
                        ),
                        errors=(),
                    ),
                    SimpleNamespace(
                        name="/mac_n_cheese_centered",
                        pose=PoseSnapshot(
                            "mac_n_cheese_centered",
                            entity_stamp_ns,
                            (1.0, 0.02, 0.25),
                            (0.0, 0.0, 0.0, 1.0),
                        ),
                        errors=(),
                    ),
                ),
            )

        def capture(self, **_kwargs: object) -> object:
            raise AssertionError("a referenced poll must use the batch read")

    context = Context()
    rclpy.init(context=context)
    dispatcher = _FakeDispatcher()
    sinks = _RecordingStreamSinks()
    executor = SingleThreadedExecutor(context=context)
    publisher = pose_source_snapshot_publisher.PoseSourceSnapshotPublisher(
        site_config_path=(
            _REPOSITORY_ROOT
            / "manyforge_ros_bringup/config/isaac_ur10e_light.pose_observer.json"
        ),
        manager_run_root=tmp_path,
        dispatcher_factory=lambda _root, _config, _callback: dispatcher,
        simulation_client_factory=lambda _node, _timeout_s: _BatchClient(),
        stream_sink_factory=sinks,
        context=context,
    )
    spin_thread = threading.Thread(target=executor.spin, daemon=True)
    try:
        executor.add_node(publisher)
        for auxiliary in publisher.auxiliary_nodes:
            executor.add_node(auxiliary)
        publisher.start()
        spin_thread.start()
        deadline = time.monotonic() + 5.0
        while (
            time.monotonic() < deadline
            and sum(
                intent.object_id == "isaac_cheese_box"
                for intent in dispatcher.intents
            )
            < 3
        ):
            time.sleep(0.01)
    finally:
        assert publisher.stop()
        for auxiliary in publisher.auxiliary_nodes:
            executor.remove_node(auxiliary)
            auxiliary.destroy_node()
        executor.remove_node(publisher)
        publisher.destroy_node()
        executor.shutdown(timeout_sec=2.0)
        spin_thread.join(timeout=2.0)
        if context.ok():
            rclpy.shutdown(context=context)

    assert batches and batches[0] == (gripper, "/mac_n_cheese_centered")
    cheese = next(
        intent.payload_value()
        for intent in dispatcher.intents
        if intent.object_id == "isaac_cheese_box"
    )
    assert cheese["kind"] == "simulation_entity_pose_with_references.v1"
    assert cheese["pose"]["position_m"] == [1.0, 0.02, 0.25]
    assert len(cheese["references"]) == 1
    reference = cheese["references"][0]
    assert reference["controllable_component_id"] == "isaac_ur10e_light_robot"
    assert reference["command_target_id"] == "gripper"
    assert reference["gripper_link"] == "robotiq_base_link"
    assert reference["coherent"] is True
    assert reference["link"] == "robotiq_base_link"
    assert reference["pose_in_reference"]["position_m"] == pytest.approx(
        [0.02, 0.0, -0.25]
    )
    assert reference["pose_in_reference"]["orientation_xyzw"] == pytest.approx(
        [0.0, 0.0, -(2.0**-0.5), 2.0**-0.5]
    )
    tcp = next(
        intent.payload_value()
        for intent in dispatcher.intents
        if intent.policy_id == "viewer.measured_tcp"
    )
    assert tcp["kind"] == "simulation_entity_pose"
    # Each submitted simulation pose is one sample of its own binding's
    # stream. These entity poses carry no stamp (live Isaac: stamp 0), so
    # every poll is an arrival sample ("0" on the wire); a zero stamp must
    # not freeze the stream after its first sample (live r10-E: stale).
    for stream_id in (
        "poses/isaac_cheese_box",
        "poses/isaac_ur10e_light_robot/viewer.measured_tcp",
    ):
        published = sinks.published[stream_id]
        assert len(published) >= 3 and set(published) == {0}


def test_entrypoint_does_not_teardown_nodes_while_polling_worker_is_live(
    monkeypatch,
) -> None:
    events: list[str] = []

    class _Context:
        def ok(self) -> bool:
            return True

    class _Auxiliary:
        def destroy_node(self) -> None:
            events.append("auxiliary:destroy")

    auxiliary = _Auxiliary()

    class _Publisher:
        auxiliary_nodes = (auxiliary,)

        def __init__(self, *, context: _Context) -> None:
            del context

        def start(self) -> None:
            events.append("publisher:start")

        def stop(self) -> bool:
            events.append("publisher:stop-incomplete")
            return False

        def destroy_node(self) -> None:
            events.append("publisher:destroy")

    class _Executor:
        def __init__(self, *, context: _Context) -> None:
            del context

        def add_node(self, _node: object) -> None:
            pass

        def remove_node(self, _node: object) -> None:
            events.append("executor:remove")

        def spin(self) -> None:
            raise KeyboardInterrupt

        def shutdown(self, *, timeout_sec: float) -> None:
            del timeout_sec
            events.append("executor:shutdown")

    monkeypatch.setattr(pose_source_snapshot_publisher, "Context", _Context)
    monkeypatch.setattr(
        pose_source_snapshot_publisher, "PoseSourceSnapshotPublisher", _Publisher
    )
    monkeypatch.setattr(
        pose_source_snapshot_publisher, "SingleThreadedExecutor", _Executor
    )
    monkeypatch.setattr(pose_source_snapshot_publisher.rclpy, "init", lambda **_: None)
    monkeypatch.setattr(
        pose_source_snapshot_publisher.rclpy,
        "shutdown",
        lambda **_: events.append("context:shutdown"),
    )

    assert pose_source_snapshot_publisher.main([]) == 1
    assert events == ["publisher:start", "publisher:stop-incomplete"]


def test_simulation_worker_future_completes_on_single_threaded_executor() -> None:
    from std_srvs.srv import Trigger

    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    context = Context()
    rclpy.init(context=context)
    node = Node("pose_source_single_executor_service_test", context=context)
    executor = SingleThreadedExecutor(context=context)
    executor.add_node(node)
    client = SimulationSceneMirrorClient(node, service_timeout_s=2.0)
    service_name = "/pose_source_single_executor_service_test/read"
    completed = threading.Event()
    replies: list[object] = []
    errors: list[Exception] = []

    def respond(_request: object, response: object) -> object:
        response.success = True
        response.message = "completed on the attached executor"
        return response

    service = node.create_service(Trigger, service_name, respond)

    def capture() -> None:
        try:
            replies.append(
                client._call_service(service_name, Trigger, Trigger.Request())
            )
        except Exception as error:
            errors.append(error)
        finally:
            completed.set()

    spinner = threading.Thread(target=executor.spin, daemon=True)
    worker = threading.Thread(target=capture, daemon=True)
    try:
        spinner.start()
        deadline = time.monotonic() + 2.0
        while not executor.is_spinning and time.monotonic() < deadline:
            time.sleep(0.001)
        assert executor.is_spinning
        worker.start()
        assert completed.wait(timeout=3.0)
        worker.join(timeout=1.0)
        assert not worker.is_alive()
        assert errors == []
        assert len(replies) == 1 and replies[0].success
    finally:
        client.cancel()
        if worker.ident is not None:
            worker.join(timeout=3.0)
        assert not worker.is_alive()
        client.close()
        executor.shutdown(timeout_sec=2.0)
        spinner.join(timeout=2.0)
        assert not spinner.is_alive()
        executor.remove_node(node)
        node.destroy_service(service)
        node.destroy_node()
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
    # The Manager-side configuration check is gone with its only caller.
    assert "--check-config" not in source


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
                ("isaac_cheese_box", "scene"),
            },
        ),
        (
            "isaac_ur10e_collision.pose_observer.json",
            "isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml",
            "isaac_ur10e_collision_robot",
            {
                ("isaac_ur10e_collision_robot", "viewer.measured_robot"),
                ("isaac_ur10e_collision_robot", "viewer.measured_tcp"),
                ("isaac_cheese_box", "scene"),
            },
        ),
        (
            "isaac_ur10e_assistant.pose_observer.json",
            "isaac_manipulator_scene_light_assistant.deployment.yaml",
            "isaac_ur10e_assistant_robot",
            {
                ("isaac_ur10e_assistant_robot", "viewer.measured_robot"),
                ("isaac_ur10e_assistant_robot", "viewer.measured_tcp"),
                ("isaac_cheese_box", "scene"),
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
                ("graspable", "scene"),
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

        cheese_references = ur_config.simulation_entities[1].references
        if config_name == "isaac_ur10e_light.pose_observer.json":
            assert len(cheese_references) == 1
            cheese_reference = cheese_references[0]
            assert cheese_reference.controllable_component_id == "isaac_ur10e_light_robot"
            assert cheese_reference.command_target_id == "gripper"
            assert cheese_reference.request.entity == rigid_gripper
            assert cheese_reference.link == "robotiq_base_link"
        else:
            assert cheese_references == ()

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


def _pose_source_launch_module():
    spec = importlib.util.spec_from_file_location(
        "pose_source_launch",
        _REPOSITORY_ROOT / "manyforge_ros_bringup/launch/pose_source.launch.py",
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_pose_source_launch_runs_the_observer_with_the_host_parameters() -> None:
    module = _pose_source_launch_module()
    kwargs = module.node_kwargs(
        "/run/x/pose_source.json", "/run/manager", "isaac_scene_objects.ab12"
    )
    assert kwargs["executable"] == "pose_source_snapshot_publisher"
    assert kwargs["output"] == "screen"
    assert kwargs["parameters"] == [
        {
            "site_config_path": "/run/x/pose_source.json",
            "manager_run_root": "/run/manager",
            "source_epoch_base": "isaac_scene_objects.ab12",
        }
    ]
    for arguments in (
        ("relative.json", "/run/manager", "base"),
        ("/run/x/pose_source.json", "", "base"),
        ("/run/x/pose_source.json", "/run/manager", ""),
    ):
        with pytest.raises(ValueError):
            module.node_kwargs(*arguments)
    assert (
        "pose_source_snapshot_publisher = "
        "manyforge_ros_bringup.pose_source_snapshot_publisher:main"
    ) in (_REPOSITORY_ROOT / "manyforge_ros_bringup/setup.py").read_text(
        encoding="utf-8"
    )
