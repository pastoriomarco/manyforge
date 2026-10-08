from __future__ import annotations

from collections.abc import Callable
import threading
from types import SimpleNamespace

import pytest
import rclpy
from rosgraph_msgs.msg import Clock
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.parameter import Parameter
from sensor_msgs.msg import JointState
from simulation_interfaces.msg import SimulationState, SimulatorFeatures
from simulation_interfaces.srv import (
    GetCurrentWorld,
    GetSimulationState,
    GetSimulatorFeatures,
    LoadWorld,
    SetSimulationState,
)

from manyforge_ros_bringup.isaac_scene_load_barrier import (
    IsaacSceneLoadBarrier,
    normalize_file_world_uri,
    select_service_name,
    world_readback_matches,
)


WORLD_PATH = "/opt/isaac/worlds/retained scene.usda"
WORLD_URI = "file:/opt/isaac/worlds/retained%20scene.usda"


def test_world_uri_normalization_is_exact_and_host_neutral() -> None:
    assert normalize_file_world_uri(WORLD_PATH) == (
        "file:/opt/isaac/worlds/retained scene.usda"
    )
    assert normalize_file_world_uri(WORLD_URI) == (
        "file:/opt/isaac/worlds/retained scene.usda"
    )
    assert normalize_file_world_uri("file://localhost" + WORLD_PATH) == (
        "file:/opt/isaac/worlds/retained scene.usda"
    )


@pytest.mark.parametrize(
    "value",
    [
        "",
        "relative.usda",
        "https://example.invalid/world.usda",
        "file://remote/world.usda",
        "file:/world.usda?query=true",
        "file:/world.usda#fragment",
        "file:/world.txt",
        "file:/",
        "file:/opt/../world.usda",
        "file:/world\x00.usda",
    ],
)
def test_world_uri_normalization_rejects_ambiguous_inputs(value: str) -> None:
    with pytest.raises(ValueError):
        normalize_file_world_uri(value)


def test_world_readback_requires_both_uri_and_name() -> None:
    exact = SimpleNamespace(
        name="retained_world",
        world_resource=SimpleNamespace(uri=WORLD_PATH),
    )

    assert world_readback_matches(
        exact,
        expected_uri=WORLD_URI,
        expected_name="retained_world",
    )
    assert not world_readback_matches(
        exact,
        expected_uri="file:/opt/isaac/worlds/other.usda",
        expected_name="retained_world",
    )
    assert not world_readback_matches(
        exact,
        expected_uri=WORLD_URI,
        expected_name="another_world",
    )
    assert not world_readback_matches(
        object(),
        expected_uri=WORLD_URI,
        expected_name="retained_world",
    )


def test_typed_service_selection_is_deterministic_and_bounded() -> None:
    service_type = "simulation_interfaces/srv/GetCurrentWorld"
    graph = [
        ("/z/current_world", [service_type]),
        ("/get_current_world", (service_type,)),
        ("/unrelated", ["example_interfaces/srv/Trigger"]),
    ]

    assert (
        select_service_name(graph, service_type=service_type)
        == "/get_current_world"
    )
    assert select_service_name([], service_type=service_type) is None
    assert select_service_name(None, service_type=service_type) is None
    assert select_service_name(
        [("/malformed", "not-a-type-list")], service_type=service_type
    ) is None


def test_result_validation_accepts_only_result_ok() -> None:
    ok = SimpleNamespace(result=1, RESULT_OK=1, error_message="")
    failed = SimpleNamespace(result=4, RESULT_OK=1, error_message="failed")

    IsaacSceneLoadBarrier._require_ok(ok, operation="test")
    with pytest.raises(RuntimeError, match="test failed: 4: failed"):
        IsaacSceneLoadBarrier._require_ok(failed, operation="test")


