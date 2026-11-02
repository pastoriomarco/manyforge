"""Typed fake-owner qualification for the restored simulator control client."""

from __future__ import annotations

import math
from types import SimpleNamespace

import pytest
from simulation_interfaces.msg import SimulationState
from simulation_interfaces.srv import (
    GetSimulationState,
    ResetSimulation,
    SetEntityState,
    SetSimulationState,
    StepSimulation,
)

from manyforge_ros import simulation_control


class _Future:
    def __init__(self, result: object) -> None:
        self._result = result

    def done(self) -> bool:
        return True

    def result(self) -> object:
        return self._result


class _Client:
    def __init__(self, handler) -> None:
        self._handler = handler

    def wait_for_service(self, *, timeout_sec: float) -> bool:
        return timeout_sec > 0.0

    def call_async(self, request: object) -> _Future:
        return _Future(self._handler(request))


class _Node:
    def __init__(self, handlers: dict[str, object]) -> None:
        self.handlers = handlers
        self.executor = SimpleNamespace(is_spinning=True)
        self.created: list[str] = []
        self.destroyed = 0

    def get_service_names_and_types(self):
        return [
            ("/reset_simulation", [simulation_control.RESET_SERVICE_TYPE]),
            (
                "/get_simulation_state",
                [simulation_control.GET_SIMULATION_STATE_SERVICE_TYPE],
            ),
            (
                "/set_simulation_state",
                [simulation_control.SET_SIMULATION_STATE_SERVICE_TYPE],
            ),
            ("/step_simulation", [simulation_control.STEP_SIMULATION_SERVICE_TYPE]),
            (
                "/set_entity_state",
                [simulation_control.SET_ENTITY_STATE_SERVICE_TYPE],
            ),
        ]

    def create_client(self, _service_type: type, service_name: str) -> _Client:
        self.created.append(service_name)
        return _Client(self.handlers[service_name])

    def destroy_client(self, _client: _Client) -> None:
        self.destroyed += 1


def _ok_response(service_type: type) -> object:
    response = service_type.Response()
    response.result.result = response.result.RESULT_OK
    return response


def test_simulation_control_runs_all_selected_typed_operations() -> None:
    observed: dict[str, object] = {}

    def reset(request):
        observed["reset_scope"] = request.scope
        return _ok_response(ResetSimulation)

    def get_state(_request):
        response = _ok_response(GetSimulationState)
        response.state.state = int(SimulationState.STATE_STOPPED)
        return response

    def set_state(request):
        observed["target_state"] = request.state.state
        return _ok_response(SetSimulationState)

    def step(request):
        observed["steps"] = request.steps
        return _ok_response(StepSimulation)

    def set_entity(request):
        observed["entity_request"] = request
        return _ok_response(SetEntityState)

    node = _Node(
        {
            "/reset_simulation": reset,
            "/get_simulation_state": get_state,
            "/set_simulation_state": set_state,
            "/step_simulation": step,
            "/set_entity_state": set_entity,
        }
    )
    client = simulation_control.SimulationControlClient(
        node,
        service_timeout_s=0.5,
    )

    assert client.reset().success
    state = client.get_simulation_state()
    assert state.success and state.state == "stopped"
    assert client.set_simulation_state(state="play").success
    assert client.step(steps=3).success
    entity = client.set_entity_state(
        entity="/World/soup_can",
        pose={
            "frame_id": "world",
            "position_m": [1.0, 2.0, 3.0],
            "orientation_rpy_deg": [0.0, 0.0, 90.0],
        },
    )
    assert entity.success and entity.entity == "/World/soup_can"

    assert observed["reset_scope"] == int(ResetSimulation.Request.SCOPE_STATE)
    assert observed["target_state"] == int(SimulationState.STATE_PLAYING)
    assert observed["steps"] == 3
    request = observed["entity_request"]
    assert request.entity == "/World/soup_can"
    assert request.state.header.frame_id == "world"
    assert request.state.pose.position.x == 1.0
    assert request.state.pose.orientation.z == pytest.approx(math.sqrt(0.5))
    assert node.destroyed == len(node.created) == 5
    assert not hasattr(node, "create_publisher")


def test_simulation_control_surfaces_typed_owner_failure() -> None:
    def failed(_request):
        response = ResetSimulation.Response()
        response.result.result = response.result.RESULT_OPERATION_FAILED
        response.result.error_message = "reset rejected"
        return response

    node = _Node(
        {
            "/reset_simulation": failed,
            "/get_simulation_state": lambda _r: _ok_response(GetSimulationState),
            "/set_simulation_state": lambda _r: _ok_response(SetSimulationState),
            "/step_simulation": lambda _r: _ok_response(StepSimulation),
            "/set_entity_state": lambda _r: _ok_response(SetEntityState),
        }
    )

    result = simulation_control.SimulationControlClient(node).reset()

    assert not result.success
    assert result.result_code == 4
    assert result.result_message == "reset rejected"
    assert result.error == "4: reset rejected"


