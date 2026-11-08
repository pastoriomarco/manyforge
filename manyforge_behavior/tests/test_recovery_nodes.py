"""Cutover gates for recovery-authoring behavior nodes."""

import pytest
import py_trees

from manyforge_behavior.nodes.recovery import (
    FallbackOverrideNode,
    RecoveryNodeUnavailableError,
    RecoverySkillNode,
)
from manyforge_behavior.program import ProgramNodePolicy
from manyforge_behavior.program_runtime import ProgramRunAttemptSource
from manyforge_behavior.program_scheduling import ProgramInterventionRegistry
from manyforge_behavior.recovery import (
    RecoveryAwareNode,
    recovery_policy_is_actionable,
    recovery_retry_permitted,
)


@pytest.mark.parametrize(
    "policy,used,expected",
    [
        ({}, 0, False),
        ({"allow_intervention": False, "max_retries": 0}, 0, False),
        # A positive budget never enables a forbidden intervention.
        ({"allow_intervention": False, "max_retries": 5}, 0, False),
        ({"allow_intervention": True, "max_retries": 2}, 0, True),
        ({"allow_intervention": True, "max_retries": 2}, 1, True),
        ({"allow_intervention": True, "max_retries": 2}, 2, False),
        # Zero is Unlimited, including far beyond the former 255 bound.
        ({"allow_intervention": True, "max_retries": 0}, 0, True),
        ({"allow_intervention": True, "max_retries": 0}, 256, True),
        ({"allow_intervention": True, "max_retries": 0}, 2**53 - 1, True),
    ],
)
def test_recovery_retry_predicate_is_one_source_independent_budget(
    policy, used, expected
) -> None:
    assert recovery_retry_permitted(policy, retries_used=used) is expected


def test_recovery_predicate_rejects_inexact_inputs() -> None:
    with pytest.raises(ValueError):
        recovery_retry_permitted({"allow_intervention": True}, retries_used=-1)
    with pytest.raises(ValueError):
        recovery_retry_permitted(
            {"allow_intervention": True, "max_retries": True}, retries_used=0
        )


@pytest.mark.parametrize(
    "recovery,held",
    [
        ({"allow_intervention": True, "max_retries": 0}, True),
        ({"allow_intervention": True, "max_retries": 1}, True),
        ({"allow_intervention": False, "max_retries": 3}, False),
    ],
)
def test_only_an_allowed_intervention_holds_a_failed_attempt(recovery, held) -> None:
    policy = ProgramNodePolicy(
        instance_id="move", recovery_policy=recovery, operator_stop_policy="inherit"
    )
    assert recovery_policy_is_actionable(policy) is held


@pytest.mark.parametrize("action", ["retry", "manual_success"])
def test_parallel_recovery_never_reinvokes_an_unresolved_sibling(action):
    attempts = ProgramRunAttemptSource("run")
    registry = ProgramInterventionRegistry(maximum_retained_runs=1)
    registry.register_run("run", ["a", "b"])

    class FailingOwner(py_trees.behaviour.Behaviour):
        def __init__(self, name):
            super().__init__(name)
            self.calls = 0
            self.attempt_identity = None

        def initialise(self):
            self.attempt_identity = attempts.begin(self.name)

        def update(self):
            self.calls += 1
            return py_trees.common.Status.FAILURE

    children = [FailingOwner("a"), FailingOwner("b")]
    held = [RecoveryAwareNode(
        child=child, node_instance_id=child.name, attempt_source=attempts,
        recovery_port=registry,
        policy=ProgramNodePolicy(
            instance_id=child.name, operator_stop_policy="hold",
            recovery_policy={"allow_intervention": True, "max_retries": 0},
        ),
    ) for child in children]
    root = py_trees.composites.Parallel(
        name="parallel", children=held,
        policy=py_trees.common.ParallelPolicy.SuccessOnAll(synchronise=True),
    )
    list(root.tick())
    first, second = registry.read_all("run")
    assert [child.calls for child in children] == [1, 1]
    registry.resolve(first.identity, action=action)
    list(root.tick())
    assert [child.calls for child in children] == [1, 1]
    assert children[1].attempt_identity == second.identity
    assert registry.read_all("run") == (second,)
    if action == "manual_success":
        assert held[0].presented_status is py_trees.common.Status.SUCCESS
        held[0].observe_while_operator_stop_held()
        assert held[0].status is py_trees.common.Status.SUCCESS
        assert registry.read_all("run") == (second,)
    list(root.tick())
    assert children[0].calls == (2 if action == "retry" else 1)
    assert children[1].calls == 1
    assert children[1].attempt_identity == second.identity


@pytest.mark.parametrize("node_type", [FallbackOverrideNode, RecoverySkillNode])
def test_recovery_nodes_fail_before_child_or_offer_access(node_type: type) -> None:
    class PoisonInput:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"recovery input was accessed: {name}")

    poison = PoisonInput()
    with pytest.raises(
        RecoveryNodeUnavailableError,
        match="composable_cell_recovery_node_unavailable",
    ):
        node_type(poison, child=poison, offer=poison)
