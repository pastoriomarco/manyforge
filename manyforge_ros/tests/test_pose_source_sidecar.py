from __future__ import annotations

import threading
from math import nan

import pytest


def test_measured_tcp_fixed_offset_rejects_non_finite_entity_position():
    from manyforge_ros.pose_source_sidecar import _compose_pose_with_local_offset
    from manyforge_ros.scene_mirror_probe import PoseSnapshot

    pose = PoseSnapshot(
        frame_id="world",
        stamp_sec=1.0,
        position_xyz=(nan, 0.0, 0.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )

    with pytest.raises(ValueError, match="entity position contains non-finite"):
        _compose_pose_with_local_offset(
            pose,
            (0.0, 0.0, 0.2),
            (0.0, 0.0, 0.0, 1.0),
        )


def test_simulation_interfaces_pose_source_polls_latest_wins_state():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import SimulatorFeatures
    from simulation_interfaces.srv import GetEntityState, GetSimulatorFeatures

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        POSE_SOURCE_STATUS_STALE,
        PoseSourceBinding,
        PoseSourceMeasuredTcpBinding,
        SimulationInterfacesPoseSource,
    )
    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_pose_source_fake_sim")
    client_node = rclpy.create_node("manyforge_pose_source_client")
    services = []
    x_position = {"value": 1.0}
    now = {"value": 100.0}

    def get_features(_request, response):
        response.features.features = [SimulatorFeatures.ENTITY_STATE_GETTING]
        return response

    def get_entity_state(request, response):
        response.result.result = response.result.RESULT_OK
        response.state.header.frame_id = "world"
        response.state.header.stamp.sec = int(now["value"])
        if request.entity == "/World/tool0":
            response.state.pose.position.x = 0.5
            response.state.pose.position.y = 0.0
            response.state.pose.position.z = 0.25
            response.state.pose.orientation.z = 2.0**-0.5
            response.state.pose.orientation.w = 2.0**-0.5
        else:
            response.state.pose.position.x = x_position["value"]
            response.state.pose.position.y = 2.0
            response.state.pose.position.z = 3.0
            response.state.pose.orientation.w = 1.0
        return response

    services.append(
        server_node.create_service(
            GetSimulatorFeatures,
            "/get_simulator_features",
            get_features,
        )
    )
    services.append(
        server_node.create_service(GetEntityState, "/get_entity_state", get_entity_state)
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationSceneMirrorClient(client_node, service_timeout_s=2.0)
        source = SimulationInterfacesPoseSource(
            client=client,
            pose_source_id="isaac_scene_objects",
            bindings=[
                PoseSourceBinding(
                    object_id="isaac_cheese_box",
                    entity="/mac_n_cheese_centered",
                    apply_policy="observe_only",
                )
            ],
            measured_tcp=PoseSourceMeasuredTcpBinding(
                entity="/World/tool0",
                tcp_frame_id="tool0",
                frame_id="world",
                entity_to_tcp_position_xyz=(0.2, 0.0, 0.0),
            ),
            max_update_hz=4.0,
            stale_after_s=1.0,
            now_fn=lambda: now["value"],
        )

        first = source.poll_once()
        assert first.status == POSE_SOURCE_STATUS_FRESH
        assert len(first.updates) == 1
        assert first.updates[0].pose.position_xyz == (1.0, 2.0, 3.0)

        x_position["value"] = 4.0
        now["value"] = 100.25
        second = source.poll_once()
        assert second.status == POSE_SOURCE_STATUS_FRESH
        assert second.updates[0].pose.position_xyz == (4.0, 2.0, 3.0)

        live = source.state.snapshot(monotonic_s=100.25)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        assert live.objects[0].status == POSE_SOURCE_STATUS_FRESH
        assert live.objects[0].update is not None
        assert live.objects[0].update.pose.position_xyz == (4.0, 2.0, 3.0)
        assert live.measured_tcp is not None
        assert live.measured_tcp.tcp_frame_id == "tool0"
        assert live.measured_tcp.pose.position_xyz == pytest.approx(
            (0.5, 0.2, 0.25)
        )
        payload = live.to_dict()
        assert payload["measuredTcp"]["tcpFrameId"] == "tool0"
        assert payload["measuredTcp"]["pose"]["positionXYZ"] == pytest.approx(
            [0.5, 0.2, 0.25]
        )

        stale = source.state.snapshot(monotonic_s=102.0)
        assert stale.status == POSE_SOURCE_STATUS_STALE
        assert stale.objects[0].status == POSE_SOURCE_STATUS_STALE
        assert stale.objects[0].age_s == pytest.approx(1.75)
        assert stale.measured_tcp_age_s == pytest.approx(1.75)
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


def test_measured_robot_joint_state_tracker_snapshots_latest():
    rclpy = pytest.importorskip("rclpy")
    sensor_msgs = pytest.importorskip("sensor_msgs.msg")

    from manyforge_ros.pose_source_sidecar import (
        MeasuredRobotJointStateTracker,
        PoseSourceLatestState,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_measured_robot_joint_state_test")
    publisher = node.create_publisher(
        sensor_msgs.JointState,
        "/manyforge_test_joint_states",
        10,
    )
    now = {"value": 12.0}
    tracker = MeasuredRobotJointStateTracker(
        node=node,
        pose_source_id="isaac_scene_objects",
        joint_state_topic="/manyforge_test_joint_states",
        now_fn=lambda: now["value"],
    )
    state = PoseSourceLatestState(
        pose_source_id="isaac_scene_objects",
        object_ids=["isaac_cheese_box"],
        stale_after_s=1.0,
    )

    try:
        msg = sensor_msgs.JointState()
        msg.header.stamp.sec = 17
        msg.header.stamp.nanosec = 250_000_000
        msg.name = ["shoulder_pan_joint", "elbow_joint"]
        msg.position = [0.25, -0.5]
        for _ in range(20):
            publisher.publish(msg)
            rclpy.spin_once(node, timeout_sec=0.05)
            if tracker.latest_update() is not None:
                break

        update = tracker.latest_update()
        assert update is not None
        assert update.joint_state_topic == "/manyforge_test_joint_states"
        assert update.joint_names == ("shoulder_pan_joint", "elbow_joint")
        assert update.joint_positions_rad == (0.25, -0.5)
        assert update.stamp_sec == pytest.approx(17.25)

        state.apply_measured_robot_update(update)
        snapshot = state.snapshot(monotonic_s=12.5)
        assert snapshot.measured_robot is not None
        assert snapshot.measured_robot_age_s == pytest.approx(0.5)
        payload = snapshot.to_dict()
        assert payload["measuredRobot"]["jointStateTopic"] == (
            "/manyforge_test_joint_states"
        )
        assert payload["measuredRobot"]["jointNames"] == [
            "shoulder_pan_joint",
            "elbow_joint",
        ]
        assert payload["measuredRobot"]["jointPositionsRad"] == [0.25, -0.5]
    finally:
        node.destroy_node()
        rclpy.shutdown()


def test_simulation_interfaces_pose_source_rejects_missing_entity_pose():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import SimulatorFeatures
    from simulation_interfaces.srv import GetEntityState, GetSimulatorFeatures

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_REJECTED,
        PoseSourceBinding,
        SimulationInterfacesPoseSource,
    )
    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_pose_source_fake_reject_sim")
    client_node = rclpy.create_node("manyforge_pose_source_reject_client")
    services = []

    def get_features(_request, response):
        response.features.features = [SimulatorFeatures.ENTITY_STATE_GETTING]
        return response

    def get_entity_state(_request, response):
        response.result.result = 999
        response.result.error_message = "entity not found"
        return response

    services.append(
        server_node.create_service(
            GetSimulatorFeatures,
            "/get_simulator_features",
            get_features,
        )
    )
    services.append(
        server_node.create_service(GetEntityState, "/get_entity_state", get_entity_state)
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationSceneMirrorClient(client_node, service_timeout_s=2.0)
        source = SimulationInterfacesPoseSource(
            client=client,
            pose_source_id="isaac_scene_objects",
            bindings=[
                PoseSourceBinding(
                    object_id="isaac_cheese_box",
                    entity="/missing",
                    apply_policy="observe_only",
                )
            ],
            now_fn=lambda: 10.0,
        )

        result = source.poll_once()
        assert result.status == POSE_SOURCE_STATUS_REJECTED
        assert result.updates == ()
        assert len(result.rejections) == 1
        assert result.rejections[0].reason == "source_error"
        assert "entity not found" in result.rejections[0].detail

        state = source.state.snapshot(monotonic_s=10.0)
        assert state.status == POSE_SOURCE_STATUS_REJECTED
        assert state.objects[0].rejection == result.rejections[0]
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


def test_simulation_interfaces_pose_source_uses_configured_external_frame():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("simulation_interfaces")

    from rclpy.executors import MultiThreadedExecutor
    from simulation_interfaces.msg import SimulatorFeatures
    from simulation_interfaces.srv import GetEntityState, GetSimulatorFeatures

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        SimulationInterfacesPoseSource,
    )
    from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient

    class Obj:
        def __init__(self, **kwargs):
            self.__dict__.update(kwargs)

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_pose_source_frame_fake_sim")
    client_node = rclpy.create_node("manyforge_pose_source_frame_client")
    services = []

    def get_features(_request, response):
        response.features.features = [SimulatorFeatures.ENTITY_STATE_GETTING]
        return response

    def get_entity_state(request, response):
        response.result.result = response.result.RESULT_OK
        response.state.header.frame_id = request.entity.rsplit("/", 1)[-1]
        response.state.pose.position.x = 0.5 if request.entity.endswith("tool0") else 1.0
        response.state.pose.position.y = 2.0
        response.state.pose.position.z = 3.0
        response.state.pose.orientation.w = 1.0
        return response

    services.append(
        server_node.create_service(
            GetSimulatorFeatures,
            "/get_simulator_features",
            get_features,
        )
    )
    services.append(
        server_node.create_service(GetEntityState, "/get_entity_state", get_entity_state)
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        client = SimulationSceneMirrorClient(client_node, service_timeout_s=2.0)
        source = SimulationInterfacesPoseSource.from_deployment_config(
            client=client,
            source_config=Obj(
                source_id="isaac_scene_objects",
                frame_id="universe",
                max_update_hz=4.0,
                stale_after_s=1.0,
                measured_tcp=Obj(
                    entity="/World/tool0",
                    tcp_frame_id="tool0",
                    frame_id="universe",
                ),
                objects={
                    "isaac_cheese_box": Obj(
                        object_id="isaac_cheese_box",
                        entity="/mac_n_cheese_centered",
                        apply_policy="continuous_unattached",
                        frame_id="",
                        confidence_min=None,
                    )
                },
            ),
            now_fn=lambda: 42.0,
        )

        result = source.poll_once()
        assert result.status == POSE_SOURCE_STATUS_FRESH
        assert result.updates[0].pose.frame_id == "universe"

        snapshot = source.state.snapshot(monotonic_s=42.0)
        assert snapshot.measured_tcp is not None
        assert snapshot.measured_tcp.frame_id == "universe"
        assert snapshot.measured_tcp.pose.frame_id == "universe"
        payload = snapshot.to_dict()
        assert payload["objects"][0]["update"]["pose"]["frameId"] == "universe"
        assert payload["measuredTcp"]["pose"]["frameId"] == "universe"
    finally:
        for service in services:
            server_node.destroy_service(service)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


def test_pose_stamped_topic_source_normalizes_latest_wins_state():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("geometry_msgs")

    from geometry_msgs.msg import PoseStamped
    from rclpy.executors import SingleThreadedExecutor

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        POSE_SOURCE_STATUS_STALE,
        PoseSourceBinding,
        PoseStampedTopicPoseSource,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_pose_stamped_topic_source_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(PoseStamped, "/tracked_box_pose", 1)
    now = {"value": 20.0}

    source = PoseStampedTopicPoseSource(
        node=node,
        pose_source_id="tracked_box",
        binding=PoseSourceBinding(
            object_id="box",
            entity="/tracked_box_pose",
            apply_policy="before_planning",
        ),
        topic="/tracked_box_pose",
        stale_after_s=0.5,
        now_fn=lambda: now["value"],
    )

    try:
        msg = PoseStamped()
        msg.header.frame_id = "world"
        msg.header.stamp.sec = 12
        msg.header.stamp.nanosec = 250_000_000
        msg.pose.position.x = 1.0
        msg.pose.position.y = 2.0
        msg.pose.position.z = 3.0
        msg.pose.orientation.w = 2.0
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: source.snapshot(monotonic_s=20.0).objects[0].update
            is not None,
        )

        live = source.snapshot(monotonic_s=20.0)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        assert live.objects[0].status == POSE_SOURCE_STATUS_FRESH
        update = live.objects[0].update
        assert update is not None
        assert update.object_id == "box"
        assert update.entity == "/tracked_box_pose"
        assert update.apply_policy == "before_planning"
        assert update.pose.frame_id == "world"
        assert update.pose.stamp_sec == pytest.approx(12.25)
        assert update.pose.position_xyz == (1.0, 2.0, 3.0)
        assert update.pose.orientation_xyzw == (0.0, 0.0, 0.0, 1.0)
        assert live.to_dict()["objects"][0]["update"]["pose"]["frameId"] == "world"

        msg.pose.position.x = 9.0
        now["value"] = 20.01
        for _ in range(5):
            publisher.publish(msg)
            executor.spin_once(timeout_sec=0.02)
        throttled = source.snapshot(monotonic_s=20.01).objects[0].update
        assert throttled is not None
        assert throttled.pose.position_xyz == (1.0, 2.0, 3.0)

        now["value"] = 20.10
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: (
                source.snapshot(monotonic_s=20.10).objects[0].update is not None
                and source.snapshot(
                    monotonic_s=20.10,
                ).objects[0].update.pose.position_xyz
                == (9.0, 2.0, 3.0)
            ),
        )

        stale = source.snapshot(monotonic_s=20.75)
        assert stale.status == POSE_SOURCE_STATUS_STALE
        assert stale.objects[0].status == POSE_SOURCE_STATUS_STALE
        assert stale.objects[0].age_s == pytest.approx(0.65)
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_pose_stamped_topic_source_rejects_invalid_pose():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("geometry_msgs")

    from geometry_msgs.msg import PoseStamped
    from rclpy.executors import SingleThreadedExecutor

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_REJECTED,
        PoseSourceBinding,
        PoseStampedTopicPoseSource,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_pose_stamped_topic_reject_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(PoseStamped, "/bad_pose", 1)
    source = PoseStampedTopicPoseSource(
        node=node,
        pose_source_id="bad_pose_source",
        binding=PoseSourceBinding(object_id="box", entity="/bad_pose"),
        topic="/bad_pose",
        now_fn=lambda: 30.0,
    )

    try:
        msg = PoseStamped()
        msg.header.frame_id = "world"
        msg.pose.orientation.w = 0.0
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: source.snapshot(monotonic_s=30.0).objects[0].rejection
            is not None,
        )

        live = source.snapshot(monotonic_s=30.0)
        assert live.status == POSE_SOURCE_STATUS_REJECTED
        rejection = live.objects[0].rejection
        assert rejection is not None
        assert rejection.reason == "invalid_pose"
        assert "zero norm" in rejection.detail
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_pose_array_topic_source_updates_indexed_objects():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("geometry_msgs")

    from geometry_msgs.msg import PoseArray
    from rclpy.executors import SingleThreadedExecutor

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        PoseArrayTopicPoseSource,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_pose_array_topic_source_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(PoseArray, "/tracked_object_poses", 1)
    now = {"value": 40.0}
    source = PoseArrayTopicPoseSource(
        node=node,
        pose_source_id="tracked_objects",
        bindings=[
            _pose_array_binding("box", 0, apply_policy="observe_only"),
            _pose_array_binding("can", 1, apply_policy="before_planning"),
        ],
        topic="/tracked_object_poses",
        frame_id="world",
        stale_after_s=0.5,
        now_fn=lambda: now["value"],
    )

    try:
        msg = PoseArray()
        msg.header.frame_id = "world"
        msg.header.stamp.sec = 41
        msg.poses.append(_pose(1.0, 2.0, 3.0))
        msg.poses.append(_pose(4.0, 5.0, 6.0))
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: source.snapshot(monotonic_s=40.0).objects[1].update
            is not None,
        )

        live = source.snapshot(monotonic_s=40.0)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        by_object = {obj.object_id: obj for obj in live.objects}
        assert by_object["box"].update is not None
        assert by_object["box"].update.pose.position_xyz == (1.0, 2.0, 3.0)
        assert by_object["box"].update.entity == "/tracked_object_poses[0]"
        assert by_object["can"].update is not None
        assert by_object["can"].update.pose.position_xyz == (4.0, 5.0, 6.0)
        assert by_object["can"].update.apply_policy == "before_planning"
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_pose_array_topic_source_rejects_missing_index_without_dropping_valid_pose():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("geometry_msgs")

    from geometry_msgs.msg import PoseArray
    from rclpy.executors import SingleThreadedExecutor

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        PoseArrayTopicPoseSource,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_pose_array_topic_reject_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(PoseArray, "/partial_object_poses", 1)
    now = {"value": 50.0}
    source = PoseArrayTopicPoseSource(
        node=node,
        pose_source_id="partial_objects",
        bindings=[
            _pose_array_binding("box", 0),
            _pose_array_binding("missing", 2),
        ],
        topic="/partial_object_poses",
        frame_id="world",
        now_fn=lambda: now["value"],
    )

    try:
        full = PoseArray()
        full.header.frame_id = "world"
        full.poses.append(_pose(1.0, 0.0, 0.0))
        full.poses.append(_pose(99.0, 0.0, 0.0))
        full.poses.append(_pose(2.0, 0.0, 0.0))
        _publish_until(
            executor,
            publisher,
            full,
            lambda: source.snapshot(monotonic_s=50.0).objects[1].update
            is not None,
        )

        now["value"] = 50.2
        msg = PoseArray()
        msg.header.frame_id = "world"
        msg.poses.append(_pose(1.0, 0.0, 0.0))
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: source.snapshot(monotonic_s=50.2).objects[1].rejection
            is not None,
        )

        live = source.snapshot(monotonic_s=50.2)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        by_object = {obj.object_id: obj for obj in live.objects}
        assert by_object["box"].update is not None
        assert by_object["missing"].update is None
        assert by_object["missing"].rejection is not None
        assert by_object["missing"].rejection.reason == "pose_index_missing"
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_tf_pose_source_polls_configured_frame_pairs():
    pytest.importorskip("geometry_msgs")

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        PoseSourceBinding,
        TfPoseSource,
    )

    buffer = _FakeTfBuffer(
        {
            ("world", "box_frame"): _transform("world", "box_frame", 1.0, 2.0, 3.0),
            ("map", "can_frame"): _transform("map", "can_frame", 4.0, 5.0, 6.0),
        }
    )
    source = TfPoseSource(
        node=object(),
        pose_source_id="tracked_tf_objects",
        bindings=[
            PoseSourceBinding(
                object_id="box",
                entity="box_frame",
                frame_id="world",
                apply_policy="observe_only",
            ),
            PoseSourceBinding(
                object_id="can",
                entity="can_frame",
                frame_id="map",
                apply_policy="before_planning",
            ),
        ],
        buffer=buffer,
        now_fn=lambda: 60.0,
    )

    result = source.poll_once()
    assert result.status == POSE_SOURCE_STATUS_FRESH
    live = source.state.snapshot(monotonic_s=60.0)
    assert live.status == POSE_SOURCE_STATUS_FRESH
    by_object = {obj.object_id: obj for obj in live.objects}
    assert by_object["box"].update is not None
    assert by_object["box"].update.pose.frame_id == "world"
    assert by_object["box"].update.pose.position_xyz == (1.0, 2.0, 3.0)
    assert by_object["can"].update is not None
    assert by_object["can"].update.apply_policy == "before_planning"
    assert buffer.lookups == [("world", "box_frame"), ("map", "can_frame")]


