"""Portable node algorithms and explicit legacy-node cutover gates."""

import math

import py_trees
import pytest

from manyforge_behavior.nodes.geometry import (
    CheckPoseDistance,
    GeometryNodeUnavailableError,
    GetLinkPose,
    quaternion_angular_distance_rad,
    translation_distance_m,
)
from manyforge_behavior.nodes.logic import (
    CheckGripperState,
    CheckKeyBoolValue,
    CommandGripper,
    ConfirmGripperGrasp,
    LogicNodeBindingUnavailableError,
    SetKeyBoolValue,
    WaitForKeyBool,
    WaitForSignalEdge,
    boolean_condition_matches,
    detect_boolean_edge,
)
from manyforge_behavior.nodes.motion import (
    MotionNodeUnavailableError,
    MoveManipulatorAction,
    ResetTrajectories,
)
from manyforge_behavior.nodes.simulation import (
    ResetSimulation,
    SetEntityState,
    SetSimulationState,
    SimulationNodeUnavailableError,
    StepSimulation,
)
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopPolicy,
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)


def test_boolean_condition_is_strict() -> None:
    assert boolean_condition_matches(True, expected=True)
    assert not boolean_condition_matches(1, expected=True)
    assert not boolean_condition_matches(False, expected=True)
    with pytest.raises(TypeError):
        boolean_condition_matches(True, expected=1)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("previous", "current", "edge", "matched"),
    [
        (False, True, "rising", True),
        (True, False, "rising", False),
        (True, False, "falling", True),
        (False, True, "either", True),
        (True, True, "either", False),
    ],
)
def test_boolean_edge_reducer(
    previous: bool,
    current: bool,
    edge: str,
    matched: bool,
) -> None:
    assert detect_boolean_edge(  # type: ignore[arg-type]
        previous,
        current,
        edge=edge,
    ) is matched


def test_pose_distance_math_is_finite_and_quaternion_sign_invariant() -> None:
    assert translation_distance_m((0, 0, 0), (3, 4, 0)) == 5.0
    assert quaternion_angular_distance_rad(
        (0, 0, 0, 1),
        (0, 0, 0, -1),
    ) == 0.0
    assert quaternion_angular_distance_rad(
        (0, 0, 0, 1),
        (0, 0, math.sin(math.pi / 4), math.cos(math.pi / 4)),
    ) == pytest.approx(math.pi / 2)
    with pytest.raises(ValueError, match="finite"):
        translation_distance_m((0, 0, math.nan), (0, 0, 0))
    with pytest.raises(ValueError, match="finite numbers"):
        translation_distance_m((0, 0, True), (0, 0, 0))
    with pytest.raises(ValueError, match="finite numbers"):
        translation_distance_m((0, 0, "1"), (0, 0, 0))
    with pytest.raises(ValueError, match="finite numeric range"):
        translation_distance_m((1e308, 0, 0), (-1e308, 0, 0))
    assert quaternion_angular_distance_rad(
        (1e308, 1e308, 1e308, 1e308),
        (1e308, 1e308, 1e308, 1e308),
    ) == pytest.approx(0.0)


@pytest.mark.parametrize(
    ("node_type", "error_type", "reason"),
    [
        (GetLinkPose, GeometryNodeUnavailableError, "geometry_node_unavailable"),
        (CheckPoseDistance, GeometryNodeUnavailableError, "geometry_node_unavailable"),
        (CheckKeyBoolValue, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (SetKeyBoolValue, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (WaitForKeyBool, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (WaitForSignalEdge, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (CheckGripperState, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (CommandGripper, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (ConfirmGripperGrasp, LogicNodeBindingUnavailableError, "logic_node_binding_unavailable"),
        (MoveManipulatorAction, MotionNodeUnavailableError, "motion_node_unavailable"),
        (ResetTrajectories, MotionNodeUnavailableError, "motion_node_unavailable"),
        (ResetSimulation, SimulationNodeUnavailableError, "simulation_node_unavailable"),
        (SetSimulationState, SimulationNodeUnavailableError, "simulation_node_unavailable"),
        (StepSimulation, SimulationNodeUnavailableError, "simulation_node_unavailable"),
        (SetEntityState, SimulationNodeUnavailableError, "simulation_node_unavailable"),
    ],
)
def test_effectful_nodes_fail_before_inputs(
    node_type: type,
    error_type: type[RuntimeError],
    reason: str,
) -> None:
    class PoisonInput:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"node input was accessed: {name}")

    poison = PoisonInput()
    with pytest.raises(error_type, match=reason):
        node_type(poison, kernel=poison, blackboard=poison)


def test_operator_stop_policy_holds_without_ticking_and_resumes_exact_run() -> None:
    class CountingNode(py_trees.behaviour.Behaviour):
        def __init__(self) -> None:
            super().__init__(name="counting")
            self.ticks = 0
            self.observations = 0

        def update(self) -> py_trees.common.Status:
            self.ticks += 1
            return py_trees.common.Status.RUNNING

        def observe_while_operator_stop_held(self) -> None:
            self.observations += 1

    child = CountingNode()
    state = OperatorMotionStopState()
    node = OperatorMotionStopPolicy(
        child=child,
        node_instance_id="motion",
        stop_state=state,
    )
    list(node.tick())
    assert child.ticks == 1

    state.hold(
        "task.1",
        {"active_stop_id": "stop.1", "active_stop_generation": 1},
    )
    list(node.tick())
    assert child.ticks == 1
    assert child.observations == 1
    assert node.status is py_trees.common.Status.RUNNING

    state.resolve("task.1")
    list(node.tick())
    assert child.ticks == 2


def test_operator_stop_state_retains_parallel_attempts_until_each_is_resolved() -> None:
    state = OperatorMotionStopState()
    metadata = {"active_stop_id": "stop.1", "active_stop_generation": 1}

    state.hold("task.1", metadata)
    state.hold("task.2", metadata)
    state.resolve("task.1")

    assert state.snapshot() == OperatorMotionStopSnapshot(
        stop_id="stop.1",
        stop_generation=1,
        held=True,
    )

    state.resolve("task.2")
    assert state.snapshot() == OperatorMotionStopSnapshot()
