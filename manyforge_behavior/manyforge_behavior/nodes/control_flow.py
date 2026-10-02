"""Control flow nodes for ManyForge behavior tree.

py_trees Decorator.tick() already ticks the child before calling update().
In update(), read self.decorated.status — do NOT call self.decorated.tick() again.
Each cycle/attempt advances once per tree tick.

A Program node's parameters, its count or policy included, are resolved once
per attempt from that attempt's input snapshot, like every other node's
(spec 420 §9): a value source is honoured and the result is checked against
the catalog schema then. An attempt whose parameters cannot be resolved
fails with a named reason before any child is ticked; it never falls back
to another value.
"""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from dataclasses import dataclass, field
from typing import Any

import py_trees

from manyforge_behavior.failure_details import (
    NodeFailureOrigin,
    exception_failure_details,
)
from manyforge_behavior.orchestration_values import (
    INPUT_INVALID,
    NodeAttemptInputResolver,
    NodeInputError,
    ProgramValueError,
    input_failure_details,
    input_failure_reason,
)


_MAX_BOUNDED_COUNT = 1_000_000


def _validated_count(value: int, *, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{field} must be an integer")
    if value != -1 and not 1 <= value <= _MAX_BOUNDED_COUNT:
        raise ValueError(f"{field} must be -1 or in [1, {_MAX_BOUNDED_COUNT}]")
    return value


@dataclass(frozen=True, slots=True)
class NodeAttemptParameters:
    """One authored node's parameters, resolved anew for each attempt."""

    node_id: str
    node_instance_id: str
    parameters: Mapping[str, Any]
    value_sources: Mapping[str, Mapping[str, Any]] = field(default_factory=dict)
    input_resolver: NodeAttemptInputResolver | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.input_resolver, NodeAttemptInputResolver):
            raise TypeError("attempt parameters require one node-attempt input resolver")

    def resolve(self) -> Mapping[str, Any]:
        assert self.input_resolver is not None  # Checked at construction.
        return self.input_resolver.snapshot(
            node_id=self.node_id,
            node_instance_id=self.node_instance_id,
            parameters=self.parameters,
            value_sources=self.value_sources,
        ).parameters


class _AttemptInputs:
    """Resolve a control node's parameters at the start of each attempt.

    Failures carry the same named reason and details as any node's input
    failure (`node_input_unavailable`, or `expression_evaluation_failed`).
    """

    failure_reason_code: str | None = None
    failure_message: str = ""
    failure_details: Mapping[str, Any] | None = None
    failure_origin: NodeFailureOrigin | None = None
    # The parameter `_apply_attempt` interprets, named by its failure.
    applied_parameter: str | None = None

    def _resolve_attempt(
        self, inputs: NodeAttemptParameters | None, fixed: Mapping[str, Any]
    ) -> Mapping[str, Any] | None:
        self.failure_reason_code = None
        self.failure_message = ""
        self.failure_details = None
        self.failure_origin = None
        if inputs is None:
            return fixed
        try:
            resolved = inputs.resolve()
            try:
                self._apply_attempt(resolved)
            except (KeyError, TypeError, ValueError) as exc:
                source = inputs.value_sources.get(self.applied_parameter or "", {})
                kind = source.get("kind")
                raise NodeInputError(
                    str(exc),
                    details=input_failure_details(
                        input_problem=INPUT_INVALID,
                        path=self.applied_parameter,
                        source_kind=(
                            kind
                            if kind
                            in {
                                "program_parameter",
                                "blackboard_key",
                                "blackboard_field",
                                "expression",
                            }
                            else "parameter"
                        ),
                        source_reference=(
                            source.get("expression")
                            if kind == "expression"
                            else source.get("reference")
                        ),
                    ),
                ) from exc
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            reason = input_failure_reason(exc)
            self.failure_reason_code = reason
            self.failure_message = str(exc)
            self.failure_details = exception_failure_details(exc)
            self.failure_origin = NodeFailureOrigin("program_node")
            self.feedback_message = f"{reason}: {exc}"  # type: ignore[attr-defined]
            return None
        return resolved

    def _apply_attempt(self, parameters: Mapping[str, Any]) -> None:
        raise NotImplementedError


class _CountedDecorator(_AttemptInputs, py_trees.decorators.Decorator):
    count_parameter: str

    @property
    def applied_parameter(self) -> str:  # type: ignore[override]
        return self.count_parameter

    def __init__(
        self,
        name: str,
        child: py_trees.behaviour.Behaviour,
        count: int | None,
        attempt_inputs: NodeAttemptParameters | None,
    ) -> None:
        super().__init__(name=name, child=child)
        if (count is None) == (attempt_inputs is None):
            raise ValueError(
                f"{self.count_parameter} is either fixed or resolved per attempt"
            )
        self._attempt_inputs = attempt_inputs
        self._fixed = (
            {}
            if count is None
            else {self.count_parameter: _validated_count(count, field=self.count_parameter)}
        )
        self._count = self._fixed.get(self.count_parameter, 0)

    def _apply_attempt(self, parameters: Mapping[str, Any]) -> None:
        self._count = _validated_count(
            parameters[self.count_parameter], field=self.count_parameter
        )

    def tick(self) -> Iterator[py_trees.behaviour.Behaviour]:
        if self.status is not py_trees.common.Status.RUNNING:
            if self._resolve_attempt(self._attempt_inputs, self._fixed) is None:
                # Fail this attempt before its child is ticked.
                if self.decorated.status is not py_trees.common.Status.INVALID:
                    self.decorated.stop(py_trees.common.Status.INVALID)
                self.stop(py_trees.common.Status.FAILURE)
                yield self
                return
        yield from super().tick()