def test_tf_pose_source_rejects_lookup_failures_and_clears_prior_update():
    pytest.importorskip("geometry_msgs")

    from manyforge_ros.pose_source_sidecar import (
        POSE_SOURCE_STATUS_FRESH,
        PoseSourceBinding,
        TfPoseSource,
    )

    buffer = _FakeTfBuffer(
        {("world", "box_frame"): _transform("world", "box_frame", 1.0, 0.0, 0.0)}
    )
    source = TfPoseSource(
        node=object(),
        pose_source_id="tracked_tf_objects",
        bindings=[
            PoseSourceBinding(
                object_id="box",
                entity="box_frame",
                frame_id="world",
            )
        ],
        buffer=buffer,
        now_fn=lambda: 70.0,
    )
    assert source.poll_once().status == POSE_SOURCE_STATUS_FRESH

    buffer.transforms.clear()
    source.poll_once()
    live = source.state.snapshot(monotonic_s=70.0)
    assert live.objects[0].update is None
    assert live.objects[0].rejection is not None
    assert live.objects[0].rejection.reason == "tf_lookup_failed"


def test_detection3d_array_topic_source_updates_associated_objects():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("vision_msgs")

    from rclpy.executors import SingleThreadedExecutor
    from vision_msgs.msg import Detection3DArray

    from manyforge_ros.pose_source_sidecar import (
        Detection3DArrayTopicPoseSource,
        POSE_SOURCE_STATUS_FRESH,
        PoseSourceBinding,
        _Detection3DObjectBinding,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_detection3d_source_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(Detection3DArray, "/detections", 1)
    now = {"value": 80.0}
    source = Detection3DArrayTopicPoseSource(
        node=node,
        pose_source_id="detections",
        bindings=[
            _Detection3DObjectBinding(
                binding=PoseSourceBinding(
                    object_id="box",
                    entity="box",
                    frame_id="world",
                    apply_policy="observe_only",
                    confidence_min=0.5,
                ),
                class_id="box",
            ),
            _Detection3DObjectBinding(
                binding=PoseSourceBinding(
                    object_id="can",
                    entity="can-01",
                    frame_id="world",
                    apply_policy="before_planning",
                    confidence_min=0.8,
                ),
                detection_id="can-01",
                max_position_variance_m2=0.01,
            ),
        ],
        topic="/detections",
        frame_id="world",
        now_fn=lambda: now["value"],
    )

    try:
        msg = Detection3DArray()
        msg.header.frame_id = "world"
        msg.detections.append(
            _detection3d("box-01", "box", 0.7, 1.0, 2.0, 3.0, covariance=0.02)
        )
        msg.detections.append(
            _detection3d("can-01", "can", 0.9, 4.0, 5.0, 6.0, covariance=0.001)
        )
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: source.snapshot(monotonic_s=80.0).objects[1].update is not None,
        )

        live = source.snapshot(monotonic_s=80.0)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        by_object = {obj.object_id: obj for obj in live.objects}
        assert by_object["box"].update is not None
        assert by_object["box"].update.pose.position_xyz == (1.0, 2.0, 3.0)
        assert by_object["box"].update.metadata["classId"] == "box"
        assert by_object["box"].update.metadata["confidence"] == pytest.approx(0.7)
        assert by_object["can"].update is not None
        assert by_object["can"].update.pose.position_xyz == (4.0, 5.0, 6.0)
        assert by_object["can"].update.metadata["detectionId"] == "can-01"
        payload = live.to_dict()
        assert payload["objects"][1]["update"]["metadata"]["confidence"] == pytest.approx(
            0.9
        )
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_detection3d_array_topic_source_selection_transforms_foundationpose_frame():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("vision_msgs")

    from rclpy.executors import SingleThreadedExecutor
    from vision_msgs.msg import Detection3DArray

    from manyforge_ros.pose_source_sidecar import (
        Detection3DArrayTopicPoseSource,
        POSE_SOURCE_STATUS_FRESH,
        PoseSourceBinding,
        _Detection3DObjectBinding,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_detection3d_selection_tf_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(Detection3DArray, "/foundationpose_output", 1)
    buffer = _FakeTfBuffer(
        {
            ("link_base", "camera_color_optical_frame"): _transform(
                "link_base",
                "camera_color_optical_frame",
                10.0,
                0.0,
                0.0,
            )
        }
    )
    source = Detection3DArrayTopicPoseSource(
        node=node,
        pose_source_id="foundationpose",
        bindings=[
            _Detection3DObjectBinding(
                binding=PoseSourceBinding(
                    object_id="graspable",
                    entity="first",
                    frame_id="link_base",
                    apply_policy="before_planning",
                    confidence_min=0.0,
                ),
                selection="first",
                max_position_variance_m2=0.0025,
            )
        ],
        topic="/foundationpose_output",
        frame_id="link_base",
        buffer=buffer,
        now_fn=lambda: 100.0,
    )

    try:
        msg = Detection3DArray()
        msg.header.frame_id = "camera_color_optical_frame"
        detection = _detection3d("", "", 0.0, 1.0, 2.0, 3.0, covariance=0.0)
        detection.header.frame_id = "camera_color_optical_frame"
        msg.detections.append(detection)
        _publish_until(
            executor,
            publisher,
            msg,
            lambda: source.snapshot(monotonic_s=100.0).objects[0].update is not None,
        )

        live = source.snapshot(monotonic_s=100.0)
        assert live.status == POSE_SOURCE_STATUS_FRESH
        update = live.objects[0].update
        assert update is not None
        assert update.pose.frame_id == "link_base"
        assert update.pose.position_xyz == pytest.approx((11.0, 2.0, 3.0))
        assert update.metadata["detectionId"] == ""
        assert update.metadata["classId"] == ""
        assert update.metadata["confidence"] == pytest.approx(0.0)
        assert update.metadata["maxPositionVarianceM2"] == pytest.approx(0.0)
        assert buffer.lookups == [("link_base", "camera_color_optical_frame")]
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_detection3d_array_topic_source_rejects_quality_failures_and_clears_update():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("vision_msgs")

    from rclpy.executors import SingleThreadedExecutor
    from vision_msgs.msg import Detection3DArray

    from manyforge_ros.pose_source_sidecar import (
        Detection3DArrayTopicPoseSource,
        PoseSourceBinding,
        _Detection3DObjectBinding,
    )

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_detection3d_reject_test")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    publisher = node.create_publisher(Detection3DArray, "/quality_detections", 1)
    now = {"value": 90.0}
    source = Detection3DArrayTopicPoseSource(
        node=node,
        pose_source_id="quality_detections",
        bindings=[
            _Detection3DObjectBinding(
                binding=PoseSourceBinding(
                    object_id="box",
                    entity="box",
                    frame_id="world",
                    confidence_min=0.8,
                ),
                class_id="box",
                max_position_variance_m2=0.01,
            )
        ],
        topic="/quality_detections",
        frame_id="world",
        now_fn=lambda: now["value"],
    )

    try:
        good = Detection3DArray()
        good.header.frame_id = "world"
        good.detections.append(
            _detection3d("box-01", "box", 0.9, 1.0, 0.0, 0.0, covariance=0.001)
        )
        _publish_until(
            executor,
            publisher,
            good,
            lambda: source.snapshot(monotonic_s=90.0).objects[0].update is not None,
        )

        now["value"] = 90.2
        low_confidence = Detection3DArray()
        low_confidence.header.frame_id = "world"
        low_confidence.detections.append(
            _detection3d("box-01", "box", 0.1, 2.0, 0.0, 0.0, covariance=0.001)
        )
        _publish_until(
            executor,
            publisher,
            low_confidence,
            lambda: source.snapshot(monotonic_s=90.2).objects[0].rejection
            is not None,
        )
        live = source.snapshot(monotonic_s=90.2)
        assert live.objects[0].update is None
        assert live.objects[0].rejection.reason == "confidence_below_min"

        now["value"] = 90.4
        high_covariance = Detection3DArray()
        high_covariance.header.frame_id = "world"
        high_covariance.detections.append(
            _detection3d("box-01", "box", 0.95, 3.0, 0.0, 0.0, covariance=0.5)
        )
        _publish_until(
            executor,
            publisher,
            high_covariance,
            lambda: (
                source.snapshot(monotonic_s=90.4).objects[0].rejection is not None
                and source.snapshot(
                    monotonic_s=90.4,
                ).objects[0].rejection.reason
                == "covariance_exceeded"
            ),
        )
        live = source.snapshot(monotonic_s=90.4)
        assert live.objects[0].update is None
        assert live.objects[0].rejection.reason == "covariance_exceeded"
    finally:
        source.close()
        node.destroy_publisher(publisher)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


class _FakeTfBuffer:
    def __init__(self, transforms):
        self.transforms = dict(transforms)
        self.lookups = []

    def lookup_transform(self, target_frame, source_frame, _time):
        self.lookups.append((target_frame, source_frame))
        try:
            return self.transforms[(target_frame, source_frame)]
        except KeyError as exc:
            raise RuntimeError(
                f"missing transform {target_frame} <- {source_frame}"
            ) from exc


def _transform(frame_id: str, child_frame_id: str, x: float, y: float, z: float):
    from geometry_msgs.msg import TransformStamped

    transform = TransformStamped()
    transform.header.frame_id = frame_id
    transform.header.stamp.sec = 61
    transform.header.stamp.nanosec = 500_000_000
    transform.child_frame_id = child_frame_id
    transform.transform.translation.x = x
    transform.transform.translation.y = y
    transform.transform.translation.z = z
    transform.transform.rotation.w = 2.0
    return transform


def _detection3d(
    detection_id: str,
    class_id: str,
    score: float,
    x: float,
    y: float,
    z: float,
    *,
    covariance: float,
):
    from vision_msgs.msg import Detection3D, ObjectHypothesisWithPose

    detection = Detection3D()
    detection.header.frame_id = "world"
    detection.id = detection_id
    result = ObjectHypothesisWithPose()
    result.hypothesis.class_id = class_id
    result.hypothesis.score = score
    result.pose.pose.position.x = x
    result.pose.pose.position.y = y
    result.pose.pose.position.z = z
    result.pose.pose.orientation.w = 1.0
    result.pose.covariance[0] = covariance
    result.pose.covariance[7] = covariance
    result.pose.covariance[14] = covariance
    detection.results.append(result)
    return detection


def _pose_array_binding(
    object_id: str,
    pose_index: int,
    *,
    apply_policy: str = "observe_only",
):
    from manyforge_ros.pose_source_sidecar import _PoseArrayObjectBinding
    from manyforge_ros.pose_source_sidecar import PoseSourceBinding

    return _PoseArrayObjectBinding(
        binding=PoseSourceBinding(
            object_id=object_id,
            entity=f"/tracked_object_poses[{pose_index}]",
            apply_policy=apply_policy,
            frame_id="world",
        ),
        pose_index=pose_index,
    )


def _pose(x: float, y: float, z: float):
    from geometry_msgs.msg import Pose

    pose = Pose()
    pose.position.x = x
    pose.position.y = y
    pose.position.z = z
    pose.orientation.w = 1.0
    return pose


def _publish_until(executor, publisher, msg, predicate, *, max_spins: int = 50) -> None:
    for _ in range(max_spins):
        publisher.publish(msg)
        executor.spin_once(timeout_sec=0.05)
        if predicate():
            return
    raise AssertionError("published condition was not reached before timeout")
