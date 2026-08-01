"""Cutover gates for recovery-authoring behavior nodes."""

import pytest

from manyforge_behavior.nodes.recovery import (
    RECOVERY_NODE_RESTORATION_GATES,
    FallbackOverrideNode,
    RecoveryNodeUnavailableError,
    RecoverySkillNode,
)


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
