"""Small policy-aware hold around an existing executable Program node."""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from typing import Any

import py_trees

from manyforge_behavior.failure_details import failure_origin
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
        reason = getattr(self.decorated, "failure_reason_code", None)
        message = getattr(self.decorated, "failure_message", "")
        owner_context = getattr(
            self.decorated, "owner_recovery_context", None
        )
        owner_context = dict(owner_context) if isinstance(owner_context, dict) else {}
        self._recovery_port.require(
            identity,
            reason_code=reason or "program_node_failed",
            message=message,
            policy_parameter="recovery_policy",
            # A node that names no reason failed by itself (its condition).
            details=None if reason is None else self.decorated.failure_details,
            origin=(
                failure_origin("program_node", node_instance_id=self._node_instance_id)
                if reason is None
                else self.decorated.failure_origin.for_node(self._node_instance_id)
            ),
            **owner_context,
        )
        self._held = True
        return py_trees.common.Status.RUNNING

    @property
    def presented_status(self) -> py_trees.common.Status:
        if self.status is not py_trees.common.Status.RUNNING:
            return self.status
        return getattr(self.decorated, "presented_status", self.decorated.status)

    @property
    def presented_held(self) -> bool:
        return (self.status is py_trees.common.Status.RUNNING
                and getattr(self.decorated, "presented_held", False))

    @property
    def presented_interrupted(self) -> bool:
        return getattr(self.decorated, "presented_interrupted", False)

    @property
    def presented_stop_failed(self) -> bool:
        return getattr(self.decorated, "presented_stop_failed", False)

    def observe_while_operator_stop_held(self) -> None:
        # A child-before-parent observation pass has already settled the exact
        # leaf. Propagate/register its result without ticking a new attempt or
        # consuming a recovery decision while its scope is held.
        if self.status is py_trees.common.Status.RUNNING and not self._held and self.decorated.status in {
            py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE}:
            self.status = self.update()

    def tick_held_results(self) -> Iterator[py_trees.behaviour.Behaviour]:
        if self._held:
            yield self
        else:
            yield from super().tick()


def recovery_retry_permitted(
    recovery_policy: Mapping[str, Any], *, retries_used: int
) -> bool:
    """The one failure-intervention predicate for offers and commands.

    ``recovery_policy`` is the captured node's two-field policy (an empty
    mapping for a default node). Intervention is an explicit opt-in;
    ``max_retries`` zero means Unlimited, a positive value bounds the explicit
    failure retries of that node in the captured run. No caller identity,
    source label or endpoint participates.
    """

    if type(retries_used) is not int or retries_used < 0:
        raise ValueError("retries_used must be one nonnegative exact integer")
    max_retries = recovery_policy.get("max_retries", 0)
    if type(max_retries) is not int or max_retries < 0:
        raise ValueError("captured recovery policy max_retries is invalid")
    return recovery_policy.get("allow_intervention") is True and (
        max_retries == 0 or retries_used < max_retries
    )


def recovery_policy_is_actionable(policy: ProgramNodePolicy) -> bool:
    """Whether a failed attempt of this node is held for intervention."""

    return recovery_retry_permitted(policy.recovery_policy, retries_used=0)


__all__ = [
    "RecoveryAwareNode",
    "recovery_policy_is_actionable",
    "recovery_retry_permitted",
]
