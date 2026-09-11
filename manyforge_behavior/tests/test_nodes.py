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
    assert (
        detect_boolean_edge(  # type: ignore[arg-type]
            previous,
            current,
            edge=edge,
        )
        is matched
    )


def test_pose_distance_math_is_finite_and_quaternion_sign_invariant() -> None:
    assert translation_distance_m((0, 0, 0), (3, 4, 0)) == 5.0
    assert (
        quaternion_angular_distance_rad(
            (0, 0, 0, 1),
            (0, 0, 0, -1),
        )
        == 0.0
    )
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
        (
            CheckKeyBoolValue,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (
            SetKeyBoolValue,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (
            WaitForKeyBool,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (
            WaitForSignalEdge,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (
            CheckGripperState,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (
            CommandGripper,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (
            ConfirmGripperGrasp,
            LogicNodeBindingUnavailableError,
            "logic_node_binding_unavailable",
        ),
        (MoveManipulatorAction, MotionNodeUnavailableError, "motion_node_unavailable"),
        (ResetTrajectories, MotionNodeUnavailableError, "motion_node_unavailable"),
        (
            ResetSimulation,
            SimulationNodeUnavailableError,
            "simulation_node_unavailable",
        ),
        (
            SetSimulationState,
            SimulationNodeUnavailableError,
            "simulation_node_unavailable",
        ),
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
    state = OperatorMotionStopState({"motion": ()})
    node = OperatorMotionStopPolicy(
        child=child,
        node_instance_id="motion",
        stop_state=state,
    )
    list(node.tick())
    assert child.ticks == 1

    state.publish(OperatorMotionStopSnapshot(1, frozenset({"motion"})))
    list(node.tick())
    assert child.ticks == 1
    assert child.observations == 1
    assert node.status is py_trees.common.Status.RUNNING

    state.publish(OperatorMotionStopSnapshot(2))
    list(node.tick())
    assert child.ticks == 2


def test_operator_stop_projection_keeps_only_current_node_holds() -> None:
    state = OperatorMotionStopState({"a": (), "b": (), "root": ("a", "b")})
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"a", "b", "root"})))
    assert state.holds_subtree("root")
    state.publish(OperatorMotionStopSnapshot(2, frozenset({"b", "root"})))
    assert state.holds_subtree("b")
    assert not state.holds_subtree("a")
    assert not state.holds_subtree("root")
    # A late older read cannot clear a newer Stop or restore an older hold.
    state.publish(OperatorMotionStopSnapshot(1))
    assert state.snapshot() == OperatorMotionStopSnapshot(2, frozenset({"b", "root"}))
    with pytest.raises(ValueError, match="without a revision"):
        state.publish(OperatorMotionStopSnapshot(2))
    with pytest.raises(ValueError, match="captured scope"):
        state.publish(OperatorMotionStopSnapshot(3, frozenset({"foreign"})))
    state.publish(OperatorMotionStopSnapshot(3))
    assert not state.holds_subtree("b")


@pytest.mark.parametrize("result", [py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE])
@pytest.mark.parametrize("observed_before_tick", [False, True])
def test_held_transaction_completes_visibly_without_starting_or_replaying_actions(
    result, observed_before_tick
):
    class Transaction(py_trees.behaviour.Behaviour):
        def __init__(self, name):
            super().__init__(name)
            self.starts = 0
            self.complete = False

        def initialise(self):
            self.starts += 1

        def update(self):
            return py_trees.common.Status.RUNNING

        def observe_while_operator_stop_held(self):
            if self.complete and self.status is py_trees.common.Status.RUNNING:
                self.stop(result)

    state = OperatorMotionStopState({"a": (), "b": (), "root": ("a", "b")})
    a, b = Transaction("a"), Transaction("b")
    leaves = [
        OperatorMotionStopPolicy(child=leaf, node_instance_id=leaf.name, stop_state=state)
        for leaf in (a, b)
    ]
    composite = (
        py_trees.composites.Sequence
        if result is py_trees.common.Status.SUCCESS
        else py_trees.composites.Selector
    )(name="branch", memory=True, children=leaves)
    root = OperatorMotionStopPolicy(
        child=composite, node_instance_id="root", stop_state=state, drain_children=True
    )
    list(root.tick())
    assert (a.starts, b.starts) == (1, 0)
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"a", "b", "root"})))
    a.complete = True
    if observed_before_tick:
        a.observe_while_operator_stop_held()
    list(root.tick())
    assert leaves[0].presented_status is result
    assert leaves[1].presented_status is py_trees.common.Status.INVALID
    assert root.status is py_trees.common.Status.RUNNING
    list(root.tick())
    assert (a.starts, b.starts) == (1, 0)
    state.publish(OperatorMotionStopSnapshot(2))
    list(root.tick())
    assert (a.starts, b.starts) == (1, 1)


