"""Retained Kernel values and the native-binding cutover gate."""

import pytest

from manyforge_behavior.kernel_client import (
    KERNEL_CLIENT_RESTORATION_GATES,
    ActionSpan,
    ExecutionState,
    KernelClient,
    KernelClientBindingUnavailableError,
    MockKernelClient,
)


def test_execution_state_vocabulary_is_closed() -> None:
    assert {state.value for state in ExecutionState} == {
        "idle",
        "planning",
        "validating",
        "executing",
        "interrupted",
        "paused",
        "stopping",
        "stopped",
        "completed",
        "failed",
        "intervention_required",
    }


def test_program_action_span_requires_exact_attempt_identity() -> None:
    span = ActionSpan(
        action_source="program_node",
        action_span_id="run/node/attempt-1",
        program_run_id="run-1",
        node_instance_id="root/node[0]",
        node_attempt_id="run/node/attempt-1",
    )
    assert span.action_span_id == span.node_attempt_id
    with pytest.raises(ValueError, match="must equal"):
        ActionSpan(
            action_source="program_node",
            action_span_id="span-1",
            node_instance_id="root/node[0]",
            node_attempt_id="attempt-2",
        )


@pytest.mark.parametrize("client_type", [KernelClient, MockKernelClient])
def test_legacy_clients_fail_before_collaborator_access(client_type: type) -> None:
    class PoisonCollaborator:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"Kernel collaborator was accessed: {name}")

    poison = PoisonCollaborator()
    with pytest.raises(
        KernelClientBindingUnavailableError,
        match="composable_cell_kernel_client_binding_unavailable",
    ):
        client_type(poison, backend=poison)

    assert KERNEL_CLIENT_RESTORATION_GATES == ("I6", "N", "I8", "P5")