def test_scene_barrier_runs_against_typed_fake_simulator() -> None:
    rclpy.init()
    simulator: Node | None = None
    barrier: IsaacSceneLoadBarrier | None = None
    executor = SingleThreadedExecutor()
    worker: threading.Thread | None = None
    try:
        simulator = Node("manyforge_fake_simulation_interfaces_owner")
        publishers = []
        state_transitions: list[int] = []

        def publish_loaded_world_topics() -> None:
            if publishers:
                return
            for topic, message_type in (
                ("/clock", Clock),
                ("/isaac_joint_states", JointState),
                ("/isaac_joint_commands", JointState),
                ("/isaac_gripper_state", JointState),
            ):
                publishers.append(
                    simulator.create_publisher(message_type, topic, 1)
                )

        _install_fake_simulator(
            simulator,
            on_play=publish_loaded_world_topics,
            state_transitions=state_transitions,
        )
        executor.add_node(simulator)
        worker = threading.Thread(target=executor.spin, daemon=True)
        worker.start()

        barrier = IsaacSceneLoadBarrier(
            parameter_overrides=[
                Parameter("world_uri", value=WORLD_URI),
                Parameter("expected_world_name", value="retained_world"),
                Parameter("startup_timeout_s", value=2.0),
                Parameter("service_timeout_s", value=1.0),
                Parameter(
                    "required_topics",
                    value=[
                        "/clock",
                        "/isaac_joint_states",
                        "/isaac_joint_commands",
                        "/isaac_gripper_state",
                    ],
                ),
            ]
        )
        result = barrier.run()

        assert result["world_name"] == "retained_world"
        assert result["world_uri"] == (
            "file:/opt/isaac/worlds/retained scene.usda"
        )
        assert result["simulation_state"] == "stopped"
        assert result["readiness_probe"] == {
            "mode": "playing_then_stopped",
            "observed_topics": [
                "/clock",
                "/isaac_gripper_state",
                "/isaac_joint_commands",
                "/isaac_joint_states",
            ],
        }
        assert state_transitions == [
            int(SimulationState.STATE_PLAYING),
            int(SimulationState.STATE_STOPPED),
        ]
        assert set(result["services"]) == {
            "features",
            "load_world",
            "current_world",
            "get_state",
            "set_state",
        }
    finally:
        if barrier is not None:
            barrier.destroy_node()
        executor.shutdown()
        if worker is not None:
            worker.join(timeout=2.0)
        if simulator is not None:
            simulator.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


def test_scene_barrier_stops_after_playing_readiness_timeout() -> None:
    rclpy.init()
    simulator: Node | None = None
    barrier: IsaacSceneLoadBarrier | None = None
    executor = SingleThreadedExecutor()
    worker: threading.Thread | None = None
    try:
        simulator = Node("manyforge_fake_timeout_simulation_interfaces_owner")
        state_transitions: list[int] = []
        _install_fake_simulator(
            simulator,
            state_transitions=state_transitions,
        )
        executor.add_node(simulator)
        worker = threading.Thread(target=executor.spin, daemon=True)
        worker.start()

        barrier = IsaacSceneLoadBarrier(
            parameter_overrides=[
                Parameter("world_uri", value=WORLD_URI),
                Parameter("expected_world_name", value="retained_world"),
                Parameter("startup_timeout_s", value=0.3),
                Parameter("service_timeout_s", value=0.1),
                Parameter("required_topics", value=["/never_materializes"]),
            ]
        )
        with pytest.raises(
            TimeoutError,
            match="missing topics=.*never_materializes",
        ):
            barrier.run()

        assert state_transitions == [
            int(SimulationState.STATE_PLAYING),
            int(SimulationState.STATE_STOPPED),
        ]
    finally:
        if barrier is not None:
            barrier.destroy_node()
        executor.shutdown()
        if worker is not None:
            worker.join(timeout=2.0)
        if simulator is not None:
            simulator.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


