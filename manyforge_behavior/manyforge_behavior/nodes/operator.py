"""Program nodes the run owner serves itself: an operator question and the
Program's own motion-group Stop.

Both stop motion groups the way the operator's group Stop does: the node
asks for the Stop when its attempt starts, and the run's tick sends it right
after the tree tick through the run owner's Kernel selection Stop
(``ProgramRunOwner.stop_motion``), with this attempt's own class 1 key, sent
again with the same key until the Kernel accepted it. The groups stay held
until the operator's Run; the nodes never release them.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from types import MappingProxyType
from typing import Any

import py_trees

from manyforge_behavior.failure_details import NodeFailureOrigin
from manyforge_behavior.nodes.interruption import InterruptedOutcome
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState
from manyforge_behavior.nodes.v2_pure import _SnapshotAdapter
from manyforge_behavior.orchestration_values import ProgramValueError
from manyforge_behavior.request_keys import RUNNER_KEYS

_NODE_FAILURE = NodeFailureOrigin("program_node")
OPERATOR_CHOICE_FAILED = "operator_choice_failed"
MOTION_GROUP_STOP_FAILED = "motion_group_stop_failed"


class _ProgramStopRequest(_SnapshotAdapter):
    """One attempt's request for the Program's own motion-group Stop."""

    group_parameter: str

    def __init__(
        self,
        *,
        audit_sink: Callable[[dict[str, Any]], None] | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._audit_sink = audit_sink
        self._stop_groups: tuple[str, ...] = ()
        self._stop_key: bytes | None = None
        # The hold revision of the Kernel's accepted Stop; None until then.
        self._stop_accepted_revision: int | None = None

    def initialise(self) -> None:
        super().initialise()
        self._stop_groups = ()
        self._stop_key = None
        self._stop_accepted_revision = None
        if self._inputs is not None:
            self._stop_groups = tuple(self._inputs.get(self.group_parameter) or ())
            if self._stop_groups:
                self._stop_key = RUNNER_KEYS.next_key()

    def _audit(self, kind: str, **facts: Any) -> None:
        if self._audit_sink is not None:
            self._audit_sink({"kind": kind, "node_instance_id": self._node_instance_id, **facts})

    @property
    def _stop_pending(self) -> bool:
        return bool(self._stop_groups) and self._stop_accepted_revision is None

    def program_stop_request(self) -> tuple[int, tuple[str, ...], bytes] | None:
        """``(node attempt, groups, key)`` while this running attempt's Stop
        has not been accepted; else None. Tick only."""

        if (
            self.status is not py_trees.common.Status.RUNNING
            or not self._stop_pending
            or self._identity is None
            or self._stop_key is None
        ):
            return None
        return self._identity.node_attempt_id, self._stop_groups, self._stop_key

    def note_program_stop_accepted(self, node_attempt_id: int, hold_revision: int) -> None:
        """The Kernel accepted this attempt's Stop at ``hold_revision``."""

        if (
            self._identity is None
            or self._identity.node_attempt_id != node_attempt_id
            or not self._stop_pending
        ):
            return
        self._stop_accepted_revision = hold_revision
        self._audit(
            "program_motion_groups_stopped",
            node_attempt_id=node_attempt_id,
            group_ids=list(self._stop_groups),
        )

    def terminate(self, new_status: py_trees.common.Status) -> None:
        # A stopped or finished attempt asks for nothing more; a Stop the
        # Kernel accepted stays in force until the operator's Run.
        self._stop_groups = ()
        self._stop_key = None