class RepeatDecorator(_CountedDecorator):
    """Repeat a child ``num_cycles`` times, or indefinitely for -1.

    Each tree tick advances at most one cycle: when the child returns SUCCESS,
    the cycle counter increments and RUNNING is returned so the tree ticks again.
    When all cycles complete, SUCCESS is returned.  FAILURE from the child
    propagates immediately. The count is fixed, or resolved per attempt.
    """

    count_parameter = "num_cycles"

    def __init__(
        self,
        name: str,
        child: py_trees.behaviour.Behaviour,
        num_cycles: int | None = None,
        *,
        attempt_inputs: NodeAttemptParameters | None = None,
    ):
        super().__init__(name, child, num_cycles, attempt_inputs)
        self._cycle_count = 0

    @property
    def _num_cycles(self) -> int:
        return self._count

    def initialise(self):
        self._cycle_count = 0

    def update(self):
        if self.decorated.status == py_trees.common.Status.SUCCESS:
            if self._num_cycles == -1:
                # No cumulative counter or inner loop for indefinite work.
                return py_trees.common.Status.RUNNING
            self._cycle_count += 1
            if self._cycle_count >= self._num_cycles:
                return py_trees.common.Status.SUCCESS
            # More cycles needed — reset child so it re-runs on the next tree tick
            return py_trees.common.Status.RUNNING
        if self.decorated.status == py_trees.common.Status.FAILURE:
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.RUNNING


class RetryDecorator(_CountedDecorator):
    """Retry on FAILURE up to ``num_attempts``, or indefinitely for -1.

    Each tree tick advances at most one attempt: when the child returns FAILURE,
    the attempt counter increments and RUNNING is returned so the tree ticks again.
    SUCCESS from the child propagates immediately. The count is fixed, or
    resolved once when the Retry node itself starts an attempt.
    """

    count_parameter = "num_attempts"

    def __init__(
        self,
        name: str,
        child: py_trees.behaviour.Behaviour,
        num_attempts: int | None = None,
        *,
        attempt_inputs: NodeAttemptParameters | None = None,
    ):
        super().__init__(name, child, num_attempts, attempt_inputs)
        self._attempt_count = 0

    @property
    def _num_attempts(self) -> int:
        return self._count

    def initialise(self):
        self._attempt_count = 0

    def update(self):
        if self.decorated.status == py_trees.common.Status.SUCCESS:
            return py_trees.common.Status.SUCCESS
        if self.decorated.status == py_trees.common.Status.FAILURE:
            if self._num_attempts == -1:
                return py_trees.common.Status.RUNNING
            self._attempt_count += 1
            if self._attempt_count >= self._num_attempts:
                return py_trees.common.Status.FAILURE
            # More attempts available — reset child so it re-runs on the next tree tick
            return py_trees.common.Status.RUNNING
        return py_trees.common.Status.RUNNING


_PARALLEL_POLICIES = {
    "all": py_trees.common.ParallelPolicy.SuccessOnAll,
    "one": py_trees.common.ParallelPolicy.SuccessOnOne,
}


class ProgramParallel(_AttemptInputs, py_trees.composites.Parallel):
    """`parallel` whose `success_policy` is resolved once per attempt."""

    applied_parameter = "success_policy"

    def __init__(
        self,
        name: str,
        children: list[py_trees.behaviour.Behaviour],
        *,
        attempt_inputs: NodeAttemptParameters,
    ) -> None:
        if not isinstance(attempt_inputs, NodeAttemptParameters):
            raise TypeError("parallel requires its per-attempt parameters")
        super().__init__(
            name=name,
            policy=py_trees.common.ParallelPolicy.SuccessOnAll(),
            children=children,
        )
        self._attempt_inputs = attempt_inputs

    def _apply_attempt(self, parameters: Mapping[str, Any]) -> None:
        try:
            policy = _PARALLEL_POLICIES[parameters["success_policy"]]
        except (KeyError, TypeError) as exc:
            raise ValueError("parallel success_policy must be 'all' or 'one'") from exc
        self.policy = policy()

    def tick(self) -> Iterator[py_trees.behaviour.Behaviour]:
        if self.status is not py_trees.common.Status.RUNNING:
            if self._resolve_attempt(self._attempt_inputs, {}) is None:
                # Fail this attempt before any child is ticked.
                for child in self.children:
                    if child.status is not py_trees.common.Status.INVALID:
                        child.stop(py_trees.common.Status.INVALID)
                self.current_child = None
                self.stop(py_trees.common.Status.FAILURE)
                yield self
                return
        yield from super().tick()


__all__ = [
    "NodeAttemptParameters",
    "ProgramParallel",
    "RepeatDecorator",
    "RetryDecorator",
]
