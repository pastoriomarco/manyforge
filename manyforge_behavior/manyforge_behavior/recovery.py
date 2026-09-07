"""Small policy-aware hold around an existing executable Program node."""

from __future__ import annotations

from collections.abc import Iterator
from typing import Any

import py_trees

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    NodeAttemptSource,
    ProgramInterventionPort,
)
from manyforge_behavior.program import ProgramNodePolicy


class RecoveryAwareNode(py_trees.decorators.Decorator):
    """Hold a failed attempt until the Program Runner resolves it.

    This decorator changes no child semantics.  It merely prevents a
    policy-addressable failure from terminalizing the whole run while the
    existing run owner publishes and resolves one exact attempt.
    """

    def __init__(
        self,
        *,
        child: py_trees.behaviour.Behaviour,
        node_instance_id: str,
        policy: ProgramNodePolicy,
        attempt_source: NodeAttemptSource,
        recovery_port: ProgramInterventionPort,
    ) -> None:
        super().__init__(name=f"recovery:{node_instance_id}", child=child)
        self._node_instance_id = node_instance_id
        self._policy = policy
        self._attempt_source = attempt_source
        self._recovery_port = recovery_port
        self._identity: NodeAttemptIdentity | None = None
        self._held = False

    def initialise(self) -> None:
        self._identity = None
        self._held = False

    def _exact_identity(self) -> NodeAttemptIdentity:
        child_identity = getattr(self.decorated, "attempt_identity", None)
        if isinstance(child_identity, NodeAttemptIdentity):
            return child_identity
        if self._identity is None:
            self._identity = self._attempt_source.begin(self._node_instance_id)
        return self._identity

    def tick(self) -> Iterator[py_trees.behaviour.Behaviour]:
        # The normal Decorator tick visits its child before update(). A held
        # failed child must not initialise a new attempt merely because an
        # independent Parallel sibling was resumed.
        if not self._held:
            yield from super().tick()
            return
        status = self.update()
        if status is not py_trees.common.Status.RUNNING:
            self._held = False
            self.stop(status)
        self.status = status
        yield self

    def update(self) -> py_trees.common.Status:
        if self._held:
            identity = self._exact_identity()
            resolution = self._recovery_port.consume_resolution(identity)
            if resolution is None:
                return py_trees.common.Status.RUNNING
            action, _result = resolution
            if action == "retry":
                self.decorated.stop(py_trees.common.Status.INVALID)
                self._identity = None
                self._held = False
                return py_trees.common.Status.RUNNING
            if action == "manual_success":
                return py_trees.common.Status.SUCCESS
            return py_trees.common.Status.FAILURE

        if self.decorated.status is not py_trees.common.Status.FAILURE:
            return self.decorated.status
        identity = self._exact_identity()
        reason = getattr(
            self.decorated, "failure_reason_code", None
        ) or "program_node_failed"
        message = getattr(self.decorated, "failure_message", "")
        owner_context = getattr(
            self.decorated, "owner_recovery_context", None
        )
        owner_context = dict(owner_context) if isinstance(owner_context, dict) else {}
        self._recovery_port.require(
            identity,
            reason_code=reason,
            message=message,
            policy_parameter="recovery_policy",
            **owner_context,
        )
        self._held = True
        return py_trees.common.Status.RUNNING


def recovery_policy_is_actionable(policy: ProgramNodePolicy) -> bool:
    recovery = policy.recovery_policy
    actions = recovery["allowed_recovery_actions"]
    return bool(
        (
            recovery["allow_intervention"]
            and "intervention" in actions
        )
        or (
            recovery["max_retries"] > 0
            and "runtime_override" in actions
        )
    )


__all__ = ["RecoveryAwareNode", "recovery_policy_is_actionable"]