@pytest.mark.parametrize(
    ("world_uri", "world_name", "initial_state", "required_topic", "play_result", "failure"),
    [
        (WORLD_PATH, "retained_world", SimulationState.STATE_STOPPED, "/clock", 1, None),
        ("file:/other.usda", "retained_world", SimulationState.STATE_STOPPED, "/clock", 1, "required existing world"),
        (WORLD_PATH, "another_world", SimulationState.STATE_STOPPED, "/clock", 1, "required existing world"),
        (WORLD_PATH, "retained_world", SimulationState.STATE_PLAYING, "/clock", 1, "not stopped"),
        (WORLD_PATH, "retained_world", SimulationState.STATE_STOPPED, "/never_materializes", 1, "missing topics"),
        (WORLD_PATH, "retained_world", SimulationState.STATE_STOPPED, "/clock", 4, "playing readiness probe.*failed"),
    ],
)
def test_existing_world_join_never_loads_and_contains_only_its_own_playback(
    world_uri: str,
    world_name: str,
    initial_state: int,
    required_topic: str,
    play_result: int,
    failure: str | None,
) -> None:
    rclpy.init()
    simulator = Node("manyforge_fake_existing_world_owner")
    executor = SingleThreadedExecutor()
    load_requests: list[bool] = []
    transitions: list[int] = []
    publishers = []
    _install_fake_simulator(
        simulator,
        on_load=lambda: load_requests.append(True),
        on_play=lambda: publishers.append(simulator.create_publisher(Clock, "/clock", 1)),
        state_transitions=transitions,
        world_uri=world_uri,
        world_name=world_name,
        initial_state=initial_state,
        play_result=play_result,
    )
    executor.add_node(simulator)
    worker = threading.Thread(target=executor.spin, daemon=True)
    worker.start()
    barrier = IsaacSceneLoadBarrier(
        parameter_overrides=[
            Parameter("world_uri", value=WORLD_URI),
            Parameter("expected_world_name", value="retained_world"),
            Parameter("require_existing_world", value=True),
            Parameter("startup_timeout_s", value=1.0),
            Parameter("service_timeout_s", value=0.5),
            Parameter("required_topics", value=[required_topic]),
        ]
    )
    try:
        if failure is None:
            assert barrier.run()["simulation_state"] == "stopped"
        else:
            with pytest.raises((RuntimeError, TimeoutError), match=failure):
                barrier.run()
        assert load_requests == []
        assert transitions == (
            [int(SimulationState.STATE_PLAYING), int(SimulationState.STATE_STOPPED)]
            if failure is None or required_topic == "/never_materializes" or play_result != 1
            else []
        )
    finally:
        barrier.destroy_node()
        executor.shutdown()
        worker.join(timeout=2.0)
        simulator.destroy_node()
        rclpy.shutdown()


def _install_fake_simulator(
    node: Node,
    *,
    on_load: Callable[[], None] | None = None,
    on_play: Callable[[], None] | None = None,
    state_transitions: list[int] | None = None,
    world_uri: str = "file:/opt/isaac/worlds/retained scene.usda",
    world_name: str = "retained_world",
    initial_state: int = SimulationState.STATE_STOPPED,
    play_result: int = 1,
) -> None:
    feature_values = [
        int(SimulatorFeatures.WORLD_LOADING),
        int(SimulatorFeatures.WORLD_INFO_GETTING),
        int(SimulatorFeatures.SIMULATION_STATE_GETTING),
        int(SimulatorFeatures.SIMULATION_STATE_SETTING),
    ]

    def features(_request, response):
        response.features.features = feature_values
        return response

    def populate_world(response):
        response.result.result = response.result.RESULT_OK
        response.world.name = world_name
        response.world.world_resource.uri = world_uri
        return response

    current_state = {"value": int(initial_state)}

    def state(_request, response):
        response.result.result = response.result.RESULT_OK
        response.state.state = current_state["value"]
        return response

    def set_state(request, response):
        target = int(request.state.state)
        current_state["value"] = target
        if state_transitions is not None:
            state_transitions.append(target)
        if target == int(SimulationState.STATE_PLAYING) and on_play is not None:
            on_play()
        response.result.result = response.result.RESULT_OK
        if target == int(SimulationState.STATE_PLAYING):
            response.result.result = play_result
        return response

    def load_world(_request, response):
        if on_load is not None:
            on_load()
        return populate_world(response)

    node.create_service(
        GetSimulatorFeatures,
        "/get_simulator_features",
        features,
    )
    node.create_service(LoadWorld, "/load_world", load_world)
    node.create_service(
        GetCurrentWorld,
        "/get_current_world",
        lambda _r, value: populate_world(value),
    )
    node.create_service(GetSimulationState, "/get_simulation_state", state)
    node.create_service(SetSimulationState, "/set_simulation_state", set_state)