def test_held_repeat_consumes_each_completed_action_only_once():
    class Transaction(py_trees.behaviour.Behaviour):
        def __init__(self):
            super().__init__("action")
            self.starts = 0

        def initialise(self):
            self.starts += 1

        def update(self):
            return py_trees.common.Status.RUNNING

        def observe_while_operator_stop_held(self):
            if self.status is py_trees.common.Status.RUNNING:
                self.stop(py_trees.common.Status.SUCCESS)

    state = OperatorMotionStopState({"action": (), "repeat": ("action",)})
    action = Transaction()
    leaf = OperatorMotionStopPolicy(child=action, node_instance_id="action", stop_state=state)
    repeat = py_trees.decorators.Repeat(name="repeat", child=leaf, num_success=2)
    root = OperatorMotionStopPolicy(
        child=repeat, node_instance_id="repeat", stop_state=state, drain_children=True
    )
    list(root.tick())
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"action", "repeat"})))
    for _ in range(3):
        list(root.tick())
        assert action.starts == 1
        assert leaf.presented_status is py_trees.common.Status.SUCCESS
        assert root.status is py_trees.common.Status.RUNNING
    state.publish(OperatorMotionStopSnapshot(2))
    list(root.tick())
    assert action.starts == 2
    state.publish(OperatorMotionStopSnapshot(3, frozenset({"action", "repeat"})))
    list(root.tick())
    assert root.status is py_trees.common.Status.SUCCESS


def test_stop_settlement_changes_wake_once_without_reopening_a_hold():
    state = OperatorMotionStopState({"a": (), "b": ()})
    held = frozenset({"a", "b"})
    stopping = OperatorMotionStopSnapshot(1, held, held)
    state.publish(stopping)
    assert state.take_observation_change()
    state.publish(OperatorMotionStopSnapshot(1, held, frozenset({"b"})))
    assert state.take_observation_change()
    state.publish(stopping)  # Delayed same-revision phase observation.
    assert not state.take_observation_change()
    assert state.snapshot().stopping_nodes == frozenset({"b"})
    state.publish(OperatorMotionStopSnapshot(1, held, frozenset(), frozenset({"b"})))
    assert state.take_observation_change()
    state.publish(OperatorMotionStopSnapshot(1, held))
    assert state.snapshot().faulted_nodes == frozenset({"b"})
    assert not state.take_observation_change()


def test_late_failed_stop_observation_cannot_poison_a_newer_settled_view():
    settled = OperatorMotionStopSnapshot(2, frozenset({"a"}))

    def crossed_read():
        state.publish(settled)
        raise RuntimeError("older read failed")

    state = OperatorMotionStopState({"a": ()}, read_scope=crossed_read)
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"a"}), frozenset({"a"})))
    with pytest.raises(RuntimeError, match="older read failed"):
        state.refresh()
    assert state.snapshot() == settled
    assert state.observation_error() is None


def test_command_publication_does_not_duplicate_or_erase_an_observation_wake():
    state = OperatorMotionStopState({"a": ()})
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"a"})), notify_observation=False)
    assert not state.take_observation_change()
    state.note_task_observation_change()
    state.publish(OperatorMotionStopSnapshot(2), notify_observation=False)
    assert state.take_observation_change()
    assert not state.take_observation_change()


def test_operator_stop_holds_only_the_authored_branch_while_parallel_continue_ticks() -> None:
    class CountingNode(py_trees.behaviour.Behaviour):
        def __init__(self, name: str) -> None:
            super().__init__(name=name)
            self.ticks = 0

        def update(self) -> py_trees.common.Status:
            self.ticks += 1
            return py_trees.common.Status.RUNNING

    held_child = CountingNode("held-child")
    continue_child = CountingNode("continue-child")
    state = OperatorMotionStopState({"held": (), "continue": (), "root": ("held", "continue")})
    held = OperatorMotionStopPolicy(
        child=held_child,
        node_instance_id="held",
        stop_state=state,
    )
    parallel = py_trees.composites.Parallel(
        name="root",
        policy=py_trees.common.ParallelPolicy.SuccessOnAll(synchronise=False),
        children=[held, continue_child],
    )

    list(parallel.tick())
    assert held_child.ticks == 1
    assert continue_child.ticks == 1

    state.publish(OperatorMotionStopSnapshot(1, frozenset({"held"})))
    list(parallel.tick())

    assert held_child.ticks == 1
    assert held.status is py_trees.common.Status.RUNNING
    assert continue_child.ticks == 2


@pytest.mark.parametrize("composite", ["parallel", "sequence"])
def test_operator_stop_mixed_ancestor_preserves_independent_work_and_sequence_order(
    composite,
) -> None:
    class CountingNode(py_trees.behaviour.Behaviour):
        def __init__(self, name):
            super().__init__(name=name)
            self.ticks = 0

        def update(self):
            self.ticks += 1
            return py_trees.common.Status.RUNNING

    state = OperatorMotionStopState({"a": (), "b": (), "root": ("a", "b")})
    a, b = CountingNode("a"), CountingNode("b")
    children = [
        OperatorMotionStopPolicy(child=node, node_instance_id=node.name, stop_state=state)
        for node in (a, b)
    ]
    branch = (
        py_trees.composites.Parallel(
            name="root",
            policy=py_trees.common.ParallelPolicy.SuccessOnAll(),
            children=children,
        )
        if composite == "parallel"
        else py_trees.composites.Sequence(name="root", memory=True, children=children)
    )
    root = OperatorMotionStopPolicy(child=branch, node_instance_id="root", stop_state=state)
    # Includes an idle/never-started node. A held mixed parent must not freeze
    # B in Parallel, or skip A to reach B in Sequence.
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"a", "root"})))
    list(root.tick())
    assert a.ticks == 0
    assert b.ticks == (1 if composite == "parallel" else 0)
    assert root.status is py_trees.common.Status.RUNNING
    state.publish(OperatorMotionStopSnapshot(2, frozenset({"root"})))
    list(root.tick())
    assert a.ticks == 1
    assert b.ticks == (2 if composite == "parallel" else 0)