class OperatorRequestChoice(InterruptedOutcome, _ProgramStopRequest):
    """`operator.request_choice`: ask the operator; only this branch waits.

    The question is open while the attempt runs and has no answer. The
    answer is handed to the run's tick (``answer``); the node ends on its
    next tick once its own Stop (if any) is accepted: it writes the chosen
    option id to ``output_key`` and succeeds or fails as the option says.
    No timeout. A Program Stop or Reset ends it as interrupted; a Halt only
    freezes it.
    """

    node_id = "operator.request_choice"
    group_parameter = "hold_motion"

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._answer: Mapping[str, Any] | None = None

    def initialise(self) -> None:
        self._clear_interruption()
        self._answer = None
        super().initialise()
        if self._inputs is not None and self._identity is not None:
            self._audit(
                "program_operator_choice_requested",
                node_attempt_id=self._identity.node_attempt_id,
                severity=self._inputs["severity"],
                message=self._inputs["message"],
                option_ids=[option["option_id"] for option in self._inputs["options"]],
                hold_motion_group_ids=list(self._stop_groups),
            )

    def question(self) -> Mapping[str, Any] | None:
        """The open question of the running attempt, or None. Tick only."""

        if (
            self.status is not py_trees.common.Status.RUNNING
            or self._inputs is None
            or self._identity is None
            or self._answer is not None
        ):
            return None
        return MappingProxyType(
            {
                "node_instance_id": self._node_instance_id,
                "node_attempt_id": self._identity.node_attempt_id,
                "message": self._inputs["message"],
                "severity": self._inputs["severity"],
                "options": tuple(
                    MappingProxyType(
                        {
                            "option_id": option["option_id"],
                            "label": option["label"],
                            "result": option["result"],
                        }
                    )
                    for option in self._inputs["options"]
                ),
                "hold_motion_group_ids": self._stop_groups,
            }
        )

    def answer(self, node_attempt_id: int, option_id: str) -> Mapping[str, Any]:
        """Record the operator's answer to the open question. Tick only.

        Raises ``ValueError`` when the attempt has no open question (answered,
        ended or another attempt) or names no such option."""

        question = self.question()
        if question is None or question["node_attempt_id"] != node_attempt_id:
            raise ValueError("the operator question is no longer open")
        option = next(
            (row for row in question["options"] if row["option_id"] == option_id),
            None,
        )
        if option is None:
            raise ValueError("the operator question has no such option")
        self._answer = option
        return option

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        answer = self._answer
        if answer is None or self._stop_pending:
            return py_trees.common.Status.RUNNING
        output_key = inputs.get("output_key")
        if output_key is not None:
            try:
                self._write(output_key, answer["option_id"])
            except ProgramValueError as exc:
                self.feedback_message = f"blackboard_write_failed: {exc}"
                return py_trees.common.Status.FAILURE
        if answer["result"] == "success":
            return py_trees.common.Status.SUCCESS
        return self._fail(
            OPERATOR_CHOICE_FAILED,
            f"The operator chose {answer['label']}",
            _NODE_FAILURE,
        )

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._record_interruption(new_status)
        super().terminate(new_status)


class MotionStopGroups(_ProgramStopRequest):
    """`motion.stop_groups`: stop the groups as the operator's Stop does and
    succeed once that Stop has completed: accepted by the Kernel and no
    selected node still stopping. A selected node whose Stop ended faulted
    fails it (`motion_group_stop_failed`)."""

    node_id = "motion.stop_groups"
    group_parameter = "groups"

    def __init__(
        self,
        *,
        operator_stop_state: OperatorMotionStopState,
        execution_selections: Mapping[str, tuple[str, ...]],
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._stop_state = operator_stop_state
        self._selections = execution_selections

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        accepted = self._stop_accepted_revision
        if accepted is None:
            return py_trees.common.Status.RUNNING
        scope = self._stop_state.snapshot()
        if scope.hold_revision < accepted:
            return py_trees.common.Status.RUNNING
        selected = frozenset(
            node for group in inputs["groups"] for node in self._selections.get(group, ())
        )
        if selected & scope.faulted_nodes:
            return self._fail(
                MOTION_GROUP_STOP_FAILED,
                "a stopped node's Stop ended uncontained",
                _NODE_FAILURE,
            )
        if selected & scope.stopping_nodes:
            return py_trees.common.Status.RUNNING
        return py_trees.common.Status.SUCCESS


__all__ = [
    "MOTION_GROUP_STOP_FAILED",
    "MotionStopGroups",
    "OPERATOR_CHOICE_FAILED",
    "OperatorRequestChoice",
]