def test_service_selection_is_deterministic_and_prefix_bounded() -> None:
    assert simulation_control.select_simulation_service_name(
        ["/vendor/reset", "/reset_simulation"],
        service_type=simulation_control.RESET_SERVICE_TYPE,
    ) == "/reset_simulation"
    assert simulation_control.select_simulation_service_name(
        ["/other/reset", "/sim_a/reset_simulation"],
        service_type=simulation_control.RESET_SERVICE_TYPE,
        service_prefix="/sim_a/",
    ) == "/sim_a/reset_simulation"
    assert (
        simulation_control.select_simulation_service_name(
            ["/other/reset"],
            service_type=simulation_control.RESET_SERVICE_TYPE,
            service_prefix="sim_a",
        )
        is None
    )
    with pytest.raises(ValueError, match="candidates exceed"):
        simulation_control.select_simulation_service_name(
            [f"/service/{index}" for index in range(4097)],
            service_type=simulation_control.RESET_SERVICE_TYPE,
        )
    with pytest.raises(ValueError, match="candidates must be unique"):
        simulation_control.select_simulation_service_name(
            ["/reset_simulation", " /reset_simulation "],
            service_type=simulation_control.RESET_SERVICE_TYPE,
        )


def test_control_service_graph_type_rows_are_bounded() -> None:
    node = _Node({})
    node.get_service_names_and_types = lambda: [
        ("/reset_simulation", [simulation_control.RESET_SERVICE_TYPE] * 4097)
    ]
    client = simulation_control.SimulationControlClient(
        node,
        service_timeout_s=0.05,
    )

    with pytest.raises(RuntimeError, match="service type rows exceed"):
        client.reset()


@pytest.mark.parametrize(
    ("raw", "expected"),
    [
        (" stop ", "stopped"),
        ("STOPPED", "stopped"),
        (" play ", "playing"),
        ("PLAYING", "playing"),
        ("pause", "paused"),
    ],
)
def test_simulation_state_normalization(raw: str, expected: str) -> None:
    assert simulation_control.normalize_simulation_state(raw) == expected


def test_reset_scope_and_entity_pose_normalization() -> None:
    assert simulation_control.normalize_reset_scope(" STATE ") == "state"
    frame_id, position, orientation = simulation_control.normalize_entity_pose(
        {
            "frame_id": " world ",
            "position_m": [1, 2.5, 3],
            "orientation_rpy_deg": [0.0, 0.0, 90.0],
        }
    )
    assert frame_id == "world"
    assert position == (1.0, 2.5, 3.0)
    assert orientation == pytest.approx(
        (0.0, 0.0, math.sqrt(0.5), math.sqrt(0.5))
    )
    _, _, direct = simulation_control.normalize_entity_pose(
        {
            "frame_id": "world",
            "position_m": [0.0, 0.0, 0.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 2.0],
        }
    )
    assert direct == (0.0, 0.0, 0.0, 1.0)


@pytest.mark.parametrize(
    ("pose", "message"),
    [
        ({}, "frame_id is required"),
        ({"frame_id": "world", "position_m": [1, 2]}, "must have length 3"),
        (
            {"frame_id": "world", "position_m": [1, 2, 3]},
            "must include orientation_xyzw or orientation_rpy_deg",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [True, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "text or boolean",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [float("inf"), 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "must be finite",
        ),
        (
            {
                "frame_id": "world",
                "position_m": [0.0, 0.0, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 0.0],
            },
            "finite nonzero norm",
        ),
    ],
)
def test_entity_pose_normalization_rejects_invalid_values(
    pose: dict[str, object],
    message: str,
) -> None:
    with pytest.raises(ValueError, match=message):
        simulation_control.normalize_entity_pose(pose)


@pytest.mark.parametrize("steps", [False, 0, -1, 1_000_001, 1.0, "1"])
def test_step_count_is_exact_and_bounded(steps: object) -> None:
    node = _Node({})
    client = simulation_control.SimulationControlClient(node)
    with pytest.raises(ValueError, match="steps must be"):
        client.step(steps=steps)  # type: ignore[arg-type]


def test_historical_live_cases_are_replaced_by_active_i9_coverage() -> None:
    assert simulation_control.HISTORICAL_SOURCE_BLOB == (
        "769e97215b0df885dd7002ca20ded0e0469b6ef7"
    )
    assert simulation_control.HISTORICAL_TEST_BLOB == (
        "1d9c3d6bb93f7ccc7f377ef7e151e91f338f890f"
    )
