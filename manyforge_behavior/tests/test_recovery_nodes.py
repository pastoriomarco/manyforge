"""Cutover gates for recovery-authoring behavior nodes."""

import pytest
import py_trees

from manyforge_behavior.nodes.recovery import (
    RECOVERY_NODE_RESTORATION_GATES,
    FallbackOverrideNode,
    RecoveryNodeUnavailableError,
    RecoverySkillNode,
)
from manyforge_behavior.program import ProgramNodePolicy
from manyforge_behavior.program_runtime import ProgramRunAttemptSource
from manyforge_behavior.program_scheduling import ProgramInterventionRegistry
from manyforge_behavior.recovery import RecoveryAwareNode


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
            modification_policy={"runtime_override": "forbidden",
                                 "persistent_edit": "forbidden",
                                 "intervention_delegate": "human_only"},
            recovery_policy={"allow_intervention": True,
                             "allowed_recovery_actions": ["intervention"],
                             "max_retries": 0, "allowed_skills": [],
                             "persistence_ceiling": "session_only"},
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

    assert RECOVERY_NODE_RESTORATION_GATES == ("I6", "N", "I8", "P5")
